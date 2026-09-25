// platform_x11.cpp - Linux/BSD backend for X11 sessions.
//   Capture:   XGetImage of the root window on a dedicated thread/connection.
//   Input:     XTest fake events on a second connection.
//   Clipboard: not implemented (X11 selections need an event-driven owner
//              window); the host simply doesn't sync the clipboard here.
// Wayland sessions are not supported by this backend.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "platform.h"
#include "rd_proto.h"

namespace td {
namespace {

class X11Capturer : public ScreenCapturer {
public:
    ~X11Capturer() override { stop(); }

    std::vector<DisplayInfo> displays() override {
        Display *d = XOpenDisplay(nullptr);
        if (!d) return {};
        Screen *s = DefaultScreenOfDisplay(d);
        DisplayInfo info{"X11 screen " + std::string(DisplayString(d)), WidthOfScreen(s), HeightOfScreen(s)};
        XCloseDisplay(d);
        return {info};
    }

    int current_display() const override { return 0; }
    FramePtr latest() override { return slot_.get(); }

    bool start(int display, int max_fps, std::string &err) override {
        stop();
        if (display != 0) {
            err = "no such display";
            return false;
        }
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) {
            err = "cannot open X display (is $DISPLAY set? Wayland sessions are not supported)";
            return false;
        }
        fps_ = std::max(1, std::min(60, max_fps));
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (dpy_) XCloseDisplay(dpy_);
        dpy_ = nullptr;
        slot_.clear();
    }

private:
    void loop() {
        Window root = DefaultRootWindow(dpy_);
        auto next = std::chrono::steady_clock::now();
        while (running_) {
            XWindowAttributes wa;
            XGetWindowAttributes(dpy_, root, &wa);
            XImage *img = XGetImage(dpy_, root, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap);
            if (img && img->bits_per_pixel == 32) {
                auto f = std::make_shared<Frame>();
                f->width = wa.width;
                f->height = wa.height;
                f->stride = wa.width * 4;
                f->pixels.resize(size_t(f->stride) * wa.height);
                for (int y = 0; y < wa.height; y++)
                    std::memcpy(f->pixels.data() + size_t(y) * f->stride, img->data + size_t(y) * img->bytes_per_line,
                                size_t(f->stride));
                slot_.publish(std::move(f));
            }
            if (img) XDestroyImage(img);
            next += std::chrono::microseconds(1000000 / fps_);
            std::this_thread::sleep_until(next);
            if (std::chrono::steady_clock::now() > next + std::chrono::milliseconds(250))
                next = std::chrono::steady_clock::now();
        }
    }

    Display *dpy_ = nullptr;
    FrameSlot slot_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int fps_ = 30;
};

class X11Input : public InputInjector {
public:
    X11Input() { dpy_ = XOpenDisplay(nullptr); }
    ~X11Input() override {
        release_all();
        if (dpy_) XCloseDisplay(dpy_);
    }

    void pointer(int x, int y, uint8_t buttons, int wheel_x, int wheel_y) override {
        if (!dpy_) return;
        XTestFakeMotionEvent(dpy_, -1, x, y, CurrentTime);
        static const struct {
            uint8_t bit;
            unsigned x_button;
        } kMap[] = {{RD_BTN_LEFT, 1}, {RD_BTN_MIDDLE, 2}, {RD_BTN_RIGHT, 3}, {RD_BTN_X1, 8}, {RD_BTN_X2, 9}};
        for (auto &m : kMap)
            if ((buttons ^ buttons_) & m.bit) XTestFakeButtonEvent(dpy_, m.x_button, (buttons & m.bit) != 0, CurrentTime);
        buttons_ = buttons;
        auto click = [this](unsigned b, int n) {
            for (int i = 0; i < n; i++) {
                XTestFakeButtonEvent(dpy_, b, True, CurrentTime);
                XTestFakeButtonEvent(dpy_, b, False, CurrentTime);
            }
        };
        if (wheel_y) click(wheel_y > 0 ? 4 : 5, std::abs(wheel_y));
        if (wheel_x) click(wheel_x > 0 ? 7 : 6, std::abs(wheel_x));
        XFlush(dpy_);
    }

    void key(uint16_t hid, bool down) override {
        int ev = hid_to_evdev(hid);
        if (!dpy_ || ev < 0) return;
        XTestFakeKeyEvent(dpy_, unsigned(ev + 8), down, CurrentTime);
        XFlush(dpy_);
        if (down) held_.push_back(hid);
        else held_.erase(std::remove(held_.begin(), held_.end(), hid), held_.end());
    }

    void release_all() override {
        auto keys = held_;
        for (uint16_t k : keys) key(k, false);
        if (dpy_ && buttons_) {
            for (unsigned b : {1u, 2u, 3u, 8u, 9u}) XTestFakeButtonEvent(dpy_, b, False, CurrentTime);
            XFlush(dpy_);
        }
        buttons_ = 0;
    }

private:
    Display *dpy_ = nullptr;
    uint8_t buttons_ = 0;
    std::vector<uint16_t> held_;
};

class NullClipboard : public Clipboard {
public:
    uint64_t change_count() override { return 0; }
    bool get_text(std::string &) override { return false; }
    void set_text(const std::string &) override {}
};

}  // namespace

bool create_native_platform(Platform &out, const PlatformOptions &, std::string &err) {
    Display *probe = XOpenDisplay(nullptr);
    if (!probe) {
        err = "cannot open X display (is $DISPLAY set? Wayland sessions are not supported). Try --demo.";
        return false;
    }
    int ev, er, maj, min;
    bool has_xtest = XTestQueryExtension(probe, &ev, &er, &maj, &min);
    XCloseDisplay(probe);
    if (!has_xtest) std::fprintf(stderr, "[warn] XTest extension missing: remote input will not work\n");
    out.capturer = std::make_unique<X11Capturer>();
    out.input = std::make_unique<X11Input>();
    out.clipboard = std::make_unique<NullClipboard>();
    out.os_name = "Linux (X11)";
    return true;
}

}  // namespace td
