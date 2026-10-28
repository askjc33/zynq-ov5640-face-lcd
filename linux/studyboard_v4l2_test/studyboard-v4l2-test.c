// SPDX-License-Identifier: MIT
/*
 * studyboard-v4l2-test.c
 *
 * 教学目标
 * --------
 * 用一个普通 C 程序完整展示 V4L2 MMAP 视频采集流程：
 *
 *   open
 *     -> VIDIOC_QUERYCAP                 查询设备能力
 *     -> VIDIOC_S_FMT                    固定为 640x480 BGR24
 *     -> VIDIOC_REQBUFS                  向 videobuf2 申请缓冲
 *     -> VIDIOC_QUERYBUF + mmap          映射到本进程地址空间
 *     -> VIDIOC_QBUF                     把空缓冲交给驱动
 *     -> VIDIOC_STREAMON                 启动 VDMA
 *     -> poll + VIDIOC_DQBUF             等待并取出完整帧
 *     -> 检查 + VIDIOC_QBUF              处理后归还缓冲
 *     -> VIDIOC_STREAMOFF                停止 VDMA
 *
 * 本程序不保存约 922 MB 的 1000 帧文件，而是检查每帧的 sequence、
 * bytesused，并计算 FNV-1a 摘要。这样能同时验证 V4L2 队列循环、
 * DMA 缓冲的 CPU 可读性和有效载荷长度。
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
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

#define DEFAULT_DEVICE       "/dev/video0"
#define DEFAULT_FRAME_COUNT  1000U
#define REQUEST_BUFFER_COUNT 4U
#define EXPECTED_WIDTH       640U
#define EXPECTED_HEIGHT      480U
#define EXPECTED_BPP         3U
#define EXPECTED_FRAME_BYTES (EXPECTED_WIDTH * EXPECTED_HEIGHT * EXPECTED_BPP)
#define POLL_TIMEOUT_MS      3000

/* 一块 V4L2 MMAP 缓冲在用户空间中的地址和长度。 */
struct mapped_buffer {
	void *address;
	size_t length;
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

/* ioctl 可能被信号打断；EINTR 时重试，其余结果原样返回。 */
static int xioctl(int fd, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && errno == EINTR);

	return result;
}

static const char *fourcc_to_string(uint32_t fourcc, char text[5])
{
	text[0] = fourcc & 0xff;
	text[1] = (fourcc >> 8) & 0xff;
	text[2] = (fourcc >> 16) & 0xff;
	text[3] = (fourcc >> 24) & 0xff;
	text[4] = '\0';
	return text;
}

/*
 * FNV-1a 不是加密哈希，但实现很小，适合板端快速证明 CPU 确实读取了
 * 每一个像素字节。摄像头噪声或画面运动通常会让相邻帧摘要变化。
 */
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

static void print_usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [device] [frame-count]\n"
		"Example: %s /dev/video0 1000\n",
		program, program);
}

int main(int argc, char **argv)
{
	const char *device_name = DEFAULT_DEVICE;
	unsigned int requested_frames = DEFAULT_FRAME_COUNT;
	struct mapped_buffer *buffers = NULL;
	unsigned int mapped_count = 0;
	unsigned int completed_frames = 0;
	unsigned int sequence_gaps = 0;
	unsigned int bad_payloads = 0;
	unsigned int digest_changes = 0;
	uint32_t previous_sequence = 0;
	uint32_t previous_digest = 0;
	bool have_previous = false;
	bool streaming = false;
	struct v4l2_capability capability;
	struct v4l2_format format;
	struct v4l2_requestbuffers request;
	struct timespec start_time;
	struct timespec finish_time;
	uint32_t effective_caps;
	unsigned int index;
	int fd = -1;
	int exit_status = EXIT_FAILURE;
	char fourcc_text[5];

	if (argc > 3) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 2)
		device_name = argv[1];
	if (argc == 3) {
		char *end = NULL;
		unsigned long value = strtoul(argv[2], &end, 10);

		if (!argv[2][0] || (end && *end) || value == 0 ||
		    value > UINT32_MAX) {
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
		requested_frames = (unsigned int)value;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/* O_NONBLOCK 让 DQBUF 不会永久睡眠；真正的等待交给 poll()。 */
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

	printf("driver=%s card=%s bus=%s\n",
	       capability.driver, capability.card, capability.bus_info);

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

	printf("format=%ux%u %s bytesperline=%u sizeimage=%u\n",
	       format.fmt.pix.width, format.fmt.pix.height,
	       fourcc_to_string(format.fmt.pix.pixelformat, fourcc_text),
	       format.fmt.pix.bytesperline, format.fmt.pix.sizeimage);

	if (format.fmt.pix.width != EXPECTED_WIDTH ||
	    format.fmt.pix.height != EXPECTED_HEIGHT ||
	    format.fmt.pix.pixelformat != V4L2_PIX_FMT_BGR24 ||
	    format.fmt.pix.sizeimage != EXPECTED_FRAME_BYTES) {
		fprintf(stderr, "Unexpected fixed capture format\n");
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
		perror("calloc buffers");
		goto cleanup;
	}

	/* QUERYBUF 返回每块缓冲在 video fd 中的 mmap offset。 */
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

	/* 初始时四块缓冲都是空的，全部 QBUF 给驱动作为 VDMA 目标。 */
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

	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
			perror("VIDIOC_STREAMON");
			goto cleanup;
		}
	}
	streaming = true;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	while (completed_frames < requested_frames && !stop_requested) {
		struct pollfd poll_descriptor;
		struct v4l2_buffer buffer;
		size_t payload_length;
		uint32_t digest;
		int poll_result;

		poll_descriptor.fd = fd;
		poll_descriptor.events = POLLIN | POLLPRI;
		poll_descriptor.revents = 0;
		poll_result = poll(&poll_descriptor, 1, POLL_TIMEOUT_MS);
		if (poll_result < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			goto cleanup;
		}
		if (poll_result == 0) {
			fprintf(stderr, "Timeout waiting for frame %u\n",
				completed_frames);
			goto cleanup;
		}
		if (poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "poll revents=0x%x\n",
				poll_descriptor.revents);
			goto cleanup;
		}

		memset(&buffer, 0, sizeof(buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno == EAGAIN)
				continue;
			perror("VIDIOC_DQBUF");
			goto cleanup;
		}

		if (buffer.index >= request.count) {
			fprintf(stderr, "Invalid buffer index %u\n", buffer.index);
			goto cleanup;
		}
		if (buffer.bytesused != EXPECTED_FRAME_BYTES) {
			fprintf(stderr,
				"Frame %u has bytesused=%u, expected=%u\n",
				completed_frames, buffer.bytesused,
				EXPECTED_FRAME_BYTES);
			bad_payloads++;
		}
		payload_length = buffer.bytesused;
		if (payload_length > buffers[buffer.index].length) {
			fprintf(stderr,
				"Frame %u payload exceeds mapped buffer: %zu > %zu\n",
				completed_frames, payload_length,
				buffers[buffer.index].length);
			payload_length = buffers[buffer.index].length;
			bad_payloads++;
		}

		if (have_previous &&
		    buffer.sequence != (uint32_t)(previous_sequence + 1U)) {
			fprintf(stderr,
				"Sequence gap: previous=%u current=%u\n",
				previous_sequence, buffer.sequence);
			sequence_gaps++;
		}

		digest = fnv1a32(buffers[buffer.index].address, payload_length);
		if (have_previous && digest != previous_digest)
			digest_changes++;

		previous_sequence = buffer.sequence;
		previous_digest = digest;
		have_previous = true;
		completed_frames++;

		if (completed_frames == 1 ||
		    completed_frames % 100 == 0 ||
		    completed_frames == requested_frames) {
			printf("frame=%u sequence=%u index=%u bytes=%u digest=0x%08x\n",
			       completed_frames, buffer.sequence, buffer.index,
			       buffer.bytesused, digest);
		}

		/* CPU 已处理完，所有权交还驱动，允许 VDMA 再次写入。 */
		if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("requeue VIDIOC_QBUF");
			goto cleanup;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &finish_time);
	if (completed_frames > 0) {
		double seconds = elapsed_seconds(&start_time, &finish_time);
		double frames_per_second = seconds > 0.0 ?
			(double)completed_frames / seconds : 0.0;

		printf("summary: frames=%u/%u seconds=%.3f fps=%.2f "
		       "sequence_gaps=%u bad_payloads=%u digest_changes=%u\n",
		       completed_frames, requested_frames, seconds,
		       frames_per_second, sequence_gaps, bad_payloads,
		       digest_changes);
	}

	if (!stop_requested && completed_frames == requested_frames &&
	    sequence_gaps == 0 && bad_payloads == 0)
		exit_status = EXIT_SUCCESS;

cleanup:
	if (streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0)
			perror("VIDIOC_STREAMOFF");
	}

	for (index = 0; index < mapped_count; ++index) {
		if (buffers[index].address &&
		    munmap(buffers[index].address, buffers[index].length) < 0)
			perror("munmap video buffer");
	}
	free(buffers);
	if (fd >= 0)
		close(fd);

	return exit_status;
}
