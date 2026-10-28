// SPDX-License-Identifier: MIT
#include "lcd_output.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace {
bool fail(const std::string &operation, int code, std::string *error)
{
	if (error)
		*error = operation + ": " + strerror(code);
	return false;
}

bool command(const std::string &directory, char value, std::string *error)
{
	const std::string path = directory + "/lcd_commit";
	int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return fail(path, errno, error);
	const char text[] = {value, '\n'};
	ssize_t count;
	do {
		count = write(fd, text, sizeof(text));
	} while (count < 0 && errno == EINTR);
	const int code = count < 0 ? errno : EIO;
	const int close_result = close(fd);
	if (count != static_cast<ssize_t>(sizeof(text)))
		return fail("write " + path, code, error);
	if (close_result < 0)
		return fail("close " + path, errno, error);
	return true;
}
} // namespace

LcdOutput::LcdOutput() : frame_bytes_(0), owned_(false) {}

LcdOutput::~LcdOutput()
{
	std::string error;
	if (!stop(&error))
		fprintf(stderr, "LCD cleanup: %s\n", error.c_str());
}

bool LcdOutput::configure(const std::string &directory, size_t bytes,
			  std::string *error)
{
	if (owned_)
		return fail("LCD already configured", EBUSY, error);
	if (directory == "-") {
		directory_.clear();
		frame_bytes_ = 0;
		return true;
	}
	if (directory.empty() || !bytes)
		return fail("LCD directory/frame size", EINVAL, error);
	struct stat info;
	const std::string path = directory + "/lcd_frame";
	if (stat(path.c_str(), &info) < 0)
		return fail(path + " (load the LCD-enabled driver first)", errno, error);
	if (info.st_size != static_cast<off_t>(bytes))
		return fail(path + " frame size mismatch", EINVAL, error);
	if (access((directory + "/lcd_commit").c_str(), W_OK) < 0)
		return fail(directory + "/lcd_commit", errno, error);
	directory_ = directory;
	frame_bytes_ = bytes;
	return true;
}

bool LcdOutput::enabled() const { return !directory_.empty(); }

bool LcdOutput::publish(const uint8_t *bgr, size_t bytes, std::string *error)
{
	if (!enabled())
		return true;
	if (!bgr || bytes != frame_bytes_)
		return fail("LCD requires one complete BGR24 frame", EINVAL, error);
	const std::string path = directory_ + "/lcd_frame";
	int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return fail(path, errno, error);

	/*
	 * 每次重新 open，文件偏移从 0 开始。驱动将数据写入非显示缓冲；
	 * 任一分块失败就不 commit，所以 LCD 不会接收到不完整的用户图像。
	 */
	size_t offset = 0;
	while (offset < bytes) {
		const size_t chunk = std::min(bytes - offset, static_cast<size_t>(4096));
		ssize_t count = write(fd, bgr + offset, chunk);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			const int code = count < 0 ? errno : EIO;
			close(fd);
			return fail("write " + path, code, error);
		}
		owned_ = true;
		offset += static_cast<size_t>(count);
	}
	if (close(fd) < 0)
		return fail("close " + path, errno, error);
	return command(directory_, '1', error);
}

bool LcdOutput::stop(std::string *error)
{
	if (!owned_)
		return true;
	if (!command(directory_, '0', error))
		return false;
	owned_ = false;
	return true;
}
