// SPDX-License-Identifier: MIT
#include "face_detector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace {

const int kExpectedOutputRows = 5;
const int kMaximumDetections = 100;

int prediction_count_for_size(int size)
{
	int count = 0;
	for (int stride = 8; stride <= 32; stride *= 2) {
		const int side = size / stride;
		count += side * side;
	}
	return count;
}

double milliseconds_since(
	const std::chrono::steady_clock::time_point &start,
	const std::chrono::steady_clock::time_point &finish)
{
	return std::chrono::duration<double, std::milli>(finish - start).count();
}

float clamp_float(float value, float minimum, float maximum)
{
	return std::max(minimum, std::min(maximum, value));
}

float intersection_over_union(const FaceDetection &a,
			      const FaceDetection &b)
{
	const float left = std::max(a.x0, b.x0);
	const float top = std::max(a.y0, b.y0);
	const float right = std::min(a.x1, b.x1);
	const float bottom = std::min(a.y1, b.y1);
	const float width = std::max(0.0f, right - left);
	const float height = std::max(0.0f, bottom - top);
	const float intersection = width * height;
	const float area_a = std::max(0.0f, a.x1 - a.x0) *
			     std::max(0.0f, a.y1 - a.y0);
	const float area_b = std::max(0.0f, b.x1 - b.x0) *
			     std::max(0.0f, b.y1 - b.y0);
	const float union_area = area_a + area_b - intersection;

	return union_area > 0.0f ? intersection / union_area : 0.0f;
}

void non_maximum_suppression(std::vector<FaceDetection> *candidates,
			     float threshold)
{
	std::vector<FaceDetection> kept;

	std::sort(candidates->begin(), candidates->end(),
		  [](const FaceDetection &a, const FaceDetection &b) {
			  return a.score > b.score;
		  });

	for (std::vector<FaceDetection>::const_iterator candidate =
		     candidates->begin();
	     candidate != candidates->end(); ++candidate) {
		bool suppressed = false;

		for (std::vector<FaceDetection>::const_iterator selected =
			     kept.begin();
		     selected != kept.end(); ++selected) {
			if (candidate->label == selected->label &&
			    intersection_over_union(*candidate, *selected) >
				    threshold) {
				suppressed = true;
				break;
			}
		}

		if (!suppressed) {
			kept.push_back(*candidate);
			if (static_cast<int>(kept.size()) >= kMaximumDetections)
				break;
		}
	}

	candidates->swap(kept);
}

void put_bgr_pixel(uint8_t *image, int width, int height, int x, int y,
		   uint8_t blue, uint8_t green, uint8_t red)
{
	if (x < 0 || y < 0 || x >= width || y >= height)
		return;

	uint8_t *pixel = image + (static_cast<size_t>(y) * width + x) * 3U;
	pixel[0] = blue;
	pixel[1] = green;
	pixel[2] = red;
}

} // namespace

FaceDetector::FaceDetector()
	: thread_count_(1), model_size_(320), prediction_count_(2100)
{
}

bool FaceDetector::load(const std::string &param_path,
			const std::string &bin_path, int thread_count, int model_size,
			std::string *error)
{
	if (model_size != 192 && model_size != 224 && model_size != 256 &&
	    model_size != 320) {
		if (error)
			*error = "model size must be 192, 224, 256 or 320";
		return false;
	}
	model_size_ = model_size;
	prediction_count_ = prediction_count_for_size(model_size_);
	thread_count_ = std::max(1, thread_count);

	/* Zynq-7020 has no NCNN-compatible Vulkan device. */
	net_.opt.use_vulkan_compute = false;
	net_.opt.num_threads = thread_count_;

	if (net_.load_param(param_path.c_str()) != 0) {
		if (error)
			*error = "cannot load NCNN parameter file: " + param_path;
		return false;
	}
	if (net_.load_model(bin_path.c_str()) != 0) {
		if (error)
			*error = "cannot load NCNN weight file: " + bin_path;
		return false;
	}

	return true;
}

bool FaceDetector::detect(const uint8_t *bgr, int width, int height,
			  float confidence_threshold, float nms_threshold,
			  std::vector<FaceDetection> *detections,
			  FaceTiming *timing, std::string *error) const
{
	const std::chrono::steady_clock::time_point preprocess_start =
		std::chrono::steady_clock::now();
	const float scale = std::min(static_cast<float>(model_size_) / width,
				   static_cast<float>(model_size_) / height);
	const int resized_width = std::max(1, std::min(model_size_,
					static_cast<int>(std::round(width * scale))));
	const int resized_height = std::max(1, std::min(model_size_,
					static_cast<int>(std::round(height * scale))));
	const int horizontal_padding = model_size_ - resized_width;
	const int vertical_padding = model_size_ - resized_height;
	const int left_padding = horizontal_padding / 2;
	const int right_padding = horizontal_padding - left_padding;
	const int top_padding = vertical_padding / 2;
	const int bottom_padding = vertical_padding - top_padding;
	const float normalization[3] = {
		1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f
	};

	/*
	 * NCNN performs BGR->RGB and bilinear resize in one operation.  Padding
	 * happens before /255 normalization, hence the constant is 114 rather
	 * than 114/255 here.
	 */
	ncnn::Mat resized = ncnn::Mat::from_pixels_resize(
		bgr, ncnn::Mat::PIXEL_BGR2RGB, width, height,
		resized_width, resized_height);
	if (resized.empty()) {
		if (error)
			*error = "NCNN failed to allocate the resized input";
		return false;
	}

	ncnn::Mat input;
	ncnn::copy_make_border(resized, input, top_padding, bottom_padding,
			       left_padding, right_padding,
			       ncnn::BORDER_CONSTANT, 114.0f);
	if (input.empty() || input.w != model_size_ ||
	    input.h != model_size_ || input.c != 3) {
		if (error) {
			std::ostringstream stream;
			stream << "unexpected NCNN input shape: dims=" << input.dims
			       << " w=" << input.w << " h=" << input.h
			       << " c=" << input.c;
			*error = stream.str();
		}
		return false;
	}
	input.substract_mean_normalize(NULL, normalization);
	const std::chrono::steady_clock::time_point preprocess_finish =
		std::chrono::steady_clock::now();

	ncnn::Extractor extractor = net_.create_extractor();
	extractor.set_light_mode(true);
	if (extractor.input("in0", input) != 0) {
		if (error)
			*error = "NCNN input blob 'in0' was not found";
		return false;
	}

	ncnn::Mat output;
	const std::chrono::steady_clock::time_point inference_start =
		std::chrono::steady_clock::now();
	if (extractor.extract("out0", output) != 0) {
		if (error)
			*error = "NCNN output blob 'out0' was not found";
		return false;
	}
	const std::chrono::steady_clock::time_point inference_finish =
		std::chrono::steady_clock::now();

	/* Fail closed if the selected size does not match the exported model. */
	if (output.dims != 2 || output.w != prediction_count_ ||
	    output.h != kExpectedOutputRows) {
		if (error) {
			std::ostringstream stream;
			stream << "unexpected out0 shape: dims=" << output.dims
			       << " w=" << output.w << " h=" << output.h
			       << " c=" << output.c << "; expected [5,"
			       << prediction_count_ << "] for input " << model_size_;
			*error = stream.str();
		}
		return false;
	}

	const std::chrono::steady_clock::time_point postprocess_start =
		std::chrono::steady_clock::now();
	const float *centre_x = output.row(0);
	const float *centre_y = output.row(1);
	const float *box_width = output.row(2);
	const float *box_height = output.row(3);
	const float *face_score = output.row(4);
	std::vector<FaceDetection> candidates;
	candidates.reserve(64);

	for (int index = 0; index < output.w; ++index) {
		if (!std::isfinite(face_score[index]) ||
		    face_score[index] < confidence_threshold)
			continue;

		float x0 = centre_x[index] - box_width[index] * 0.5f;
		float y0 = centre_y[index] - box_height[index] * 0.5f;
		float x1 = centre_x[index] + box_width[index] * 0.5f;
		float y1 = centre_y[index] + box_height[index] * 0.5f;

		/* Undo this model's letterbox and return to source coordinates. */
		x0 = (x0 - left_padding) / scale;
		y0 = (y0 - top_padding) / scale;
		x1 = (x1 - left_padding) / scale;
		y1 = (y1 - top_padding) / scale;

		FaceDetection detection;
		detection.x0 = clamp_float(x0, 0.0f,
					   static_cast<float>(width - 1));
		detection.y0 = clamp_float(y0, 0.0f,
					   static_cast<float>(height - 1));
		detection.x1 = clamp_float(x1, 0.0f,
					   static_cast<float>(width - 1));
		detection.y1 = clamp_float(y1, 0.0f,
					   static_cast<float>(height - 1));
		detection.score = face_score[index];
		detection.label = 0;

		if (detection.x1 > detection.x0 &&
		    detection.y1 > detection.y0)
			candidates.push_back(detection);
	}

	non_maximum_suppression(&candidates, nms_threshold);
	detections->swap(candidates);
	const std::chrono::steady_clock::time_point postprocess_finish =
		std::chrono::steady_clock::now();

	if (timing) {
		timing->preprocess_ms = milliseconds_since(preprocess_start,
						   preprocess_finish);
		timing->inference_ms = milliseconds_since(inference_start,
						  inference_finish);
		timing->postprocess_ms = milliseconds_since(postprocess_start,
						    postprocess_finish);
	}

	return true;
}

void draw_face_detections_bgr(
	uint8_t *bgr, int width, int height,
	const std::vector<FaceDetection> &detections)
{
	const int thickness = 3;

	for (std::vector<FaceDetection>::const_iterator detection =
		     detections.begin();
	     detection != detections.end(); ++detection) {
		const int x0 = static_cast<int>(std::floor(detection->x0));
		const int y0 = static_cast<int>(std::floor(detection->y0));
		const int x1 = static_cast<int>(std::ceil(detection->x1));
		const int y1 = static_cast<int>(std::ceil(detection->y1));

		for (int offset = 0; offset < thickness; ++offset) {
			for (int x = x0; x <= x1; ++x) {
				put_bgr_pixel(bgr, width, height, x, y0 + offset,
					      0, 0, 255);
				put_bgr_pixel(bgr, width, height, x, y1 - offset,
					      0, 0, 255);
			}
			for (int y = y0; y <= y1; ++y) {
				put_bgr_pixel(bgr, width, height, x0 + offset, y,
					      0, 0, 255);
				put_bgr_pixel(bgr, width, height, x1 - offset, y,
					      0, 0, 255);
			}
		}
	}
}

bool write_bgr24_ppm(const std::string &path, const uint8_t *bgr,
		     int width, int height, std::string *error)
{
	std::ofstream output(path.c_str(), std::ios::binary);
	if (!output) {
		if (error)
			*error = "cannot open output image: " + path;
		return false;
	}

	output << "P6\n" << width << " " << height << "\n255\n";
	for (size_t pixel = 0;
	     pixel < static_cast<size_t>(width) * height; ++pixel) {
		const char rgb[3] = {
			static_cast<char>(bgr[pixel * 3U + 2U]),
			static_cast<char>(bgr[pixel * 3U + 1U]),
			static_cast<char>(bgr[pixel * 3U + 0U])
		};
		output.write(rgb, sizeof(rgb));
	}

	if (!output) {
		if (error)
			*error = "failed while writing output image: " + path;
		return false;
	}
	return true;
}
