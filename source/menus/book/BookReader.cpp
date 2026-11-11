#include "BookReader.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "menu_book_reader.h"
#include "mupdf_ctx.h"
#include "orientation.h"
#include "paths.h"
#include "settings.h"

extern "C" {
#include "SDL_helper.h"
#include "common.h"
#include "fs.h"
}

extern void Log_Write(const std::string &msg);
extern void Log_Error(const std::string &msg);

static const int BAR_H = 72;     // top bar
static const int IND_H = 34;     // page/battery indicator strip
static const int SLIDER_H = 84;  // bottom page slider (with top bar)
static const uint32_t FLUSH_EVERY_MS = 30000;

// ── .txt support: converted once to XHTML so MuPDF can reflow it ────────────

static bool valid_utf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i];
    int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0 || (n > 0 && i + n >= s.size())) return false;
    for (int k = 1; k <= n; k++)
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    i += n + 1;
  }
  return true;
}

static std::string latin1_to_utf8(const std::string &s) {
  std::string o;
  o.reserve(s.size() * 2);
  for (unsigned char c : s) {
    if (c < 0x80) {
      o += (char)c;
    } else {
      o += (char)(0xC0 | (c >> 6));
      o += (char)(0x80 | (c & 0x3F));
    }
  }
  return o;
}

static void append_escaped(std::string &out, const std::string &s) {
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\t': out += "    "; break;
      default: out += c;
    }
  }
}

static std::string convert_txt(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return "";
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : path) { h ^= c; h *= 1099511628211ULL; }
  h ^= (uint64_t)st.st_size; h *= 1099511628211ULL;
  h ^= (uint64_t)st.st_mtime; h *= 1099511628211ULL;
  char out_path[512];
  snprintf(out_path, sizeof(out_path), "%s/%016llx.xhtml", PAGECACHE_DIR, (unsigned long long)h);
  if (FS_FileExists(out_path)) return out_path;

  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string text;
  char buf[16384];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  fclose(f);
  if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
      (unsigned char)text[2] == 0xBF)
    text.erase(0, 3);
  if (!valid_utf8(text)) text = latin1_to_utf8(text);
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());

  // Split into lines; hard-wrapped text (blank-line paragraphs) is re-joined.
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) nl = text.size();
    lines.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  int blank = 0;
  for (auto &l : lines)
    if (l.find_first_not_of(" \t") == std::string::npos) blank++;
  bool join = blank > (int)lines.size() / 20;

  std::string html =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title></title>"
      "<style>body{margin:1em;} p{margin:0 0 0.7em 0;}</style></head><body>\n";
  std::string para;
  auto flush_para = [&]() {
    if (para.empty()) return;
    html += "<p>";
    append_escaped(html, para);
    html += "</p>\n";
    para.clear();
  };
  for (auto &l : lines) {
    bool is_blank = l.find_first_not_of(" \t") == std::string::npos;
    if (is_blank) {
      flush_para();
    } else if (join) {
      if (!para.empty()) para += ' ';
      para += l;
    } else {
      para = l;
      flush_para();
    }
  }
  flush_para();
  html += "</body></html>\n";

  FS_RecursiveMakeDir(PAGECACHE_DIR);
  FILE *o = fopen(out_path, "wb");
  if (!o) return "";
  fwrite(html.data(), 1, html.size(), o);
  fclose(o);
  return out_path;
}

// ── construction ─────────────────────────────────────────────────────────────

BookReader::BookReader(const std::string &path, bool start_at_end) : _path(path) {
  while (_path.size() > 1 && _path.back() == '/') _path.pop_back();
  struct stat st;
  bool is_dir = stat(_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  _title = is_dir ? _path.substr(_path.find_last_of('/') + 1) : Library_DisplayName(_path);
  _image_book = is_dir || Library_IsComicArchive(_path);
  _fit = g_settings.fit_mode;

  _type = Library_Resolve(_path);
  if (_type == CONTENT_UNKNOWN) {
    _type_asked = g_settings.ask_type;
    _type = Library_DefaultType(_path);
  }

  Orientation_Update();
  _rot = Orientation_Rotation();
  _vp = compute_viewport();

  // Opening big EPUBs can take a moment: show feedback first.
  Screen_Begin(_rot);
  draw_message("Opening " + _title + "\xE2\x80\xA6");
  Screen_End();

  if (!open_document()) return;
  int start = Progress_GetPage(_path);
  int total = Progress_GetTotal(_path);
  if (start_at_end)
    start = 1 << 30;  // clamped to the last page once the page count is known
  else if (total > 0 && start + 1 >= total)
    start = 0;        // finished before: start over
  create_layout(start);

  if (_type_asked) {
    _overlay = OV_TYPE;
    build_menu();
  }
  _nav_hide_at = SDL_GetTicks() + 3000;
}

BookReader::~BookReader() {
  delete _layout;
  _layout = nullptr;
  if (_doc) fz_drop_document(ctx, _doc);
  _doc = nullptr;
  Progress_Flush();
  if (_settings_changed) Settings_Save();
}

void BookReader::reflow_size(SDL_Rect vp, float *w, float *h) const {
  static const float px[] = {20.0f, 24.0f, 29.0f, 35.0f};  // on-screen body text size
  float k = 12.0f / px[std::min(std::max(g_settings.text_size, 0), 3)];
  *w = vp.w * k;
  *h = vp.h * k;
}

bool BookReader::open_document() {
  if (_image_book) return true;

  _doc_path = _path;
  if (Library_Ext(_path) == ".txt") {
    _doc_path = convert_txt(_path);
    if (_doc_path.empty()) {
      _error = "Could not read this text file.";
      return false;
    }
  }
  if (!MuPDF_Context()) {
    _error = "The document engine failed to start.";
    return false;
  }
  fz_try(ctx) {
    _doc = fz_open_document(ctx, _doc_path.c_str());
    if (fz_needs_password(ctx, _doc)) fz_throw(ctx, FZ_ERROR_GENERIC, "password");
    _reflowable = fz_is_document_reflowable(ctx, _doc) != 0;
    if (_reflowable) {
      reflow_size(_vp, &_reflow_w, &_reflow_h);
      fz_layout_document(ctx, _doc, _reflow_w, _reflow_h, 12);
    }
  }
  fz_catch(ctx) {
    bool pw = _doc && fz_needs_password(ctx, _doc);
    if (_doc) fz_drop_document(ctx, _doc);
    _doc = nullptr;
    _error = pw ? "Password-protected documents are not supported."
                : "This file could not be opened. It may be damaged.";
    Log_Error("BookReader: open failed: " + _path);
    return false;
  }
  return true;
}

void BookReader::create_layout(int page) {
  delete _layout;
  _layout = nullptr;
  if (_image_book) {
    _layout = new CBZPageLayout(_path.c_str(), page, _vp, _fit, want_invert(), _spread, rtl());
    return;
  }
  _layout = new PageLayout(_doc, page, _vp, _fit, want_invert());
  if (_layout->page_count() <= 0) {
    _error = "This document has no pages.";
    return;
  }
  _layout->set_bg_path(_doc_path.c_str(), _reflowable ? _reflow_w : 0, _reflow_h, 12);
  Progress_SetTotal(_path, _layout->page_count());
}

// ── geometry ─────────────────────────────────────────────────────────────────

bool BookReader::top_bar_visible() const {
  if (g_settings.top_bar == TOPBAR_ALWAYS) return true;
  if (g_settings.top_bar == TOPBAR_HIDDEN) return false;
  return _bar_shown;
}

SDL_Rect BookReader::compute_viewport() const {
  int W = _rot ? 720 : 1280, H = _rot ? 1280 : 720;
  int top = 0, bottom = 0;
  if (g_settings.top_bar == TOPBAR_ALWAYS) {
    top = BAR_H;
  } else if (g_settings.indicator) {
    // Reserved strip: the page never covers the indicator.
    if (g_settings.indicator_pos == INDICATOR_TOP)
      top = IND_H;
    else
      bottom = IND_H;
  }
  SDL_Rect r = {0, top, W, H - top - bottom};
  return r;
}

void BookReader::relayout_reflowable() {
  if (!_doc || !_reflowable) return;
  float w, h;
  reflow_size(_vp, &w, &h);
  if (std::fabs(w - _reflow_w) < 0.5f && std::fabs(h - _reflow_h) < 0.5f) {
    if (_layout) _layout->set_viewport(_vp);
    return;
  }
  int page = _layout ? _layout->current_page() : 0;
  delete _layout;  // stops background threads before the document changes
  _layout = nullptr;
  fz_try(ctx) {
    fz_bookmark mark = fz_make_bookmark(ctx, _doc, fz_location_from_page_number(ctx, _doc, page));
    fz_layout_document(ctx, _doc, w, h, 12);
    page = fz_page_number_from_location(ctx, _doc, fz_lookup_bookmark(ctx, _doc, mark));
  }
  fz_catch(ctx) { page = 0; }
  _reflow_w = w;
  _reflow_h = h;
  create_layout(page);
  after_page_change();
}

void BookReader::apply_viewport() {
  _vp = compute_viewport();
  if (_reflowable)
    relayout_reflowable();
  else if (_layout)
    _layout->set_viewport(_vp);
  _dirty = true;
  if (_layout) {
    const PageView &v = _layout->view();
    SDL_Rect sdl_vp;
    float sx = 0, sy = 0;
    SDL_RenderGetViewport(RENDERER, &sdl_vp);
    SDL_RenderGetScale(RENDERER, &sx, &sy);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "rotate: rot=%d vp=%dx%d+%d+%d content=%.0fx%.0f zoom=%.4f fit=%d shown=%dx%d | sdl vp=%dx%d scale=%.3f,%.3f",
             _rot, _vp.w, _vp.h, _vp.x, _vp.y, v.cw, v.ch, v.zoom, v.fit, (int)(v.cw * v.zoom),
             (int)(v.ch * v.zoom), sdl_vp.w, sdl_vp.h, sx, sy);
    Log_Write(buf);
  }
}

// ── state ────────────────────────────────────────────────────────────────────

bool BookReader::want_invert() const { return g_settings.dark_mode && _type == CONTENT_BOOK; }

bool BookReader::rtl() const { return _type == CONTENT_MANGA && g_settings.manga_rtl; }

int BookReader::page_count() const { return _layout ? _layout->page_count() : 0; }
int BookReader::current_page() const { return _layout ? _layout->current_page() : 0; }

void BookReader::apply_type(ContentType t) {
  _type = t;
  if (_layout) _layout->set_invert(want_invert());
  if (_image_book && _layout) static_cast<CBZPageLayout *>(_layout)->set_rtl(rtl());
  _dirty = true;
}

void BookReader::after_page_change() {
  if (!_layout) return;
  int p = _layout->current_page();
  if (p != _last_saved_page) {
    Progress_SetPage(_path, p);
    _last_saved_page = p;
  }
  _dirty = true;
}

// ── actions ──────────────────────────────────────────────────────────────────

void BookReader::turn(int dir) {
  if (!_layout) return;
  PageView &v = _layout->view();
  float step = v.vh * 0.85f;
  if (dir > 0) {
    if (!v.at_bottom()) {
      _layout->pan(0, -step);  // more of this page below: scroll first
    } else if (current_page() + _layout->pages_shown() <= page_count() - 1 ||
               (_image_book && static_cast<CBZPageLayout *>(_layout)->is_enumerating())) {
      _layout->next_page(1);
      after_page_change();
    } else {
      edge(+1);
    }
  } else {
    if (!v.at_top()) {
      _layout->pan(0, step);
    } else if (current_page() > 0) {
      _layout->previous_page(1);
      // Going back lands at the bottom of a tall page.
      if (!_layout->view().fits_h()) _layout->reset_view(ANCHOR_BOTTOM);
      after_page_change();
    } else {
      edge(-1);
    }
  }
  _dirty = true;
}

// Turning past the last (or first) page: the first press shows where the next
// press goes, the second press opens the next (or previous) chapter.
void BookReader::edge(int dir) {
  uint32_t now = SDL_GetTicks();
  if (_edge_dir == dir && now < _edge_until && !_edge_target.empty()) {
    _next_path = _edge_target;
    _next_at_end = dir < 0;
    _exit = true;
    return;
  }
  _edge_dir = dir;
  _edge_until = now + 3500;
  _edge_target = Library_Sibling(_path, dir);
  _dirty = true;
}

void BookReader::go_to_page_dialog() {
  if (!_layout || page_count() <= 0) return;
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return;
  char header[64];
  snprintf(header, sizeof(header), "Go to page (1 - %d)", page_count());
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetType(&kbd, SwkbdType_NumPad);
  swkbdConfigSetHeaderText(&kbd, header);
  swkbdConfigSetStringLenMax(&kbd, 6);
  swkbdConfigSetStringLenMin(&kbd, 1);
  swkbdConfigSetBlurBackground(&kbd, 1);
  char buf[16] = {0};
  Result rc = swkbdShow(&kbd, buf, sizeof(buf));
  swkbdClose(&kbd);
  if (R_SUCCEEDED(rc) && buf[0]) {
    _layout->goto_page(atoi(buf) - 1);
    after_page_change();
  }
}

void BookReader::edit_notes() {
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return;
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetType(&kbd, SwkbdType_Normal);
  swkbdConfigSetHeaderText(&kbd, "Notes");
  swkbdConfigSetStringLenMax(&kbd, 2000);
  swkbdConfigSetStringLenMin(&kbd, 0);
  swkbdConfigSetBlurBackground(&kbd, 1);
  swkbdConfigSetInitialText(&kbd, _notes.c_str());
  std::vector<char> buf(8192, 0);
  Result rc = swkbdShow(&kbd, buf.data(), buf.size());
  swkbdClose(&kbd);
  if (R_SUCCEEDED(rc)) {
    _notes = buf.data();
    Notes_Write(_path, _notes);
  }
}

void BookReader::cycle_fit() {
  if (!_layout) return;
  // Fit page -> Fit width -> (image books) Two-page spread -> Fit page
  if (_spread) {
    _spread = false;
    _fit = FIT_PAGE;
    static_cast<CBZPageLayout *>(_layout)->set_spread(false);
    _layout->set_fit(_fit);
  } else if (_fit == FIT_PAGE) {
    _fit = FIT_WIDTH;
    _layout->set_fit(_fit);
  } else if (_image_book) {
    _fit = FIT_PAGE;
    _spread = true;
    _layout->set_fit(_fit);
    static_cast<CBZPageLayout *>(_layout)->set_spread(true);
  } else {
    _fit = FIT_PAGE;
    _layout->set_fit(_fit);
  }
  _dirty = true;
}

void BookReader::toggle_dark() {
  g_settings.dark_mode = !g_settings.dark_mode;
  _settings_changed = true;
  if (_layout) _layout->set_invert(want_invert());
  _dirty = true;
}

// ── menu ─────────────────────────────────────────────────────────────────────

static const char *ORIENT_NAMES[] = {"Auto", "Landscape", "Portrait (R on top)",
                                     "Portrait (L on top)"};
static const char *TOPBAR_NAMES[] = {"Always hidden", "Tap to show", "Always shown"};

void BookReader::build_menu() {
  _menu.items.clear();
  _menu.subtitle = _title;
  if (_overlay == OV_TYPE) {
    _menu.title = "Manga or book?";
    _menu.subtitle = "Choose how \"" + _title + "\" looks in dark mode";
    _menu.min_w = 640;
    _menu.items = {
        {"Manga / comic", "", "Keeps the artwork's colours, black around the page"},
        {"Book / text", "", "Inverts white paper to black for comfortable reading"},
        {"Whole folder is manga", "", "Applies to every file in this folder"},
        {"Whole folder is books", "", "Applies to every file in this folder"},
    };
    _menu.sel = _type == CONTENT_BOOK ? 1 : 0;
    return;
  }
  _menu.title = "Reader";
  _menu.min_w = 600;
  std::string fit = _spread ? "Two pages" : _fit == FIT_WIDTH ? "Fit width" : "Fit page";
  _menu.items = {
      {"Go to page\xE2\x80\xA6", "", ""},
      {"Rotation", ORIENT_NAMES[g_settings.orientation], ""},
      {"Page fit", fit, ""},
      {"Display as", _type == CONTENT_BOOK ? "Book" : "Manga", ""},
      {"Reading direction", rtl() ? "Right to left" : "Left to right", ""},
      {"Dark mode", g_settings.dark_mode ? "On" : "Off", ""},
      {"Top bar", TOPBAR_NAMES[g_settings.top_bar], ""},
      {"Page & battery indicator", g_settings.indicator ? "On" : "Off", ""},
      {"Notes\xE2\x80\xA6", "", ""},
      {"Controls", "", ""},
      {"Close book", "", ""},
  };
  _menu.items[4].enabled = _type == CONTENT_MANGA;
}

void BookReader::on_menu(int i, int d) {
  if (_overlay == OV_TYPE) {
    std::string dir = _path.substr(0, _path.find_last_of('/'));
    switch (i) {
      case 0: Library_SetMark(_path, CONTENT_MANGA); apply_type(CONTENT_MANGA); break;
      case 1: Library_SetMark(_path, CONTENT_BOOK); apply_type(CONTENT_BOOK); break;
      case 2: Library_SetMark(dir, CONTENT_MANGA); apply_type(CONTENT_MANGA); break;
      case 3: Library_SetMark(dir, CONTENT_BOOK); apply_type(CONTENT_BOOK); break;
    }
    _overlay = OV_NONE;
    return;
  }
  switch (i) {
    case 0: _overlay = OV_NONE; go_to_page_dialog(); return;
    case 1:
      g_settings.orientation = (g_settings.orientation + d + 4) % 4;
      _settings_changed = true;
      break;
    case 2: cycle_fit(); break;
    case 3: {
      ContentType t = _type == CONTENT_BOOK ? CONTENT_MANGA : CONTENT_BOOK;
      Library_SetMark(_path, t);
      apply_type(t);
      break;
    }
    case 4:
      g_settings.manga_rtl = !g_settings.manga_rtl;
      _settings_changed = true;
      apply_type(_type);
      break;
    case 5: toggle_dark(); break;
    case 6:
      g_settings.top_bar = (g_settings.top_bar + d + 3) % 3;
      _settings_changed = true;
      apply_viewport();
      break;
    case 7:
      g_settings.indicator = !g_settings.indicator;
      _settings_changed = true;
      apply_viewport();
      break;
    case 8: _overlay = OV_NOTES; _notes = Notes_Read(_path); return;
    case 9: _overlay = OV_HELP; return;
    case 10: _exit = true; return;
  }
  build_menu();
}

// ── input ────────────────────────────────────────────────────────────────────

void BookReader::handle_overlay_input(u64 kDown, int dirs) {
  if (_overlay == OV_MENU || _overlay == OV_TYPE) {
    MenuResult r = _menu.input(dirs, kDown, _touch);
    if (r.activated >= 0) on_menu(r.activated, r.delta ? r.delta : 1);
    if (r.closed || (_overlay == OV_MENU && (kDown & HidNpadButton_Plus))) {
      if (_overlay == OV_TYPE) {
        // Dismissed: remember the default so we don't ask on every open.
        Library_SetMark(_path, _type);
      }
      _overlay = OV_NONE;
    }
  } else if (_overlay == OV_HELP) {
    if ((kDown & (HidNpadButton_B | HidNpadButton_A | HidNpadButton_Plus)) || _touch.tapped)
      _overlay = OV_NONE;
  } else if (_overlay == OV_NOTES) {
    if (kDown & HidNpadButton_A) edit_notes();
    if ((kDown & (HidNpadButton_B | HidNpadButton_Y)) || _touch.tapped) _overlay = OV_NONE;
  }
}

void BookReader::handle_input(u64 kDown, u64 kHeld, PadState &pad) {
  int dirs = _keys.update(kDown, kHeld);
  if (kDown || dirs) _dirty = true;

  if (_overlay != OV_NONE) {
    handle_overlay_input(kDown, dirs);
    return;
  }
  if (!_layout) {
    if (kDown & (HidNpadButton_B | HidNpadButton_A)) _exit = true;
    return;
  }

  if (kDown & HidNpadButton_B) {
    _exit = true;
    return;
  }
  if (kDown & HidNpadButton_Plus) {
    _overlay = OV_MENU;
    build_menu();
    _menu.sel = 0;
    return;
  }
  if (kDown & HidNpadButton_Minus) toggle_dark();
  if (kDown & HidNpadButton_X) {
    if (g_settings.top_bar == TOPBAR_TAP) _bar_shown = !_bar_shown;
  }
  if (kDown & HidNpadButton_Y) cycle_fit();
  if (kDown & HidNpadButton_A) go_to_page_dialog();
  if (kDown & (HidNpadButton_StickL | HidNpadButton_StickR)) {
    _layout->reset_view(ANCHOR_TOP);
  }

  if (kDown & (HidNpadButton_ZR | HidNpadButton_R)) turn(+1);
  if (kDown & (HidNpadButton_ZL | HidNpadButton_L)) turn(-1);

  // D-pad in reading orientation. Left stick is used for panning, so only
  // the D-pad (not the stick's digital directions) turns pages here.
  int dp_only = Input_ToLogical(Input_PhysDirs(kDown & (HidNpadButton_Left | HidNpadButton_Right |
                                                         HidNpadButton_Up | HidNpadButton_Down)));
  if (dp_only & DIR_RIGHT) turn(rtl() ? -1 : +1);
  if (dp_only & DIR_LEFT) turn(rtl() ? +1 : -1);
  if (dp_only & DIR_DOWN) turn(+1);
  if (dp_only & DIR_UP) turn(-1);

  // Left stick flick: turns one page per push, but only along an axis where
  // the page fits the screen. Where the page is larger (zoomed / tall pages)
  // the stick keeps panning smoothly instead.
  int stick = Input_ToLogical(Input_PhysDirs(kDown & (HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
                                                       HidNpadButton_StickLUp | HidNpadButton_StickLDown)));
  if (stick) {
    const PageView &pv = _layout->view();
    if (pv.fits_w()) {
      if (stick & DIR_RIGHT) turn(rtl() ? -1 : +1);
      else if (stick & DIR_LEFT) turn(rtl() ? +1 : -1);
    }
    if (pv.fits_h() && !(stick & (DIR_LEFT | DIR_RIGHT))) {
      if (stick & DIR_DOWN) turn(+1);
      else if (stick & DIR_UP) turn(-1);
    }
  }

  // Left stick: analog pan (camera style).
  HidAnalogStickState sl = padGetStickPos(&pad, 0);
  const float dead = 5000.0f;
  float sx = fabsf((float)sl.x) > dead ? (sl.x - copysignf(dead, sl.x)) / (32767.0f - dead) : 0;
  float sy = fabsf((float)sl.y) > dead ? (sl.y - copysignf(dead, sl.y)) / (32767.0f - dead) : 0;
  if (sx != 0 || sy != 0) {
    float lx, ly;
    Screen_VecToLogical(sx, -sy, &lx, &ly);
    float speed = 8.0f * g_settings.scroll_speed;
    _layout->pan(-lx * speed, -ly * speed);
    _dirty = true;
  }

  // Right stick: zoom around the viewport centre (up = in).
  HidAnalogStickState sr = padGetStickPos(&pad, 1);
  float rx = fabsf((float)sr.x) > dead ? sr.x / 32767.0f : 0;
  float ry = fabsf((float)sr.y) > dead ? sr.y / 32767.0f : 0;
  if (rx != 0 || ry != 0) {
    float lx, ly;
    Screen_VecToLogical(rx, -ry, &lx, &ly);
    float amount = -ly;  // logical up
    if (fabsf(amount) > 0.2f) {
      PageView &v = _layout->view();
      float f = 1.0f + 0.012f * g_settings.zoom_step * amount;
      _layout->zoom_at(v.zoom * f, v.vw / 2, v.vh / 2);
      _dirty = true;
    }
  }
}

void BookReader::handle_touch() {
  _touch.update();
  if (_touch.count || _touch.released) _dirty = true;
  if (_touch.began || _touch.tapped) _nav_hide_at = SDL_GetTicks() + 3000;
  if (_overlay != OV_NONE || !_layout) return;

  int W = Screen_W(), H = Screen_H();
  bool bar = top_bar_visible();
  bool slider = bar && g_settings.top_bar == TOPBAR_TAP;

  // Gestures that start on the bars belong to the bars.
  if (_touch.began)
    _touch_on_bar = (bar && _touch.start_y < BAR_H) || (slider && _touch.start_y > H - SLIDER_H);

  if (_touch_on_bar) {
    int n = page_count();
    if (slider && _touch.start_y > H - SLIDER_H && n > 1 && _touch.count == 1) {
      float t = (_touch.x - 40) / (float)(W - 80);
      t = std::min(std::max(t, 0.0f), 1.0f);
      _slider_drag = true;
      _slider_page = (int)lroundf(t * (n - 1));
    }
    if (_touch.released) {
      if (_slider_drag) {
        _layout->goto_page(_slider_page);
        after_page_change();
      } else if (_touch.tapped && _touch.start_y < BAR_H) {
        if (_touch.start_x < 180)
          _exit = true;
        else if (_touch.start_x > W - 130) {
          _overlay = OV_MENU;
          build_menu();
          _menu.sel = 0;
        }
      }
      _slider_drag = false;
      _touch_on_bar = false;
    }
    return;
  }

  PageView &v = _layout->view();
  if (_touch.count == 2 && _touch.prev_count == 2) {
    float pd = hypotf(_touch.prev_x1 - _touch.prev_x, _touch.prev_y1 - _touch.prev_y);
    float cd = hypotf(_touch.x1 - _touch.x, _touch.y1 - _touch.y);
    if (pd > 10 && cd > 10) {
      float mx = (_touch.x + _touch.x1) / 2 - v.vx, my = (_touch.y + _touch.y1) / 2 - v.vy;
      _layout->zoom_at(v.zoom * cd / pd, mx, my);
      // two-finger pan
      float dx = ((_touch.x + _touch.x1) - (_touch.prev_x + _touch.prev_x1)) / 2;
      float dy = ((_touch.y + _touch.y1) - (_touch.prev_y + _touch.prev_y1)) / 2;
      _layout->pan(dx, dy);
    }
  } else if (_touch.count == 1 && _touch.prev_count == 1 && _touch.dragging && !_touch.multi) {
    // Drag pans only along axes where the page is larger than the screen.
    float dx = v.fits_w() ? 0 : _touch.x - _touch.prev_x;
    float dy = v.fits_h() ? 0 : _touch.y - _touch.prev_y;
    if (dx != 0 || dy != 0) _layout->pan(dx, dy);
  }

  if (_touch.released && !_touch.multi) {
    float dx = _touch.prev_x - _touch.start_x, dy = _touch.prev_y - _touch.start_y;
    if (_touch.tapped) {
      float x = _touch.start_x;
      bool left = x < W * 0.30f, right = x > W * 0.70f;
      if (g_settings.tap_zones && (left || right)) {
        bool forward = right != rtl();
        turn(forward ? +1 : -1);
      } else if (!left && !right && g_settings.top_bar == TOPBAR_TAP) {
        _bar_shown = !_bar_shown;
      }
    } else if (g_settings.swipe_turn && _touch.dragging && v.fits_w() && fabsf(dx) > 110 &&
               fabsf(dx) > fabsf(dy) * 1.5f) {
      // Swipe left = forward in left-to-right books, backwards for manga RTL.
      bool forward = (dx < 0) != rtl();
      if (forward) {
        if (current_page() + _layout->pages_shown() <= page_count() - 1) {
          _layout->next_page(1);
          after_page_change();
        } else {
          edge(+1);
        }
      } else if (current_page() > 0) {
        _layout->previous_page(1);
        after_page_change();
      } else {
        edge(-1);
      }
    }
  }
}

// ── drawing ──────────────────────────────────────────────────────────────────

void BookReader::draw_message(const std::string &msg) {
  const Theme &t = UI_Theme();
  int W = Screen_W(), H = Screen_H();
  UI_Fill(0, 0, W, H, t.page_bg);
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= msg.size()) {
    size_t nl = msg.find('\n', pos);
    if (nl == std::string::npos) nl = msg.size();
    for (auto &l : UI_Wrap(FONT_25, msg.substr(pos, nl - pos), W - 120, 3)) lines.push_back(l);
    if (nl == pos) lines.push_back("");
    pos = nl + 1;
  }
  int lh = UI_TextH(FONT_25) + 6;
  int y = H / 2 - (int)lines.size() * lh / 2;
  for (auto &l : lines) {
    UI_TextC(FONT_25, W / 2, y, t.page_fg, l);
    y += lh;
  }
}

static std::string page_label(int cur, int shown, int total, bool counting) {
  char buf[64];
  if (shown > 1)
    snprintf(buf, sizeof(buf), "%d-%d / %s", cur + 1, cur + shown, counting ? "\xE2\x80\xA6" : std::to_string(total).c_str());
  else
    snprintf(buf, sizeof(buf), "%d / %s", cur + 1, counting ? "\xE2\x80\xA6" : std::to_string(total).c_str());
  return buf;
}

void BookReader::draw_indicator() {
  if (!g_settings.indicator || g_settings.top_bar == TOPBAR_ALWAYS || !_layout) return;
  const Theme &t = UI_Theme();
  int W = Screen_W(), H = Screen_H();
  int y = g_settings.indicator_pos == INDICATOR_TOP ? 0 : H - IND_H;
  UI_Fill(0, y, W, IND_H, t.page_bg);  // own strip: nothing overlaps it
  int cy = y + IND_H / 2;
  bool counting = _image_book && static_cast<CBZPageLayout *>(_layout)->is_enumerating();
  std::string s = page_label(current_page(), _layout->pages_shown(), page_count(), counting);
  int x = 16;
  UI_Text(FONT_20, x, cy - UI_TextH(FONT_20) / 2, t.page_fg, s);
  x += UI_TextW(FONT_20, s) + 18;
  x += UI_Battery(x, cy, t.page_fg, FONT_20, true);
  if (g_settings.indicator_clock) {
    x += 18;
    UI_Text(FONT_20, x, cy - UI_TextH(FONT_20) / 2, t.page_fg, UI_Clock());
  }
}

void BookReader::draw_top_bar() {
  const Theme &t = UI_Theme();
  int W = Screen_W();
  SDL_Color bg = t.surface;
  if (g_settings.top_bar == TOPBAR_TAP) bg.a = 238;
  UI_Fill(0, 0, W, BAR_H, bg);
  UI_Fill(0, BAR_H - 1, W, 1, t.border);

  // Back
  UI_Text(FONT_32, 20, (BAR_H - UI_TextH(FONT_32)) / 2 - 2, t.text, "\xE2\x80\xB9");
  if (W >= 1000) UI_Text(FONT_20, 44, (BAR_H - UI_TextH(FONT_20)) / 2, t.text, "Library");

  // Menu button
  int mw = UI_TextW(FONT_20, "Menu") + 28;
  UI_RoundRect(W - mw - 16, (BAR_H - 38) / 2, mw, 38, 19, t.surface2);
  UI_TextC(FONT_20, W - mw / 2 - 16, (BAR_H - UI_TextH(FONT_20)) / 2, t.text, "Menu");

  // Battery + clock
  int right = W - mw - 32;
  std::string clock = UI_Clock();
  int cw = UI_TextW(FONT_20, clock);
  UI_Text(FONT_20, right - cw, (BAR_H - UI_TextH(FONT_20)) / 2, t.dim, clock);
  right -= cw + 14;
  int bw = UI_BatteryW(FONT_20, W >= 1000);
  UI_Battery(right - bw, BAR_H / 2, t.dim, FONT_20, W >= 1000);
  right -= bw + 14;

  // Title + page
  int left = W >= 1000 ? 150 : 56;
  int avail = right - left;
  bool counting = _image_book && _layout && static_cast<CBZPageLayout *>(_layout)->is_enumerating();
  std::string sub = _layout ? "Page " + page_label(current_page(), _layout->pages_shown(), page_count(), counting) : "";
  UI_Text(FONT_22, left, 12, t.text, UI_Ellipsize(FONT_22, _title, avail));
  UI_Text(FONT_16, left, 14 + UI_TextH(FONT_22), t.dim, sub);
}

void BookReader::draw_bottom_bar() {
  const Theme &t = UI_Theme();
  int W = Screen_W(), H = Screen_H();
  SDL_Color bg = t.surface;
  bg.a = 238;
  UI_Fill(0, H - SLIDER_H, W, SLIDER_H, bg);
  int n = std::max(1, page_count());
  int cur = _slider_drag ? _slider_page : current_page();
  int x0 = 40, x1 = W - 40, ty = H - SLIDER_H + 50;
  UI_RoundRect(x0, ty - 3, x1 - x0, 6, 3, t.surface2);
  float f = n > 1 ? cur / (float)(n - 1) : 0;
  if (rtl()) {
    int kx = x1 - (int)((x1 - x0) * f);
    UI_RoundRect(kx, ty - 3, x1 - kx, 6, 3, t.accent);
    UI_Circle(kx, ty, 11, t.accent);
  } else {
    int kx = x0 + (int)((x1 - x0) * f);
    UI_RoundRect(x0, ty - 3, kx - x0, 6, 3, t.accent);
    UI_Circle(kx, ty, 11, t.accent);
  }
  std::string lbl = std::to_string(cur + 1) + " / " + std::to_string(n);
  UI_TextC(FONT_18, W / 2, H - SLIDER_H + 10, _slider_drag ? t.accent : t.dim, lbl);
}

void BookReader::draw_nav_buttons() {
  uint32_t now = SDL_GetTicks();
  if (!g_settings.screen_buttons || now >= _nav_hide_at) return;
  uint32_t left = _nav_hide_at - now;
  Uint8 a = (Uint8)(left < 600 ? left * 150 / 600 : 150);
  int W = Screen_W(), H = Screen_H();
  SDL_Color bg = {0, 0, 0, a}, fg = {255, 255, 255, (Uint8)std::min(255, a + 80)};
  int s = 64;
  UI_RoundRect(14, H / 2 - s / 2, s, s, s / 2, bg);
  UI_RoundRect(W - 14 - s, H / 2 - s / 2, s, s, s / 2, bg);
  UI_TextC(FONT_40, 14 + s / 2, H / 2 - UI_TextH(FONT_40) / 2 - 2, fg, "\xE2\x80\xB9");
  UI_TextC(FONT_40, W - 14 - s / 2, H / 2 - UI_TextH(FONT_40) / 2 - 2, fg, "\xE2\x80\xBA");
}

void BookReader::draw_edge_banner() {
  if (!_edge_dir || SDL_GetTicks() >= _edge_until) {
    _edge_dir = 0;
    return;
  }
  const Theme &t = UI_Theme();
  int W = Screen_W(), H = Screen_H();
  std::string line1, line2;
  if (_edge_dir > 0) {
    line1 = "End of " + _title;
    line2 = _edge_target.empty() ? "This is the last book in the folder"
                                 : "Next: " + Library_DisplayName(_edge_target) + "  \xC2\xB7  turn again to open";
  } else {
    line1 = "Start of " + _title;
    line2 = _edge_target.empty() ? "This is the first book in the folder"
                                 : "Previous: " + Library_DisplayName(_edge_target) + "  \xC2\xB7  turn again to open";
  }
  int maxw = W - 64;
  line1 = UI_Ellipsize(FONT_22, line1, maxw - 48);
  line2 = UI_Ellipsize(FONT_18, line2, maxw - 48);
  int w = std::min(maxw, std::max(UI_TextW(FONT_22, line1), UI_TextW(FONT_18, line2)) + 48);
  int h = UI_TextH(FONT_22) + UI_TextH(FONT_18) + 30;
  int x = (W - w) / 2, y = H - h - 80;
  UI_RoundRect(x, y, w, h, 18, t.surface);
  UI_RoundOutline(x, y, w, h, 18, 2, t.accent);
  UI_TextC(FONT_22, W / 2, y + 12, t.text, line1);
  UI_TextC(FONT_18, W / 2, y + 16 + UI_TextH(FONT_22), _edge_target.empty() ? t.dim : t.accent, line2);
}

void BookReader::draw_help() {
  const Theme &t = UI_Theme();
  int W = Screen_W(), H = Screen_H();
  UI_Fill(0, 0, W, H, t.scrim);
  struct Row { const char *btn; const char *text; };
  const Row rows[] = {
      {"ZR", "Next page  (also R, tap right side)"},
      {"ZL", "Previous page  (also L, tap left side)"},
      {"D", "D-pad: turn pages / scroll tall pages"},
      {"LS", "Left stick: flick to turn pages, moves a zoomed page"},
      {"RS", "Right stick up/down: zoom  (or pinch)"},
      {"Y", "Fit page / fit width / two pages"},
      {"X", "Show or hide the top bar  (or tap centre)"},
      {"A", "Go to page"},
      {"-", "Dark mode on/off"},
      {"+", "Reader menu"},
      {"B", "Close book"},
  };
  int n = sizeof(rows) / sizeof(rows[0]);
  int pw = std::min(W - 40, 720), row_h = 46;
  int ph = 90 + n * row_h + 20;
  int px = (W - pw) / 2, py = (H - ph) / 2;
  UI_RoundRect(px, py, pw, ph, 18, t.surface);
  UI_Text(FONT_28, px + 28, py + 24, t.text, "Controls");
  for (int i = 0; i < n; i++) {
    int cy = py + 90 + i * row_h + row_h / 2;
    UI_ButtonGlyph(px + 28, cy, rows[i].btn);
    UI_Text(FONT_20, px + 100, cy - UI_TextH(FONT_20) / 2, t.text,
            UI_Ellipsize(FONT_20, rows[i].text, pw - 128));
  }
}

void BookReader::draw_notes() {
  const Theme &t = UI_Theme();
  int W = Screen_W(), H = Screen_H();
  UI_Fill(0, 0, W, H, t.scrim);
  int pw = std::min(W - 40, 820), ph = std::min(H - 80, 520);
  int px = (W - pw) / 2, py = (H - ph) / 2;
  UI_RoundRect(px, py, pw, ph, 18, t.surface);
  UI_Text(FONT_28, px + 28, py + 24, t.text, "Notes");
  UI_Text(FONT_18, px + 28, py + 28 + UI_TextH(FONT_28), t.dim, UI_Ellipsize(FONT_18, _title, pw - 56));
  int body_y = py + 100;
  int hint_y = py + ph - 44;
  if (_notes.empty())
    UI_Text(FONT_22, px + 28, body_y, t.dim, "No notes yet.");
  else
    SDL_DrawTextWrapped(RENDERER, FONT_20, px + 28, body_y, pw - 56, hint_y - body_y - 12, t.text,
                        _notes.c_str());
  int x = px + 28;
  x += UI_Hint(x, hint_y + 16, "A", "Edit", t.text) + 24;
  UI_Hint(x, hint_y + 16, "B", "Close", t.text);
}

void BookReader::draw() {
  const Theme &t = UI_Theme();
  Screen_Begin(_rot);
  int W = Screen_W(), H = Screen_H();
  UI_Fill(0, 0, W, H, t.page_bg);

  if (!_error.empty() || !_layout) {
    draw_message(_error.empty() ? "Loading\xE2\x80\xA6" : _error + "\n\nPress B to go back.");
    Screen_End();
    return;
  }

  if (_image_book && static_cast<CBZPageLayout *>(_layout)->failed()) {
    draw_message("No images were found in this file.  Press B to go back.");
    Screen_End();
    return;
  }

  _layout->draw();
  if (!_layout->is_ready()) {
    std::string msg = "Loading\xE2\x80\xA6";
    UI_TextC(FONT_22, W / 2, H / 2 - UI_TextH(FONT_22) / 2, t.page_fg, msg);
  }

  bool bar = top_bar_visible();
  if (!bar || g_settings.top_bar == TOPBAR_ALWAYS) draw_indicator();
  if (bar) {
    draw_top_bar();
    if (g_settings.top_bar == TOPBAR_TAP) draw_bottom_bar();
  }
  draw_nav_buttons();
  draw_edge_banner();

  switch (_overlay) {
    case OV_MENU:
    case OV_TYPE: _menu.draw(); break;
    case OV_HELP: draw_help(); break;
    case OV_NOTES: draw_notes(); break;
    default: break;
  }
  Screen_End();
}

// ── main loop ────────────────────────────────────────────────────────────────

void BookReader::run() {
  PadState pad;
  padInitializeDefault(&pad);
  padUpdate(&pad);  // swallow the A press that opened the book

  while (!_exit && appletMainLoop()) {
    SDL_TextCache_NextFrame();
    SDL_PumpEvents();

    Orientation_Update();
    int rot = Orientation_Rotation();
    if (rot != _rot) {
      _rot = rot;
      Screen_Begin(_rot);  // so touch mapping uses the new rotation right away
      apply_viewport();
    }

    padUpdate(&pad);
    u64 kDown = padGetButtonsDown(&pad);
    u64 kHeld = padGetButtons(&pad);

    handle_touch();
    handle_input(kDown, kHeld, pad);
    if (_exit) break;

    if (_layout && _layout->poll()) _dirty = true;
    bool scanning = _image_book && _layout && static_cast<CBZPageLayout *>(_layout)->is_enumerating();
    if (_image_book && _layout && !scanning && !_total_saved) {
      int total = static_cast<CBZPageLayout *>(_layout)->final_page_count();
      if (total > 0) Progress_SetTotal(_path, total);
      _total_saved = true;
    }
    // Don't record page 1 while an archive is still being scanned: the saved
    // position is restored once scanning finishes.
    if (_layout && !scanning && _layout->current_page() != _last_saved_page && _layout->is_ready())
      after_page_change();

    uint32_t now = SDL_GetTicks();
    bool animating = (g_settings.screen_buttons && now < _nav_hide_at + 50) ||
                     (_edge_dir && now < _edge_until + 50);
    bool loading = _layout && !_layout->is_ready();
    if (_dirty || animating || loading || now - _last_draw > 1000) {
      draw();
      _last_draw = now;
      _dirty = false;
    } else {
      svcSleepThread(16000000LL);  // idle: don't burn battery redrawing a still page
    }

    if (now - _last_flush > FLUSH_EVERY_MS) {
      Progress_Flush();
      _last_flush = now;
    }
  }
}

void Menu_OpenBook(const char *path) {
  std::string next = path;
  bool at_end = false;
  while (!next.empty()) {
    BookReader *reader = new BookReader(next, at_end);
    reader->run();
    std::string follow = reader->next_path();
    at_end = reader->next_at_end();
    delete reader;  // frees this chapter's RAM cache before the next one loads
    if (!follow.empty()) Recent_Push(follow);
    next = follow;
  }
}
