// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L

#include "video_stream_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace {

const int kSendDeadlineMs = 500;

void put_u16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = static_cast<uint8_t>(value >> 8);
	bytes[1] = static_cast<uint8_t>(value);
}

void put_u32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = static_cast<uint8_t>(value >> 24);
	bytes[1] = static_cast<uint8_t>(value >> 16);
	bytes[2] = static_cast<uint8_t>(value >> 8);
	bytes[3] = static_cast<uint8_t>(value);
}

bool before_deadline(const struct timespec &deadline, int *remaining_ms)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	const int64_t nanoseconds =
		static_cast<int64_t>(deadline.tv_sec - now.tv_sec) * 1000000000LL +
		static_cast<int64_t>(deadline.tv_nsec - now.tv_nsec);
	if (nanoseconds <= 0)
		return false;
	*remaining_ms = static_cast<int>((nanoseconds + 999999LL) / 1000000LL);
	return true;
}

} // namespace

VideoStreamServer::VideoStreamServer()
	: mutex_ready_(false), condition_ready_(false), started_(false),
	  running_(false), connected_(false), listen_fd_(-1), pending_slot_(-1),
	  sending_slot_(-1), width_(0), height_(0), port_(0), stride_(0),
	  frame_bytes_(0), offered_(0), sent_(0), overwritten_(0),
	  connections_(0), disconnects_(0)
{
	slots_[0] = NULL;
	slots_[1] = NULL;
	sequences_[0] = 0;
	sequences_[1] = 0;
}

VideoStreamServer::~VideoStreamServer()
{
	stop();
}

bool VideoStreamServer::start(uint16_t port, unsigned int stride,
			      uint16_t width, uint16_t height, std::string *error)
{
	if (started_ || !port || !stride || !width || !height) {
		*error = "invalid video stream configuration";
		return false;
	}
	width_ = width;
	height_ = height;
	port_ = port;
	stride_ = stride;
	frame_bytes_ = static_cast<size_t>(width) * height * 3U;

	int result = pthread_mutex_init(&mutex_, NULL);
	if (result) {
		*error = std::string("video stream mutex: ") + strerror(result);
		return false;
	}
	mutex_ready_ = true;
	result = pthread_cond_init(&available_, NULL);
	if (result) {
		*error = std::string("video stream condition: ") + strerror(result);
		stop();
		return false;
	}
	condition_ready_ = true;
	for (unsigned int i = 0; i < 2; ++i) {
		slots_[i] = static_cast<uint8_t *>(malloc(frame_bytes_));
		if (!slots_[i]) {
			*error = "cannot allocate video stream frame slots";
			stop();
			return false;
		}
	}

	listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd_ < 0) {
		*error = std::string("video stream socket: ") + strerror(errno);
		stop();
		return false;
	}
	const int flags = fcntl(listen_fd_, F_GETFL, 0);
	if (flags < 0 || fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
		*error = std::string("video stream nonblocking socket: ") +
			strerror(errno);
		stop();
		return false;
	}
	int one = 1;
	setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&address),
		 sizeof(address)) < 0 || listen(listen_fd_, 1) < 0) {
		*error = std::string("video stream bind/listen: ") + strerror(errno);
		stop();
		return false;
	}

	pthread_mutex_lock(&mutex_);
	running_ = true;
	pthread_mutex_unlock(&mutex_);
	result = pthread_create(&thread_, NULL, thread_entry, this);
	if (result) {
		*error = std::string("video stream thread: ") + strerror(result);
		stop();
		return false;
	}
	started_ = true;
	return true;
}

void VideoStreamServer::offer(const uint8_t *bgr, size_t length,
			      uint32_t sequence, unsigned int captured_frames)
{
	if (!started_ || length != frame_bytes_ ||
	    (captured_frames - 1U) % stride_ != 0U)
		return;
	pthread_mutex_lock(&mutex_);
	if (running_ && connected_) {
		const int slot = sending_slot_ == 0 ? 1 :
			(sending_slot_ == 1 ? 0 : (pending_slot_ == 0 ? 1 : 0));
		if (pending_slot_ >= 0)
			overwritten_++;
		memcpy(slots_[slot], bgr, frame_bytes_);
		sequences_[slot] = sequence;
		pending_slot_ = slot;
		offered_++;
		pthread_cond_signal(&available_);
	}
	pthread_mutex_unlock(&mutex_);
}

bool VideoStreamServer::send_all(int client, const uint8_t *data,
				 size_t length, const struct timespec &deadline)
{
	while (length) {
		int remaining_ms;
		if (!before_deadline(deadline, &remaining_ms))
			return false;
		const ssize_t sent = send(client, data, length,
					  MSG_DONTWAIT | MSG_NOSIGNAL);
		if (sent > 0) {
			data += sent;
			length -= static_cast<size_t>(sent);
			continue;
		}
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd descriptor;
			descriptor.fd = client;
			descriptor.events = POLLOUT;
			descriptor.revents = 0;
			const int result = poll(&descriptor, 1, remaining_ms);
			if (result > 0 && !(descriptor.revents &
					     (POLLERR | POLLHUP | POLLNVAL)))
				continue;
			if (result < 0 && errno == EINTR)
				continue;
		}
		return false;
	}
	return true;
}

bool VideoStreamServer::send_frame(int client, const uint8_t *frame,
				    uint32_t sequence)
{
	// 20-byte header: magic, width, height, fourcc, sequence, byte count.
	uint8_t header[20];
	memcpy(header, "SBV1", 4);
	put_u16(header + 4, width_);
	put_u16(header + 6, height_);
	memcpy(header + 8, "BGR3", 4);
	put_u32(header + 12, sequence);
	put_u32(header + 16, static_cast<uint32_t>(frame_bytes_));
	struct timespec deadline;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_nsec += static_cast<long>(kSendDeadlineMs) * 1000000L;
	deadline.tv_sec += deadline.tv_nsec / 1000000000L;
	deadline.tv_nsec %= 1000000000L;
	return send_all(client, header, sizeof(header), deadline) &&
		send_all(client, frame, frame_bytes_, deadline);
}

void *VideoStreamServer::thread_entry(void *opaque)
{
	static_cast<VideoStreamServer *>(opaque)->run();
	return NULL;
}

void VideoStreamServer::run()
{
	int client = -1;
	for (;;) {
		pthread_mutex_lock(&mutex_);
		const bool running = running_;
		pthread_mutex_unlock(&mutex_);
		if (!running)
			break;

		if (client < 0) {
			struct pollfd descriptor;
			descriptor.fd = listen_fd_;
			descriptor.events = POLLIN;
			descriptor.revents = 0;
			const int result = poll(&descriptor, 1, 100);
			if (result <= 0 || !(descriptor.revents & POLLIN))
				continue;
			client = accept(listen_fd_, NULL, NULL);
			if (client < 0)
				continue;
			pthread_mutex_lock(&mutex_);
			connected_ = true;
			connections_++;
			pthread_mutex_unlock(&mutex_);
			printf("network: PC connected on port %u\n", port_);
			fflush(stdout);
		}

		pthread_mutex_lock(&mutex_);
		while (running_ && pending_slot_ < 0)
			pthread_cond_wait(&available_, &mutex_);
		if (!running_) {
			pthread_mutex_unlock(&mutex_);
			break;
		}
		const int slot = pending_slot_;
		const uint32_t sequence = sequences_[slot];
		pending_slot_ = -1;
		sending_slot_ = slot;
		pthread_mutex_unlock(&mutex_);

		const bool okay = send_frame(client, slots_[slot], sequence);
		pthread_mutex_lock(&mutex_);
		sending_slot_ = -1;
		if (okay) {
			sent_++;
		} else {
			connected_ = false;
			pending_slot_ = -1;
			disconnects_++;
		}
		pthread_mutex_unlock(&mutex_);
		if (!okay) {
			close(client);
			client = -1;
			fprintf(stderr, "network: PC disconnected or send timed out\n");
		}
	}
	if (client >= 0)
		close(client);
	pthread_mutex_lock(&mutex_);
	connected_ = false;
	pthread_mutex_unlock(&mutex_);
}

void VideoStreamServer::stop()
{
	if (mutex_ready_) {
		pthread_mutex_lock(&mutex_);
		running_ = false;
		if (condition_ready_)
			pthread_cond_broadcast(&available_);
		pthread_mutex_unlock(&mutex_);
	}
	if (started_) {
		pthread_join(thread_, NULL);
		started_ = false;
	}
	if (listen_fd_ >= 0) {
		close(listen_fd_);
		listen_fd_ = -1;
	}
	free(slots_[0]);
	free(slots_[1]);
	slots_[0] = NULL;
	slots_[1] = NULL;
	if (condition_ready_) {
		pthread_cond_destroy(&available_);
		condition_ready_ = false;
	}
	if (mutex_ready_) {
		pthread_mutex_destroy(&mutex_);
		mutex_ready_ = false;
	}
}

void VideoStreamServer::print_summary() const
{
	printf("network_summary: port=%u stride=%u sampled=%llu sent=%llu "
	       "overwritten=%llu connections=%llu disconnects=%llu\n",
	       port_, stride_, static_cast<unsigned long long>(offered_),
	       static_cast<unsigned long long>(sent_),
	       static_cast<unsigned long long>(overwritten_),
	       static_cast<unsigned long long>(connections_),
	       static_cast<unsigned long long>(disconnects_));
}
