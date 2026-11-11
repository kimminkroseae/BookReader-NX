#include "PageLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "memory_mode.h"
#include <string>

extern "C" {
#include "common.h"
}

extern void Log_Write(const std::string &msg);
extern void Log_Error(const std::string &msg);

// Largest texture edge we ask MuPDF to rasterise (keeps memory sane when zoomed).
static const float MAX_RENDER_EDGE = 4096.0f;

void InvertLuminance(unsigned char *px, int w, int h, int stride, int n) {
  for (int y = 0; y < h; y++) {
    unsigned char *p = px + (size_t)y * stride;
    for (int x = 0; x < w; x++, p += n) {
      int r = p[0], g = p[1], b = p[2];
      int d = 255 - 2 * ((r * 77 + g * 150 + b * 29) >> 8);
      r += d;
      g += d;
      b += d;
      p[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
      p[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
      p[2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
    }
  }
}

static float cap_zoom(float z, const fz_rect &b) {
  float m = std::max(b.x1 - b.x0, b.y1 - b.y0);
  if (m > 0 && m * z > MAX_RENDER_EDGE) z = MAX_RENDER_EDGE / m;
  return z;
}

static float fit_zoom(const fz_rect &b, float vw, float vh, int fit) {
  float w = std::max(b.x1 - b.x0, 1.0f), h = std::max(b.y1 - b.y0, 1.0f);
  float zp = std::min(vw / w, vh / h);
  return cap_zoom(fit == FIT_WIDTH ? vw / w : zp, b);
}

static SDL_Texture *pixmap_to_texture(fz_pixmap *pix) {
  if (!pix) return nullptr;
  Uint32 amask = pix->n == 4 ? 0xFF000000 : 0;
  SDL_Surface *s = SDL_CreateRGBSurfaceFrom(pix->samples, pix->w, pix->h, pix->n * 8,
                                            (int)pix->stride, 0x000000FF, 0x0000FF00,
                                            0x00FF0000, amask);
  SDL_Texture *t = s ? SDL_CreateTextureFromSurface(RENDERER, s) : nullptr;
  if (s) SDL_FreeSurface(s);
  return t;
}

static bool get_page_bounds(fz_context *c, fz_document *d, int num, fz_rect *out) {
  bool ok = false;
  fz_page *page = nullptr;
  fz_try(c) {
    page = fz_load_page(c, d, num);
    *out = fz_bound_page(c, page);
    ok = out->x1 > out->x0 && out->y1 > out->y0;
  }
  fz_catch(c) {}
  if (page) fz_drop_page(c, page);
  return ok;
}

// ── construction ─────────────────────────────────────────────────────────────

PageLayout::PageLayout() {}

PageLayout::PageLayout(fz_document *d, int current_page, SDL_Rect vp, int fit, bool invert)
    : doc(d) {
  _invert = invert;
  _view.fit = fit;
  _view.set_viewport(vp.x, vp.y, vp.w, vp.h);
  if (!ctx || !doc) return;
  fz_try(ctx) { pages_count = fz_count_pages(ctx, doc); }
  fz_catch(ctx) { pages_count = 0; }
  if (pages_count <= 0) return;
  _current_page = std::min(std::max(0, current_page), pages_count - 1);
  load_current(ANCHOR_TOP);  // synchronous: background slots are not set up yet
}

PageLayout::~PageLayout() {
  // Abort all first so the three threads stop in parallel, then wait.
  abort_slot(_bg);
  for (int i = 0; i < N_PF; i++) abort_slot(_pf[i]);
  cancel_slot(_bg);
  for (int i = 0; i < N_PF; i++) cancel_slot(_pf[i]);
  free_slot_ctx(_bg);
  for (int i = 0; i < N_PF; i++) free_slot_ctx(_pf[i]);
  ram_clear();
  FreeTextureIfNeeded(&page_texture);
}

bool PageLayout::init_slot_ctx(BgRender &slot) {
  // Only the context is cloned here; the document is opened lazily on the
  // worker thread so the main thread never parses the file more than once.
  slot.bg_ctx = fz_clone_context(ctx);
  return slot.bg_ctx != nullptr;
}

void PageLayout::free_slot_ctx(BgRender &slot) {
  if (slot.result) {
    fz_drop_pixmap(slot.bg_ctx, slot.result);
    slot.result = nullptr;
  }
  if (slot.bg_doc) {
    fz_drop_document(slot.bg_ctx, slot.bg_doc);
    slot.bg_doc = nullptr;
  }
  if (slot.bg_ctx) {
    fz_drop_context(slot.bg_ctx);
    slot.bg_ctx = nullptr;
  }
}

void PageLayout::set_bg_path(const char *path, float reflow_w, float reflow_h, float em) {
  if (!path || !ctx || !doc) return;
  _bg_path = path;
  _reflow_w = reflow_w;
  _reflow_h = reflow_h;
  _reflow_em = em;
  init_slot_ctx(_bg);
  for (int i = 0; i < N_PF; i++) init_slot_ctx(_pf[i]);
  _budget = App_CacheBudget();
  start_prefetch();
}

// ── background rendering ─────────────────────────────────────────────────────

void PageLayout::do_render_slot(BgRender &slot) {
  fz_context *c = slot.bg_ctx;
  fz_page *page = nullptr;
  fz_pixmap *pix = nullptr;
  fz_device *dev = nullptr;

  if (!slot.bg_doc) {
    fz_try(c) {
      slot.bg_doc = fz_open_document(c, _bg_path.c_str());
      if (_reflow_w > 0 && fz_is_document_reflowable(c, slot.bg_doc))
        fz_layout_document(c, slot.bg_doc, _reflow_w, _reflow_h, _reflow_em);
    }
    fz_catch(c) {
      if (slot.bg_doc) fz_drop_document(c, slot.bg_doc);
      slot.bg_doc = nullptr;
      slot.doc_failed = true;
    }
  }

  if (slot.bg_doc) {
    fz_try(c) {
      if (__atomic_load_n(&slot.cancel, __ATOMIC_ACQUIRE))
        fz_throw(c, FZ_ERROR_GENERIC, "cancelled");
      page = fz_load_page(c, slot.bg_doc, slot.target_page);
      fz_rect b = fz_bound_page(c, page);
      float z = slot.target_zoom > 0 ? cap_zoom(slot.target_zoom, b)
                                     : fit_zoom(b, slot.vw, slot.vh, slot.fit);
      slot.used_zoom = z;
      if (__atomic_load_n(&slot.cancel, __ATOMIC_ACQUIRE))
        fz_throw(c, FZ_ERROR_GENERIC, "cancelled");
      // Same as fz_new_pixmap_from_page_contents, but abortable via the cookie.
      fz_matrix ctm = fz_scale(z, z);
      fz_irect bbox = fz_round_rect(fz_transform_rect(b, ctm));
      pix = fz_new_pixmap_with_bbox(c, fz_device_rgb(c), bbox, nullptr, 0);
      fz_clear_pixmap_with_value(c, pix, 0xFF);
      dev = fz_new_draw_device(c, ctm, pix);
      // The device already applies ctm; running with ctm again would scale twice.
      fz_run_page_contents(c, page, dev, fz_identity, &slot.cookie);
      fz_close_device(c, dev);
      if (slot.invert && !__atomic_load_n(&slot.cancel, __ATOMIC_ACQUIRE))
        InvertLuminance(pix->samples, pix->w, pix->h, (int)pix->stride, pix->n);
    }
    fz_always(c) {
      if (dev) fz_drop_device(c, dev);
      if (page) fz_drop_page(c, page);
    }
    fz_catch(c) {
      if (pix) fz_drop_pixmap(c, pix);
      pix = nullptr;
    }
  }

  if (pix && !__atomic_load_n(&slot.cancel, __ATOMIC_ACQUIRE))
    slot.result = pix;
  else if (pix)
    fz_drop_pixmap(c, pix);
  __atomic_store_n(&slot.done, 1, __ATOMIC_RELEASE);
}

void PageLayout::bg_thread_entry(void *arg) {
  auto *self = static_cast<PageLayout *>(arg);
  self->do_render_slot(self->_bg);
}
void PageLayout::pf_thread_entry_0(void *arg) {
  auto *self = static_cast<PageLayout *>(arg);
  self->do_render_slot(self->_pf[0]);
}
void PageLayout::pf_thread_entry_1(void *arg) {
  auto *self = static_cast<PageLayout *>(arg);
  self->do_render_slot(self->_pf[1]);
}

bool PageLayout::slot_params_match(const BgRender &slot) const {
  return slot.invert == _invert && slot.vw == _view.vw && slot.vh == _view.vh && slot.fit == _view.fit;
}

// Starts a render on an idle slot. Never blocks.
bool PageLayout::start_slot(BgRender &slot, int page, float z, void (*entry)(void *),
                            int core, int prio) {
  if (!slot.bg_ctx || slot.doc_failed || slot.active) return false;
  slot.target_page = page;
  slot.target_zoom = z;
  slot.used_zoom = 0;
  slot.vw = _view.vw;
  slot.vh = _view.vh;
  slot.fit = _view.fit;
  slot.invert = _invert;
  slot.result = nullptr;
  memset(&slot.cookie, 0, sizeof(slot.cookie));
  __atomic_store_n(&slot.cancel, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&slot.done, 0, __ATOMIC_RELEASE);
  if (R_FAILED(threadCreate(&slot.thread, entry, this, nullptr, 0x80000, prio, core)))
    return false;
  if (R_FAILED(threadStart(&slot.thread))) {
    threadClose(&slot.thread);
    return false;
  }
  slot.active = true;
  return true;
}

// Asks a running render to stop as soon as possible. Never blocks.
void PageLayout::abort_slot(BgRender &slot) {
  if (!slot.active) return;
  __atomic_store_n(&slot.cancel, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&slot.cookie.abort, 1, __ATOMIC_RELEASE);
}

// Blocking cancel, only used on teardown.
void PageLayout::cancel_slot(BgRender &slot) {
  if (!slot.active) return;
  abort_slot(slot);
  threadWaitForExit(&slot.thread);
  threadClose(&slot.thread);
  slot.active = false;
  if (slot.result) {
    fz_drop_pixmap(slot.bg_ctx, slot.result);
    slot.result = nullptr;
  }
}

// Joins a finished slot and returns its pixmap if it is still valid (caller
// drops it with slot.bg_ctx). Returns null for cancelled / stale results.
fz_pixmap *PageLayout::reap_pixmap(BgRender &slot) {
  if (!slot.active || !__atomic_load_n(&slot.done, __ATOMIC_ACQUIRE)) return nullptr;
  threadWaitForExit(&slot.thread);
  threadClose(&slot.thread);
  slot.active = false;
  fz_pixmap *pix = slot.result;
  slot.result = nullptr;
  if (pix && (__atomic_load_n(&slot.cancel, __ATOMIC_ACQUIRE) || !slot_params_match(slot))) {
    fz_drop_pixmap(slot.bg_ctx, pix);
    pix = nullptr;
  }
  return pix;
}

// ── RAM cache of rendered pages ──────────────────────────────────────────────

static int page_dist(int a, int b) { return a > b ? a - b : b - a; }

static SDL_Texture *rgb_to_texture(const unsigned char *px, int w, int h) {
  SDL_Surface *s = SDL_CreateRGBSurfaceFrom((void *)px, w, h, 24, w * 3, 0x000000FF, 0x0000FF00,
                                            0x00FF0000, 0);
  SDL_Texture *t = s ? SDL_CreateTextureFromSurface(RENDERER, s) : nullptr;
  if (s) SDL_FreeSurface(s);
  return t;
}

bool PageLayout::ram_insert(int page, fz_pixmap *pix, float zoom) {
  if (!pix || pix->n != 3 || _ram.count(page)) return false;
  size_t bytes = (size_t)pix->w * pix->h * 3;
  int d = page_dist(page, _current_page);
  while (_ram_bytes + bytes > _budget) {
    auto far = _ram.end();
    for (auto it = _ram.begin(); it != _ram.end(); ++it)
      if (far == _ram.end() || page_dist(it->first, _current_page) > page_dist(far->first, _current_page))
        far = it;
    if (far == _ram.end() || page_dist(far->first, _current_page) <= d) return false;
    _ram_bytes -= far->second.bytes;
    free(far->second.px);
    _ram.erase(far);
  }
  unsigned char *px = (unsigned char *)malloc(bytes);
  if (!px) return false;
  for (int y = 0; y < pix->h; y++)
    memcpy(px + (size_t)y * pix->w * 3, pix->samples + (size_t)y * pix->stride, (size_t)pix->w * 3);
  _ram[page] = {px, pix->w, pix->h, zoom, bytes};
  _ram_bytes += bytes;
  return true;
}

void PageLayout::ram_clear() {
  for (auto &kv : _ram) free(kv.second.px);
  _ram.clear();
  _ram_bytes = 0;
}

// Pages to have rendered, nearest first with a forward bias: +1 +2 -1 +3 +4 -2 ...
int PageLayout::fill_order(int *out, int max) const {
  int n = 0, f = _current_page + 1, b = _current_page - 1;
  while (n < max && (f < pages_count || b >= 0)) {
    for (int k = 0; k < 2 && n < max && f < pages_count; k++) out[n++] = f++;
    if (n < max && b >= 0) out[n++] = b--;
  }
  return n;
}

void PageLayout::start_prefetch() {
  static void (*const entries[N_PF])(void *) = {pf_thread_entry_0, pf_thread_entry_1};
  static const int cores[N_PF] = {1, 2};
  static int order[4096];
  int n = fill_order(order, 4096);

  // Render outward until the budget is full of pages nearer than the next one.
  size_t avg = _ram.empty() ? (size_t)(_view.vw * _view.vh * 3) : _ram_bytes / _ram.size();
  int far = -1;
  for (auto &kv : _ram) far = std::max(far, page_dist(kv.first, _current_page));
  int in_flight = 0;
  for (int i = 0; i < N_PF; i++)
    if (_pf[i].active) in_flight++;

  for (int k = 0; k < n; k++) {
    int page = order[k];
    if (_ram.count(page)) continue;
    bool busy = false;
    for (int i = 0; i < N_PF; i++)
      if (_pf[i].active && _pf[i].target_page == page) busy = true;
    if (busy) continue;
    bool room = _ram_bytes + (size_t)(in_flight + 1) * avg <= _budget;
    if (!room && page_dist(page, _current_page) >= far) return;
    int idle = -1;
    for (int i = 0; i < N_PF && idle < 0; i++)
      if (!_pf[i].active && _pf[i].bg_ctx && !_pf[i].doc_failed) idle = i;
    if (idle < 0) return;
    if (!start_slot(_pf[idle], page, 0, entries[idle], cores[idle], 0x31)) return;
    in_flight++;
  }
}

void PageLayout::invalidate_prefetch() {
  for (int i = 0; i < N_PF; i++) abort_slot(_pf[i]);
  ram_clear();
}

// ── rendering the current page ───────────────────────────────────────────────

float PageLayout::render_zoom() const { return cap_zoom(_view.zoom, page_bounds); }

void PageLayout::render_sync(float z) {
  fz_page *page = nullptr;
  fz_pixmap *pix = nullptr;
  fz_try(ctx) {
    page = fz_load_page(ctx, doc, _current_page);
    pix = fz_new_pixmap_from_page_contents(ctx, page, fz_scale(z, z), fz_device_rgb(ctx), 0);
    if (_invert) InvertLuminance(pix->samples, pix->w, pix->h, (int)pix->stride, pix->n);
    SDL_Texture *t = pixmap_to_texture(pix);
    if (t) {
      FreeTextureIfNeeded(&page_texture);
      page_texture = t;
      _tex_page = _current_page;
      _tex_zoom = z;
    }
  }
  fz_always(ctx) {
    if (pix) fz_drop_pixmap(ctx, pix);
    if (page) fz_drop_page(ctx, page);
  }
  fz_catch(ctx) {
    Log_Error("PageLayout: failed to render page " + std::to_string(_current_page));
    _retry_at = SDL_GetTicks() + 2000;
  }
}

void PageLayout::request_render() {
  float z = render_zoom();
  if (page_texture && _tex_page == _current_page && std::fabs(_tex_zoom - z) < 0.001f) return;

  // Already rendered in RAM: just upload it.
  auto it = _ram.find(_current_page);
  if (it != _ram.end() && std::fabs(it->second.zoom - z) < 0.001f) {
    if (SDL_Texture *t = rgb_to_texture(it->second.px, it->second.w, it->second.h)) {
      abort_slot(_bg);
      FreeTextureIfNeeded(&page_texture);
      page_texture = t;
      _tex_page = _current_page;
      _tex_zoom = z;
      start_prefetch();
      return;
    }
  }
  if (_bg.active) {
    bool same = _bg.target_page == _current_page && std::fabs(_bg.target_zoom - z) < 0.001f &&
                slot_params_match(_bg) && !__atomic_load_n(&_bg.cancel, __ATOMIC_ACQUIRE);
    if (!same) abort_slot(_bg);  // poll() starts the new render once it stops
    return;
  }
  if (start_slot(_bg, _current_page, z, bg_thread_entry, 2, 0x2F)) return;
  render_sync(z);
}

void PageLayout::load_current(int anchor) {
  fz_rect b;
  if (!get_page_bounds(ctx, doc, _current_page, &b)) {
    Log_Error("PageLayout: failed to load bounds of page " + std::to_string(_current_page));
    return;
  }
  page_bounds = b;
  _view.set_content(b.x1 - b.x0, b.y1 - b.y0);
  _view.reset(anchor);
  request_render();
}

bool PageLayout::poll() {
  bool changed = false;
  if (_bg.active && __atomic_load_n(&_bg.done, __ATOMIC_ACQUIRE)) {
    bool failed = !_bg.result && !__atomic_load_n(&_bg.cancel, __ATOMIC_ACQUIRE);
    int page = _bg.target_page;
    float used = _bg.used_zoom;
    if (fz_pixmap *pix = reap_pixmap(_bg)) {
      if (page == _current_page && std::fabs(used - render_zoom()) < 0.001f) {
        if (SDL_Texture *t = pixmap_to_texture(pix)) {
          FreeTextureIfNeeded(&page_texture);
          page_texture = t;
          _tex_page = page;
          _tex_zoom = used;
          changed = true;
        }
      }
      // Keep fit-zoom renders so coming back to this page is instant.
      if (!_view.zoomed()) ram_insert(page, pix, used);
      fz_drop_pixmap(_bg.bg_ctx, pix);
    } else if (failed && page == _current_page) {
      _retry_at = SDL_GetTicks() + 2000;  // render failed: don't hammer it
    }
  }

  for (int i = 0; i < N_PF; i++) {
    int page = _pf[i].target_page;
    float used = _pf[i].used_zoom;
    if (fz_pixmap *pix = reap_pixmap(_pf[i])) {
      ram_insert(page, pix, used);
      fz_drop_pixmap(_pf[i].bg_ctx, pix);
    }
  }

  if (pages_count > 0 && !_bg.active) {
    float z = render_zoom();
    bool stale = !page_texture || _tex_page != _current_page || std::fabs(_tex_zoom - z) > 0.001f;
    uint32_t now = SDL_GetTicks();
    if (stale && now - _zoom_changed_at > 150 && now >= _retry_at) {
      request_render();
      changed = true;
    }
    // The rest of the book is rendered once the current page is on screen.
    if (!stale) start_prefetch();
  }
  return changed;
}

// ── navigation / view ────────────────────────────────────────────────────────

void PageLayout::goto_page(int num) {
  if (pages_count <= 0) return;
  _current_page = std::min(std::max(0, num), pages_count - 1);
  load_current(ANCHOR_TOP);
}

void PageLayout::next_page(int n) { goto_page(_current_page + n); }
void PageLayout::previous_page(int n) { goto_page(_current_page - n); }

void PageLayout::set_viewport(SDL_Rect vp) {
  _view.set_viewport(vp.x, vp.y, vp.w, vp.h);
  if (!doc) return;
  invalidate_prefetch();
  load_current(ANCHOR_TOP);
}

void PageLayout::set_fit(int fit) {
  _view.fit = fit;
  if (!doc) return;
  invalidate_prefetch();
  _view.reset(ANCHOR_TOP);
  request_render();
}

void PageLayout::set_invert(bool invert) {
  if (invert == _invert) return;
  _invert = invert;
  if (!doc) return;
  invalidate_prefetch();
  abort_slot(_bg);
  _tex_zoom = -1;  // keep showing the old texture until the new one is ready
  request_render();
}

void PageLayout::zoom_at(float z, float px, float py) {
  float before = _view.zoom;
  _view.zoom_at(z, px, py);
  if (_view.zoom != before) {
    _zoom_changed_at = SDL_GetTicks();
    on_zoom_changed();
  }
}

void PageLayout::pan(float dx, float dy) { _view.pan(dx, dy); }

void PageLayout::reset_view(int anchor) {
  float before = _view.zoom;
  _view.reset(anchor);
  if (_view.zoom != before) {
    _zoom_changed_at = 0;
    on_zoom_changed();
  }
}

void PageLayout::draw() {
  if (!page_texture || _tex_page != _current_page) return;
  SDL_Rect r = _view.rect();
  SDL_RenderCopy(RENDERER, page_texture, nullptr, &r);
}
