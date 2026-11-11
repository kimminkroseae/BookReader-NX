#ifndef WOOK_READER_BOOK_READER_HPP
#define WOOK_READER_BOOK_READER_HPP

#include <mupdf/fitz.h>
#include <switch.h>

#include <string>

#include "CBZPageLayout.hpp"
#include "PageLayout.hpp"
#include "library.h"
#include "ui.h"

class BookReader {
 public:
  BookReader(const std::string &path, bool start_at_end = false);
  ~BookReader();

  // Runs the reader until the user closes the book (or the app exits).
  void run();

  // Set when the user turned past the end/start into the next/previous book.
  const std::string &next_path() const { return _next_path; }
  bool next_at_end() const { return _next_at_end; }

 private:
  enum Overlay { OV_NONE, OV_MENU, OV_HELP, OV_NOTES, OV_TYPE };

  // setup
  bool open_document();
  void create_layout(int page);
  SDL_Rect compute_viewport() const;
  void apply_viewport();
  void relayout_reflowable();
  void reflow_size(SDL_Rect vp, float *w, float *h) const;

  // state helpers
  bool is_image_book() const { return _image_book; }
  bool want_invert() const;
  bool rtl() const;
  int page_count() const;
  int current_page() const;
  bool top_bar_visible() const;
  void apply_type(ContentType t);
  void after_page_change();

  // actions
  void turn(int dir);  // +1 next, -1 previous (logical, scrolls first)
  void edge(int dir);  // turned past the first/last page
  void go_to_page_dialog();
  void edit_notes();
  void cycle_fit();
  void toggle_dark();

  // input
  void handle_input(u64 kDown, u64 kHeld, PadState &pad);
  void handle_touch();
  void handle_overlay_input(u64 kDown, int dirs);
  void build_menu();
  void on_menu(int index, int delta);

  // drawing
  void draw();
  void draw_indicator();
  void draw_top_bar();
  void draw_bottom_bar();
  void draw_nav_buttons();
  void draw_edge_banner();
  void draw_help();
  void draw_notes();
  void draw_message(const std::string &msg);

  std::string _path;          // what the user opened
  std::string _doc_path;      // what MuPDF opens (txt files are converted)
  std::string _title;
  bool _image_book = false;   // comic archive or image folder
  fz_document *_doc = nullptr;
  bool _reflowable = false;
  PageLayout *_layout = nullptr;
  std::string _error;

  ContentType _type = CONTENT_UNKNOWN;
  bool _type_asked = false;
  int _fit = FIT_PAGE;
  bool _spread = false;
  int _rot = 0;
  SDL_Rect _vp = {0, 0, 1280, 720};
  float _reflow_w = 0, _reflow_h = 0;

  bool _bar_shown = false;  // tap/X toggled top bar
  Overlay _overlay = OV_NONE;
  ListMenu _menu;
  std::string _notes;

  TouchTracker _touch;
  KeyRepeat _keys;
  bool _slider_drag = false;
  int _slider_page = 0;
  bool _touch_on_bar = false;
  uint32_t _nav_hide_at = 0;
  bool _settings_changed = false;
  bool _exit = false;
  bool _dirty = true;
  uint32_t _last_draw = 0;
  uint32_t _last_flush = 0;
  int _last_saved_page = -1;
  bool _total_saved = false;
  int _edge_dir = 0;  // +1 at the end, -1 at the start (banner showing)
  uint32_t _edge_until = 0;
  std::string _edge_target;
  std::string _next_path;
  bool _next_at_end = false;
};

void Menu_OpenBook(const char *path);

#endif
