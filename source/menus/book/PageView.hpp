#ifndef WOOK_READER_PAGE_VIEW_HPP
#define WOOK_READER_PAGE_VIEW_HPP

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>

#include "settings.h"

enum ViewAnchor { ANCHOR_TOP = -1, ANCHOR_CENTER = 0, ANCHOR_BOTTOM = 1 };

// Zoom/pan geometry for one page (or a two-page spread) inside a viewport.
// Content units are arbitrary (PDF points or image pixels); zoom maps them to
// viewport pixels. cx/cy is the content centre in viewport coordinates.
struct PageView {
  float vx = 0, vy = 0;      // viewport origin on the canvas
  float vw = 1280, vh = 720;  // viewport size
  float cw = 1, ch = 1;       // content size at zoom 1
  float zoom = 1, fit_zoom = 1, min_zoom = 1, max_zoom = 4;
  float cx = 640, cy = 360;
  int fit = FIT_PAGE;

  void set_viewport(float x, float y, float w, float h) {
    vx = x;
    vy = y;
    vw = std::max(w, 1.0f);
    vh = std::max(h, 1.0f);
  }
  void set_content(float w, float h) {
    cw = std::max(w, 1.0f);
    ch = std::max(h, 1.0f);
  }

  float fit_zoom_for(float w, float h) const {
    float zp = std::min(vw / w, vh / h);
    return fit == FIT_WIDTH ? vw / w : zp;
  }

  void reset(int anchor = ANCHOR_TOP) {
    float zp = std::min(vw / cw, vh / ch);
    fit_zoom = fit_zoom_for(cw, ch);
    min_zoom = std::min(zp, fit_zoom);
    max_zoom = std::max(fit_zoom * 4.0f, std::max(vw / cw, vh / ch) * 2.0f);
    zoom = fit_zoom;
    cx = vw / 2;
    float h = ch * zoom;
    cy = anchor == ANCHOR_TOP ? h / 2 : anchor == ANCHOR_BOTTOM ? vh - h / 2 : vh / 2;
    clamp();
  }

  void clamp() {
    float w = cw * zoom, h = ch * zoom;
    cx = (w <= vw) ? vw / 2 : std::min(std::max(cx, vw - w / 2), w / 2);
    cy = (h <= vh) ? vh / 2 : std::min(std::max(cy, vh - h / 2), h / 2);
  }

  // Moves the content by (dx, dy) viewport pixels.
  void pan(float dx, float dy) {
    cx += dx;
    cy += dy;
    clamp();
  }

  // Zoom keeping viewport point (px, py) fixed.
  void zoom_at(float z, float px, float py) {
    z = std::min(std::max(z, min_zoom), max_zoom);
    if (z == zoom) return;
    float r = z / zoom;
    cx = px + (cx - px) * r;
    cy = py + (cy - py) * r;
    zoom = z;
    clamp();
  }

  bool fits_w() const { return cw * zoom <= vw + 0.5f; }
  bool fits_h() const { return ch * zoom <= vh + 0.5f; }
  bool at_top() const { return fits_h() || cy >= ch * zoom / 2 - 0.5f; }
  bool at_bottom() const { return fits_h() || cy <= vh - ch * zoom / 2 + 0.5f; }
  bool at_left() const { return fits_w() || cx >= cw * zoom / 2 - 0.5f; }
  bool at_right() const { return fits_w() || cx <= vw - cw * zoom / 2 + 0.5f; }
  bool zoomed() const { return std::fabs(zoom - fit_zoom) > fit_zoom * 0.01f; }

  SDL_Rect rect() const {
    float w = cw * zoom, h = ch * zoom;
    float x0 = vx + cx - w / 2, y0 = vy + cy - h / 2;
    SDL_Rect r = {(int)lroundf(x0), (int)lroundf(y0), 0, 0};
    r.w = (int)lroundf(x0 + w) - r.x;
    r.h = (int)lroundf(y0 + h) - r.y;
    return r;
  }
};

#endif
