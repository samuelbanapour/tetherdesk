// platform_demo.cpp - a synthetic, fully interactive desktop.
//
// Runs anywhere with no OS permissions, which makes it ideal for trying
// TetherDesk out and for automated tests. It exercises every part of the
// pipeline: static regions (wallpaper), text (notepad you can type into),
// photographic motion (plasma window), and input (paint with the mouse).
// Ctrl+C / Ctrl+V inside the notepad use the demo clipboard, which the host
// syncs to viewers just like a real one.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iterator>
#include <thread>

#include "platform.h"
#include "rd_font.h"
#include "rd_proto.h"

namespace td {
namespace {

const DisplayInfo kDisplays[] = {{"Demo Display 1", 1280, 800}, {"Demo Display 2 (wide)", 1920, 1080}};

struct DemoState {
    std::mutex mu;
    int width = 1280, height = 800;
    int cursor_x = 640, cursor_y = 400;
    uint8_t buttons = 0;
    bool shift = false, ctrl = false;
    int last_x = -1, last_y = -1;
    int brush = 4;
    float hue = 0;
    std::vector<uint32_t> paint;  // 0 = transparent, else 0xFFRRGGBB
    std::string notepad = "Welcome to TetherDesk! Click here and type.\n";
    std::string clipboard;
    uint64_t clip_count = 0;
    int keys_typed = 0;
};

// ------------------------------------------------------------ drawing kit

struct Canvas {
    uint32_t *px;
    int w, h;

    void fill(int x, int y, int rw, int rh, uint32_t c) {
        int x0 = std::max(0, x), y0 = std::max(0, y);
        int x1 = std::min(w, x + rw), y1 = std::min(h, y + rh);
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) px[yy * w + xx] = c;
    }
    void blend(int x, int y, uint32_t c, int a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        uint32_t &d = px[y * w + x];
        auto mix = [a](uint32_t s, uint32_t t) { return (s * a + t * (255 - a)) / 255; };
        uint32_t r = mix((c >> 16) & 255, (d >> 16) & 255), g = mix((c >> 8) & 255, (d >> 8) & 255),
                 b = mix(c & 255, d & 255);
        d = 0xFF000000u | r << 16 | g << 8 | b;
    }
    int text(int x, int y, const std::string &s, uint32_t c, int max_w = 1 << 30) {
        const rd_font_face &f = *rd_font_face_at(rd_font_body());
        const uint8_t *alpha = rd_font_alpha(rd_font_body());
        if (!alpha) return 0;
        int pen = x;
        for (unsigned char ch : s) {
            if (ch < 32 || ch > 126) ch = '?';
            const rd_glyph &g = f.glyphs[ch - 32];
            if (pen + g.advance - x > max_w) break;
            for (int gy = 0; gy < f.height; gy++)
                for (int gx = 0; gx < g.w; gx++) blend(pen + gx, y + gy, c, alpha[gy * f.atlas_w + g.x + gx]);
            pen += g.advance;
        }
        return pen - x;
    }
};

int text_width(const std::string &s) {
    int w = 0;
    const rd_font_face &f = *rd_font_face_at(rd_font_body());
    for (unsigned char ch : s) w += f.glyphs[(ch < 32 || ch > 126 ? '?' : ch) - 32].advance;
    return w;
}

uint32_t hsv(float h, float s, float v) {
    float r, g, b, f = h * 6 - std::floor(h * 6), p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (int(h * 6) % 6) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    default: r = v, g = p, b = q; break;
    }
    return 0xFF000000u | uint32_t(r * 255) << 16 | uint32_t(g * 255) << 8 | uint32_t(b * 255);
}

void draw_window(Canvas &c, int x, int y, int w, int h, const std::string &title) {
    for (int i = 1; i <= 6; i++)  // soft shadow
        for (int yy = y + i; yy < y + h + i; yy++) c.blend(x + w + i - 1, yy, 0, 40 - i * 6);
    c.fill(x, y, w, 28, 0xFF2B3240);
    c.fill(x, y + 28, w, h - 28, 0xFFF7F8FA);
    const uint32_t dots[3] = {0xFFFF5F57, 0xFFFEBC2E, 0xFF28C840};
    for (int i = 0; i < 3; i++) c.fill(x + 12 + i * 18, y + 9, 10, 10, dots[i]);
    c.text(x + (w - text_width(title)) / 2, y + 5, title, 0xFFE8ECF3);
}

void draw_cursor(Canvas &c, int x, int y) {
    // Classic arrow: a filled triangle with a white outline.
    for (int row = 0; row < 18; row++) {
        int span = row < 12 ? row : 12 - (row - 12) * 2;
        for (int col = 0; col <= span && col < 12; col++) {
            bool edge = col == 0 || col == span || row == 17 || (row < 12 && col == row);
            c.blend(x + col, y + row, edge ? 0xFFFFFFFF : 0xFF000000, 255);
        }
    }
}

char hid_to_ascii(uint16_t hid, bool shift) {
    if (hid >= 4 && hid <= 29) return char((shift ? 'A' : 'a') + hid - 4);
    if (hid >= 30 && hid <= 39) return (shift ? ")!@#$%^&*(" : "0123456789")[(hid - 29) % 10];
    static const char lo[] = " -=[]\\#;'`,./", hi[] = " _+{}|~:\"~<>?";
    if (hid >= 44 && hid <= 56) return (shift ? hi : lo)[hid - 44];
    return 0;
}

// ------------------------------------------------------------ capturer

class DemoCapturer : public ScreenCapturer {
public:
    explicit DemoCapturer(std::shared_ptr<DemoState> st) : st_(std::move(st)) {}
    ~DemoCapturer() override { stop(); }

    std::vector<DisplayInfo> displays() override { return {std::begin(kDisplays), std::end(kDisplays)}; }
    int current_display() const override { return display_; }
    FramePtr latest() override { return slot_.get(); }

    bool start(int display, int max_fps, std::string &err) override {
        if (display < 0 || display >= int(std::size(kDisplays))) {
            err = "no such display";
            return false;
        }
        stop();
        display_ = display;
        fps_ = std::max(1, std::min(120, max_fps));
        const DisplayInfo &d = kDisplays[display];
        {
            std::lock_guard<std::mutex> lock(st_->mu);
            st_->width = d.width;
            st_->height = d.height;
            st_->paint.assign(size_t(d.width) * d.height, 0);
            st_->cursor_x = d.width / 2;
            st_->cursor_y = d.height / 2;
        }
        build_wallpaper(d.width, d.height);
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        slot_.clear();
    }

private:
    void build_wallpaper(int w, int h) {
        wallpaper_.resize(size_t(w) * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float fx = float(x) / w, fy = float(y) / h;
                int r = int(18 + 40 * fx), g = int(40 + 60 * fy), b = int(90 + 90 * (1 - fx * fy));
                if (((x / 40) + (y / 40)) % 2 == 0) r += 4, g += 4, b += 4;  // faint checker
                wallpaper_[size_t(y) * w + x] = 0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
            }
    }

    void loop() {
        auto next = std::chrono::steady_clock::now();
        const auto t0 = next;
        while (running_) {
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            render(t);
            next += std::chrono::microseconds(1000000 / fps_);
            std::this_thread::sleep_until(next);
            if (std::chrono::steady_clock::now() > next + std::chrono::milliseconds(250))
                next = std::chrono::steady_clock::now();
        }
    }

    void render(double t) {
        auto f = std::make_shared<Frame>();
        std::lock_guard<std::mutex> lock(st_->mu);
        const int w = st_->width, h = st_->height;
        f->width = w;
        f->height = h;
        f->stride = w * 4;
        f->pixels.resize(size_t(f->stride) * h);
        Canvas c{reinterpret_cast<uint32_t *>(f->pixels.data()), w, h};
        std::copy(wallpaper_.begin(), wallpaper_.end(), c.px);

        // Paint layer.
        for (size_t i = 0; i < st_->paint.size(); i++)
            if (st_->paint[i]) c.px[i] = st_->paint[i];

        // Menu bar with a live clock.
        c.fill(0, 0, w, 28, 0xFF151922);
        c.text(12, 5, "TetherDesk demo host  -  " + kDisplays[display_].name, 0xFFE8ECF3);
        std::time_t now = std::time(nullptr);
        char clock[64];
        std::strftime(clock, sizeof clock, "%a %H:%M:%S", std::localtime(&now));
        c.text(w - 12 - text_width(clock), 5, clock, 0xFFE8ECF3);

        // Notepad window.
        const int nx = 60, ny = 70, nw = 560, nh = 330;
        draw_window(c, nx, ny, nw, nh, "Notepad");
        int ty = ny + 38;
        std::string line;
        auto flush = [&](bool force) {
            if (!force && text_width(line) < nw - 32) return;
            if (ty < ny + nh - 60) c.text(nx + 14, ty, line, 0xFF1D2330, nw - 28);
            ty += 20;
            line.clear();
        };
        for (char ch : st_->notepad) {
            if (ch == '\n') flush(true);
            else {
                line += ch;
                flush(false);
            }
        }
        if (std::fmod(t, 1.0) < 0.55) line += '|';
        flush(true);
        c.fill(nx, ny + nh - 44, nw, 1, 0xFFD5DAE3);
        c.text(nx + 14, ny + nh - 36,
               "Clipboard: " + (st_->clipboard.empty() ? std::string("(empty)") : st_->clipboard.substr(0, 60)),
               0xFF5A6478, nw - 28);
        c.text(nx + 14, ny + nh - 18 - 2, "Ctrl+C copies the notepad, Ctrl+V pastes", 0xFF8A93A6, nw - 28);

        // Animated plasma window drifting around (photographic content).
        const int pw = 300, ph = 220;
        int px = int(w * 0.62 + std::sin(t * 0.7) * w * 0.12);
        int py = int(h * 0.20 + std::cos(t * 0.5) * h * 0.06);
        draw_window(c, px, py, pw, ph, "Live plasma");
        for (int y = 0; y < ph - 28; y++)
            for (int x = 0; x < pw; x++) {
                double v = std::sin(x * 0.045 + t * 2.1) + std::sin((y * 0.05 + t * 1.3)) +
                           std::sin((x + y) * 0.03 + t) + std::sin(std::sqrt(double(x * x + y * y)) * 0.05 - t * 1.7);
                int X = px + x, Y = py + 28 + y;
                if (X >= 0 && X < w && Y >= 0 && Y < h) c.px[Y * w + X] = hsv(float(std::fmod(v * 0.125 + 1.0, 1.0)), 0.75f, 0.95f);
            }

        // Paint hint + status panel.
        const int sx = 60, sy = h - 130;
        draw_window(c, sx, sy, 560, 100, "Status");
        char status[160];
        std::snprintf(status, sizeof status, "Pointer %d, %d   buttons %d   brush %dpx   keys typed %d", st_->cursor_x,
                      st_->cursor_y, st_->buttons, st_->brush, st_->keys_typed);
        c.text(sx + 14, sy + 38, status, 0xFF1D2330);
        c.text(sx + 14, sy + 60, "Drag on the wallpaper to paint - wheel = brush size - middle-click clears",
               0xFF5A6478, 532);

        draw_cursor(c, st_->cursor_x, st_->cursor_y);
        slot_.publish(std::move(f));
    }

    std::shared_ptr<DemoState> st_;
    std::vector<uint32_t> wallpaper_;
    FrameSlot slot_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int display_ = 0;
    int fps_ = 30;
};

// ------------------------------------------------------------ input

class DemoInput : public InputInjector {
public:
    explicit DemoInput(std::shared_ptr<DemoState> st) : st_(std::move(st)) {}

    void pointer(int x, int y, uint8_t buttons, int, int wheel_y) override {
        std::lock_guard<std::mutex> lock(st_->mu);
        DemoState &s = *st_;
        s.cursor_x = std::max(0, std::min(s.width - 1, x));
        s.cursor_y = std::max(0, std::min(s.height - 1, y));
        if (wheel_y) s.brush = std::max(1, std::min(40, s.brush + (wheel_y > 0 ? 1 : -1)));
        if ((buttons & RD_BTN_MIDDLE) && !(s.buttons & RD_BTN_MIDDLE)) std::fill(s.paint.begin(), s.paint.end(), 0);
        if (buttons & RD_BTN_LEFT) {
            if (s.last_x < 0) s.last_x = s.cursor_x, s.last_y = s.cursor_y;
            stroke(s, s.last_x, s.last_y, s.cursor_x, s.cursor_y);
            s.last_x = s.cursor_x;
            s.last_y = s.cursor_y;
        } else {
            s.last_x = s.last_y = -1;
        }
        s.buttons = buttons;
    }

    void key(uint16_t hid, bool down) override {
        std::lock_guard<std::mutex> lock(st_->mu);
        DemoState &s = *st_;
        if (hid == RD_HID_LSHIFT || hid == RD_HID_RSHIFT) s.shift = down;
        if (hid == RD_HID_LCTRL || hid == RD_HID_RCTRL || hid == RD_HID_LGUI || hid == RD_HID_RGUI) s.ctrl = down;
        if (!down) return;
        if (s.ctrl && hid == 6) {  // Ctrl+C
            s.clipboard = s.notepad;
            s.clip_count++;
            return;
        }
        if (s.ctrl && hid == 25) {  // Ctrl+V
            s.notepad += s.clipboard;
            return;
        }
        if (s.ctrl) return;
        s.keys_typed++;
        if (hid == 42) {  // Backspace
            if (!s.notepad.empty()) s.notepad.pop_back();
        } else if (hid == 40 || hid == 88) {
            s.notepad += '\n';
        } else if (char ch = hid_to_ascii(hid, s.shift)) {
            s.notepad += ch;
        }
        if (s.notepad.size() > 2000) s.notepad.erase(0, s.notepad.size() - 2000);
    }

    void release_all() override {
        std::lock_guard<std::mutex> lock(st_->mu);
        st_->shift = st_->ctrl = false;
        st_->buttons = 0;
        st_->last_x = st_->last_y = -1;
    }

private:
    static void stroke(DemoState &s, int x0, int y0, int x1, int y1) {
        int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0)) + 1;
        for (int i = 0; i <= steps; i++) {
            int cx = x0 + (x1 - x0) * i / steps, cy = y0 + (y1 - y0) * i / steps;
            s.hue = std::fmod(s.hue + 0.0015f, 1.0f);
            uint32_t col = hsv(s.hue, 0.8f, 1.0f);
            int r = s.brush / 2;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    int X = cx + dx, Y = cy + dy;
                    if (dx * dx + dy * dy <= r * r + 1 && X >= 0 && Y >= 0 && X < s.width && Y < s.height)
                        s.paint[size_t(Y) * s.width + X] = col;
                }
        }
    }

    std::shared_ptr<DemoState> st_;
};

class DemoClipboard : public Clipboard {
public:
    explicit DemoClipboard(std::shared_ptr<DemoState> st) : st_(std::move(st)) {}
    uint64_t change_count() override {
        std::lock_guard<std::mutex> lock(st_->mu);
        return st_->clip_count;
    }
    bool get_text(std::string &out) override {
        std::lock_guard<std::mutex> lock(st_->mu);
        out = st_->clipboard;
        return true;
    }
    void set_text(const std::string &text) override {
        std::lock_guard<std::mutex> lock(st_->mu);
        st_->clipboard = text;
        st_->clip_count++;
    }

private:
    std::shared_ptr<DemoState> st_;
};

}  // namespace

void create_demo_platform(Platform &out) {
    auto st = std::make_shared<DemoState>();
    out.capturer = std::make_unique<DemoCapturer>(st);
    out.input = std::make_unique<DemoInput>(st);
    out.clipboard = std::make_unique<DemoClipboard>(st);
    out.os_name = "TetherDesk demo desktop";
}

}  // namespace td
