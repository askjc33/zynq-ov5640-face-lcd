// SPDX-License-Identifier: MIT
#ifndef STUDYBOARD_FACE_DETECTOR_H
#define STUDYBOARD_FACE_DETECTOR_H

#include <stdint.h>

#include <string>
#include <vector>

#include "net.h"

struct FaceDetection {
	float x0;
	float y0;
	float x1;
	float y1;
	float score;
	int label;
};

struct FaceTiming {
	double preprocess_ms;
	double inference_ms;
	double postprocess_ms;
};

/*
 * Runtime contract for the exported Ultralytics YOLOv8n model:
 *
 *   input  "in0"  : RGB float32 CHW, 1x3xSxS, values in [0, 1]
 *   output "out0" : 5xN, N=(S/8)^2+(S/16)^2+(S/32)^2
 *                    row 0..3 = centre-x, centre-y, width, height
 *                    row 4    = face confidence
 *
 * Supported exported sizes are 224, 256 and 320.  The camera frame is
 * 640x480 BGR24 and is letterboxed with value 114.  Model size must match
 * the exported NCNN parameter/weight pair; 320 remains the default baseline.
 */
class FaceDetector {
public:
	FaceDetector();

	bool load(const std::string &param_path, const std::string &bin_path,
		  int thread_count, int model_size, std::string *error);
	int input_size() const { return model_size_; }
	int prediction_count() const { return prediction_count_; }

	bool detect(const uint8_t *bgr, int width, int height,
		    float confidence_threshold, float nms_threshold,
		    std::vector<FaceDetection> *detections,
		    FaceTiming *timing, std::string *error) const;

private:
	ncnn::Net net_;
	int thread_count_;
	int model_size_;
	int prediction_count_;
};

void draw_face_detections_bgr(uint8_t *bgr, int width, int height,
			      const std::vector<FaceDetection> &detections);

bool write_bgr24_ppm(const std::string &path, const uint8_t *bgr,
		     int width, int height, std::string *error);

#endif
