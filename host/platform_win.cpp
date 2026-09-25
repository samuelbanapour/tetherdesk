// platform_win.cpp - Windows backend.
//   Capture:   GDI BitBlt from the desktop DC into a top-down DIB section,
//              one entry per monitor (EnumDisplayMonitors), cursor overlaid.
//   Input:     SendInput with absolute virtual-desktop coordinates and
//              hardware scan codes (layout-independent).
//   Clipboard: Win32 clipboard, CF_UNICODETEXT <-> UTF-8.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "platform.h"
#include "rd_proto.h"

namespace td {
namespace {

struct Monitor {
    RECT rc;
    std::string name;
};

BOOL CALLBACK enum_proc(HMONITOR m, HDC, LPRECT, LPARAM user) {
    MONITORINFOEXA mi;
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoA(m, &mi)) {
        auto *v = reinterpret_cast<std::vector<Monitor> *>(user);
        v->push_back({mi.rcMonitor, std::string(mi.szDevice) + ((mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "")});
    }
    return TRUE;
}

std::vector<Monitor> monitors() {
    std::vector<Monitor> v;
    EnumDisplayMonitors(nullptr, nullptr, enum_proc, reinterpret_cast<LPARAM>(&v));
    return v;
}

class WinCapturer : public ScreenCapturer {
public:
    ~WinCapturer() override { stop(); }

    std::vector<DisplayInfo> displays() override {
        std::vector<DisplayInfo> out;
        for (auto &m : monitors()) out.push_back({m.name, int(m.rc.right - m.rc.left), int(m.rc.bottom - m.rc.top)});
        return out;
    }

    int current_display() const override { return current_; }
    FramePtr latest() override { return slot_.get(); }

    bool start(int display, int max_fps, std::string &err) override {
        stop();
        auto mons = monitors();
        if (display < 0 || display >= int(mons.size())) {
            err = "no such display";
            return false;
        }
        current_ = display;
        rect_ = mons[display].rc;
        fps_ = std::max(1, std::min(60, max_fps));
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        slot_.clear();
    }

    RECT rect() const { return rect_; }

private:
    void loop() {
        const int w = rect_.right - rect_.left, h = rect_.bottom - rect_.top;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, dib);

        auto next = std::chrono::steady_clock::now();
        while (running_) {
            BitBlt(mem, 0, 0, w, h, screen, rect_.left, rect_.top, SRCCOPY | CAPTUREBLT);
            CURSORINFO ci = {sizeof ci};
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                ICONINFO ii;
                if (GetIconInfo(ci.hCursor, &ii)) {
                    DrawIconEx(mem, ci.ptScreenPos.x - rect_.left - int(ii.xHotspot),
                               ci.ptScreenPos.y - rect_.top - int(ii.yHotspot), ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                }
            }
            GdiFlush();
            auto f = std::make_shared<Frame>();
            f->width = w;
            f->height = h;
            f->stride = w * 4;
            f->pixels.resize(size_t(f->stride) * h);
            std::memcpy(f->pixels.data(), bits, f->pixels.size());
            slot_.publish(std::move(f));

            next += std::chrono::microseconds(1000000 / fps_);
            std::this_thread::sleep_until(next);
            if (std::chrono::steady_clock::now() > next + std::chrono::milliseconds(250))
                next = std::chrono::steady_clock::now();
        }
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }

    FrameSlot slot_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    RECT rect_ = {0, 0, 1, 1};
    int current_ = 0;
    int fps_ = 30;
};

class WinInput : public InputInjector {
public:
    explicit WinInput(WinCapturer *cap) : cap_(cap) {}
    ~WinInput() override { release_all(); }

    void pointer(int x, int y, uint8_t buttons, int wheel_x, int wheel_y) override {
        RECT r = cap_->rect();
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = std::max(2, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = std::max(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
        INPUT in[8] = {};
        int n = 0;
        in[n].type = INPUT_MOUSE;
        in[n].mi.dx = LONG(int64_t(r.left + x - vx) * 65535 / (vw - 1));
        in[n].mi.dy = LONG(int64_t(r.top + y - vy) * 65535 / (vh - 1));
        in[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        n++;
        static const struct {
            uint8_t bit;
            DWORD down, up, data;
        } kMap[] = {{RD_BTN_LEFT, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0},
                    {RD_BTN_RIGHT, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0},
                    {RD_BTN_MIDDLE, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0},
                    {RD_BTN_X1, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1},
                    {RD_BTN_X2, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2}};
        for (auto &m : kMap) {
            if (!((buttons ^ buttons_) & m.bit)) continue;
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = (buttons & m.bit) ? m.down : m.up;
            in[n].mi.mouseData = m.data;
            n++;
        }
        buttons_ = buttons;
        if (wheel_y) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
            in[n].mi.mouseData = DWORD(wheel_y * WHEEL_DELTA);
            n++;
        }
        if (wheel_x) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL;
            in[n].mi.mouseData = DWORD(wheel_x * WHEEL_DELTA);
            n++;
        }
        SendInput(UINT(n), in, sizeof(INPUT));
    }

    void key(uint16_t hid, bool down) override {
        int sc = hid_to_win_scancode(hid);
        if (sc < 0) return;
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = WORD(sc & 0xFF);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | ((sc & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &in, sizeof in);
        if (down) held_.push_back(hid);
        else held_.erase(std::remove(held_.begin(), held_.end(), hid), held_.end());
    }

    void release_all() override {
        auto keys = held_;
        for (uint16_t k : keys) key(k, false);
        if (buttons_) {
            POINT p;
            GetCursorPos(&p);
            RECT r = cap_->rect();
            pointer(p.x - r.left, p.y - r.top, 0, 0, 0);
        }
    }

private:
    WinCapturer *cap_;
    uint8_t buttons_ = 0;
    std::vector<uint16_t> held_;
};

class WinClipboard : public Clipboard {
public:
    uint64_t change_count() override { return GetClipboardSequenceNumber(); }

    bool get_text(std::string &out) override {
        if (!OpenClipboard(nullptr)) return false;
        bool ok = false;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto *w = static_cast<const wchar_t *>(GlobalLock(h))) {
                int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                if (n > 0) {
                    out.resize(size_t(n));
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
                    out.resize(size_t(n - 1));
                    ok = true;
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return ok;
    }

    void set_text(const std::string &text) override {
        int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (n <= 0 || !OpenClipboard(nullptr)) return;
        EmptyClipboard();
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size_t(n) * sizeof(wchar_t))) {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, static_cast<wchar_t *>(GlobalLock(g)), n);
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        }
        CloseClipboard();
    }
};

}  // namespace

bool create_native_platform(Platform &out, const PlatformOptions &, std::string &) {
    // Capture in real pixels on scaled (HiDPI) monitors.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto cap = std::make_unique<WinCapturer>();
    out.input = std::make_unique<WinInput>(cap.get());
    out.capturer = std::move(cap);
    out.clipboard = std::make_unique<WinClipboard>();
    out.os_name = "Windows";
    return true;
}

}  // namespace td
