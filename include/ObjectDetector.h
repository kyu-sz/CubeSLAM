/**
 * This file is part of CubeSLAM.
 *
 * Copyright (C) 2018, Carnegie Mellon University
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef OBJECTDETECTOR_H
#define OBJECTDETECTOR_H

#include <vector>
#include <string>

#include <opencv2/opencv.hpp>

namespace ORB_SLAM2 {

struct Object {
    cv::Rect bbox;
    float conf;
    int classIdx;

    Object(const cv::Rect& bbox_, float conf_, int classIdx_): bbox(bbox_), conf(conf_), classIdx(classIdx_) {}
};

/// Based on YOLOv3 from DarkNet.
class ObjectDetector {
public:
    ObjectDetector(
            const char *cfgFile,
            const char *weightFile,
            float nmsThresh=.45,
            float thresh=.5);

    void Detect(const cv::Mat &im, std::vector<Object> &objects);

private:
    // Remove the bounding boxes with low confidence using non-maxima suppression
    void Postprocess(const cv::Mat& im, const std::vector<cv::Mat>& outs, std::vector<Object>& objects);

private:
    cv::dnn::Net mNet;
    cv::Mat mBlob;
    float mNmsThresh;
    float mConfThresh;

    const int mInputWidth = 416;        // Width of network's input image
    const int mInputHeight = 416;       // Height of network's input image

    std::vector<cv::String> mOutputNames;
};

}

#endif //OBJECTDETECTOR_H
