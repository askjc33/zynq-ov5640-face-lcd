// SPDX-License-Identifier: MIT
/* Regular files emulate the transport only, not sysfs validation or VDMA. */
#include "lcd_output.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

int main()
{
	char name[] = "/tmp/studyboard-lcd-test.XXXXXX";
	char *directory = mkdtemp(name);
	assert(directory);
	const std::string raw = std::string(directory) + "/lcd_frame";
	const std::string commit = std::string(directory) + "/lcd_commit";
	const size_t bytes = 921600;
	int fd = open(raw.c_str(), O_CREAT | O_RDWR, 0600);
	assert(fd >= 0 && ftruncate(fd, bytes) == 0);
	close(fd);
	fd = open(commit.c_str(), O_CREAT | O_RDWR, 0600);
	assert(fd >= 0);
	close(fd);

	std::string error;
	LcdOutput lcd;
	assert(lcd.configure("-", bytes, &error) && !lcd.enabled());
	assert(lcd.publish(NULL, 0, &error));
	assert(!lcd.configure(std::string(directory) + "/missing", bytes, &error));
	assert(!lcd.configure(directory, bytes - 1, &error));
	assert(lcd.configure(directory, bytes, &error) && lcd.enabled());
	std::vector<uint8_t> image(bytes), actual(bytes);
	assert(!lcd.publish(&image[0], bytes - 1, &error));
	assert(!lcd.publish(NULL, bytes, &error));
	for (int round = 0; round < 3; ++round) {
		for (size_t i = 0; i < bytes; ++i)
			image[i] = static_cast<uint8_t>(i + round * 17);
		assert(lcd.publish(&image[0], bytes, &error));
		fd = open(raw.c_str(), O_RDONLY);
		assert(fd >= 0 && read(fd, &actual[0], bytes) == static_cast<ssize_t>(bytes));
		close(fd);
		assert(actual == image);
		char command[2];
		fd = open(commit.c_str(), O_RDONLY);
		assert(fd >= 0 && read(fd, command, 2) == 2);
		close(fd);
		assert(command[0] == '1' && command[1] == '\n');
	}
	assert(lcd.stop(&error) && lcd.stop(&error));
	fd = open(commit.c_str(), O_RDONLY);
	char command[2];
	assert(fd >= 0 && read(fd, command, 2) == 2);
	close(fd);
	assert(command[0] == '0');
	assert(unlink(commit.c_str()) == 0);
	assert(!lcd.publish(&image[0], bytes, &error));
	fd = open(commit.c_str(), O_CREAT | O_RDWR, 0600);
	assert(fd >= 0);
	close(fd);
	assert(lcd.stop(&error));
	unlink(raw.c_str());
	unlink(commit.c_str());
	rmdir(directory);
	return 0;
}
