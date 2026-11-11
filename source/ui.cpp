#include "ui.h"

#include <SDL2/SDL2_gfxPrimitives.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <vector>

#include "settings.h"

extern "C" {
#include "SDL_helper.h"
#include "common.h"
}

static inline SDL_Color C(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255) {
  SDL_Color c = {r, g, b, a};
  return c;
}

// ── Theme ────────────────────────────────────────────────────────────────────

const Theme &UI_Theme() {
  static const Theme dark = {
      C(14, 15, 18),     // bg
      C(30, 32, 37),     // surface
      C(44, 47, 54),     // surface2
      C(62, 66, 75),     // border
      C(236, 238, 241),  // text
      C(150, 156, 166),  // dim
      C(92, 160, 255),   // accent
      C(10, 20, 40),     // on_accent
      C(255, 128, 96),   // manga
      C(96, 204, 150),   // book
      C(0, 0, 0, 170),   // scrim
      C(0, 0, 0),        // page_bg
      C(255, 255, 255),  // page_fg
  };
  static const Theme light = {
      C(242, 243, 246),  C(255, 255, 255), C(228, 231, 236), C(206, 210, 218),
      C(20, 22, 27),     C(96, 102, 112),  C(36, 99, 235),   C(255, 255, 255),
      C(222, 84, 52),    C(22, 150, 92),   C(0, 0, 0, 120),  C(255, 255, 255),
      C(0, 0, 0),
  };
  return g_settings.dark_mode ? dark : light;
}

// ── Canvas ───────────────────────────────────────────────────────────────────

static SDL_Texture *s_land = nullptr, *s_port = nullptr;
static int s_rot = 0;
static int s_w = 1280, s_h = 720;

void Screen_Init() {
  s_land = SDL_CreateTexture(RENDERER, SDL_PIXELFORMAT_RGBA8888,
                             SDL_TEXTUREACCESS_TARGET, 1280, 720);
  s_port = SDL_CreateTexture(RENDERER, SDL_PIXELFORMAT_RGBA8888,
                             SDL_TEXTUREACCESS_TARGET, 720, 1280);
  if (s_land) SDL_SetTextureBlendMode(s_land, SDL_BLENDMODE_NONE);
  if (s_port) SDL_SetTextureBlendMode(s_port, SDL_BLENDMODE_NONE);
}

void Screen_Shutdown() {
  if (s_land) SDL_DestroyTexture(s_land);
  if (s_port) SDL_DestroyTexture(s_port);
  s_land = s_port = nullptr;
}

// Switching between the canvas and the screen must never inherit a stale
// scale / viewport / clip (e.g. from SDL's logical-size handling), otherwise
// content can shrink a little more on every rotation.
static void reset_render_state() {
  SDL_RenderSetLogicalSize(RENDERER, 0, 0);
  SDL_RenderSetScale(RENDERER, 1.0f, 1.0f);
  SDL_RenderSetViewport(RENDERER, nullptr);
  SDL_RenderSetClipRect(RENDERER, nullptr);
}

void Screen_Begin(int rotation) {
  // Without render-target support we can only draw unrotated.
  if (rotation != 0 && !s_port) rotation = 0;
  s_rot = rotation;
  s_w = rotation ? 720 : 1280;
  s_h = rotation ? 1280 : 720;
  SDL_SetRenderTarget(RENDERER, rotation ? s_port : s_land);
  reset_render_state();
  SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_BLEND);
}

void Screen_End() {
  SDL_Texture *canvas = s_rot ? s_port : s_land;
  if (canvas) {
    SDL_SetRenderTarget(RENDERER, nullptr);
    reset_render_state();
    SDL_SetRenderDrawColor(RENDERER, 0, 0, 0, 255);
    SDL_RenderClear(RENDERER);
    if (s_rot == 0) {
      SDL_RenderCopy(RENDERER, canvas, nullptr, nullptr);
    } else {
      SDL_Rect dst = {(1280 - 720) / 2, (720 - 1280) / 2, 720, 1280};
      SDL_RenderCopyEx(RENDERER, canvas, nullptr, &dst, (double)s_rot, nullptr,
                       SDL_FLIP_NONE);
    }
  }
  SDL_RenderPresent(RENDERER);
}

int Screen_W() { return s_w; }
int Screen_H() { return s_h; }
int Screen_Rotation() { return s_rot; }

void Screen_ToLogical(float px, float py, float *lx, float *ly) {
  switch (s_rot) {
    case 90: *lx = py; *ly = 1280.0f - px; break;
    case 270: *lx = 720.0f - py; *ly = px; break;
    default: *lx = px; *ly = py; break;
  }
}

void Screen_VecToLogical(float dx, float dy, float *lx, float *ly) {
  switch (s_rot) {
    case 90: *lx = dy; *ly = -dx; break;
    case 270: *lx = -dy; *ly = dx; break;
    default: *lx = dx; *ly = dy; break;
  }
}

// ── Input ────────────────────────────────────────────────────────────────────

int Input_PhysDirs(u64 b) {
  int d = 0;
  if (b & (HidNpadButton_Left | HidNpadButton_StickLLeft)) d |= DIR_LEFT;
  if (b & (HidNpadButton_Right | HidNpadButton_StickLRight)) d |= DIR_RIGHT;
  if (b & (HidNpadButton_Up | HidNpadButton_StickLUp)) d |= DIR_UP;
  if (b & (HidNpadButton_Down | HidNpadButton_StickLDown)) d |= DIR_DOWN;
  return d;
}

int Input_ToLogical(int p) {
  if (s_rot == 0) return p;
  int out = 0;
  auto map1 = [&](int bit, float dx, float dy) {
    if (!(p & bit)) return;
    float lx, ly;
    Screen_VecToLogical(dx, dy, &lx, &ly);
    if (lx < -0.5f) out |= DIR_LEFT;
    if (lx > 0.5f) out |= DIR_RIGHT;
    if (ly < -0.5f) out |= DIR_UP;
    if (ly > 0.5f) out |= DIR_DOWN;
  };
  map1(DIR_LEFT, -1, 0);
  map1(DIR_RIGHT, 1, 0);
  map1(DIR_UP, 0, -1);
  map1(DIR_DOWN, 0, 1);
  return out;
}

int KeyRepeat::update(u64 kDown, u64 kHeld) {
  int down = Input_ToLogical(Input_PhysDirs(kDown));
  int hold = Input_ToLogical(Input_PhysDirs(kHeld));
  uint32_t now = SDL_GetTicks();
  if (down) {
    held = down;
    next = now + 380;
    return down;
  }
  if (!(hold & held)) {
    held = 0;
    return 0;
  }
  if (now >= next) {
    next = now + 85;
    return held & hold;
  }
  return 0;
}

void TouchTracker::update() {
  HidTouchScreenState ts = {0};
  hidGetTouchScreenStates(&ts, 1);
  prev_count = count;
  count = (int)ts.count;
  began = tapped = released = false;

  if (count >= 1) {
    float lx, ly;
    Screen_ToLogical((float)ts.touches[0].x, (float)ts.touches[0].y, &lx, &ly);
    if (prev_count == 0) {
      start_x = prev_x = x = lx;
      start_y = prev_y = y = ly;
      dragging = false;
      multi = false;
      began = true;
      down_at = SDL_GetTicks();
      vel_x = vel_y = 0;
    } else {
      prev_x = x;
      prev_y = y;
      x = lx;
      y = ly;
      vel_x = x - prev_x;
      vel_y = y - prev_y;
      float mx = x - start_x, my = y - start_y;
      if (mx * mx + my * my > 18.0f * 18.0f) dragging = true;
    }
  }
  if (count >= 2) {
    float lx, ly;
    Screen_ToLogical((float)ts.touches[1].x, (float)ts.touches[1].y, &lx, &ly);
    if (prev_count < 2) {
      prev_x1 = x1 = lx;
      prev_y1 = y1 = ly;
    } else {
      prev_x1 = x1;
      prev_y1 = y1;
      x1 = lx;
      y1 = ly;
    }
    multi = true;
    dragging = true;
  }
  if (count == 0 && prev_count > 0) {
    released = true;
    tapped = !dragging && !multi;
  }
}

// ── Drawing ──────────────────────────────────────────────────────────────────

void UI_Fill(int x, int y, int w, int h, SDL_Color c) {
  SDL_SetRenderDrawBlendMode(RENDERER, c.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(RENDERER, c.r, c.g, c.b, c.a);
  SDL_Rect r = {x, y, w, h};
  SDL_RenderFillRect(RENDERER, &r);
  SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_BLEND);
}

static bool offscreen(int x, int y, int w, int h) {
  return x + w < 0 || y + h < 0 || x > s_w || y > s_h || w <= 0 || h <= 0;
}

// Horizontal inset of a rounded corner on pixel row `row` (0 = outermost).
static int corner_inset(int r, int row) {
  if (r <= 0 || row >= r) return 0;
  float dy = r - row - 0.5f;
  float dx = sqrtf(std::max(0.0f, (float)r * r - dy * dy));
  return (int)lroundf(r - dx);
}

static void begin_fill(SDL_Color c) {
  SDL_SetRenderDrawBlendMode(RENDERER, c.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(RENDERER, c.r, c.g, c.b, c.a);
}

// Scanline rounded rectangle: exact for any size, including thin pills.
void UI_RoundRect(int x, int y, int w, int h, int r, SDL_Color c) {
  if (offscreen(x, y, w, h)) return;
  r = std::min(r, std::min(w, h) / 2);
  if (r <= 0) {
    UI_Fill(x, y, w, h, c);
    return;
  }
  std::vector<SDL_Rect> rows;
  rows.reserve(2 * r + 1);
  for (int i = 0; i < r; i++) {
    int in = corner_inset(r, i);
    rows.push_back({x + in, y + i, w - 2 * in, 1});
    rows.push_back({x + in, y + h - 1 - i, w - 2 * in, 1});
  }
  if (h - 2 * r > 0) rows.push_back({x, y + r, w, h - 2 * r});
  begin_fill(c);
  SDL_RenderFillRects(RENDERER, rows.data(), (int)rows.size());
  SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_BLEND);
}

// Rounded ring of the given thickness (no overlapping pixels, so alpha is even).
void UI_RoundOutline(int x, int y, int w, int h, int r, int thick, SDL_Color c) {
  if (offscreen(x, y, w, h) || thick <= 0) return;
  r = std::min(r, std::min(w, h) / 2);
  int ix = x + thick, iy = y + thick, iw = w - 2 * thick, ih = h - 2 * thick;
  int ir = std::max(0, r - thick);
  std::vector<SDL_Rect> rows;
  rows.reserve(2 * h);
  for (int row = 0; row < h; row++) {
    int from_edge = std::min(row, h - 1 - row);
    int oin = corner_inset(r, from_edge);
    int ox0 = x + oin, ox1 = x + w - oin;
    int yy = y + row;
    int irow = yy - iy;
    if (iw <= 0 || ih <= 0 || irow < 0 || irow >= ih) {
      rows.push_back({ox0, yy, ox1 - ox0, 1});
      continue;
    }
    int iin = corner_inset(ir, std::min(irow, ih - 1 - irow));
    int ix0 = ix + iin, ix1 = ix + iw - iin;
    if (ix0 > ox0) rows.push_back({ox0, yy, ix0 - ox0, 1});
    if (ox1 > ix1) rows.push_back({ix1, yy, ox1 - ix1, 1});
  }
  begin_fill(c);
  SDL_RenderFillRects(RENDERER, rows.data(), (int)rows.size());
  SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_BLEND);
}

void UI_Circle(int cx, int cy, int r, SDL_Color c) {
  filledCircleRGBA(RENDERER, cx, cy, r, c.r, c.g, c.b, c.a);
  aacircleRGBA(RENDERER, cx, cy, r, c.r, c.g, c.b, c.a);
  SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_BLEND);
}

int UI_TextW(TTF_Font *f, const std::string &s) {
  if (!f || s.empty()) return 0;
  int w = 0;
  TTF_SizeUTF8(f, s.c_str(), &w, nullptr);
  return w;
}

int UI_TextH(TTF_Font *f) { return f ? TTF_FontHeight(f) : 0; }

void UI_Text(TTF_Font *f, int x, int y, SDL_Color c, const std::string &s) {
  if (!f || s.empty()) return;
  SDL_DrawText(RENDERER, f, x, y, c, s.c_str());
}

void UI_TextC(TTF_Font *f, int cx, int y, SDL_Color c, const std::string &s) {
  UI_Text(f, cx - UI_TextW(f, s) / 2, y, c, s);
}

void UI_TextR(TTF_Font *f, int rx, int y, SDL_Color c, const std::string &s) {
  UI_Text(f, rx - UI_TextW(f, s), y, c, s);
}

std::string UI_Ellipsize(TTF_Font *f, const std::string &s, int max_w) {
  static std::unordered_map<std::string, std::string> cache;
  if (UI_TextW(f, s) <= max_w) return s;
  std::string key = std::to_string((uintptr_t)f) + "|" + std::to_string(max_w) + "|" + s;
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  if (cache.size() > 4000) cache.clear();

  static const std::string ell = "\xE2\x80\xA6";  // …
  // Binary search over UTF-8 character boundaries for the longest prefix.
  std::vector<size_t> bounds;
  for (size_t i = 0; i < s.size(); i++)
    if (((unsigned char)s[i] & 0xC0) != 0x80) bounds.push_back(i);
  int lo = 0, hi = (int)bounds.size() - 1;  // candidate = prefix ending at bounds[k]
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (UI_TextW(f, s.substr(0, bounds[mid]) + ell) <= max_w)
      lo = mid;
    else
      hi = mid - 1;
  }
  std::string out = bounds.empty() ? "" : s.substr(0, bounds[lo]);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  out += ell;
  cache[key] = out;
  return out;
}

std::vector<std::string> UI_Wrap(TTF_Font *f, const std::string &s, int max_w, int max_lines) {
  std::vector<std::string> lines;
  std::string rest = s;
  while (!rest.empty() && (int)lines.size() < max_lines) {
    if ((int)lines.size() == max_lines - 1 || UI_TextW(f, rest) <= max_w) {
      lines.push_back(UI_Ellipsize(f, rest, max_w));
      break;
    }
    // longest prefix that fits, preferring a break at a space
    size_t fit = 0;
    for (size_t i = 1; i <= rest.size(); i++) {
      if (i < rest.size() && ((unsigned char)rest[i] & 0xC0) == 0x80) continue;
      if (UI_TextW(f, rest.substr(0, i)) > max_w) break;
      fit = i;
    }
    if (fit == 0) fit = 1;
    size_t brk = rest.rfind(' ', fit);
    if (brk != std::string::npos && brk > fit / 2 && fit < rest.size()) fit = brk + 1;
    std::string line = rest.substr(0, fit);
    while (!line.empty() && line.back() == ' ') line.pop_back();
    lines.push_back(line);
    rest = rest.substr(fit);
  }
  return lines;
}

int UI_Pill(int x, int y, TTF_Font *f, const std::string &s, SDL_Color bg, SDL_Color fg) {
  int th = UI_TextH(f);
  int w = UI_TextW(f, s) + 16;
  UI_RoundRect(x, y, w, th + 4, (th + 4) / 2, bg);
  UI_Text(f, x + 8, y + 2, fg, s);
  return w;
}

static const int GLYPH_D = 30;  // round button diameter

int UI_ButtonGlyphW(const char *btn) {
  if (strlen(btn) <= 1) return GLYPH_D;
  return UI_TextW(FONT_16, btn) + 18;
}

int UI_ButtonGlyph(int x, int cy, const char *btn) {
  const Theme &t = UI_Theme();
  SDL_Color bg = t.text, fg = t.bg;
  int w = UI_ButtonGlyphW(btn);
  if (strlen(btn) <= 1) {
    UI_Circle(x + GLYPH_D / 2, cy, GLYPH_D / 2, bg);
    UI_TextC(FONT_18, x + GLYPH_D / 2, cy - UI_TextH(FONT_18) / 2, fg, btn);
  } else {
    UI_RoundRect(x, cy - GLYPH_D / 2 + 2, w, GLYPH_D - 4, (GLYPH_D - 4) / 2, bg);
    UI_TextC(FONT_16, x + w / 2, cy - UI_TextH(FONT_16) / 2, fg, btn);
  }
  return w;
}

int UI_HintW(const char *btn, const char *label) {
  return UI_ButtonGlyphW(btn) + 8 + UI_TextW(FONT_20, label);
}

int UI_Hint(int x, int cy, const char *btn, const char *label, SDL_Color fg) {
  int gw = UI_ButtonGlyph(x, cy, btn);
  UI_Text(FONT_20, x + gw + 8, cy - UI_TextH(FONT_20) / 2, fg, label);
  return gw + 8 + UI_TextW(FONT_20, label);
}

int UI_BatteryPercent(bool *charging) {
  static uint32_t last = 0;
  static u32 pct = 100;
  static bool chg = false, valid = false;
  uint32_t now = SDL_GetTicks();
  if (!valid || now - last > 5000) {
    last = now;
    u32 p;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&p))) {
      pct = p;
      valid = true;
    }
    PsmChargerType ct;
    chg = R_SUCCEEDED(psmGetChargerType(&ct)) && ct != PsmChargerType_Unconnected;
  }
  if (charging) *charging = chg;
  return (int)pct;
}

static const int BAT_W = 26, BAT_H = 13;

int UI_BatteryW(TTF_Font *f, bool with_text) {
  int w = BAT_W + 3;
  if (with_text) w += 6 + UI_TextW(f, std::to_string(UI_BatteryPercent(nullptr)) + "%");
  return w;
}

int UI_Battery(int x, int cy, SDL_Color fg, TTF_Font *f, bool with_text) {
  bool chg = false;
  int pct = UI_BatteryPercent(&chg);
  int y = cy - BAT_H / 2;
  // body outline (2px) + nub
  UI_Fill(x, y, BAT_W, 2, fg);
  UI_Fill(x, y + BAT_H - 2, BAT_W, 2, fg);
  UI_Fill(x, y, 2, BAT_H, fg);
  UI_Fill(x + BAT_W - 2, y, 2, BAT_H, fg);
  UI_Fill(x + BAT_W, y + 4, 3, BAT_H - 8, fg);
  int inner = (BAT_W - 6) * pct / 100;
  SDL_Color fill = fg;
  if (chg) fill = UI_Theme().book;
  else if (pct <= 15) fill = C(235, 70, 60);
  if (inner > 0) UI_Fill(x + 3, y + 3, inner, BAT_H - 6, fill);
  int w = BAT_W + 3;
  if (with_text) {
    std::string s = std::to_string(pct) + "%";
    UI_Text(f, x + w + 6, cy - UI_TextH(f) / 2, fg, s);
    w += 6 + UI_TextW(f, s);
  }
  return w;
}

std::string UI_Clock() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  if (!t) return "";
  int h = t->tm_hour % 12;
  if (h == 0) h = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d %s", h, t->tm_min, t->tm_hour < 12 ? "AM" : "PM");
  return buf;
}

// ── ListMenu ─────────────────────────────────────────────────────────────────

void ListMenu::clamp_sel(int step) {
  int n = (int)items.size();
  if (n == 0) return;
  for (int i = 0; i < n; i++) {
    sel = (sel + n) % n;
    if (items[sel].enabled) return;
    sel += step ? step : 1;
  }
}

void ListMenu::draw() {
  const Theme &t = UI_Theme();
  int W = Screen_W(), H = Screen_H();
  UI_Fill(0, 0, W, H, t.scrim);

  bool has_hint = false;
  for (auto &it : items)
    if (!it.hint.empty()) has_hint = true;
  _row_h = has_hint ? 72 : 60;

  int pw = std::min(W - 48, std::max(min_w, 560));
  int head = 28 + UI_TextH(FONT_28) + (subtitle.empty() ? 0 : UI_TextH(FONT_18) + 4) + 16;
  int max_rows = (H - 96 - head - 24) / _row_h;
  int rows = std::min((int)items.size(), std::max(1, max_rows));
  int ph = head + rows * _row_h + 24;
  _panel = {(W - pw) / 2, (H - ph) / 2, pw, ph};

  UI_RoundRect(_panel.x, _panel.y, pw, ph, 18, t.surface);
  UI_Text(FONT_28, _panel.x + 28, _panel.y + 24, t.text, title);
  if (!subtitle.empty())
    UI_Text(FONT_18, _panel.x + 28, _panel.y + 28 + UI_TextH(FONT_28), t.dim,
            UI_Ellipsize(FONT_18, subtitle, pw - 56));

  // scroll window keeps the selection visible
  int first = 0;
  if (sel >= rows) first = sel - rows + 1;
  _rows_y = _panel.y + head;

  for (int i = 0; i < rows && first + i < (int)items.size(); i++) {
    const MenuItem &it = items[first + i];
    int ry = _rows_y + i * _row_h;
    bool selected = (first + i == sel);
    if (selected) {
      UI_RoundRect(_panel.x + 12, ry + 2, pw - 24, _row_h - 4, 12, t.surface2);
      UI_Fill(_panel.x + 12, ry + 14, 4, _row_h - 28, t.accent);
    }
    SDL_Color fg = it.enabled ? t.text : t.dim;
    int ty = it.hint.empty() ? ry + (_row_h - UI_TextH(FONT_22)) / 2 : ry + 10;
    int value_w = it.value.empty() ? 0 : UI_TextW(FONT_22, it.value) + 60;
    UI_Text(FONT_22, _panel.x + 32, ty, fg,
            UI_Ellipsize(FONT_22, it.label, pw - 64 - value_w));
    if (!it.hint.empty())
      UI_Text(FONT_16, _panel.x + 32, ty + UI_TextH(FONT_22) + 2, t.dim,
              UI_Ellipsize(FONT_16, it.hint, pw - 64 - value_w));
    if (!it.value.empty()) {
      int vx = _panel.x + pw - 32;
      int vy = ry + (_row_h - UI_TextH(FONT_22)) / 2;
      SDL_Color vc = selected ? t.accent : t.dim;
      UI_TextR(FONT_22, vx, vy, vc, "\xE2\x80\xBA");  // ›
      int vw = UI_TextW(FONT_22, it.value);
      UI_Text(FONT_22, vx - 18 - vw, vy, selected ? t.text : t.dim, it.value);
      UI_TextR(FONT_22, vx - 26 - vw, vy, vc, "\xE2\x80\xB9");  // ‹
    }
  }
}

MenuResult ListMenu::input(int dirs, u64 kDown, const TouchTracker &t) {
  MenuResult r;
  int n = (int)items.size();
  if (n == 0) {
    if (kDown & (HidNpadButton_B | HidNpadButton_A)) r.closed = true;
    return r;
  }
  clamp_sel(1);
  if (dirs & DIR_UP) {
    sel--;
    clamp_sel(-1);
  }
  if (dirs & DIR_DOWN) {
    sel++;
    clamp_sel(1);
  }
  if ((dirs & (DIR_LEFT | DIR_RIGHT)) && !items[sel].value.empty()) {
    r.activated = sel;
    r.delta = (dirs & DIR_RIGHT) ? 1 : -1;
  }
  if (kDown & HidNpadButton_A) {
    r.activated = sel;
    r.delta = 1;
  }
  if (kDown & HidNpadButton_B) r.closed = true;

  if (t.tapped) {
    SDL_Point p = {(int)t.start_x, (int)t.start_y};
    if (!SDL_PointInRect(&p, &_panel)) {
      r.closed = true;
    } else if (p.y >= _rows_y) {
      int rows = (_panel.y + _panel.h - 24 - _rows_y) / _row_h;
      int first = sel >= rows ? sel - rows + 1 : 0;
      int idx = first + (p.y - _rows_y) / _row_h;
      if (idx >= 0 && idx < n && items[idx].enabled) {
        sel = idx;
        r.activated = idx;
        // tapping the left half of a value steps backwards
        r.delta = (!items[idx].value.empty() && p.x < _panel.x + _panel.w / 2) ? -1 : 1;
      }
    }
  }
  return r;
}
