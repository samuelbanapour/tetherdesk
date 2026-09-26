// platform.h - OS abstraction for the host: screen capture, input injection
// and clipboard. Each OS provides these in its own translation unit
// (platform_mac.mm, platform_x11.cpp, platform_win.cpp); the demo backend
// (platform_demo.cpp) works everywhere and needs no OS permissions.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace td {

// One captured screen image: 32-bit BGRA, top-down.
struct Frame {
    int width = 0;
    int height = 0;
    int stride = 0;      // bytes per row
    uint64_t seq = 0;    // increases whenever the content may have changed
    std::vector<uint8_t> pixels;
};
using FramePtr = std::shared_ptr<const Frame>;

struct DisplayInfo {
    std::string name;
    int width = 0;   // capture size in pixels
    int height = 0;
};

class ScreenCapturer {
public:
    virtual ~ScreenCapturer() = default;
    virtual std::vector<DisplayInfo> displays() = 0;
    // Starts (or restarts) capturing `display` at up to `max_fps`.
    virtual bool start(int display, int max_fps, std::string &err) = 0;
    virtual void stop() = 0;
    // Latest complete frame, or null if none captured yet. Cheap; thread-safe.
    virtual FramePtr latest() = 0;
    virtual int current_display() const = 0;
    // HiDPI ("Retina") displays can be captured at full pixel density or at
    // half (logical) resolution. Takes effect on the next start().
    virtual bool supports_high_resolution() const { return false; }
    virtual bool high_resolution() const { return false; }
    virtual void set_high_resolution(bool) {}
};

// Coordinates passed to the injector are in capture pixels of the
// currently captured display; implementations map them to OS coordinates.
class InputInjector {
public:
    virtual ~InputInjector() = default;
    virtual void pointer(int x, int y, uint8_t buttons, int wheel_x, int wheel_y) = 0;
    virtual void key(uint16_t hid_usage, bool down) = 0;
    // Called when the owning viewer leaves, so no key or button stays stuck.
    virtual void release_all() = 0;
};

class Clipboard {
public:
    virtual ~Clipboard() = default;
    // Monotonic counter that changes whenever the clipboard content changes.
    virtual uint64_t change_count() = 0;
    virtual bool get_text(std::string &out) = 0;
    virtual void set_text(const std::string &text) = 0;
};

struct Platform {
    std::unique_ptr<ScreenCapturer> capturer;
    std::unique_ptr<InputInjector> input;
    std::unique_ptr<Clipboard> clipboard;
    std::string os_name;
};

struct PlatformOptions {
    bool native_resolution = true;  // capture HiDPI displays at full pixel density
};

// Real OS backends (implemented per platform). Returns false with a
// human-readable reason if the OS refuses (e.g. missing permissions).
bool create_native_platform(Platform &out, const PlatformOptions &opts, std::string &err);
// A synthetic, interactive desktop for demos and tests.
void create_demo_platform(Platform &out);

// Shared helper for polling-style capturers: owns the latest-frame slot.
class FrameSlot {
public:
    void publish(std::shared_ptr<Frame> f) {
        std::lock_guard<std::mutex> lock(mu_);
        f->seq = ++seq_;
        latest_ = std::move(f);
    }
    FramePtr get() {
        std::lock_guard<std::mutex> lock(mu_);
        return latest_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        latest_.reset();
    }

private:
    std::mutex mu_;
    FramePtr latest_;
    uint64_t seq_ = 0;
};

// USB HID usage -> OS key codes (keymap.cpp). Return -1 when unmapped.
int hid_to_mac_keycode(uint16_t hid);
int hid_to_win_scancode(uint16_t hid);  // bit 0x100 set = extended (E0) key
int hid_to_evdev(uint16_t hid);

}  // namespace td
