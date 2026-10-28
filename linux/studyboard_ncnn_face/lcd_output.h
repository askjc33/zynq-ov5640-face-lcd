// SPDX-License-Identifier: MIT
#ifndef STUDYBOARD_LCD_OUTPUT_H
#define STUDYBOARD_LCD_OUTPUT_H

#include <stddef.h>
#include <stdint.h>
#include <string>

/*
 * 教学型 LCD 输出接口：不是 framebuffer，也不是 V4L2 的一部分。
 * 只负责把一张完整、已画框的 BGR24 图像交给本项目驱动的 MM2S 通道。
 * publish() 同步等待驱动完成整帧提交；首次调用还会启动连续 MM2S。
 * 同一时刻只能有一个线程调用 publish()。matched 模式由推理线程调用；
 * preview 模式先由推理线程完成首次发布，再交给采集循环调用。
 */
class LcdOutput {
public:
	LcdOutput();
	~LcdOutput();
	bool configure(const std::string &sysfs_directory, size_t frame_bytes,
		       std::string *error);
	bool enabled() const;
	bool publish(const uint8_t *bgr, size_t bytes, std::string *error);
	bool stop(std::string *error);

private:
	LcdOutput(const LcdOutput &);
	LcdOutput &operator=(const LcdOutput &);
	std::string directory_;
	size_t frame_bytes_;
	bool owned_;
};

#endif
