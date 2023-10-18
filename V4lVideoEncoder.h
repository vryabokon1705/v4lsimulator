#ifndef V4LVIDEOENCODER_H
#define V4LVIDEOENCODER_H

#pragma once

#include <gz/common/FlagSet.hh>
#include <gz/common/av/Export.hh>
#include <gz/common/HWVideo.hh>
#include <gz/utils/ImplPtr.hh>

using namespace std;

class V4lVideoEncoder
{

    string V4L_FORMAT = "video4linux2,v4l2";

public:


    V4lVideoEncoder();
    ~V4lVideoEncoder();

    bool isEncoding() const;

    bool addFrame(const unsigned char *frame,
        const unsigned int width,
        const unsigned int height,
        const std::chrono::steady_clock::time_point &timestamp);

    bool start(const std::string &format,
                         const std::string &filename,
                         const unsigned int width,
                         const unsigned int height,
                         const unsigned int fps,
                         const unsigned int bitRate);

    bool stop();
    void reset();

private:
    GZ_UTILS_UNIQUE_IMPL_PTR(dataPtr)
};

#endif