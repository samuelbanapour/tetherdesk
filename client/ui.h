// ui.h - a tiny immediate-mode UI toolkit on top of SDL_Renderer.
//
// All coordinates are in *points*; the toolkit multiplies by the display
// scale so the UI is crisp on HiDPI screens and in browsers with
// devicePixelRatio > 1. Text uses the embedded Aileron atlases (1x and 2x).
#pragma once

#include <SDL.h>

#include <map>
#include <string>

namespace td {

struct Color {
    uint8_t r, g, b, a = 255;
};

namespace theme {
constexpr Color bg{13, 17, 23};
constexpr Color panel{22, 27, 36, 240};
constexpr Color panel_border{48, 56, 70};
constexpr Color field{12, 15, 20};
constexpr Color text{230, 235, 243};
constexpr Color dim{139, 149, 167};
constexpr Color accent{72, 145, 255};
constexpr Color accent_hover{98, 163, 255};
constexpr Color button{38, 45, 58};
constexpr Color button_hover{52, 61, 78};
constexpr Color good{63, 185, 120};
constexpr Color warn{240, 180, 60};
constexpr Color bad{240, 90, 90};
}  // namespace theme

struct Rect {
    float x, y, w, h;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

class Ui {
public:
    bool init(SDL_Renderer *r);
    void shutdown();

    // Call once per frame before drawing. scale = output pixels per point.
    void begin(float scale, float mouse_x, float mouse_y, bool mouse_down);
    // Feed mouse button events (points) so clicks are never missed between frames.
    void on_mouse(bool down, float x, float y);
    void end();

    float scale() const { return scale_; }
    float mouse_x() const { return mx_; }
    float mouse_y() const { return my_; }
    bool mouse_over(const Rect &r) const { return r.contains(mx_, my_); }
    // True if the mouse went down and up inside r since the last frame.
    bool clicked(const Rect &r) const;
    // True if any widget consumed the click this frame.
    bool consumed_click() const { return consumed_; }

    void fill(const Rect &r, Color c, float radius = 0);
    void outline(const Rect &r, Color c, float radius = 0);
    // Returns width in points. size: 1 = body text, larger scales it up.
    float text(float x, float y, const std::string &s, Color c, float size = 1.0f);
    float text_width(const std::string &s, float size = 1.0f) const;
    float line_height(float size = 1.0f) const;
    void text_centered(const Rect &r, const std::string &s, Color c, float size = 1.0f);

    bool button(const Rect &r, const std::string &label, bool active = false, bool enabled = true);
    // Toggles `value` when clicked. Returns true if it changed.
    bool checkbox(float x, float y, const std::string &label, bool &value);
    // Draws a single-line text field. Editing is driven by the caller.
    void field(const Rect &r, const std::string &label, const std::string &value, bool focused, bool secret);

private:
    SDL_Renderer *r_ = nullptr;
    SDL_Texture *atlas(int face);  // lazily built per font size
    int face_for(float size) const;
    std::map<int, SDL_Texture *> atlases_;
    float scale_ = 1;
    float mx_ = 0, my_ = 0;
    bool down_ = false;
    // Click tracking: press position + release position since last frame.
    bool pressed_ = false, released_ = false;
    float press_x_ = 0, press_y_ = 0, release_x_ = 0, release_y_ = 0;
    bool consumed_ = false;
};

}  // namespace td
