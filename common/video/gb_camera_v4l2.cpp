/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_camera_v4l2.hpp"
#include "snes9x.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef __linux__
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>
#endif

void S9xGBSetCameraCallback(bool (*cb)(unsigned char *dst, int width, int height));

namespace {

/* Latest frame as 8-bit luma, native camera size. The capture thread writes
 * it, the core's capture callback reads it. */
std::mutex                 g_lock;
std::vector<unsigned char> g_frame;
int                        g_frame_width = 0;
int                        g_frame_height = 0;
bool                       g_frame_valid = false;

std::thread                g_thread;
std::atomic<bool>          g_stop{false};
bool                       g_running = false;
int                        g_running_index = -1;

bool CameraGetImageCB(unsigned char *dst, int out_width, int out_height)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_frame_valid || g_frame_width <= 0 || g_frame_height <= 0 || g_frame.empty())
        return false;

    /* Nearest-neighbour resample of the whole frame onto the sensor, mirrored
     * horizontally, exactly as the win32 bridge hands it to the core. */
    const unsigned char *src = g_frame.data();
    const int sw = g_frame_width, sh = g_frame_height;
    for (int y = 0; y < out_height; y++)
    {
        int sy = y * sh / out_height;
        if (sy < 0) sy = 0; else if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < out_width; x++)
        {
            int sx = sw - 1 - (x * sw / out_width);
            if (sx < 0) sx = 0; else if (sx >= sw) sx = sw - 1;
            dst[y * out_width + x] = src[(size_t)sy * sw + sx];
        }
    }
    return true;
}

#ifdef __linux__

struct Device
{
    std::string node;
    std::string name;
};

int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do
        r = ioctl(fd, request, arg);
    while (r == -1 && errno == EINTR);
    return r;
}

/* Video capture nodes under /dev, in node order. A UVC camera usually exposes
 * a second, metadata-only node right after its capture node; that one has no
 * capture capability (or no capture format) and is skipped. */
std::vector<Device> EnumDevices()
{
    std::vector<Device> devices;

    for (int i = 0; i < 64; i++)
    {
        char node[32];
        snprintf(node, sizeof(node), "/dev/video%d", i);

        int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        v4l2_capability cap{};
        bool usable = xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0;
        if (usable)
        {
            uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                      : cap.capabilities;
            usable = (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING);
        }
        if (usable)
        {
            v4l2_fmtdesc desc{};
            desc.index = 0;
            desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            usable = xioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0;
        }
        if (usable)
        {
            std::string name((const char *)cap.card, strnlen((const char *)cap.card, sizeof(cap.card)));
            if (name.empty())
                name = "Camera";
            devices.push_back({ node, name });
        }
        close(fd);
    }

    /* Two identical cameras get told apart by their node. */
    for (size_t i = 0; i < devices.size(); i++)
    {
        bool duplicate = false;
        for (size_t j = 0; j < devices.size(); j++)
            if (j != i && devices[j].name == devices[i].name)
                duplicate = true;
        if (duplicate)
            devices[i].name += " (" + devices[i].node.substr(5) + ")";
    }

    return devices;
}

/* Raw formats we can turn into luma, most preferred first. Luma-first
 * layouts are cheapest; RGB needs a weighted sum per pixel. */
const uint32_t kPreferredFormats[] = {
    V4L2_PIX_FMT_YUYV,   V4L2_PIX_FMT_UYVY,   V4L2_PIX_FMT_YVYU,   V4L2_PIX_FMT_VYUY,
    V4L2_PIX_FMT_GREY,   V4L2_PIX_FMT_NV12,   V4L2_PIX_FMT_NV21,   V4L2_PIX_FMT_YUV420,
    V4L2_PIX_FMT_YVU420, V4L2_PIX_FMT_RGB24,  V4L2_PIX_FMT_BGR24,  V4L2_PIX_FMT_RGB32,
    V4L2_PIX_FMT_BGR32,  V4L2_PIX_FMT_XRGB32, V4L2_PIX_FMT_XBGR32, V4L2_PIX_FMT_RGB565
};

inline unsigned char Luma(int r, int g, int b)
{
    return (unsigned char)((r * 54 + g * 183 + b * 19) >> 8);
}

/* Converts one captured buffer to 8-bit luma. Returns false for a buffer too
 * short for the format it claims. */
bool ToLuma(const unsigned char *src, size_t length, const v4l2_pix_format &pix, std::vector<unsigned char> &luma)
{
    const int w = (int)pix.width, h = (int)pix.height;
    if (w <= 0 || h <= 0)
        return false;
    size_t bpl = pix.bytesperline;

    auto need = [&](size_t bytes_per_pixel, size_t rows) {
        if (bpl == 0) bpl = (size_t)w * bytes_per_pixel;
        return length >= bpl * (rows - 1) + (size_t)w * bytes_per_pixel;
    };

    luma.resize((size_t)w * h);

    switch (pix.pixelformat)
    {
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_YVYU:
        if (!need(2, h)) return false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                luma[(size_t)y * w + x] = src[y * bpl + x * 2];
        return true;

    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_VYUY:
        if (!need(2, h)) return false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                luma[(size_t)y * w + x] = src[y * bpl + x * 2 + 1];
        return true;

    case V4L2_PIX_FMT_GREY:
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV21:
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_YVU420:
        /* The Y plane comes first in all of these. */
        if (!need(1, h)) return false;
        for (int y = 0; y < h; y++)
            memcpy(&luma[(size_t)y * w], src + y * bpl, w);
        return true;

    case V4L2_PIX_FMT_RGB24:
    case V4L2_PIX_FMT_BGR24:
    {
        if (!need(3, h)) return false;
        const bool bgr = pix.pixelformat == V4L2_PIX_FMT_BGR24;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const unsigned char *p = src + y * bpl + x * 3;
                luma[(size_t)y * w + x] = bgr ? Luma(p[2], p[1], p[0]) : Luma(p[0], p[1], p[2]);
            }
        return true;
    }

    case V4L2_PIX_FMT_RGB32:
    case V4L2_PIX_FMT_XRGB32:
        /* x r g b */
        if (!need(4, h)) return false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const unsigned char *p = src + y * bpl + x * 4;
                luma[(size_t)y * w + x] = Luma(p[1], p[2], p[3]);
            }
        return true;

    case V4L2_PIX_FMT_BGR32:
    case V4L2_PIX_FMT_XBGR32:
        /* b g r x */
        if (!need(4, h)) return false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const unsigned char *p = src + y * bpl + x * 4;
                luma[(size_t)y * w + x] = Luma(p[2], p[1], p[0]);
            }
        return true;

    case V4L2_PIX_FMT_RGB565:
        if (!need(2, h)) return false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const unsigned char *p = src + y * bpl + x * 2;
                const unsigned v = p[0] | (p[1] << 8);
                const int r = ((v >> 11) & 0x1f) * 255 / 31;
                const int g = ((v >> 5) & 0x3f) * 255 / 63;
                const int b = (v & 0x1f) * 255 / 31;
                luma[(size_t)y * w + x] = Luma(r, g, b);
            }
        return true;

    default:
        return false;
    }
}

struct MappedBuffer
{
    void *start = nullptr;
    size_t length = 0;
};

void PublishInvalid()
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_frame_valid = false;
}

/* Streams frames from one node until asked to stop. Any failure just ends the
 * thread: like the win32 bridge, the game then sees a blank viewfinder until
 * the camera is re-applied from the settings. */
void CaptureThread(std::string node)
{
    int fd = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        PublishInvalid();
        return;
    }

    /* The sensor is 128x112, so a small mode keeps USB bandwidth and the
     * conversion cheap; the driver rounds to what the camera can do. */
    v4l2_format format{};
    bool have_format = false;
    for (uint32_t fourcc : kPreferredFormats)
    {
        format = {};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = 320;
        format.fmt.pix.height = 240;
        format.fmt.pix.pixelformat = fourcc;
        format.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd, VIDIOC_S_FMT, &format) == 0 && format.fmt.pix.pixelformat == fourcc)
        {
            have_format = true;
            break;
        }
    }

    std::vector<MappedBuffer> buffers;
    bool streaming = false;

    if (have_format)
    {
        v4l2_requestbuffers request{};
        request.count = 4;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_REQBUFS, &request) == 0 && request.count >= 2)
        {
            bool mapped = true;
            for (uint32_t i = 0; i < request.count && mapped; i++)
            {
                v4l2_buffer buffer{};
                buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buffer.memory = V4L2_MEMORY_MMAP;
                buffer.index = i;
                if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0)
                {
                    mapped = false;
                    break;
                }
                void *start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
                if (start == MAP_FAILED)
                {
                    mapped = false;
                    break;
                }
                buffers.push_back({ start, buffer.length });
                if (xioctl(fd, VIDIOC_QBUF, &buffer) != 0)
                    mapped = false;
            }

            if (mapped)
            {
                v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                streaming = xioctl(fd, VIDIOC_STREAMON, &type) == 0;
            }
        }
    }

    std::vector<unsigned char> luma;
    while (streaming && !g_stop.load())
    {
        pollfd pfd{ fd, POLLIN, 0 };
        int ready = poll(&pfd, 1, 100);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &buffer) != 0)
        {
            if (errno == EAGAIN)
                continue;
            break;
        }

        if (buffer.index < buffers.size() && !(buffer.flags & V4L2_BUF_FLAG_ERROR) &&
            ToLuma((const unsigned char *)buffers[buffer.index].start, buffer.bytesused, format.fmt.pix, luma))
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_frame.swap(luma);
            g_frame_width = (int)format.fmt.pix.width;
            g_frame_height = (int)format.fmt.pix.height;
            g_frame_valid = true;
        }

        if (xioctl(fd, VIDIOC_QBUF, &buffer) != 0)
            break;
    }

    if (streaming)
    {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd, VIDIOC_STREAMOFF, &type);
    }
    for (auto &b : buffers)
        munmap(b.start, b.length);
    close(fd);

    PublishInvalid();
}

#endif /* __linux__ */

} // namespace

void S9xGBCameraRegister()
{
    S9xGBSetCameraCallback(&CameraGetImageCB);
}

void S9xGBCameraEnumerate(std::vector<std::string> &names)
{
    names.clear();
#ifdef __linux__
    for (auto &device : EnumDevices())
        names.push_back(device.name);
#endif
}

bool S9xGBCameraStart(int device_index)
{
    S9xGBCameraStop();
#ifdef __linux__
    auto devices = EnumDevices();
    if (device_index < 0 || device_index >= (int)devices.size())
        return false;

    g_stop.store(false);
    g_thread = std::thread(CaptureThread, devices[device_index].node);
    g_running = true;
    g_running_index = device_index;
    return true;
#else
    (void)device_index;
    return false;
#endif
}

void S9xGBCameraStop()
{
    if (g_running)
    {
        g_stop.store(true);
        if (g_thread.joinable())
            g_thread.join();
        g_running = false;
        g_running_index = -1;
    }
    std::lock_guard<std::mutex> lock(g_lock);
    g_frame_valid = false;
}

bool S9xGBCameraIsRunning()
{
    return g_running;
}

void S9xGBCameraApply()
{
    if (!Settings.GBVideoCamera)
    {
        S9xGBCameraStop();
        return;
    }

    const int index = Settings.GBVideoCameraIndex;
    if (g_running && g_running_index == index)
        return;

    S9xGBCameraStart(index);
}
