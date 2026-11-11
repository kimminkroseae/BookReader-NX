#ifndef WOOK_READER_SETTINGS_H
#define WOOK_READER_SETTINGS_H

enum TopBarMode { TOPBAR_HIDDEN = 0, TOPBAR_TAP = 1, TOPBAR_ALWAYS = 2 };
enum IndicatorPos { INDICATOR_TOP = 0, INDICATOR_BOTTOM = 1 };
// Content rotation applied to the screen (degrees clockwise).
enum OrientationMode {
  ORIENT_AUTO = 0,
  ORIENT_LANDSCAPE = 1,
  ORIENT_PORTRAIT_R_TOP = 2,  // rotate 90  (right Joy-Con on top)
  ORIENT_PORTRAIT_L_TOP = 3   // rotate 270 (left Joy-Con on top)
};
enum FitMode { FIT_PAGE = 0, FIT_WIDTH = 1 };

struct Settings {
  // Display
  bool dark_mode = true;
  int grid_cols = 5;  // library columns in landscape (3..8)

  // Reader
  int top_bar = TOPBAR_TAP;
  bool indicator = true;
  int indicator_pos = INDICATOR_TOP;
  bool indicator_clock = false;
  int fit_mode = FIT_PAGE;
  bool screen_buttons = false;
  int text_size = 1;  // reflowable books: 0 small .. 3 x-large

  // Orientation
  int orientation = ORIENT_AUTO;
  bool rotate_reverse = false;

  // Manga / dark mode
  bool ask_type = true;
  bool manga_rtl = false;

  // Controls
  int scroll_speed = 2;  // 1..4
  int zoom_step = 2;     // 1..4
  bool swipe_turn = true;
  bool tap_zones = true;
};

extern Settings g_settings;

void Settings_Load();
void Settings_Save();

#endif
