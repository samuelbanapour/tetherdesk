#include "ui.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "rd_font.h"

namespace td {

namespace {
// Layout sizes are in points relative to 14 px body text.
constexpr float kBaseSize = 14.0f;
}

bool Ui::init(SDL_Renderer *r) {
    r_ = r;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    return atlas(rd_font_body()) != nullptr;
}

void Ui::shutdown() {
    for (auto &t : atlases_)
        if (t.second) SDL_DestroyTexture(t.second);
    atlases_.clear();
}

SDL_Texture *Ui::atlas(int face) {
    auto it = atlases_.find(face);
    if (it != atlases_.end()) return it->second;
    const rd_font_face *f = rd_font_face_at(face);
    const uint8_t *alpha = rd_font_alpha(face);
    SDL_Texture *t = nullptr;
    if (alpha) {
        std::vector<uint32_t> px(size_t(f->atlas_w) * f->height);
        for (size_t i = 0; i < px.size(); i++) px[i] = (uint32_t(alpha[i]) << 24) | 0x00FFFFFF;
        // Nearest sampling: glyphs are drawn at exactly their rasterized size.
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        t = SDL_CreateTexture(r_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, f->atlas_w, f->height);
        if (t) {
            SDL_UpdateTexture(t, nullptr, px.data(), f->atlas_w * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        }
    }
    atlases_[face] = t;
    return t;
}

// The face whose pixel size matches `size` points at the current scale.
int Ui::face_for(float size) const { return rd_font_best(kBaseSize * size * scale_); }

void Ui::begin(float scale, float mouse_x, float mouse_y, bool mouse_down) {
    scale_ = scale;
    mx_ = mouse_x;
    my_ = mouse_y;
    down_ = mouse_down;
    consumed_ = false;
}

void Ui::on_mouse(bool down, float x, float y) {
    if (down) {
        pressed_ = true;
        released_ = false;
        press_x_ = x;
        press_y_ = y;
    } else if (pressed_) {
        released_ = true;
        release_x_ = x;
        release_y_ = y;
    }
}

void Ui::end() {
    if (released_) pressed_ = released_ = false;
}

bool Ui::clicked(const Rect &r) const {
    return released_ && r.contains(press_x_, press_y_) && r.contains(release_x_, release_y_);
}

static SDL_Rect px_rect(const Rect &r, float s) {
    int x0 = int(std::lround(r.x * s)), y0 = int(std::lround(r.y * s));
    int x1 = int(std::lround((r.x + r.w) * s)), y1 = int(std::lround((r.y + r.h) * s));
    return {x0, y0, x1 - x0, y1 - y0};
}

void Ui::fill(const Rect &r, Color c, float radius) {
    SDL_SetRenderDrawColor(r_, c.r, c.g, c.b, c.a);
    SDL_Rect p = px_rect(r, scale_);
    int rad = std::min({int(radius * scale_), p.w / 2, p.h / 2});
    if (rad <= 0) {
        SDL_RenderFillRect(r_, &p);
        return;
    }
    SDL_Rect mid = {p.x, p.y + rad, p.w, p.h - 2 * rad};
    SDL_RenderFillRect(r_, &mid);
    for (int i = 0; i < rad; i++) {
        float dy = rad - i - 0.5f;
        int inset = rad - int(std::sqrt(float(rad * rad) - dy * dy) + 0.5f);
        SDL_Rect top = {p.x + inset, p.y + i, p.w - 2 * inset, 1};
        SDL_Rect bot = {p.x + inset, p.y + p.h - 1 - i, p.w - 2 * inset, 1};
        SDL_RenderFillRect(r_, &top);
        SDL_RenderFillRect(r_, &bot);
    }
}

void Ui::outline(const Rect &r, Color c, float radius) {
    SDL_SetRenderDrawColor(r_, c.r, c.g, c.b, c.a);
    SDL_Rect p = px_rect(r, scale_);
    int rad = std::min({int(radius * scale_), p.w / 2, p.h / 2});
    if (rad <= 0) {
        SDL_RenderDrawRect(r_, &p);
        return;
    }
    SDL_RenderDrawLine(r_, p.x + rad, p.y, p.x + p.w - rad - 1, p.y);
    SDL_RenderDrawLine(r_, p.x + rad, p.y + p.h - 1, p.x + p.w - rad - 1, p.y + p.h - 1);
    SDL_RenderDrawLine(r_, p.x, p.y + rad, p.x, p.y + p.h - rad - 1);
    SDL_RenderDrawLine(r_, p.x + p.w - 1, p.y + rad, p.x + p.w - 1, p.y + p.h - rad - 1);
    const int steps = std::max(4, rad * 2);
    for (int q = 0; q < 4; q++) {
        float cx = (q == 0 || q == 3) ? p.x + rad : p.x + p.w - rad - 1;
        float cy = (q < 2) ? p.y + rad : p.y + p.h - rad - 1;
        float a0 = float(M_PI) * (1.0f + 0.5f * q);
        for (int i = 0; i < steps; i++) {
            float a = a0 + float(M_PI) * 0.5f * i / steps, b = a0 + float(M_PI) * 0.5f * (i + 1) / steps;
            SDL_RenderDrawLineF(r_, cx + std::cos(a) * rad, cy + std::sin(a) * rad, cx + std::cos(b) * rad,
                                cy + std::sin(b) * rad);
        }
    }
}

float Ui::line_height(float size) const { return rd_font_face_at(face_for(size))->height / scale_; }

float Ui::text_width(const std::string &s, float size) const {
    const rd_font_face *f = rd_font_face_at(face_for(size));
    float w = 0;
    for (unsigned char ch : s) w += f->glyphs[(ch < 32 || ch > 126 ? '?' : ch) - 32].advance;
    return w / scale_;
}

float Ui::text(float x, float y, const std::string &s, Color c, float size) {
    const int fi = face_for(size);
    const rd_font_face &face = *rd_font_face_at(fi);
    SDL_Texture *tex = atlas(fi);
    if (!tex) return 0;
    SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(tex, c.a);
    // Whole output pixels only, so every glyph lands pixel-aligned.
    int pen = int(std::lround(x * scale_));
    const int top = int(std::lround(y * scale_));
    for (unsigned char ch : s) {
        if (ch < 32 || ch > 126) ch = '?';
        const rd_glyph &g = face.glyphs[ch - 32];
        if (ch != ' ') {
            SDL_Rect src = {g.x, 0, g.w, face.height};
            SDL_Rect dst = {pen, top, g.w, face.height};
            SDL_RenderCopy(r_, tex, &src, &dst);
        }
        pen += g.advance;
    }
    return pen / scale_ - x;
}

void Ui::text_centered(const Rect &r, const std::string &s, Color c, float size) {
    float w = text_width(s, size);
    text(r.x + (r.w - w) / 2, r.y + (r.h - line_height(size)) / 2, s, c, size);
}

bool Ui::button(const Rect &r, const std::string &label, bool active, bool enabled) {
    bool hover = enabled && mouse_over(r);
    Color bg = active ? (hover ? theme::accent_hover : theme::accent) : (hover ? theme::button_hover : theme::button);
    if (!enabled) bg.a = 120;
    fill(r, bg, 6);
    Color tc = enabled ? theme::text : theme::dim;
    text_centered(r, label, tc);
    bool hit = enabled && clicked(r);
    if (hit || (released_ && r.contains(press_x_, press_y_))) consumed_ = true;
    return hit;
}

bool Ui::checkbox(float x, float y, const std::string &label, bool &value) {
    Rect hit{x, y, 24 + text_width(label), 22};
    Rect box{x, y + 2, 18, 18};
    bool hover = mouse_over(hit);
    fill(box, value ? theme::accent : theme::field, 4);
    outline(box, value ? theme::accent : (hover ? theme::dim : theme::panel_border), 4);
    if (value) {  // check mark
        SDL_SetRenderDrawColor(r_, 255, 255, 255, 255);
        const float s = scale_;
        for (float o = -0.6f; o <= 0.6f; o += 0.6f) {
            SDL_RenderDrawLineF(r_, (box.x + 4) * s, (box.y + 9 + o) * s, (box.x + 8) * s, (box.y + 13 + o) * s);
            SDL_RenderDrawLineF(r_, (box.x + 8) * s, (box.y + 13 + o) * s, (box.x + 14) * s, (box.y + 5 + o) * s);
        }
    }
    text(x + 26, y + 2, label, theme::text);
    if (clicked(hit)) {
        consumed_ = true;
        value = !value;
        return true;
    }
    return false;
}

void Ui::field(const Rect &r, const std::string &label, const std::string &value, bool focused, bool secret) {
    text(r.x, r.y - line_height() - 4, label, theme::dim);
    fill(r, theme::field, 6);
    outline(r, focused ? theme::accent : theme::panel_border, 6);
    std::string shown = secret ? std::string(value.size(), '*') : value;
    const float pad = 10, avail = r.w - 2 * pad;
    // Scroll long values so the caret end stays visible.
    while (!shown.empty() && text_width(shown) > avail - 4) shown.erase(0, 1);
    float ty = r.y + (r.h - line_height()) / 2;
    float w = text(r.x + pad, ty, shown, theme::text);
    if (focused && (SDL_GetTicks() / 530) % 2 == 0) fill({r.x + pad + w + 1, ty + 2, 1.5f, line_height() - 4}, theme::text);
    if (clicked(r)) consumed_ = true;
}

}  // namespace td
