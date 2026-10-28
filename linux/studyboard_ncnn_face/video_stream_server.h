// SPDX-License-Identifier: MIT
#ifndef STUDYBOARD_VIDEO_STREAM_SERVER_H
#define STUDYBOARD_VIDEO_STREAM_SERVER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <string>

// Optional, one-client, latest-frame TCP stream. The capture thread only
// copies a sampled frame; all socket operations happen on the worker thread.
class VideoStreamServer {
public:
	VideoStreamServer();
	~VideoStreamServer();

	bool start(uint16_t port, unsigned int stride, uint16_t width,
		   uint16_t height, std::string *error);
	void offer(const uint8_t *bgr, size_t length, uint32_t sequence,
		   unsigned int captured_frames);
	void stop();
	void print_summary() const;
	bool enabled() const { return started_; }

private:
	VideoStreamServer(const VideoStreamServer &);
	VideoStreamServer &operator=(const VideoStreamServer &);
	static void *thread_entry(void *opaque);
	void run();
	bool send_frame(int client, const uint8_t *frame, uint32_t sequence);
	bool send_all(int client, const uint8_t *data, size_t length,
		      const struct timespec &deadline);

	mutable pthread_mutex_t mutex_;
	pthread_cond_t available_;
	pthread_t thread_;
	bool mutex_ready_;
	bool condition_ready_;
	bool started_;
	bool running_;
	bool connected_;
	int listen_fd_;
	uint8_t *slots_[2];
	uint32_t sequences_[2];
	int pending_slot_;
	int sending_slot_;
	uint16_t width_;
	uint16_t height_;
	uint16_t port_;
	unsigned int stride_;
	size_t frame_bytes_;
	uint64_t offered_;
	uint64_t sent_;
	uint64_t overwritten_;
	uint64_t connections_;
	uint64_t disconnects_;
};

#endif
