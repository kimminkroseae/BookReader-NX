#ifndef WOOK_READER_PAGE_LAYOUT_HPP
#define WOOK_READER_PAGE_LAYOUT_HPP

#include <SDL2/SDL.h>
#include <mupdf/fitz.h>
#include <switch.h>

#include <map>
#include <string>

#include "PageView.hpp"

extern fz_context *ctx;

static inline void FreeTextureIfNeeded(SDL_Texture **texture) {
  if (texture && *texture) {
    SDL_DestroyTexture(*texture);
    *texture = NULL;
  }
}

// In-place luminance inversion of packed RGB(A) pixels: white paper becomes
// black, black text becomes white, colours keep their hue.
void InvertLuminance(unsigned char *px, int w, int h, int stride, int n);

// One background rasterisation slot. Each slot owns a cloned context and an
// independently opened document, so no locking is needed.
struct BgRender {
  Thread thread;
  fz_context *bg_ctx = nullptr;
  fz_document *bg_doc = nullptr;
  fz_pixmap *result = nullptr;
  volatile int cancel = 0;
  volatile int done = 0;
  bool active = false;
  int target_page = -1;
  float target_zoom = 0;  // <= 0: compute the fit zoom from vw/vh/fit
  float used_zoom = 0;    // zoom actually rendered at
  float vw = 0, vh = 0;
  int fit = 0;
  bool invert = false;
  bool doc_failed = false;
  fz_cookie cookie;  // abort flag for an in-progress render
};

// Page layout for MuPDF documents (PDF, EPUB, XPS, FB2, HTML, TXT).
// CBZPageLayout derives from it for image archives.
class PageLayout {
 public:
  PageLayout(fz_document *doc, int current_page, SDL_Rect viewport, int fit, bool invert);
  virtual ~PageLayout();

  int current_page() const { return _current_page; }
  virtual int page_count() const { return pages_count; }
  virtual bool is_ready() const { return page_texture != nullptr && _tex_page == _current_page; }
  virtual bool is_spread() const { return false; }
  virtual int pages_shown() const { return 1; }

  // Enables background rendering + prefetch. Reflow parameters are applied to
  // the background documents when the document is reflowable.
  void set_bg_path(const char *path, float reflow_w, float reflow_h, float em);

  // Per-frame work. Returns true when something visible changed.
  virtual bool poll();

  virtual void next_page(int n);
  virtual void previous_page(int n);
  virtual void goto_page(int num);

  virtual void set_viewport(SDL_Rect vp);
  virtual void set_fit(int fit);
  virtual void set_invert(bool invert);

  void zoom_at(float z, float px, float py);  // px/py in viewport coordinates
  void pan(float dx, float dy);
  void reset_view(int anchor);

  virtual void draw();

  PageView &view() { return _view; }

 protected:
  PageLayout();  // for CBZPageLayout (no MuPDF document)
  virtual void on_zoom_changed() {}

  PageView _view;
  int _current_page = 0;
  int pages_count = 0;
  bool _invert = false;

 private:
  void load_current(int anchor);
  void request_render();
  void render_sync(float z);
  float render_zoom() const;
  void invalidate_prefetch();

  fz_document *doc = nullptr;
  fz_rect page_bounds = fz_empty_rect;
  SDL_Texture *page_texture = nullptr;
  int _tex_page = -1;
  float _tex_zoom = 0;
  uint32_t _zoom_changed_at = 0;
  uint32_t _retry_at = 0;

  std::string _bg_path;
  float _reflow_w = 0, _reflow_h = 0, _reflow_em = 0;
  BgRender _bg;  // current page

  // Workers that render the rest of the book into RAM, nearest pages first.
  static constexpr int N_PF = 2;
  BgRender _pf[N_PF];
  struct RamPix {
    unsigned char *px;  // packed RGB
    int w, h;
    float zoom;
    size_t bytes;
  };
  std::map<int, RamPix> _ram;
  size_t _ram_bytes = 0;
  size_t _budget = 0;
  bool ram_insert(int page, fz_pixmap *pix, float zoom);
  void ram_clear();
  int fill_order(int *out, int max) const;

  bool init_slot_ctx(BgRender &slot);
  void free_slot_ctx(BgRender &slot);
  void do_render_slot(BgRender &slot);
  static void bg_thread_entry(void *arg);
  static void pf_thread_entry_0(void *arg);
  static void pf_thread_entry_1(void *arg);
  bool start_slot(BgRender &slot, int page, float z, void (*entry)(void *), int core, int prio);
  void abort_slot(BgRender &slot);
  void cancel_slot(BgRender &slot);
  fz_pixmap *reap_pixmap(BgRender &slot);
  bool slot_params_match(const BgRender &slot) const;
  void start_prefetch();
};

#endif
