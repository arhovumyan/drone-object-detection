// Linux/Jetson camera backend via V4L2 (YUYV -> BGRA).
//
// This targets standard V4L2 UVC devices and is the starting point for the
// Jetson. NOTE: Jetson CSI cameras driven by the Argus ISP are best captured
// through GStreamer (nvarguscamerasrc); wire that in here when deploying on a
// CSI sensor. This file is compiled only on UNIX, never on the Windows build.

#include "camera.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

namespace dd {
namespace {

inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

class V4l2Camera : public ICamera {
public:
    explicit V4l2Camera(bool flip) : flip_(flip) {}
    ~V4l2Camera() override {
        if (fd_ >= 0) {
            v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &t);
            for (auto& b : buffers_) if (b.start) munmap(b.start, b.length);
            close(fd_);
        }
    }

    bool open(const CameraConfig& cfg) {
        const char* dev = "/dev/video0";
        fd_ = ::open(dev, O_RDWR);
        if (fd_ < 0) { std::cerr << "open " << dev << " failed: " << strerror(errno) << "\n"; return false; }

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = cfg.requestWidth;
        fmt.fmt.pix.height      = cfg.requestHeight;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            std::cerr << "VIDIOC_S_FMT failed: " << strerror(errno) << "\n"; return false;
        }
        w_ = fmt.fmt.pix.width;
        h_ = fmt.fmt.pix.height;
        if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
            std::cerr << "Camera did not accept YUYV; got something else.\n"; return false;
        }
        std::cout << "Capture format: " << w_ << "x" << h_ << " YUYV\n";

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
            std::cerr << "VIDIOC_REQBUFS failed\n"; return false;
        }
        buffers_.resize(req.count);
        for (unsigned i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) return false;
            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) { buffers_[i].start = nullptr; return false; }
            if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) return false;
        }
        v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_STREAMON, &t) < 0) {
            std::cerr << "VIDIOC_STREAMON failed\n"; return false;
        }
        return true;
    }

    bool grab(Frame& out) override {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) return false;

        const uint8_t* yuyv = static_cast<const uint8_t*>(buffers_[buf.index].start);
        out.width = w_; out.height = h_;
        out.bgra.resize(static_cast<size_t>(w_) * h_ * 4);
        // YUYV: 2 pixels per 4 bytes (Y0 U Y1 V).
        for (int y = 0; y < h_; ++y) {
            int dy = flip_ ? (h_ - 1 - y) : y;
            const uint8_t* src = yuyv + static_cast<size_t>(y) * w_ * 2;
            uint8_t* dst = &out.bgra[static_cast<size_t>(dy) * w_ * 4];
            for (int x = 0; x < w_; x += 2) {
                int y0 = src[0], u = src[1], y1 = src[2], v = src[3];
                src += 4;
                auto conv = [&](int Y) {
                    int c = Y - 16, d = u - 128, e = v - 128;
                    dst[0] = clamp8((298 * c + 516 * d + 128) >> 8);          // B
                    dst[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);// G
                    dst[2] = clamp8((298 * c + 409 * e + 128) >> 8);          // R
                    dst[3] = 255;
                    dst += 4;
                };
                conv(y0);
                conv(y1);
            }
        }
        ioctl(fd_, VIDIOC_QBUF, &buf);
        return true;
    }

    int width()  const override { return w_; }
    int height() const override { return h_; }

private:
    struct Buf { void* start = nullptr; size_t length = 0; };
    int fd_ = -1;
    int w_ = 0, h_ = 0;
    bool flip_ = false;
    std::vector<Buf> buffers_;
};

} // namespace

std::unique_ptr<ICamera> CreateCamera(const CameraConfig& cfg) {
    auto cam = std::make_unique<V4l2Camera>(cfg.flipVertical);
    if (!cam->open(cfg)) return nullptr;
    return cam;
}

} // namespace dd
