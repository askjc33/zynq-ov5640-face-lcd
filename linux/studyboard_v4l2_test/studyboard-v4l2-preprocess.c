// SPDX-License-Identifier: MIT
/*
 * studyboard-v4l2-preprocess.c
 *
 * StudyBoard OV5640 real-time CPU preprocessing example.
 *
 * Data flow
 * ---------
 *   OV5640/PL/VDMA
 *        -> V4L2 MMAP buffer (640x480 BGR24)
 *        -> two-slot latest-frame mailbox (CPU copy)
 *        -> BGR24 to RGB24
 *        -> bilinear resize (default 224x224)
 *        -> HWC uint8 to CHW float32, normalized to [0, 1]
 *        -> future inference engine
 *
 * The capture thread returns each MMAP buffer to the driver immediately after
 * copying it.  The worker is allowed to skip stale mailbox generations when
 * preprocessing is slower than the camera.  This preserves low latency and
 * prevents a slow CPU algorithm from stopping VDMA capture.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE         "/dev/video0"
#define DEFAULT_CAPTURE_FRAMES 300U
#define DEFAULT_OUTPUT_WIDTH   224U
#define DEFAULT_OUTPUT_HEIGHT  224U
#define DEFAULT_OUTPUT_PATH    "-"

#define REQUEST_BUFFER_COUNT   4U
#define SOURCE_WIDTH           640U
#define SOURCE_HEIGHT          480U
#define CHANNEL_COUNT          3U
#define SOURCE_FRAME_BYTES     (SOURCE_WIDTH * SOURCE_HEIGHT * CHANNEL_COUNT)
#define POLL_TIMEOUT_MS        3000
#define MAILBOX_SLOT_COUNT     2U
#define REPORT_INTERVAL        30U
#define MAX_OUTPUT_DIMENSION   4096U

struct mapped_buffer {
	void *address;
	size_t length;
};

/* One precomputed coordinate for one output row or column. */
struct axis_map {
	unsigned int lower;
	unsigned int upper;
	float upper_weight;
};

struct preprocessing_stats {
	unsigned int processed_frames;
	unsigned int digest_changes;
	uint32_t last_digest;
	uint32_t last_source_sequence;
	bool have_digest;
	bool failed;

	double bgr_to_rgb_seconds;
	double resize_seconds;
	double normalize_seconds;
	double total_seconds;
	double maximum_total_ms;

	float last_minimum[CHANNEL_COUNT];
	float last_maximum[CHANNEL_COUNT];
	float last_mean[CHANNEL_COUNT];
};

/*
 * Shared two-slot mailbox.
 *
 * processing_slot tells the capture thread which slot the worker currently
 * owns.  The capture thread may overwrite only the other slot.  Fields shared
 * by the two threads are accessed while mutex is held.
 */
struct latest_frame_mailbox {
	pthread_mutex_t mutex;
	pthread_cond_t frame_available;
	uint8_t *slots[MAILBOX_SLOT_COUNT];
	uint32_t slot_sequence[MAILBOX_SLOT_COUNT];
	uint32_t slot_bytesused[MAILBOX_SLOT_COUNT];
	unsigned int latest_slot;
	int processing_slot;
	uint64_t latest_generation;
	bool capture_done;

	unsigned int output_width;
	unsigned int output_height;
	const char *output_path;
	struct preprocessing_stats stats;
};

struct worker_argument {
	struct latest_frame_mailbox *mailbox;
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static int xioctl(int fd, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && errno == EINTR);

	return result;
}

static uint32_t fnv1a32(const uint8_t *data, size_t length)
{
	uint32_t hash = 2166136261U;
	size_t index;

	for (index = 0; index < length; ++index) {
		hash ^= data[index];
		hash *= 16777619U;
	}
	return hash;
}

static double elapsed_seconds(const struct timespec *start,
			      const struct timespec *finish)
{
	return (double)(finish->tv_sec - start->tv_sec) +
	       (double)(finish->tv_nsec - start->tv_nsec) / 1000000000.0;
}

/* The VDMA byte order exposed by this driver is B, G, R. */
static void bgr24_to_rgb24(const uint8_t *source, uint8_t *destination,
			   size_t pixel_count)
{
	size_t pixel;

	for (pixel = 0; pixel < pixel_count; ++pixel) {
		destination[pixel * 3U + 0U] = source[pixel * 3U + 2U];
		destination[pixel * 3U + 1U] = source[pixel * 3U + 1U];
		destination[pixel * 3U + 2U] = source[pixel * 3U + 0U];
	}
}

/*
 * Map output pixel centres back into source coordinates.  Precomputing this
 * table avoids repeating division and boundary handling for every frame.
 */
static void build_axis_map(struct axis_map *map, unsigned int destination_size,
			   unsigned int source_size)
{
	unsigned int destination;

	for (destination = 0; destination < destination_size; ++destination) {
		float source_coordinate =
			((float)destination + 0.5f) * (float)source_size /
			(float)destination_size - 0.5f;

		if (source_coordinate <= 0.0f) {
			map[destination].lower = 0;
			map[destination].upper = 0;
			map[destination].upper_weight = 0.0f;
		} else if (source_coordinate >= (float)(source_size - 1U)) {
			map[destination].lower = source_size - 1U;
			map[destination].upper = source_size - 1U;
			map[destination].upper_weight = 0.0f;
		} else {
			unsigned int lower = (unsigned int)source_coordinate;

			map[destination].lower = lower;
			map[destination].upper = lower + 1U;
			map[destination].upper_weight =
				source_coordinate - (float)lower;
		}
	}
}

static uint8_t rounded_u8(float value)
{
	if (value <= 0.0f)
		return 0;
	if (value >= 255.0f)
		return 255;
	return (uint8_t)(value + 0.5f);
}

/* RGB-interleaved bilinear resize. */
static void resize_rgb24_bilinear(const uint8_t *source,
				  unsigned int source_width,
				  uint8_t *destination,
				  unsigned int destination_width,
				  unsigned int destination_height,
				  const struct axis_map *x_map,
				  const struct axis_map *y_map)
{
	unsigned int y;

	for (y = 0; y < destination_height; ++y) {
		const struct axis_map *ym = &y_map[y];
		const uint8_t *row0 = source +
			(size_t)ym->lower * source_width * CHANNEL_COUNT;
		const uint8_t *row1 = source +
			(size_t)ym->upper * source_width * CHANNEL_COUNT;
		float wy = ym->upper_weight;
		unsigned int x;

		for (x = 0; x < destination_width; ++x) {
			const struct axis_map *xm = &x_map[x];
			float wx = xm->upper_weight;
			unsigned int channel;

			for (channel = 0; channel < CHANNEL_COUNT; ++channel) {
				float top =
					(float)row0[xm->lower * 3U + channel] +
					((float)row0[xm->upper * 3U + channel] -
					 (float)row0[xm->lower * 3U + channel]) * wx;
				float bottom =
					(float)row1[xm->lower * 3U + channel] +
					((float)row1[xm->upper * 3U + channel] -
					 (float)row1[xm->lower * 3U + channel]) * wx;
				float value = top + (bottom - top) * wy;

				destination[((size_t)y * destination_width + x) * 3U +
					    channel] = rounded_u8(value);
			}
		}
	}
}

/*
 * Convert RGB HWC uint8 into the common inference layout CHW float32.
 * Plane 0 contains all R values, plane 1 all G, plane 2 all B.  The current
 * neutral normalization is value / 255.  A future model can add its own mean
 * and scale constants here.
 */
static void rgb24_to_chw_float(const uint8_t *rgb, float *tensor,
			       size_t pixel_count, float minimum[3],
			       float maximum[3], float mean[3])
{
	double sum[3] = { 0.0, 0.0, 0.0 };
	size_t pixel;
	unsigned int channel;

	for (channel = 0; channel < CHANNEL_COUNT; ++channel) {
		minimum[channel] = 1.0f;
		maximum[channel] = 0.0f;
	}

	for (pixel = 0; pixel < pixel_count; ++pixel) {
		for (channel = 0; channel < CHANNEL_COUNT; ++channel) {
			float value = (float)rgb[pixel * 3U + channel] /
				      255.0f;

			tensor[(size_t)channel * pixel_count + pixel] = value;
			if (value < minimum[channel])
				minimum[channel] = value;
			if (value > maximum[channel])
				maximum[channel] = value;
			sum[channel] += value;
		}
	}

	for (channel = 0; channel < CHANNEL_COUNT; ++channel)
		mean[channel] = (float)(sum[channel] / (double)pixel_count);
}

static int write_ppm(const char *path, const uint8_t *rgb,
		     unsigned int width, unsigned int height)
{
	size_t bytes = (size_t)width * height * CHANNEL_COUNT;
	FILE *file = fopen(path, "wb");

	if (!file)
		return -1;
	if (fprintf(file, "P6\n%u %u\n255\n", width, height) < 0 ||
	    fwrite(rgb, 1, bytes, file) != bytes || fclose(file) != 0)
		return -1;
	return 0;
}

static void mark_worker_failed(struct latest_frame_mailbox *mailbox)
{
	pthread_mutex_lock(&mailbox->mutex);
	mailbox->stats.failed = true;
	mailbox->processing_slot = -1;
	pthread_mutex_unlock(&mailbox->mutex);
}

static void *preprocessing_worker(void *opaque)
{
	struct worker_argument *argument = opaque;
	struct latest_frame_mailbox *mailbox = argument->mailbox;
	const size_t source_pixels = (size_t)SOURCE_WIDTH * SOURCE_HEIGHT;
	const size_t output_pixels =
		(size_t)mailbox->output_width * mailbox->output_height;
	const size_t output_bytes = output_pixels * CHANNEL_COUNT;
	uint8_t *source_rgb = NULL;
	uint8_t *resized_rgb = NULL;
	float *tensor = NULL;
	struct axis_map *x_map = NULL;
	struct axis_map *y_map = NULL;
	uint64_t processed_generation = 0;
	bool have_output = false;

	source_rgb = malloc(SOURCE_FRAME_BYTES);
	resized_rgb = malloc(output_bytes);
	tensor = malloc(output_pixels * CHANNEL_COUNT * sizeof(*tensor));
	x_map = malloc((size_t)mailbox->output_width * sizeof(*x_map));
	y_map = malloc((size_t)mailbox->output_height * sizeof(*y_map));
	if (!source_rgb || !resized_rgb || !tensor || !x_map || !y_map) {
		fprintf(stderr, "worker: cannot allocate preprocessing buffers\n");
		mark_worker_failed(mailbox);
		goto done;
	}

	build_axis_map(x_map, mailbox->output_width, SOURCE_WIDTH);
	build_axis_map(y_map, mailbox->output_height, SOURCE_HEIGHT);

	for (;;) {
		uint64_t generation;
		unsigned int slot;
		uint32_t source_sequence;
		uint32_t source_bytesused;
		struct timespec start;
		struct timespec after_color;
		struct timespec after_resize;
		struct timespec after_normalize;
		float minimum[3];
		float maximum[3];
		float mean[3];
		uint32_t digest;
		unsigned int processed_number;
		double color_seconds;
		double resize_seconds;
		double normalize_seconds;
		double total_seconds;

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
		source_sequence = mailbox->slot_sequence[slot];
		source_bytesused = mailbox->slot_bytesused[slot];
		mailbox->processing_slot = (int)slot;
		pthread_mutex_unlock(&mailbox->mutex);

		clock_gettime(CLOCK_MONOTONIC, &start);
		bgr24_to_rgb24(mailbox->slots[slot], source_rgb, source_pixels);
		clock_gettime(CLOCK_MONOTONIC, &after_color);

		resize_rgb24_bilinear(source_rgb, SOURCE_WIDTH, resized_rgb,
					mailbox->output_width,
					mailbox->output_height, x_map, y_map);
		clock_gettime(CLOCK_MONOTONIC, &after_resize);

		rgb24_to_chw_float(resized_rgb, tensor, output_pixels,
				   minimum, maximum, mean);
		clock_gettime(CLOCK_MONOTONIC, &after_normalize);

		/* Digest verifies that complete resized images continue to change. */
		digest = fnv1a32(resized_rgb, output_bytes);
		color_seconds = elapsed_seconds(&start, &after_color);
		resize_seconds = elapsed_seconds(&after_color, &after_resize);
		normalize_seconds = elapsed_seconds(&after_resize,
					       &after_normalize);
		total_seconds = elapsed_seconds(&start, &after_normalize);

		pthread_mutex_lock(&mailbox->mutex);
		if (mailbox->stats.have_digest &&
		    digest != mailbox->stats.last_digest)
			mailbox->stats.digest_changes++;
		mailbox->stats.have_digest = true;
		mailbox->stats.last_digest = digest;
		mailbox->stats.last_source_sequence = source_sequence;
		mailbox->stats.processed_frames++;
		mailbox->stats.bgr_to_rgb_seconds += color_seconds;
		mailbox->stats.resize_seconds += resize_seconds;
		mailbox->stats.normalize_seconds += normalize_seconds;
		mailbox->stats.total_seconds += total_seconds;
		if (total_seconds * 1000.0 > mailbox->stats.maximum_total_ms)
			mailbox->stats.maximum_total_ms = total_seconds * 1000.0;
		memcpy(mailbox->stats.last_minimum, minimum, sizeof(minimum));
		memcpy(mailbox->stats.last_maximum, maximum, sizeof(maximum));
		memcpy(mailbox->stats.last_mean, mean, sizeof(mean));
		processed_number = mailbox->stats.processed_frames;
		mailbox->processing_slot = -1;
		processed_generation = generation;
		pthread_mutex_unlock(&mailbox->mutex);
		have_output = true;

		if (processed_number == 1 ||
		    processed_number % REPORT_INTERVAL == 0U) {
			printf("processed=%u source_sequence=%u bytes=%u "
			       "digest=0x%08x total=%.3fms "
			       "(color=%.3f resize=%.3f normalize=%.3f)\n",
			       processed_number, source_sequence, source_bytesused,
			       digest, total_seconds * 1000.0,
			       color_seconds * 1000.0, resize_seconds * 1000.0,
			       normalize_seconds * 1000.0);
			fflush(stdout);
		}
	}

	if (have_output && strcmp(mailbox->output_path, "-") != 0) {
		if (write_ppm(mailbox->output_path, resized_rgb,
			      mailbox->output_width, mailbox->output_height) < 0) {
			fprintf(stderr, "cannot write %s: %s\n",
				mailbox->output_path, strerror(errno));
			mark_worker_failed(mailbox);
		} else {
			printf("saved_last_rgb=%s (%ux%u PPM)\n",
			       mailbox->output_path, mailbox->output_width,
			       mailbox->output_height);
		}
	}

done:
	free(y_map);
	free(x_map);
	free(tensor);
	free(resized_rgb);
	free(source_rgb);
	return NULL;
}

static int parse_unsigned(const char *text, unsigned int maximum,
			  unsigned int *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !text[0] || (end && *end) || parsed == 0 ||
	    parsed > maximum)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

static void print_usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [device] [capture-frames] [output-width] "
		"[output-height] [last-output.ppm|-]\n"
		"Example: %s /dev/video0 300 224 224 "
		"/home/root/preprocess-last.ppm\n",
		program, program);
}

int main(int argc, char **argv)
{
	const char *device_name = DEFAULT_DEVICE;
	unsigned int requested_frames = DEFAULT_CAPTURE_FRAMES;
	struct latest_frame_mailbox mailbox;
	struct worker_argument worker_argument;
	struct mapped_buffer *buffers = NULL;
	unsigned int mapped_count = 0;
	pthread_t worker_thread;
	bool mutex_initialized = false;
	bool condition_initialized = false;
	bool worker_started = false;
	bool streaming = false;
	struct v4l2_capability capability;
	struct v4l2_format format;
	struct v4l2_requestbuffers request;
	struct timespec capture_start;
	struct timespec capture_finish;
	unsigned int captured_frames = 0;
	unsigned int sequence_gaps = 0;
	unsigned int bad_payloads = 0;
	uint32_t previous_sequence = 0;
	bool have_previous_sequence = false;
	uint32_t effective_caps;
	unsigned int index;
	int fd = -1;
	int result;
	int exit_status = EXIT_FAILURE;

	memset(&mailbox, 0, sizeof(mailbox));
	mailbox.processing_slot = -1;
	mailbox.output_width = DEFAULT_OUTPUT_WIDTH;
	mailbox.output_height = DEFAULT_OUTPUT_HEIGHT;
	mailbox.output_path = DEFAULT_OUTPUT_PATH;

	if (argc > 6) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2)
		device_name = argv[1];
	if (argc >= 3 &&
	    parse_unsigned(argv[2], UINT32_MAX, &requested_frames) < 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 4 &&
	    parse_unsigned(argv[3], MAX_OUTPUT_DIMENSION,
			   &mailbox.output_width) < 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 5 &&
	    parse_unsigned(argv[4], MAX_OUTPUT_DIMENSION,
			   &mailbox.output_height) < 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 6)
		mailbox.output_path = argv[5];

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

	for (index = 0; index < MAILBOX_SLOT_COUNT; ++index) {
		mailbox.slots[index] = malloc(SOURCE_FRAME_BYTES);
		if (!mailbox.slots[index]) {
			perror("malloc mailbox slot");
			goto cleanup;
		}
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
	effective_caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ?
		capability.device_caps : capability.capabilities;
	if (!(effective_caps & V4L2_CAP_VIDEO_CAPTURE) ||
	    !(effective_caps & V4L2_CAP_STREAMING)) {
		fprintf(stderr,
			"Device does not provide VIDEO_CAPTURE + STREAMING\n");
		goto cleanup;
	}

	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = SOURCE_WIDTH;
	format.fmt.pix.height = SOURCE_HEIGHT;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
		perror("VIDIOC_S_FMT");
		goto cleanup;
	}
	if (format.fmt.pix.width != SOURCE_WIDTH ||
	    format.fmt.pix.height != SOURCE_HEIGHT ||
	    format.fmt.pix.pixelformat != V4L2_PIX_FMT_BGR24 ||
	    format.fmt.pix.sizeimage != SOURCE_FRAME_BYTES) {
		fprintf(stderr,
			"Unexpected format: %ux%u fourcc=0x%08x size=%u\n",
			format.fmt.pix.width, format.fmt.pix.height,
			format.fmt.pix.pixelformat, format.fmt.pix.sizeimage);
		goto cleanup;
	}

	memset(&request, 0, sizeof(request));
	request.count = REQUEST_BUFFER_COUNT;
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

	buffers = calloc(request.count, sizeof(*buffers));
	if (!buffers) {
		perror("calloc mapped buffers");
		goto cleanup;
	}

	for (index = 0; index < request.count; ++index) {
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

	for (index = 0; index < request.count; ++index) {
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

	worker_argument.mailbox = &mailbox;
	result = pthread_create(&worker_thread, NULL, preprocessing_worker,
				&worker_argument);
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

	printf("pipeline: %ux%u BGR24 -> RGB24 -> %ux%u bilinear -> "
	       "CHW float32 [0,1]\n",
	       SOURCE_WIDTH, SOURCE_HEIGHT, mailbox.output_width,
	       mailbox.output_height);
	printf("capture_target=%u mailbox_slots=%u output_ppm=%s\n",
	       requested_frames, MAILBOX_SLOT_COUNT, mailbox.output_path);

	while (captured_frames < requested_frames && !stop_requested) {
		struct pollfd poll_descriptor;
		struct v4l2_buffer buffer;
		size_t copy_length;
		unsigned int mailbox_slot;
		uint32_t frame_sequence;
		int poll_result;

		poll_descriptor.fd = fd;
		poll_descriptor.events = POLLIN | POLLPRI;
		poll_descriptor.revents = 0;
		poll_result = poll(&poll_descriptor, 1, POLL_TIMEOUT_MS);
		if (poll_result < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			goto finish_capture;
		}
		if (poll_result == 0) {
			fprintf(stderr, "Timeout waiting for frame %u\n",
				captured_frames);
			goto finish_capture;
		}
		if (poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "poll revents=0x%x\n",
				poll_descriptor.revents);
			goto finish_capture;
		}

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN)
				continue;
			perror("VIDIOC_DQBUF");
			goto finish_capture;
		}
		if (buffer.index >= request.count) {
			fprintf(stderr, "Invalid buffer index %u\n", buffer.index);
			goto finish_capture;
		}
		/*
		 * Some drivers rewrite v4l2_buffer fields during QBUF.  Preserve
		 * the completed-frame sequence before returning the buffer so the
		 * later progress message always reports the real captured sequence.
		 */
		frame_sequence = buffer.sequence;

		if (buffer.bytesused != SOURCE_FRAME_BYTES) {
			bad_payloads++;
			fprintf(stderr, "Frame %u bytesused=%u expected=%u\n",
				captured_frames, buffer.bytesused,
				SOURCE_FRAME_BYTES);
		}
		if (have_previous_sequence &&
		    frame_sequence != (uint32_t)(previous_sequence + 1U)) {
			sequence_gaps++;
			fprintf(stderr,
				"Sequence gap: previous=%u current=%u\n",
				previous_sequence, frame_sequence);
		}
		previous_sequence = frame_sequence;
		have_previous_sequence = true;

		copy_length = buffer.bytesused;
		if (copy_length > SOURCE_FRAME_BYTES)
			copy_length = SOURCE_FRAME_BYTES;
		if (copy_length > buffers[buffer.index].length)
			copy_length = buffers[buffer.index].length;

		pthread_mutex_lock(&mailbox.mutex);
		if (mailbox.processing_slot == 0)
			mailbox_slot = 1;
		else if (mailbox.processing_slot == 1)
			mailbox_slot = 0;
		else
			mailbox_slot = mailbox.latest_slot ^ 1U;

		memcpy(mailbox.slots[mailbox_slot],
		       buffers[buffer.index].address, copy_length);
		if (copy_length < SOURCE_FRAME_BYTES)
			memset(mailbox.slots[mailbox_slot] + copy_length, 0,
			       SOURCE_FRAME_BYTES - copy_length);
		mailbox.slot_sequence[mailbox_slot] = frame_sequence;
		mailbox.slot_bytesused[mailbox_slot] = buffer.bytesused;
		mailbox.latest_slot = mailbox_slot;
		mailbox.latest_generation = (uint64_t)captured_frames + 1U;
		pthread_cond_signal(&mailbox.frame_available);
		pthread_mutex_unlock(&mailbox.mutex);

		/* VDMA may reuse this buffer as soon as QBUF returns. */
		if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("requeue VIDIOC_QBUF");
			goto finish_capture;
		}

		captured_frames++;
		if (captured_frames == 1 ||
		    captured_frames % (REPORT_INTERVAL * 2U) == 0U) {
			printf("captured=%u sequence=%u v4l2_index=%u "
			       "published_slot=%u\n",
			       captured_frames, frame_sequence, buffer.index,
			       mailbox_slot);
			fflush(stdout);
		}
	}

finish_capture:
	if (streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
			perror("VIDIOC_STREAMOFF");
		streaming = false;
	}
	clock_gettime(CLOCK_MONOTONIC, &capture_finish);

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

	if (captured_frames > 0) {
		const struct preprocessing_stats *stats = &mailbox.stats;
		double capture_seconds = elapsed_seconds(&capture_start,
							 &capture_finish);
		double capture_fps = capture_seconds > 0.0 ?
			(double)captured_frames / capture_seconds : 0.0;
		unsigned int dropped_old = captured_frames >=
			stats->processed_frames ?
			captured_frames - stats->processed_frames : 0;
		double count = stats->processed_frames ?
			(double)stats->processed_frames : 1.0;

		printf("capture_summary: frames=%u/%u seconds=%.3f fps=%.2f "
		       "sequence_gaps=%u bad_payloads=%u\n",
		       captured_frames, requested_frames, capture_seconds,
		       capture_fps, sequence_gaps, bad_payloads);
		printf("preprocess_summary: processed=%u dropped_old=%u "
		       "digest_changes=%u last_sequence=%u\n",
		       stats->processed_frames, dropped_old,
		       stats->digest_changes, stats->last_source_sequence);
		printf("average_ms: color=%.3f resize=%.3f normalize=%.3f "
		       "total=%.3f maximum_total=%.3f pure_preprocess_fps=%.2f\n",
		       stats->bgr_to_rgb_seconds * 1000.0 / count,
		       stats->resize_seconds * 1000.0 / count,
		       stats->normalize_seconds * 1000.0 / count,
		       stats->total_seconds * 1000.0 / count,
		       stats->maximum_total_ms,
		       stats->total_seconds > 0.0 ?
			count / stats->total_seconds : 0.0);
		printf("last_tensor: R[min=%.3f max=%.3f mean=%.3f] "
		       "G[min=%.3f max=%.3f mean=%.3f] "
		       "B[min=%.3f max=%.3f mean=%.3f]\n",
		       stats->last_minimum[0], stats->last_maximum[0],
		       stats->last_mean[0], stats->last_minimum[1],
		       stats->last_maximum[1], stats->last_mean[1],
		       stats->last_minimum[2], stats->last_maximum[2],
		       stats->last_mean[2]);

		if (!stop_requested && captured_frames == requested_frames &&
		    sequence_gaps == 0 && bad_payloads == 0 &&
		    stats->processed_frames > 0 && !stats->failed)
			exit_status = EXIT_SUCCESS;
	}

cleanup:
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

	for (index = 0; index < mapped_count; ++index) {
		if (buffers[index].address)
			munmap(buffers[index].address, buffers[index].length);
	}
	free(buffers);
	if (fd >= 0)
		close(fd);
	for (index = 0; index < MAILBOX_SLOT_COUNT; ++index)
		free(mailbox.slots[index]);
	if (condition_initialized)
		pthread_cond_destroy(&mailbox.frame_available);
	if (mutex_initialized)
		pthread_mutex_destroy(&mailbox.mutex);

	return exit_status;
}
