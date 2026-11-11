#include "settings.h"

#include <libconfig.h>

#include <algorithm>

#include "paths.h"

Settings g_settings;

static void read_int(config_t *c, const char *name, int *v, int lo, int hi) {
  int tmp;
  if (config_lookup_int(c, name, &tmp) == CONFIG_TRUE)
    *v = std::clamp(tmp, lo, hi);
}

static void read_bool(config_t *c, const char *name, bool *v) {
  int tmp;
  if (config_lookup_bool(c, name, &tmp) == CONFIG_TRUE) *v = tmp != 0;
}

void Settings_Load() {
  config_t c;
  config_init(&c);
  if (config_read_file(&c, OPTIONS_FILE) == CONFIG_TRUE) {
    Settings &s = g_settings;
    read_bool(&c, "DarkMode", &s.dark_mode);
    read_int(&c, "GridColumns", &s.grid_cols, 3, 8);
    read_int(&c, "TopBar", &s.top_bar, 0, 2);
    read_bool(&c, "Indicator", &s.indicator);
    read_int(&c, "IndicatorPos", &s.indicator_pos, 0, 1);
    read_bool(&c, "IndicatorClock", &s.indicator_clock);
    read_int(&c, "FitMode", &s.fit_mode, 0, 1);
    read_bool(&c, "ScreenButtons", &s.screen_buttons);
    read_int(&c, "TextSize", &s.text_size, 0, 3);
    read_int(&c, "Orientation", &s.orientation, 0, 3);
    read_bool(&c, "RotateReverse", &s.rotate_reverse);
    read_bool(&c, "AskType", &s.ask_type);
    read_bool(&c, "MangaRTL", &s.manga_rtl);
    read_int(&c, "ScrollSpeed", &s.scroll_speed, 1, 4);
    read_int(&c, "ZoomAmount", &s.zoom_step, 1, 4);
    read_bool(&c, "SwipeTurn", &s.swipe_turn);
    read_bool(&c, "TapZones", &s.tap_zones);
  }
  config_destroy(&c);
}

// Returns the named root setting, (re)creating it with the right type.
static config_setting_t *get_or_add(config_t *c, const char *name, int type) {
  config_setting_t *root = config_root_setting(c);
  config_setting_t *s = config_setting_get_member(root, name);
  if (s && config_setting_type(s) != type) {
    config_setting_remove(root, name);
    s = nullptr;
  }
  if (!s) s = config_setting_add(root, name, type);
  return s;
}

static void write_int(config_t *c, const char *name, int v) {
  config_setting_t *s = get_or_add(c, name, CONFIG_TYPE_INT);
  if (s) config_setting_set_int(s, v);
}

static void write_bool(config_t *c, const char *name, bool v) {
  config_setting_t *s = get_or_add(c, name, CONFIG_TYPE_BOOL);
  if (s) config_setting_set_bool(s, v);
}

void Settings_Save() {
  config_t c;
  config_init(&c);
  const Settings &s = g_settings;
  write_bool(&c, "DarkMode", s.dark_mode);
  write_int(&c, "GridColumns", s.grid_cols);
  write_int(&c, "TopBar", s.top_bar);
  write_bool(&c, "Indicator", s.indicator);
  write_int(&c, "IndicatorPos", s.indicator_pos);
  write_bool(&c, "IndicatorClock", s.indicator_clock);
  write_int(&c, "FitMode", s.fit_mode);
  write_bool(&c, "ScreenButtons", s.screen_buttons);
  write_int(&c, "TextSize", s.text_size);
  write_int(&c, "Orientation", s.orientation);
  write_bool(&c, "RotateReverse", s.rotate_reverse);
  write_bool(&c, "AskType", s.ask_type);
  write_bool(&c, "MangaRTL", s.manga_rtl);
  write_int(&c, "ScrollSpeed", s.scroll_speed);
  write_int(&c, "ZoomAmount", s.zoom_step);
  write_bool(&c, "SwipeTurn", s.swipe_turn);
  write_bool(&c, "TapZones", s.tap_zones);
  config_write_file(&c, OPTIONS_FILE);
  config_destroy(&c);
}
