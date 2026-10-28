// SPDX-License-Identifier: MIT
/*
 * OV5640 V4L2 -> latest complete frame -> NCNN YOLOv8n face detector.
 *
 * The capture loop immediately copies each completed V4L2 MMAP buffer into a
 * two-slot mailbox and QBUFs it back to the driver.  Inference always consumes
 * the newest available complete frame and may deliberately skip stale frames.
 * This is essential on Cortex-A9: camera capture remains at about 28.5 fps even
 * when one inference takes longer than one camera frame period.
 */

#define _POSIX_C_SOURCE 200809L

#include "face_detector.h"
#include "lcd_output.h"
#include "video_stream_server.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#if NCNN_SIMPLEOMP
#include <simpleomp.h>
#elif defined(_OPENMP)
#include <omp.h>
#endif

namespace {

const char *const kDefaultDevice = "/dev/video0";
const char *const kDefaultParam =
	"/home/root/face-model/model.ncnn.param";
const char *const kDefaultBin =
	"/home/root/face-model/model.ncnn.bin";
const char *const kDefaultOutput =
	"/home/root/face-last.ppm";

const unsigned int kSourceWidth = 640;
const unsigned int kSourceHeight = 480;
const unsigned int kChannelCount = 3;
const size_t kFrameBytes =
	static_cast<size_t>(kSourceWidth) * kSourceHeight * kChannelCount;
const unsigned int kRequestBufferCount = 4;
const unsigned int kMailboxSlotCount = 2;
const unsigned int kDefaultCaptureFrames = 300;
const int kPollTimeoutMs = 3000;
const unsigned int kDefaultPreviewFrameStride = 2;
const unsigned int kDefaultModelSize = 320;
const uint32_t kMaxOverlayAgeFrames = 60;

struct MappedBuffer {
	void *address;
	size_t length;
};

struct RuntimeStats {
	unsigned int processed_frames;
	unsigned int total_detections;
	uint32_t last_sequence;
	bool failed;
	double preprocess_ms;
	double inference_ms;
	double postprocess_ms;
	double maximum_inference_ms;
	unsigned int lcd_frames;
	double lcd_publish_ms;
};

struct LatestFrameMailbox {
	pthread_mutex_t mutex;
	pthread_cond_t frame_available;
	uint8_t *slots[kMailboxSlotCount];
	uint32_t slot_sequence[kMailboxSlotCount];
	uint32_t slot_bytesused[kMailboxSlotCount];
	unsigned int latest_slot;
	int processing_slot;
	uint64_t latest_generation;
	bool capture_done;

	FaceDetector *detector;
	LcdOutput *lcd;
	float confidence_threshold;
	float nms_threshold;
	std::string output_path;
	bool preview_mode;
	bool preview_ready;
	bool have_preview_detections;
	uint32_t preview_detection_sequence;
	std::vector<FaceDetection> preview_detections;
	RuntimeStats stats;
};

volatile sig_atomic_t stop_requested;

void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

int xioctl(int fd, unsigned long request, void *argument)
{
	int result;
	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && errno == EINTR);
	return result;
}

double elapsed_seconds(const struct timespec &start,
		       const struct timespec &finish)
{
	return static_cast<double>(finish.tv_sec - start.tv_sec) +
	       static_cast<double>(finish.tv_nsec - start.tv_nsec) /
		       1000000000.0;
}

void mark_worker_failed(LatestFrameMailbox *mailbox)
{
	pthread_mutex_lock(&mailbox->mutex);
	mailbox->stats.failed = true;
	mailbox->processing_slot = -1;
	pthread_mutex_unlock(&mailbox->mutex);
}

void *inference_worker(void *opaque)
{
	LatestFrameMailbox *mailbox =
		static_cast<LatestFrameMailbox *>(opaque);
	uint64_t processed_generation = 0;
	std::vector<uint8_t> last_annotated;
	bool have_annotated_frame = false;

	last_annotated.resize(kFrameBytes);

	for (;;) {
		uint64_t generation;
		unsigned int slot;
		uint32_t sequence;
		uint32_t bytesused;

		pthread_mutex_lock(&mailbox->mutex);
		while (mailbox->latest_generation == processed_generation &&
		       !mailbox->capture_done)
			pthread_cond_wait(&mailbox->frame_available,
					  &mailbox->mutex);

		if (mailbox->latest_generation == processed_generation &&
		    mailbox->capture_done) {
			pthread_mutex_unlock(&mailbox->mutex);
			break;
		}

		generation = mailbox->latest_generation;
		slot = mailbox->latest_slot;
		sequence = mailbox->slot_sequence[slot];
		bytesused = mailbox->slot_bytesused[slot];
		mailbox->processing_slot = static_cast<int>(slot);
		pthread_mutex_unlock(&mailbox->mutex);

		std::vector<FaceDetection> detections;
		FaceTiming timing;
		std::string error;
		if (!mailbox->detector->detect(
			    mailbox->slots[slot], kSourceWidth, kSourceHeight,
			    mailbox->confidence_threshold, mailbox->nms_threshold,
			    &detections, &timing, &error)) {
			fprintf(stderr, "face detector failed: %s\n", error.c_str());
			mark_worker_failed(mailbox);
			break;
		}

		memcpy(&last_annotated[0], mailbox->slots[slot], kFrameBytes);
		draw_face_detections_bgr(&last_annotated[0], kSourceWidth,
					 kSourceHeight, detections);
		have_annotated_frame = true;
		/* 独立副本已经形成，先释放 mailbox，LCD 等待不能阻塞采集线程。 */
		pthread_mutex_lock(&mailbox->mutex);
		if (mailbox->preview_mode) {
			mailbox->preview_detections = detections;
			mailbox->preview_detection_sequence = sequence;
			mailbox->have_preview_detections = true;
		}
		mailbox->processing_slot = -1;
		pthread_mutex_unlock(&mailbox->mutex);

		const bool publish_here = mailbox->lcd->enabled() &&
			(!mailbox->preview_mode || !mailbox->preview_ready);
		double lcd_ms = 0.0;
		if (publish_here) {
			struct timespec start, finish;
			clock_gettime(CLOCK_MONOTONIC, &start);
			if (!mailbox->lcd->publish(&last_annotated[0], kFrameBytes, &error)) {
				fprintf(stderr, "LCD publish failed: %s\n", error.c_str());
				mark_worker_failed(mailbox);
				break;
			}
			clock_gettime(CLOCK_MONOTONIC, &finish);
			lcd_ms = 1000.0 * elapsed_seconds(start, finish);
		}

		pthread_mutex_lock(&mailbox->mutex);
		mailbox->stats.processed_frames++;
		mailbox->stats.total_detections += detections.size();
		mailbox->stats.last_sequence = sequence;
		mailbox->stats.preprocess_ms += timing.preprocess_ms;
		mailbox->stats.inference_ms += timing.inference_ms;
		mailbox->stats.postprocess_ms += timing.postprocess_ms;
		mailbox->stats.maximum_inference_ms = std::max(
			mailbox->stats.maximum_inference_ms, timing.inference_ms);
		if (publish_here) {
			mailbox->stats.lcd_frames++;
			mailbox->stats.lcd_publish_ms += lcd_ms;
			if (mailbox->preview_mode)
				mailbox->preview_ready = true;
		}
		const unsigned int processed = mailbox->stats.processed_frames;
		mailbox->processing_slot = -1;
		processed_generation = generation;
		pthread_mutex_unlock(&mailbox->mutex);

		printf("inference=%u source_sequence=%u bytes=%u faces=%u "
		       "preprocess=%.2fms inference=%.2fms postprocess=%.2fms\n",
		       processed, sequence, bytesused,
		       static_cast<unsigned int>(detections.size()),
		       timing.preprocess_ms, timing.inference_ms,
		       timing.postprocess_ms);
		if (publish_here)
			printf("  lcd: source_sequence=%u publish=%.2fms\n", sequence, lcd_ms);
		for (size_t index = 0; index < detections.size(); ++index) {
			const FaceDetection &face = detections[index];
			printf("  face[%u] score=%.3f box=(%.1f,%.1f)-(%.1f,%.1f)\n",
			       static_cast<unsigned int>(index), face.score,
			       face.x0, face.y0, face.x1, face.y1);
		}
		fflush(stdout);
	}

	if (have_annotated_frame && mailbox->output_path != "-") {
		std::string error;
		if (!write_bgr24_ppm(mailbox->output_path, &last_annotated[0],
				     kSourceWidth, kSourceHeight, &error)) {
			fprintf(stderr, "%s\n", error.c_str());
			mark_worker_failed(mailbox);
		} else {
			printf("saved_last_annotated=%s (%ux%u PPM)\n",
			       mailbox->output_path.c_str(), kSourceWidth,
			       kSourceHeight);
		}
	}

	return NULL;
}

bool parse_unsigned(const char *text, unsigned int maximum,
		    bool allow_zero, unsigned int *value)
{
	char *end = NULL;
	errno = 0;
	const unsigned long parsed = strtoul(text, &end, 10);
	if (errno || !text[0] || (end && *end) ||
	    (!allow_zero && parsed == 0) || parsed > maximum)
		return false;
	*value = static_cast<unsigned int>(parsed);
	return true;
}

bool parse_threshold(const char *text, float *value)
{
	char *end = NULL;
	errno = 0;
	const float parsed = strtof(text, &end);
	if (errno || !text[0] || (end && *end) || parsed <= 0.0f ||
	    parsed >= 1.0f)
		return false;
	*value = parsed;
	return true;
}

void print_usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [device] [model.param] [model.bin] "
		"[capture-frames|0] [threads] [confidence] [nms] "
		"[last-annotated.ppm|-] [lcd-sysfs-directory|-] "
		"[matched|preview] [preview-frame-stride:1..8] "
		"[model-input-size:192|224|256|320]\n\n"
		"0 capture frames means run until Ctrl-C.\n"
		"LCD is disabled unless its sysfs directory is supplied.\n"
		"Optional TCP video: STUDYBOARD_STREAM_PORT=5000 "
		"STUDYBOARD_STREAM_STRIDE=6 (raw BGR24, no boxes).\n"
		"Example:\n"
		"  %s /dev/video0 /home/root/face-model/model.ncnn.param "
		"/home/root/face-model/model.ncnn.bin 300 1 0.25 0.45 "
		"/home/root/face-last.ppm\n",
		program, program);
}

} // namespace

int main(int argc, char **argv)
{
	const char *device_name = kDefaultDevice;
	const char *param_path = kDefaultParam;
	const char *bin_path = kDefaultBin;
	unsigned int requested_frames = kDefaultCaptureFrames;
	unsigned int thread_count = 1;
	float confidence_threshold = 0.25f;
	float nms_threshold = 0.45f;
	const char *output_path = kDefaultOutput;
	const char *lcd_directory = "-";
	bool preview_mode = false;
	unsigned int preview_stride = kDefaultPreviewFrameStride;
	unsigned int model_size = kDefaultModelSize;
	unsigned int stream_port = 0;
	unsigned int stream_stride = 6;

	if (argc > 13) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2)
		device_name = argv[1];
	if (argc >= 3)
		param_path = argv[2];
	if (argc >= 4)
		bin_path = argv[3];
	if (argc >= 5 &&
	    !parse_unsigned(argv[4], UINT32_MAX, true, &requested_frames)) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 6 &&
	    !parse_unsigned(argv[5], 16, false, &thread_count)) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 7 && !parse_threshold(argv[6], &confidence_threshold)) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 8 && !parse_threshold(argv[7], &nms_threshold)) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 9)
		output_path = argv[8];
	if (argc >= 10)
		lcd_directory = argv[9];
	if (argc >= 11) {
		if (strcmp(argv[10], "preview") == 0)
			preview_mode = true;
		else if (strcmp(argv[10], "matched") != 0) {
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (preview_mode && strcmp(lcd_directory, "-") == 0) {
		fprintf(stderr, "preview mode needs an LCD sysfs directory\n");
		return EXIT_FAILURE;
	}
	if (argc >= 12 &&
	    !parse_unsigned(argv[11], 8, false, &preview_stride)) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 13 &&
	    (!parse_unsigned(argv[12], 320, false, &model_size) ||
	     (model_size != 192 && model_size != 224 &&
	      model_size != 256 && model_size != 320))) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	const char *stream_port_text = getenv("STUDYBOARD_STREAM_PORT");
	if (stream_port_text && stream_port_text[0] &&
	    !parse_unsigned(stream_port_text, 65535, true, &stream_port)) {
		fprintf(stderr, "STUDYBOARD_STREAM_PORT must be 0..65535\n");
		return EXIT_FAILURE;
	}
	const char *stream_stride_text = getenv("STUDYBOARD_STREAM_STRIDE");
	if (stream_stride_text && stream_stride_text[0] &&
	    !parse_unsigned(stream_stride_text, 120, false, &stream_stride)) {
		fprintf(stderr, "STUDYBOARD_STREAM_STRIDE must be 1..120\n");
		return EXIT_FAILURE;
	}

	LcdOutput lcd;
	FaceDetector detector;
	std::string detector_error;
	if (!lcd.configure(lcd_directory, kFrameBytes, &detector_error)) {
		fprintf(stderr, "%s\n", detector_error.c_str());
		return EXIT_FAILURE;
	}
	if (!detector.load(param_path, bin_path, thread_count, model_size,
			   &detector_error)) {
		fprintf(stderr, "%s\n", detector_error.c_str());
		return EXIT_FAILURE;
	}

	LatestFrameMailbox mailbox;
	VideoStreamServer network;
	bool network_enabled = false;
	std::string network_error;
	memset(&mailbox.stats, 0, sizeof(mailbox.stats));
	memset(mailbox.slots, 0, sizeof(mailbox.slots));
	memset(mailbox.slot_sequence, 0, sizeof(mailbox.slot_sequence));
	memset(mailbox.slot_bytesused, 0, sizeof(mailbox.slot_bytesused));
	mailbox.latest_slot = 0;
	mailbox.processing_slot = -1;
	mailbox.latest_generation = 0;
	mailbox.capture_done = false;
	mailbox.detector = &detector;
	mailbox.lcd = &lcd;
	mailbox.confidence_threshold = confidence_threshold;
	mailbox.nms_threshold = nms_threshold;
	mailbox.output_path = output_path;
	mailbox.preview_mode = preview_mode;
	mailbox.preview_ready = false;
	mailbox.have_preview_detections = false;
	mailbox.preview_detection_sequence = 0;

	bool mutex_initialized = false;
	bool condition_initialized = false;
	bool worker_started = false;
	bool streaming = false;
	bool capture_clock_started = false;
	pthread_t worker_thread;
	MappedBuffer *buffers = NULL;
	uint8_t *preview_frame = NULL;
	unsigned int mapped_count = 0;
	unsigned int captured_frames = 0;
	unsigned int sequence_gaps = 0;
	unsigned int bad_payloads = 0;
	uint32_t previous_sequence = 0;
	bool have_previous_sequence = false;
	int fd = -1;
	int exit_status = EXIT_FAILURE;
	int result;
	uint32_t effective_caps = 0;
	struct timespec capture_start;
	struct timespec capture_finish;
	struct v4l2_capability capability;
	struct v4l2_format format;
	struct v4l2_requestbuffers request;
	memset(&capability, 0, sizeof(capability));
	memset(&format, 0, sizeof(format));
	memset(&request, 0, sizeof(request));

	result = pthread_mutex_init(&mailbox.mutex, NULL);
	if (result) {
		fprintf(stderr, "pthread_mutex_init: %s\n", strerror(result));
		goto cleanup;
	}
	mutex_initialized = true;
	result = pthread_cond_init(&mailbox.frame_available, NULL);
	if (result) {
		fprintf(stderr, "pthread_cond_init: %s\n", strerror(result));
		goto cleanup;
	}
	condition_initialized = true;

	for (unsigned int index = 0; index < kMailboxSlotCount; ++index) {
		mailbox.slots[index] =
			static_cast<uint8_t *>(malloc(kFrameBytes));
		if (!mailbox.slots[index]) {
			perror("malloc mailbox slot");
			goto cleanup;
		}
	}
	if (preview_mode) {
		preview_frame = static_cast<uint8_t *>(malloc(kFrameBytes));
		if (!preview_frame) {
			perror("malloc preview frame");
			goto cleanup;
		}
	}
	if (stream_port) {
		if (!network.start(static_cast<uint16_t>(stream_port),
				   stream_stride, kSourceWidth, kSourceHeight,
				   &network_error)) {
			fprintf(stderr, "network: %s\n", network_error.c_str());
			goto cleanup;
		}
		network_enabled = true;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	fd = open(device_name, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("open video device");
		goto cleanup;
	}

	memset(&capability, 0, sizeof(capability));
	if (xioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
		perror("VIDIOC_QUERYCAP");
		goto cleanup;
	}
	effective_caps =
		(capability.capabilities & V4L2_CAP_DEVICE_CAPS) ?
			capability.device_caps : capability.capabilities;
	if (!(effective_caps & V4L2_CAP_VIDEO_CAPTURE) ||
	    !(effective_caps & V4L2_CAP_STREAMING)) {
		fprintf(stderr,
			"Device does not provide VIDEO_CAPTURE + STREAMING\n");
		goto cleanup;
	}

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = kSourceWidth;
	format.fmt.pix.height = kSourceHeight;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
		perror("VIDIOC_S_FMT");
		goto cleanup;
	}
	if (format.fmt.pix.width != kSourceWidth ||
	    format.fmt.pix.height != kSourceHeight ||
	    format.fmt.pix.pixelformat != V4L2_PIX_FMT_BGR24 ||
	    format.fmt.pix.sizeimage != kFrameBytes) {
		fprintf(stderr,
			"Unexpected format: %ux%u fourcc=0x%08x size=%u\n",
			format.fmt.pix.width, format.fmt.pix.height,
			format.fmt.pix.pixelformat, format.fmt.pix.sizeimage);
		goto cleanup;
	}

	request.count = kRequestBufferCount;
	request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	request.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &request) < 0) {
		perror("VIDIOC_REQBUFS");
		goto cleanup;
	}
	if (request.count < 3) {
		fprintf(stderr, "Driver returned only %u buffers\n", request.count);
		goto cleanup;
	}

	buffers = static_cast<MappedBuffer *>(
		calloc(request.count, sizeof(*buffers)));
	if (!buffers) {
		perror("calloc mapped buffers");
		goto cleanup;
	}

	for (unsigned int index = 0; index < request.count; ++index) {
		struct v4l2_buffer buffer;
		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
			perror("VIDIOC_QUERYBUF");
			goto cleanup;
		}

		buffers[index].length = buffer.length;
		buffers[index].address = mmap(NULL, buffer.length,
					      PROT_READ | PROT_WRITE,
					      MAP_SHARED, fd, buffer.m.offset);
		if (buffers[index].address == MAP_FAILED) {
			buffers[index].address = NULL;
			perror("mmap video buffer");
			goto cleanup;
		}
		mapped_count++;
	}

	for (unsigned int index = 0; index < request.count; ++index) {
		struct v4l2_buffer buffer;
		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("initial VIDIOC_QBUF");
			goto cleanup;
		}
	}

	result = pthread_create(&worker_thread, NULL, inference_worker, &mailbox);
	if (result) {
		fprintf(stderr, "pthread_create: %s\n", strerror(result));
		goto cleanup;
	}
	worker_started = true;

	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
			perror("VIDIOC_STREAMON");
			goto finish_capture;
		}
	}
	streaming = true;
	clock_gettime(CLOCK_MONOTONIC, &capture_start);
	capture_clock_started = true;

	printf("model: input=in0 [1,3,%d,%d], output=out0 [1,5,%d]\n",
	       detector.input_size(), detector.input_size(),
	       detector.prediction_count());
	printf("pipeline: 640x480 BGR24 -> letterbox %ux%u -> NCNN "
	       "YOLOv8n -> NMS -> source coordinates\n",
	       model_size, model_size);
#if NCNN_SIMPLEOMP
	printf("parallel_runtime: NCNN simpleomp enabled "
	       "requested_threads=%u max_threads=%d\n",
	       thread_count, omp_get_max_threads());
#elif defined(_OPENMP)
	printf("parallel_runtime: OpenMP enabled (_OPENMP=%d) "
	       "requested_threads=%u max_threads=%d\n",
	       _OPENMP, thread_count, omp_get_max_threads());
#else
	printf("parallel_runtime: OpenMP disabled; requested_threads=%u "
	       "runs serially\n", thread_count);
#endif
	printf("settings: capture_frames=%u (0=infinite) threads=%u "
	       "confidence=%.3f nms=%.3f output=%s\n",
	       requested_frames, thread_count, confidence_threshold,
	       nms_threshold, output_path);
	printf("lcd_output: %s mode=%s\n", lcd_directory,
	       preview_mode ? "preview with latest completed detection" :
			      "matched inference image");
	if (preview_mode)
		printf("preview: display every %u captured frames, hide boxes older "
		       "than %u frames\n", preview_stride,
		       kMaxOverlayAgeFrames);
	if (network_enabled)
		printf("network: raw 640x480 BGR24 TCP port=%u, sample every "
		       "%u captured frames (no red boxes)\n",
		       stream_port, stream_stride);

	while ((requested_frames == 0 || captured_frames < requested_frames) &&
	       !stop_requested) {
		pthread_mutex_lock(&mailbox.mutex);
		const bool worker_failed = mailbox.stats.failed;
		pthread_mutex_unlock(&mailbox.mutex);
		if (worker_failed)
			break;

		struct pollfd descriptor;
		descriptor.fd = fd;
		descriptor.events = POLLIN | POLLPRI;
		descriptor.revents = 0;
		const int poll_result = poll(&descriptor, 1, kPollTimeoutMs);
		if (poll_result < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (poll_result == 0) {
			fprintf(stderr, "Timeout waiting for frame %u\n",
				captured_frames);
			break;
		}
		if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "poll revents=0x%x\n", descriptor.revents);
			break;
		}

		struct v4l2_buffer buffer;
		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN)
				continue;
			perror("VIDIOC_DQBUF");
			break;
		}
		if (buffer.index >= request.count) {
			fprintf(stderr, "Invalid buffer index %u\n", buffer.index);
			break;
		}

		const uint32_t sequence = buffer.sequence;
		if (buffer.bytesused != kFrameBytes) {
			bad_payloads++;
			fprintf(stderr, "Frame %u bytesused=%u expected=%u\n",
				captured_frames, buffer.bytesused,
				static_cast<unsigned int>(kFrameBytes));
		}
		if (have_previous_sequence &&
		    sequence != static_cast<uint32_t>(previous_sequence + 1U)) {
			sequence_gaps++;
			fprintf(stderr, "Sequence gap: previous=%u current=%u\n",
				previous_sequence, sequence);
		}
		previous_sequence = sequence;
		have_previous_sequence = true;

		size_t copy_length = std::min(
			static_cast<size_t>(buffer.bytesused), kFrameBytes);
		copy_length = std::min(copy_length,
				       buffers[buffer.index].length);

		pthread_mutex_lock(&mailbox.mutex);
		unsigned int slot;
		if (mailbox.processing_slot == 0)
			slot = 1;
		else if (mailbox.processing_slot == 1)
			slot = 0;
		else
			slot = mailbox.latest_slot ^ 1U;

		memcpy(mailbox.slots[slot], buffers[buffer.index].address,
		       copy_length);
		if (copy_length < kFrameBytes)
			memset(mailbox.slots[slot] + copy_length, 0,
			       kFrameBytes - copy_length);
		mailbox.slot_sequence[slot] = sequence;
		mailbox.slot_bytesused[slot] = buffer.bytesused;
		mailbox.latest_slot = slot;
		mailbox.latest_generation =
			static_cast<uint64_t>(captured_frames) + 1U;
		pthread_cond_signal(&mailbox.frame_available);
		pthread_mutex_unlock(&mailbox.mutex);

		if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("requeue VIDIOC_QBUF");
			break;
		}
		captured_frames++;
		if (network_enabled && copy_length == kFrameBytes)
			network.offer(mailbox.slots[slot], kFrameBytes,
				      sequence, captured_frames);

		if (preview_mode &&
		    ((captured_frames - 1U) % preview_stride) == 0U) {
			std::vector<FaceDetection> overlay;
			pthread_mutex_lock(&mailbox.mutex);
			const bool preview_ready = mailbox.preview_ready;
			if (preview_ready && mailbox.have_preview_detections &&
			    static_cast<uint32_t>(sequence -
				mailbox.preview_detection_sequence) <=
				kMaxOverlayAgeFrames)
				overlay = mailbox.preview_detections;
			pthread_mutex_unlock(&mailbox.mutex);
			if (!preview_ready)
				continue;
			/* The capture loop owns this slot until its next DQBUF. */
			memcpy(preview_frame, mailbox.slots[slot], kFrameBytes);
			draw_face_detections_bgr(preview_frame, kSourceWidth,
					 kSourceHeight, overlay);
			struct timespec lcd_start, lcd_finish;
			clock_gettime(CLOCK_MONOTONIC, &lcd_start);
			std::string lcd_error;
			if (!lcd.publish(preview_frame, kFrameBytes, &lcd_error)) {
				fprintf(stderr, "LCD preview publish failed: %s\n",
					lcd_error.c_str());
				pthread_mutex_lock(&mailbox.mutex);
				mailbox.stats.failed = true;
				pthread_mutex_unlock(&mailbox.mutex);
				break;
			}
			clock_gettime(CLOCK_MONOTONIC, &lcd_finish);
			pthread_mutex_lock(&mailbox.mutex);
			mailbox.stats.lcd_frames++;
			mailbox.stats.lcd_publish_ms +=
				1000.0 * elapsed_seconds(lcd_start, lcd_finish);
			pthread_mutex_unlock(&mailbox.mutex);
		}
	}

finish_capture:
	if (streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
			perror("VIDIOC_STREAMOFF");
		streaming = false;
	}
	if (capture_clock_started)
		clock_gettime(CLOCK_MONOTONIC, &capture_finish);
	if (network_enabled) {
		network.stop();
		network.print_summary();
	}

	if (condition_initialized) {
		pthread_mutex_lock(&mailbox.mutex);
		mailbox.capture_done = true;
		pthread_cond_broadcast(&mailbox.frame_available);
		pthread_mutex_unlock(&mailbox.mutex);
	}
	if (worker_started) {
		pthread_join(worker_thread, NULL);
		worker_started = false;
	}

	if (capture_clock_started && captured_frames > 0) {
		const double seconds = elapsed_seconds(capture_start,
						       capture_finish);
		const double capture_fps = seconds > 0.0 ?
			static_cast<double>(captured_frames) / seconds : 0.0;
		const unsigned int processed = mailbox.stats.processed_frames;
		const unsigned int dropped = captured_frames >= processed ?
			captured_frames - processed : 0;
		const double count = processed ? processed : 1;

		printf("capture_summary: frames=%u seconds=%.3f fps=%.2f "
		       "sequence_gaps=%u bad_payloads=%u\n",
		       captured_frames, seconds, capture_fps, sequence_gaps,
		       bad_payloads);
		printf("inference_summary: processed=%u skipped_stale=%u "
		       "total_faces=%u last_sequence=%u\n",
		       processed, dropped, mailbox.stats.total_detections,
		       mailbox.stats.last_sequence);
		printf("average_ms: preprocess=%.2f inference=%.2f "
		       "postprocess=%.2f maximum_inference=%.2f "
		       "effective_inference_fps=%.2f\n",
		       mailbox.stats.preprocess_ms / count,
		       mailbox.stats.inference_ms / count,
		       mailbox.stats.postprocess_ms / count,
		       mailbox.stats.maximum_inference_ms,
		       mailbox.stats.inference_ms > 0.0 ?
			       1000.0 * count / mailbox.stats.inference_ms : 0.0);
		if (lcd.enabled())
			printf("lcd_summary: frames=%u average_publish_ms=%.2f\n",
			       mailbox.stats.lcd_frames,
			       mailbox.stats.lcd_frames ? mailbox.stats.lcd_publish_ms /
				       mailbox.stats.lcd_frames : 0.0);

		if (processed > 0 && sequence_gaps == 0 && bad_payloads == 0 &&
		    !mailbox.stats.failed)
			exit_status = EXIT_SUCCESS;
	}

cleanup:
	network.stop();
	if (streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		xioctl(fd, VIDIOC_STREAMOFF, &type);
	}
	if (worker_started) {
		pthread_mutex_lock(&mailbox.mutex);
		mailbox.capture_done = true;
		pthread_cond_broadcast(&mailbox.frame_available);
		pthread_mutex_unlock(&mailbox.mutex);
		pthread_join(worker_thread, NULL);
	}
	/* 必须先停 MM2S，再关闭 V4L2 fd（后者也有驱动端兜底清理）。 */
	if (!lcd.stop(&detector_error)) {
		fprintf(stderr, "LCD stop failed: %s\n", detector_error.c_str());
		exit_status = EXIT_FAILURE;
	}
	for (unsigned int index = 0; index < mapped_count; ++index) {
		if (buffers[index].address)
			munmap(buffers[index].address, buffers[index].length);
	}
	free(buffers);
	if (fd >= 0)
		close(fd);
	for (unsigned int index = 0; index < kMailboxSlotCount; ++index)
		free(mailbox.slots[index]);
	free(preview_frame);
	if (condition_initialized)
		pthread_cond_destroy(&mailbox.frame_available);
	if (mutex_initialized)
		pthread_mutex_destroy(&mailbox.mutex);

	return exit_status;
}
