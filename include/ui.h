#ifndef WOOK_READER_UI_H
#define WOOK_READER_UI_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>

#include <string>
#include <vector>

// ── Theme ────────────────────────────────────────────────────────────────────
struct Theme {
  SDL_Color bg, surface, surface2, border, text, dim, accent, on_accent,
      manga, book, scrim, page_bg, page_fg;
};
const Theme &UI_Theme();  // follows g_settings.dark_mode

// ── Rotating canvas ──────────────────────────────────────────────────────────
// Everything is drawn into a logical canvas (1280x720 or 720x1280) which is
// then blitted to the panel rotated by 0/90/270 degrees.
void Screen_Init();
void Screen_Shutdown();
void Screen_Begin(int rotation);  // binds the canvas for this frame
void Screen_End();                // blits rotated + presents
int Screen_W();
int Screen_H();
int Screen_Rotation();
void Screen_ToLogical(float px, float py, float *lx, float *ly);
void Screen_VecToLogical(float dx, float dy, float *lx, float *ly);  // y-down

// ── Input helpers ────────────────────────────────────────────────────────────
enum { DIR_LEFT = 1, DIR_RIGHT = 2, DIR_UP = 4, DIR_DOWN = 8 };
int Input_PhysDirs(u64 buttons);       // D-pad + left stick digital bits
int Input_ToLogical(int phys_dirs);    // rotate by current screen rotation

// D-pad auto-repeat in logical directions.
struct KeyRepeat {
  int held = 0;
  uint32_t next = 0;
  int update(u64 kDown, u64 kHeld);  // returns logical dirs that fire now
};

// Touch state in logical canvas coordinates.
struct TouchTracker {
  int count = 0, prev_count = 0;
  float x = 0, y = 0, prev_x = 0, prev_y = 0;    // finger 0
  float x1 = 0, y1 = 0, prev_x1 = 0, prev_y1 = 0;  // finger 1
  float start_x = 0, start_y = 0;
  bool dragging = false;   // moved past the slop since touch-down
  bool multi = false;      // a second finger was used during this gesture
  bool began = false;      // finger went down this frame
  bool tapped = false;     // released this frame without dragging
  bool released = false;   // released this frame (any gesture)
  uint32_t down_at = 0;
  float vel_y = 0, vel_x = 0;  // last-frame motion, for flings
  void update();
};

// ── Drawing ──────────────────────────────────────────────────────────────────
void UI_Fill(int x, int y, int w, int h, SDL_Color c);
void UI_RoundRect(int x, int y, int w, int h, int r, SDL_Color c);
void UI_RoundOutline(int x, int y, int w, int h, int r, int thick, SDL_Color c);
void UI_Circle(int cx, int cy, int r, SDL_Color c);

int UI_TextW(TTF_Font *f, const std::string &s);
int UI_TextH(TTF_Font *f);
void UI_Text(TTF_Font *f, int x, int y, SDL_Color c, const std::string &s);
void UI_TextC(TTF_Font *f, int cx, int y, SDL_Color c, const std::string &s);  // centred
void UI_TextR(TTF_Font *f, int rx, int y, SDL_Color c, const std::string &s);  // right-aligned
std::string UI_Ellipsize(TTF_Font *f, const std::string &s, int max_w);
// Wraps into at most max_lines lines (last one ellipsized). Returns lines.
std::vector<std::string> UI_Wrap(TTF_Font *f, const std::string &s, int max_w, int max_lines);

// Small coloured label; returns its width.
int UI_Pill(int x, int y, TTF_Font *f, const std::string &s, SDL_Color bg, SDL_Color fg);
// Controller button glyph ("A", "B", "ZL", "+", ...); returns width.
int UI_ButtonGlyph(int x, int cy, const char *btn);
int UI_ButtonGlyphW(const char *btn);
// Glyph + label hint; returns total width.
int UI_Hint(int x, int cy, const char *btn, const char *label, SDL_Color fg);
int UI_HintW(const char *btn, const char *label);

// Battery glyph + "87%"; returns width. h is the glyph height.
int UI_Battery(int x, int cy, SDL_Color fg, TTF_Font *f, bool with_text);
int UI_BatteryW(TTF_Font *f, bool with_text);
int UI_BatteryPercent(bool *charging);
std::string UI_Clock();

// ── List menu (reader menu, dialogs) ─────────────────────────────────────────
struct MenuItem {
  std::string label;
  std::string value;     // empty = plain action
  std::string hint;      // optional description under the label
  bool enabled = true;
};

struct MenuResult {
  int activated = -1;  // item index
  int delta = 0;       // +1 / -1 for value changes (A = +1)
  bool closed = false;
};

class ListMenu {
 public:
  std::string title;
  std::string subtitle;
  std::vector<MenuItem> items;
  int sel = 0;
  int min_w = 520;

  void draw();
  MenuResult input(int dirs, u64 kDown, const TouchTracker &t);

 private:
  SDL_Rect _panel = {0, 0, 0, 0};
  int _rows_y = 0, _row_h = 64;
  void clamp_sel(int step);
};

#endif
