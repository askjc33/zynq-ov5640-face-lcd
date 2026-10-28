// SPDX-License-Identifier: MIT
/*
 * studyboard-v4l2-latest.c
 *
 * 这是从“能抓帧”走向“实时视觉应用”的第二个教学程序。
 *
 * 为什么需要两个线程
 * ------------------
 * studyboard-v4l2-test 在 DQBUF 后直接扫描约 900 KiB 图像，再 QBUF。
 * 如果算法比摄像头慢，四块 DMA 缓冲最终都会被用户占住，采集就只能
 * 等待。真实视觉系统通常更关心低延迟的最新画面，而不是处理每一帧：
 *
 *   主线程（生产者）                     处理线程（消费者）
 *   poll -> DQBUF                         等待新 generation
 *   memcpy 到双缓冲邮箱   ----------->   处理最新完整 CPU 帧
 *   立即 QBUF                            FNV/未来的 OpenCV 或模型
 *
 * V4L2 MMAP缓冲必须在 memcpy 完成后立刻 QBUF，因为它的所有权要还给
 * 驱动。处理线程不能在 QBUF 后继续读取原 MMAP地址；VDMA随时可能覆盖它。
 * 所以这里付出一次约 900 KiB memcpy，换取严格的缓冲所有权和稳定采集。
 *
 * 双缓冲邮箱只有两个 CPU缓冲：一个可能正被处理线程读取，另一个允许
 * 采集线程反复覆盖为“最新帧”。处理赶不上时，中间旧帧会被主动丢弃，
 * 但处理线程永远不会读到被并发改写的半帧。
 */

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

#define DEFAULT_DEVICE              "/dev/video0"
#define DEFAULT_CAPTURE_FRAMES      1000U
#define DEFAULT_PROCESSING_DELAY_MS 0U
#define REQUEST_BUFFER_COUNT        4U
#define EXPECTED_WIDTH              640U
#define EXPECTED_HEIGHT             480U
#define EXPECTED_BPP                3U
#define EXPECTED_FRAME_BYTES        (EXPECTED_WIDTH * EXPECTED_HEIGHT * EXPECTED_BPP)
#define POLL_TIMEOUT_MS             3000
#define MAILBOX_SLOT_COUNT          2U

struct mapped_buffer {
	void *address;
	size_t length;
};

/*
 * latest_frame_mailbox 是两个线程共享的“只保留最新帧”邮箱。
 * 凡是可能同时被两个线程访问的字段，都只能在 mutex 保护下读写。
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
	unsigned int processing_delay_ms;

	/* 以下统计量由处理线程写，主线程在 pthread_join 后读取。 */
	unsigned int processed_frames;
	unsigned int digest_changes;
	uint32_t last_digest;
};

struct processing_thread_argument {
	struct latest_frame_mailbox *mailbox;
	size_t frame_bytes;
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

static void sleep_milliseconds(unsigned int milliseconds)
{
	struct timespec request;
	struct timespec remaining;

	request.tv_sec = milliseconds / 1000U;
	request.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	while (!stop_requested && nanosleep(&request, &remaining) < 0 &&
	       errno == EINTR)
		request = remaining;
}

/*
 * 处理线程只读取 mailbox.processing_slot 指向的CPU缓冲。
 * 采集线程看到 processing_slot 后只会覆盖另一个slot，因此FNV扫描期间
 * 不需要一直持有mutex，也不会阻塞下一帧的采集和QBUF。
 */
static void *processing_thread_main(void *opaque)
{
	struct processing_thread_argument *argument = opaque;
	struct latest_frame_mailbox *mailbox = argument->mailbox;
	uint64_t processed_generation = 0;
	bool have_digest = false;

	for (;;) {
		uint64_t generation;
		unsigned int slot;
		uint32_t source_sequence;
		uint32_t source_bytesused;
		uint32_t digest;
		unsigned int processed_number;

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

		/*
		 * 在锁内取得当前最新版，并声明processing_slot所有权。
		 * 解锁后采集线程只能写另一个slot。
		 */
		generation = mailbox->latest_generation;
		slot = mailbox->latest_slot;
		source_sequence = mailbox->slot_sequence[slot];
		source_bytesused = mailbox->slot_bytesused[slot];
		mailbox->processing_slot = (int)slot;
		pthread_mutex_unlock(&mailbox->mutex);

		digest = fnv1a32(mailbox->slots[slot], argument->frame_bytes);
		if (mailbox->processing_delay_ms)
			sleep_milliseconds(mailbox->processing_delay_ms);

		pthread_mutex_lock(&mailbox->mutex);
		if (have_digest && digest != mailbox->last_digest)
			mailbox->digest_changes++;
		mailbox->last_digest = digest;
		have_digest = true;
		mailbox->processed_frames++;
		processed_number = mailbox->processed_frames;
		mailbox->processing_slot = -1;
		processed_generation = generation;
		pthread_mutex_unlock(&mailbox->mutex);

		if (processed_number == 1 || processed_number % 100U == 0U) {
			printf("processed=%u generation=%llu source_sequence=%u "
			       "bytes=%u digest=0x%08x\n",
			       processed_number,
			       (unsigned long long)generation,
			       source_sequence, source_bytesused, digest);
			fflush(stdout);
		}
	}

	return NULL;
}

static void print_usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [device] [capture-frames] [processing-delay-ms]\n"
		"Example: %s /dev/video0 1000 0\n"
		"Example with simulated 80 ms inference: %s /dev/video0 1000 80\n",
		program, program, program);
}

static int parse_unsigned(const char *text, unsigned int maximum,
			  unsigned int *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !text[0] || (end && *end) || parsed > maximum)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

int main(int argc, char **argv)
{
	const char *device_name = DEFAULT_DEVICE;
	unsigned int requested_frames = DEFAULT_CAPTURE_FRAMES;
	unsigned int processing_delay_ms = DEFAULT_PROCESSING_DELAY_MS;
	struct mapped_buffer *buffers = NULL;
	unsigned int mapped_count = 0;
	struct latest_frame_mailbox mailbox;
	struct processing_thread_argument thread_argument;
	pthread_t processing_thread;
	bool mutex_initialized = false;
	bool condition_initialized = false;
	bool processing_thread_started = false;
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

	if (argc > 4) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2)
		device_name = argv[1];
	if (argc >= 3 &&
	    (parse_unsigned(argv[2], UINT32_MAX, &requested_frames) < 0 ||
	     requested_frames == 0)) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 4 &&
	    parse_unsigned(argv[3], 60000U, &processing_delay_ms) < 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	memset(&mailbox, 0, sizeof(mailbox));
	mailbox.processing_slot = -1;
	mailbox.processing_delay_ms = processing_delay_ms;

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
		mailbox.slots[index] = malloc(EXPECTED_FRAME_BYTES);
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
	format.fmt.pix.width = EXPECTED_WIDTH;
	format.fmt.pix.height = EXPECTED_HEIGHT;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_BGR24;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
		perror("VIDIOC_S_FMT");
		goto cleanup;
	}
	if (format.fmt.pix.width != EXPECTED_WIDTH ||
	    format.fmt.pix.height != EXPECTED_HEIGHT ||
	    format.fmt.pix.pixelformat != V4L2_PIX_FMT_BGR24 ||
	    format.fmt.pix.sizeimage != EXPECTED_FRAME_BYTES) {
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

	thread_argument.mailbox = &mailbox;
	thread_argument.frame_bytes = EXPECTED_FRAME_BYTES;
	result = pthread_create(&processing_thread, NULL,
				processing_thread_main, &thread_argument);
	if (result) {
		fprintf(stderr, "pthread_create: %s\n", strerror(result));
		goto cleanup;
	}
	processing_thread_started = true;

	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
			perror("VIDIOC_STREAMON");
			goto finish_capture;
		}
	}
	streaming = true;
	clock_gettime(CLOCK_MONOTONIC, &capture_start);

	printf("capture target=%u, processing_delay_ms=%u, "
	       "mailbox_slots=%u\n",
	       requested_frames, processing_delay_ms, MAILBOX_SLOT_COUNT);

	while (captured_frames < requested_frames && !stop_requested) {
		struct pollfd poll_descriptor;
		struct v4l2_buffer buffer;
		size_t copy_length;
		unsigned int mailbox_slot;
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

		if (buffer.bytesused != EXPECTED_FRAME_BYTES) {
			bad_payloads++;
			fprintf(stderr,
				"Frame %u bytesused=%u, expected=%u\n",
				captured_frames, buffer.bytesused,
				EXPECTED_FRAME_BYTES);
		}
		if (have_previous_sequence &&
		    buffer.sequence != (uint32_t)(previous_sequence + 1U)) {
			sequence_gaps++;
			fprintf(stderr,
				"Sequence gap: previous=%u current=%u\n",
				previous_sequence, buffer.sequence);
		}
		previous_sequence = buffer.sequence;
		have_previous_sequence = true;

		copy_length = buffer.bytesused;
		if (copy_length > EXPECTED_FRAME_BYTES)
			copy_length = EXPECTED_FRAME_BYTES;
		if (copy_length > buffers[buffer.index].length)
			copy_length = buffers[buffer.index].length;

		/*
		 * 只在选择slot和复制完整帧期间持锁。处理线程不会读取我们选中的
		 * slot；复制完成并更新generation后，它才可能取得该slot。
		 */
		pthread_mutex_lock(&mailbox.mutex);
		if (mailbox.processing_slot == 0)
			mailbox_slot = 1;
		else if (mailbox.processing_slot == 1)
			mailbox_slot = 0;
		else
			mailbox_slot = mailbox.latest_slot ^ 1U;

		memcpy(mailbox.slots[mailbox_slot],
		       buffers[buffer.index].address, copy_length);
		if (copy_length < EXPECTED_FRAME_BYTES)
			memset(mailbox.slots[mailbox_slot] + copy_length, 0,
			       EXPECTED_FRAME_BYTES - copy_length);
		mailbox.slot_sequence[mailbox_slot] = buffer.sequence;
		mailbox.slot_bytesused[mailbox_slot] = buffer.bytesused;
		mailbox.latest_slot = mailbox_slot;
		mailbox.latest_generation = (uint64_t)captured_frames + 1U;
		pthread_cond_signal(&mailbox.frame_available);
		pthread_mutex_unlock(&mailbox.mutex);

		/* 复制完成后立刻归还DMA缓冲，不等待FNV或未来的模型推理。 */
		if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("requeue VIDIOC_QBUF");
			goto finish_capture;
		}

		captured_frames++;
		if (captured_frames == 1 || captured_frames % 100U == 0U) {
			printf("captured=%u sequence=%u v4l2_index=%u "
			       "published_slot=%u\n",
			       captured_frames, buffer.sequence, buffer.index,
			       mailbox_slot);
			fflush(stdout);
		}
	}

	if (!stop_requested && captured_frames == requested_frames &&
	    sequence_gaps == 0 && bad_payloads == 0)
		exit_status = EXIT_SUCCESS;

finish_capture:
	if (streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
			perror("VIDIOC_STREAMOFF");
			exit_status = EXIT_FAILURE;
		}
		streaming = false;
	}
	clock_gettime(CLOCK_MONOTONIC, &capture_finish);

	if (condition_initialized) {
		pthread_mutex_lock(&mailbox.mutex);
		mailbox.capture_done = true;
		pthread_cond_broadcast(&mailbox.frame_available);
		pthread_mutex_unlock(&mailbox.mutex);
	}
	if (processing_thread_started) {
		pthread_join(processing_thread, NULL);
		processing_thread_started = false;
	}

	if (captured_frames > 0) {
		double seconds = elapsed_seconds(&capture_start, &capture_finish);
		double capture_fps = seconds > 0.0 ?
			(double)captured_frames / seconds : 0.0;
		unsigned int dropped_for_processing = captured_frames >=
			mailbox.processed_frames ?
			captured_frames - mailbox.processed_frames : 0;

		printf("capture_summary: frames=%u/%u seconds=%.3f fps=%.2f "
		       "sequence_gaps=%u bad_payloads=%u\n",
		       captured_frames, requested_frames, seconds, capture_fps,
		       sequence_gaps, bad_payloads);
		printf("processing_summary: processed=%u dropped_old=%u "
		       "digest_changes=%u last_digest=0x%08x\n",
		       mailbox.processed_frames, dropped_for_processing,
		       mailbox.digest_changes, mailbox.last_digest);
	}

cleanup:
	/* 处理前面任意失败路径，保证线程、STREAMON和映射均被释放。 */
	if (streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		xioctl(fd, VIDIOC_STREAMOFF, &type);
	}
	if (processing_thread_started) {
		pthread_mutex_lock(&mailbox.mutex);
		mailbox.capture_done = true;
		pthread_cond_broadcast(&mailbox.frame_available);
		pthread_mutex_unlock(&mailbox.mutex);
		pthread_join(processing_thread, NULL);
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
