// Library browser + Settings page.

extern "C" {
#include "MenuChooser.h"
#include "SDL_helper.h"
#include "common.h"
#include "fs.h"
#include "textures.h"
}

#include <SDL2/SDL_image.h>
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <mupdf/fitz.h>
#include <switch.h>
#include <sys/stat.h>
#include <turbojpeg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "../book/stb_image.h"  // declarations only (implementation in CBZPageLayout.cpp)
#include "library.h"
#include "memory_mode.h"
#include "menu_book_reader.h"
#include "mupdf_ctx.h"
#include "natsort.h"
#include "orientation.h"
#include "paths.h"
#include "settings.h"
#include "ui.h"

using namespace std;

extern void Log_Write(const std::string &msg);
extern void Log_Error(const std::string &msg);

#ifndef APP_VERSION_STR
#define APP_VERSION_STR "dev"
#endif

// ── Thumbnail helpers (worker-thread safe: no renderer access) ──────────────

// Crops src to the target aspect ratio (centred) and box-filters it down to
// tw x th. Returns malloc'd RGBA.
static unsigned char *crop_fill_rgba(const unsigned char *src, int sw, int sh, int stride, int n,
                                     int tw, int th) {
  if (!src || sw <= 0 || sh <= 0 || tw <= 0 || th <= 0) return nullptr;
  float ta = (float)tw / th, sa = (float)sw / sh;
  float cx0 = 0, cy0 = 0, cw = (float)sw, ch = (float)sh;
  if (sa > ta) {
    cw = sh * ta;
    cx0 = (sw - cw) / 2;
  } else {
    ch = sw / ta;
    cy0 = (sh - ch) / 2;
  }
  unsigned char *out = (unsigned char *)malloc((size_t)tw * th * 4);
  if (!out) return nullptr;
  for (int ty = 0; ty < th; ty++) {
    int y0 = (int)(cy0 + ty * ch / th);
    int y1 = std::max(y0 + 1, (int)ceilf(cy0 + (ty + 1) * ch / th));
    y1 = std::min(y1, sh);
    for (int tx = 0; tx < tw; tx++) {
      int x0 = (int)(cx0 + tx * cw / tw);
      int x1 = std::max(x0 + 1, (int)ceilf(cx0 + (tx + 1) * cw / tw));
      x1 = std::min(x1, sw);
      unsigned r = 0, g = 0, b = 0, cnt = 0;
      for (int y = y0; y < y1; y++) {
        const unsigned char *p = src + (size_t)y * stride + (size_t)x0 * n;
        for (int x = x0; x < x1; x++, p += n) {
          r += p[0];
          g += p[1];
          b += p[2];
          cnt++;
        }
      }
      unsigned char *o = out + ((size_t)ty * tw + tx) * 4;
      if (!cnt) cnt = 1;
      o[0] = (unsigned char)(r / cnt);
      o[1] = (unsigned char)(g / cnt);
      o[2] = (unsigned char)(b / cnt);
      o[3] = 255;
    }
  }
  return out;
}

// Decodes an encoded image (JPEG decoded at a reduced scale when possible) and
// produces a tw x th cover thumbnail.
static unsigned char *make_thumb_from_encoded(const vector<uint8_t> &data, int tw, int th,
                                              int *out_iw, int *out_ih) {
  const uint8_t *d = data.data();
  size_t n = data.size();
  unsigned char *px = nullptr;
  int w = 0, h = 0;
  if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) {
    tjhandle tj = tjInitDecompress();
    if (tj) {
      int sub = 0, cs = 0;
      if (tjDecompressHeader3(tj, d, (unsigned long)n, &w, &h, &sub, &cs) == 0 && w > 0 && h > 0) {
        int nsf = 0;
        tjscalingfactor *sf = tjGetScalingFactors(&nsf);
        float need = fmaxf((float)tw / w, (float)th / h);
        int bw = w, bh = h;
        for (int i = 0; sf && i < nsf; i++) {
          int sw2 = TJSCALED(w, sf[i]), sh2 = TJSCALED(h, sf[i]);
          if (sw2 >= w * need - 1 && sh2 >= h * need - 1 && sw2 * sh2 < bw * bh) {
            bw = sw2;
            bh = sh2;
          }
        }
        px = (unsigned char *)malloc((size_t)bw * bh * 4);
        if (px && tjDecompress2(tj, d, (unsigned long)n, px, bw, 0, bh, TJPF_RGBA, TJFLAG_FASTDCT) != 0) {
          free(px);
          px = nullptr;
        }
        w = bw;
        h = bh;
      }
      tjDestroy(tj);
    }
  }
  if (!px) {
    int ch = 0;
    px = stbi_load_from_memory(d, (int)n, &w, &h, &ch, 4);
  }
  if (!px) {
    SDL_RWops *rw = SDL_RWFromConstMem(d, (int)n);
    SDL_Surface *s = rw ? IMG_Load_RW(rw, 1) : nullptr;
    SDL_Surface *c = s ? SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0) : nullptr;
    if (s) SDL_FreeSurface(s);
    if (c) {
      unsigned char *out = crop_fill_rgba((unsigned char *)c->pixels, c->w, c->h, c->pitch, 4, tw, th);
      SDL_FreeSurface(c);
      if (out) {
        *out_iw = tw;
        *out_ih = th;
      }
      return out;
    }
    return nullptr;
  }
  unsigned char *out = crop_fill_rgba(px, w, h, w * 4, 4, tw, th);
  free(px);
  if (out) {
    *out_iw = tw;
    *out_ih = th;
  }
  return out;
}

// ── Cover thumbnail loading ───────────────────────────────────────────────────


// FNV-1a key shared with CBZPageLayout — must stay in sync.
static uint64_t comic_cache_key(const char* path) {
    struct stat st;
    uint64_t h = 14695981039346656037ULL;
    if (stat(path, &st) == 0) {
        for (const char* p = path; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
        h ^= (uint64_t)(uint32_t)st.st_size;  h *= 1099511628211ULL;
        h ^= (uint64_t)(uint32_t)st.st_mtime; h *= 1099511628211ULL;
    }
    return h;
}

// Read first line of the page-name cache (.lst) — written by CBZPageLayout
// after a full enumeration. Returns the alphabetically first page name, or "".
static string cover_name_from_page_cache(const char* path) {
    char lst[512];
    snprintf(lst, sizeof(lst), "%s/%016llx.lst2", PAGECACHE_DIR,
             (unsigned long long)comic_cache_key(path));
    FILE* f = fopen(lst, "r");
    if (!f) return "";
    char line[512] = {};
    bool got = fgets(line, sizeof(line), f) != nullptr;
    fclose(f);
    if (!got) return "";
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
    return string(line, len);
}

// Cover image disk cache (.cvr / .cvrn):
//   <key>.cvrn  — page name the bytes came from (text, one line)
//   <key>.cvr   — raw JPEG/PNG bytes of the cover image
// This eliminates both archive passes on every folder browse after the first.
// The cache auto-invalidates via the FNV-1a key (includes file size + mtime).

static bool load_cover_cache(uint64_t key, string& out_name, vector<uint8_t>& out_data) {
    char npath[512], dpath[512];
    snprintf(npath, sizeof(npath), "%s/%016llx.cvrn", PAGECACHE_DIR, (unsigned long long)key);
    snprintf(dpath, sizeof(dpath), "%s/%016llx.cvr",  PAGECACHE_DIR, (unsigned long long)key);
    FILE* fn = fopen(npath, "r");
    if (!fn) return false;
    char line[512] = {};
    bool got = fgets(line, sizeof(line), fn) != nullptr;
    fclose(fn);
    if (!got) return false;
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
    FILE* fd = fopen(dpath, "rb");
    if (!fd) return false;
    fseek(fd, 0, SEEK_END); long sz = ftell(fd); fseek(fd, 0, SEEK_SET);
    if (sz <= 0) { fclose(fd); return false; }
    out_data.resize((size_t)sz);
    bool ok = fread(out_data.data(), 1, (size_t)sz, fd) == (size_t)sz;
    fclose(fd);
    if (!ok) { out_data.clear(); return false; }
    out_name = string(line, len);
    return true;
}

static void save_cover_cache(uint64_t key, const string& name, const vector<uint8_t>& data) {
    if (name.empty() || data.empty()) return;
    mkdir(PAGECACHE_DIR, 0755);
    char npath[512], dpath[512];
    snprintf(npath, sizeof(npath), "%s/%016llx.cvrn", PAGECACHE_DIR, (unsigned long long)key);
    snprintf(dpath, sizeof(dpath), "%s/%016llx.cvr",  PAGECACHE_DIR, (unsigned long long)key);
    FILE* fn = fopen(npath, "w");
    if (fn) { fprintf(fn, "%s\n", name.c_str()); fclose(fn); }
    FILE* fd = fopen(dpath, "wb");
    if (fd) { fwrite(data.data(), 1, data.size(), fd); fclose(fd); }
}

static bool is_junk(const char* name) {
  if (strstr(name, "__MACOSX/")) return true;
  const char* base = strrchr(name, '/');
  base = base ? base + 1 : name;
  return base[0] == '.' && base[1] == '_';
}

static bool is_image_file(const char* name) {
  if (!name) return false;
  const char* dot = strrchr(name, '.');
  if (!dot) return false;
  char ext[8] = {};
  for (int i = 0; i < 7 && dot[i]; i++)
    ext[i] = (char)tolower((unsigned char)dot[i]);
  return strcmp(ext, ".jpg")  == 0 || strcmp(ext, ".jpeg") == 0 ||
         strcmp(ext, ".png")  == 0 || strcmp(ext, ".webp") == 0 ||
         strcmp(ext, ".bmp")  == 0 || strcmp(ext, ".gif")  == 0;
}

// Scan a RAR3 archive's file headers WITHOUT decompression.
// Jumps between blocks using PACK_SIZE from each file header (a raw fseek).
// Works identically for solid and non-solid RAR3 — no decompressor involved.
// Returns the alphabetically-first image entry name, or "" if not RAR3 / no image.
static string find_cover_in_rar3(const char* path) {
  string best;
  FILE* f = fopen(path, "rb");
  if (!f) return best;

  // RAR3 marker block (also serves as magic): 52 61 72 21 1A 07 00
  uint8_t magic[7];
  if (fread(magic, 1, 7, f) != 7 ||
      memcmp(magic, "\x52\x61\x72\x21\x1a\x07\x00", 7) != 0) {
    fclose(f); return best;  // not RAR3 (RAR5 has 01 at byte 6 → fails here)
  }

  // 3-second budget to prevent hangs on corrupt/huge archives
  static const uint64_t BUDGET_TICKS = 57600000ULL;  // ~3s at 19.2 MHz
  uint64_t t0 = armGetSystemTick();

  long pos = 7;  // byte offset of first real block (archive header)
  for (;;) {
    if (armGetSystemTick() - t0 > BUDGET_TICKS) break;
    if (fseek(f, pos, SEEK_SET) != 0) break;

    // Base block header: HEAD_CRC(2) HEAD_TYPE(1) HEAD_FLAGS(2) HEAD_SIZE(2)
    uint8_t h[7];
    if (fread(h, 1, 7, f) != 7) break;

    const uint8_t  btype = h[2];
    const uint16_t flags = (uint16_t)(h[3] | (h[4] << 8));
    const uint16_t hsize = (uint16_t)(h[5] | (h[6] << 8));
    if (hsize < 7) break;           // corrupt
    if (btype == 0x7B) break;       // end-of-archive marker

    // add_size: for all blocks, stored as 4 bytes at pos+7 when flags & 0x8000.
    // For file blocks (0x74) it equals PACK_SIZE (compressed data size).
    uint32_t add_size = 0;
    if (flags & 0x8000) {
      uint8_t s[4];
      if (fseek(f, pos + 7, SEEK_SET) != 0) break;
      if (fread(s, 1, 4, f) != 4) break;
      add_size = (uint32_t)s[0] | (uint32_t)s[1]<<8 |
                 (uint32_t)s[2]<<16 | (uint32_t)s[3]<<24;
    }

    if (btype == 0x74) {  // file block
      // RAR3 file header layout from pos:
      //  0- 6  base header (already read)
      //  7-10  PACK_SIZE   (= add_size)
      // 11-14  UNP_SIZE
      // 15     HOST_OS
      // 16-19  FILE_CRC
      // 20-23  FTIME
      // 24     UNP_VER
      // 25     METHOD
      // 26-27  NAME_SIZE
      // 28-31  ATTR
      // [32-39 HIGH_PACK/UNP if flags & 0x0100]
      // 32 (or 40)  filename bytes
      uint8_t ns[2];
      if (fseek(f, pos + 26, SEEK_SET) == 0 && fread(ns, 1, 2, f) == 2) {
        const uint16_t name_size = (uint16_t)(ns[0] | (ns[1] << 8));
        if (name_size > 0 && name_size < 512) {
          const long name_off = pos + 32 + ((flags & 0x0100) ? 8 : 0);
          char namebuf[513];
          if (fseek(f, name_off, SEEK_SET) == 0 &&
              (size_t)fread(namebuf, 1, name_size, f) == name_size) {
            namebuf[name_size] = '\0';
            for (int i = 0; namebuf[i]; i++)
              if (namebuf[i] == '\\') namebuf[i] = '/';
            if (is_image_file(namebuf) && !is_junk(namebuf)) {
              string sn(namebuf);
              if (best.empty() || natural_less(sn, best)) best = sn;
            }
          }
        }
      }
    }

    const long next = pos + (long)hsize + (long)add_size;
    if (next <= pos) break;  // no forward progress — corrupt archive
    pos = next;
  }
  fclose(f);
  return best;
}

// Extract cover from a comic archive as pre-scaled RGBA pixels.
// Background-thread safe: no SDL renderer calls, no shared fz_context.
// Returns malloc'd RGBA pixels (stride = iw*4), or nullptr. Caller must free().
//
// Cover selection priority (fastest to slowest):
//   1. Cover disk cache (.cvr) matches page-name cache → instant, correct
//   2. Cover disk cache exists, no page-name cache → instant, best-effort
//   3. Page-name cache exists, cover cache stale/absent → re-extract correct page
//   4. No caches → take first image entry from archive (fast), save to cover cache
static unsigned char* extract_cbz_cover_pixels(const char* path, int tw, int th,
                                                int* out_iw, int* out_ih) {
  uint64_t key = comic_cache_key(path);

  // ── Step 1: check what the page-name cache says the cover should be ─────────
  string pcache_name = cover_name_from_page_cache(path);

  // ── Step 2: check cover disk cache ──────────────────────────────────────────
  string cvr_name;
  vector<uint8_t> cover_data;
  bool cvr_hit = load_cover_cache(key, cvr_name, cover_data);

  if (cvr_hit) {
    // Cover cache is valid if it matches the page-name cache, or if we have no
    // page-name cache yet (best-effort is fine until the comic is opened).
    if (pcache_name.empty() || pcache_name == cvr_name) {
      // Fast path: decode cached bytes directly, no archive access.
      goto decode;
    }
    // Page-name cache disagrees → cover cache is stale. Fall through to re-extract.
    cover_data.clear();
  }

  // ── Step 3/4: extract from archive ──────────────────────────────────────────
  {
    // Determine target entry name.
    string target_name = pcache_name;  // correct if comic was opened before

    if (target_name.empty()) {
      // Fast path for RAR3 (CBR): scan file headers directly using PACK_SIZE fseeks.
      // Zero decompression — correct for both solid and non-solid, completes in ms.
      target_name = find_cover_in_rar3(path);
    }

    // Single-pass libarchive: find target (if not known) AND extract in one traversal.
    {
      struct archive* a = archive_read_new();
      archive_read_support_format_all(a);
      archive_read_support_filter_all(a);
      if (archive_read_open_filename(a, path, 1048576) != ARCHIVE_OK) {
        archive_read_free(a); return nullptr;
      }

      // Helper lambda: read entry data into cover_data
      auto read_entry_data = [&](struct archive* ar, struct archive_entry* e) {
        la_int64_t sz = archive_entry_size(e);
        if (sz > 0) {
          cover_data.resize((size_t)sz);
          uint8_t* dst = cover_data.data(); size_t rem = (size_t)sz;
          while (rem > 0) { la_ssize_t n = archive_read_data(ar, dst, rem); if (n <= 0) break; dst += n; rem -= (size_t)n; }
          cover_data.resize((size_t)sz - rem);
        } else {
          cover_data.reserve(4 * 1024 * 1024);
          uint8_t chunk[65536]; la_ssize_t n;
          while ((n = archive_read_data(ar, chunk, sizeof(chunk))) > 0)
            cover_data.insert(cover_data.end(), chunk, chunk + (size_t)n);
        }
      };

      struct archive_entry* entry;
      if (!target_name.empty()) {
        // Target known (from page-name cache or RAR3 scan): seek directly.
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
          if (archive_entry_filetype(entry) == AE_IFDIR) { archive_read_data_skip(a); continue; }
          const char* name = archive_entry_pathname(entry);
          if (!name || string(name) != target_name) { archive_read_data_skip(a); continue; }
          read_entry_data(a, entry);
          break;
        }
      } else {
        // Target unknown: scan for first image (sorted), extract on first encounter.
        // For formats with sorted entries (most CBZ), the first image IS the cover.
        // For unsorted archives, we take the first image found (3s budget).
        static const uint64_t BUDGET_TICKS = 57600000ULL; // ~3s at 19.2 MHz
        uint64_t t0 = armGetSystemTick();
        string best_name;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
          if (archive_entry_filetype(entry) == AE_IFDIR) { archive_read_data_skip(a); continue; }
          const char* n = archive_entry_pathname(entry);
          if (!n || !is_image_file(n) || is_junk(n)) { archive_read_data_skip(a); continue; }
          string sn(n);
          if (best_name.empty() || natural_less(sn, best_name)) {
            best_name = sn;
            // Extract this candidate (may be overwritten if a better name found,
            // but most archives have sorted entries so this is usually final)
            cover_data.clear();
            read_entry_data(a, entry);
          } else {
            archive_read_data_skip(a);
          }
          if (armGetSystemTick() - t0 > BUDGET_TICKS) break;
        }
        target_name = best_name;
      }
      archive_read_free(a);
      if (cover_data.empty()) return nullptr;
    }

    // Save to cover disk cache so future folder browses are instant.
    save_cover_cache(key, target_name, cover_data);
    cvr_name = target_name;
  }

  decode:
  return make_thumb_from_encoded(cover_data, tw, th, out_iw, out_ih);
}

// Extract cover from a PDF/EPUB as pre-scaled RGBA pixels.
// Background-thread safe: uses fz_clone_context (independent MuPDF context).
// Returns malloc'd RGBA pixels, or nullptr. Caller must free().
static unsigned char* extract_pdf_cover_pixels(const char* path, int tw, int th,
                                                int* out_iw, int* out_ih) {
  if (!ctx) return nullptr;
  fz_context* bg_ctx = fz_clone_context(ctx);
  if (!bg_ctx) return nullptr;

  fz_document* doc  = nullptr;
  fz_page*     page = nullptr;
  fz_pixmap*   pix  = nullptr;
  unsigned char* result = nullptr;

  fz_try(bg_ctx) {
    doc = fz_open_document(bg_ctx, path);
    if (!doc || fz_count_pages(bg_ctx, doc) == 0)
      fz_throw(bg_ctx, FZ_ERROR_GENERIC, "no pages");
    page = fz_load_page(bg_ctx, doc, 0);
    fz_rect bounds = fz_bound_page(bg_ctx, page);
    float bw = bounds.x1 - bounds.x0, bh = bounds.y1 - bounds.y0;
    float scale = fmaxf((float)tw / bw, (float)th / bh);  // fill, then crop
    pix = fz_new_pixmap_from_page_contents(
        bg_ctx, page, fz_scale(scale, scale), fz_device_rgb(bg_ctx), 0);
    result = crop_fill_rgba(pix->samples, pix->w, pix->h, (int)pix->stride, pix->n, tw, th);
    if (result) { *out_iw = tw; *out_ih = th; }
  }
  fz_catch(bg_ctx) { result = nullptr; }

  if (pix)  fz_drop_pixmap(bg_ctx, pix);
  if (page) fz_drop_page(bg_ctx, page);
  if (doc)  fz_drop_document(bg_ctx, doc);
  fz_drop_context(bg_ctx);
  return result;
}



static unsigned char *extract_dir_cover_pixels(const char *dir, int tw, int th, int *iw, int *ih) {
  DIR *d = opendir(dir);
  if (!d) return nullptr;
  string best;
  while (struct dirent *e = readdir(d)) {
    if (e->d_type == DT_DIR || e->d_name[0] == '.' || !is_image_file(e->d_name)) continue;
    if (best.empty() || natural_less(e->d_name, best)) best = e->d_name;
  }
  closedir(d);
  if (best.empty()) return nullptr;
  FILE *f = fopen((string(dir) + "/" + best).c_str(), "rb");
  if (!f) return nullptr;
  vector<uint8_t> data;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
  fclose(f);
  if (data.empty()) return nullptr;
  return make_thumb_from_encoded(data, tw, th, iw, ih);
}

// ── Texture LRU (owns all cover textures) ───────────────────────────────────

struct LRUTextureCache {
  static const int MAX_ENTRIES = 150;
  list<string> order;  // front = most recently used
  unordered_map<string, pair<SDL_Texture *, list<string>::iterator>> entries;

  SDL_Texture *get(const string &key) {
    auto it = entries.find(key);
    if (it == entries.end()) return nullptr;
    order.erase(it->second.second);
    order.push_front(key);
    it->second.second = order.begin();
    return it->second.first;
  }
  void put(const string &key, SDL_Texture *tex) {
    if (!tex) return;
    auto it = entries.find(key);
    if (it != entries.end()) {
      SDL_DestroyTexture(it->second.first);
      order.erase(it->second.second);
      entries.erase(it);
    }
    while ((int)entries.size() >= MAX_ENTRIES) {
      SDL_DestroyTexture(entries[order.back()].first);
      entries.erase(order.back());
      order.pop_back();
    }
    order.push_front(key);
    entries[key] = {tex, order.begin()};
  }
  void clear() {
    for (auto &kv : entries) SDL_DestroyTexture(kv.second.first);
    entries.clear();
    order.clear();
  }
};
static LRUTextureCache g_cover_cache;

// ── Background cover loader ─────────────────────────────────────────────────

enum CoverKind { COVER_ARCHIVE = 0, COVER_MUPDF = 1, COVER_DIR = 2, COVER_NONE = 3 };

struct CoverLoader {
  Mutex mtx;
  Thread thread;
  bool started = false;
  volatile int should_exit = 0;
  bool busy = false;  // a job is queued or being processed
  // job
  bool has_job = false;
  string job_path;
  int job_tw = 0, job_th = 0, job_index = -1, job_kind = 0;
  uint32_t job_gen = 0;
  // result
  bool has_result = false;
  unsigned char *res_px = nullptr;
  int res_w = 0, res_h = 0, res_index = -1;
  uint32_t res_gen = 0;
  string res_path;
};

// ── On-disk thumbnail cache ─────────────────────────────────────────────────
// Every cover is rendered once at a fixed size and saved as a small JPEG, so
// later launches (and other grid sizes) only need a fast decode.

static const int THUMB_W = 400, THUMB_H = 580;

static string thumb_path(const string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return "";
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : path) { h ^= c; h *= 1099511628211ULL; }
  h ^= (uint64_t)st.st_size; h *= 1099511628211ULL;
  h ^= (uint64_t)st.st_mtime; h *= 1099511628211ULL;
  h ^= (uint64_t)(THUMB_W * 10000 + THUMB_H); h *= 1099511628211ULL;
  char buf[512];
  snprintf(buf, sizeof(buf), "%s/%016llx.thm", PAGECACHE_DIR, (unsigned long long)h);
  return buf;
}

static unsigned char *load_thumb(const string &tp, int *w, int *h) {
  FILE *f = fopen(tp.c_str(), "rb");
  if (!f) return nullptr;
  vector<uint8_t> data;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
  fclose(f);
  if (data.size() < 4) return nullptr;
  tjhandle tj = tjInitDecompress();
  if (!tj) return nullptr;
  int sub = 0, cs = 0;
  unsigned char *px = nullptr;
  if (tjDecompressHeader3(tj, data.data(), (unsigned long)data.size(), w, h, &sub, &cs) == 0 &&
      *w > 0 && *h > 0) {
    px = (unsigned char *)malloc((size_t)*w * *h * 4);
    if (px && tjDecompress2(tj, data.data(), (unsigned long)data.size(), px, *w, 0, *h, TJPF_RGBA,
                            TJFLAG_FASTDCT) != 0) {
      free(px);
      px = nullptr;
    }
  }
  tjDestroy(tj);
  return px;
}

static void save_thumb(const string &tp, const unsigned char *px, int w, int h) {
  tjhandle tj = tjInitCompress();
  if (!tj) return;
  unsigned char *jpg = nullptr;
  unsigned long size = 0;
  if (tjCompress2(tj, px, w, 0, h, TJPF_RGBA, &jpg, &size, TJSAMP_420, 85, TJFLAG_FASTDCT) == 0 &&
      jpg && size) {
    mkdir(PAGECACHE_DIR, 0755);
    string tmp = tp + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (f) {
      bool ok = fwrite(jpg, 1, size, f) == size;
      fclose(f);
      if (ok) rename(tmp.c_str(), tp.c_str());
      else remove(tmp.c_str());
    }
  }
  if (jpg) tjFree(jpg);
  tjDestroy(tj);
}

static void cover_worker(void *arg) {
  CoverLoader *cl = static_cast<CoverLoader *>(arg);
  while (!__atomic_load_n(&cl->should_exit, __ATOMIC_ACQUIRE)) {
    mutexLock(&cl->mtx);
    if (!cl->has_job) {
      mutexUnlock(&cl->mtx);
      svcSleepThread(3000000LL);
      continue;
    }
    string path = cl->job_path;
    int tw = cl->job_tw, th = cl->job_th, idx = cl->job_index, kind = cl->job_kind;
    uint32_t gen = cl->job_gen;
    cl->has_job = false;
    mutexUnlock(&cl->mtx);

    (void)tw;
    (void)th;
    int iw = 0, ih = 0;
    string tp = thumb_path(path);
    unsigned char *px = tp.empty() ? nullptr : load_thumb(tp, &iw, &ih);
    if (!px) {
      if (kind == COVER_ARCHIVE) px = extract_cbz_cover_pixels(path.c_str(), THUMB_W, THUMB_H, &iw, &ih);
      else if (kind == COVER_MUPDF) px = extract_pdf_cover_pixels(path.c_str(), THUMB_W, THUMB_H, &iw, &ih);
      else if (kind == COVER_DIR) px = extract_dir_cover_pixels(path.c_str(), THUMB_W, THUMB_H, &iw, &ih);
      if (px && !tp.empty()) save_thumb(tp, px, iw, ih);
    }

    for (;;) {
      mutexLock(&cl->mtx);
      if (!cl->has_result) break;
      mutexUnlock(&cl->mtx);
      if (__atomic_load_n(&cl->should_exit, __ATOMIC_ACQUIRE)) {
        free(px);
        return;
      }
      svcSleepThread(1000000LL);
    }
    cl->has_result = true;
    cl->res_px = px;
    cl->res_w = iw;
    cl->res_h = ih;
    cl->res_index = idx;
    cl->res_gen = gen;
    cl->res_path = path;
    cl->busy = false;
    mutexUnlock(&cl->mtx);
  }
}

// ── Library model ───────────────────────────────────────────────────────────

enum EntryKind { E_CONTINUE, E_RECENT, E_IMAGES, E_FOLDER, E_BOOK };

struct Entry {
  string path;
  string name;
  EntryKind kind;
  CoverKind cover;
  string label;  // E_CONTINUE caption
  int last = 0, total = 0;
  uint8_t cover_state = 0;  // 0 none, 1 requested, 2 loaded, 3 failed
};

static CoverKind cover_kind_for(const string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return COVER_DIR;
  if (Library_IsComicArchive(path)) return COVER_ARCHIVE;
  if (Library_Ext(path) == ".txt") return COVER_NONE;
  return COVER_MUPDF;
}

// Lists a directory: folders first, then books, natural order. Reports
// whether the folder itself contains loose images.
static void scan_dir(const string &dir, vector<Entry> &folders, vector<Entry> &books,
                     bool *has_images) {
  *has_images = false;
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  while (struct dirent *e = readdir(d)) {
    if (e->d_name[0] == '.') continue;  // hidden + . / ..
    string name = e->d_name;
    string full = dir + "/" + name;
    bool isdir = e->d_type == DT_DIR;
    if (e->d_type == DT_UNKNOWN) {
      struct stat st;
      isdir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    if (isdir) {
      folders.push_back({full, name, E_FOLDER, COVER_NONE});
    } else if (Library_IsSupportedBook(name)) {
      books.push_back({full, Library_DisplayName(name), E_BOOK, cover_kind_for(full)});
    } else if (Library_IsImageFile(name)) {
      *has_images = true;
    }
  }
  closedir(d);
  auto by_name = [](const Entry &a, const Entry &b) { return natural_less(a.name, b.name); };
  sort(folders.begin(), folders.end(), by_name);
  sort(books.begin(), books.end(), by_name);
}

// ── Browser ─────────────────────────────────────────────────────────────────

struct SettingDef {
  const char *label;
  const char *desc;
  function<string()> value;   // empty string = action row
  function<void(int)> change;
  bool info = false;  // read-only row
};

struct SettingCat {
  const char *name;
  vector<SettingDef> items;
};

struct Chip {
  SDL_Rect r;
  u64 button;
};

struct HintDef {
  const char *btn;
  const char *label;
  u64 button;
};

class Chooser {
 public:
  Chooser();
  ~Chooser();
  void run();

 private:
  enum Overlay { OV_NONE, OV_TYPE, OV_NOTES, OV_CONFIRM, OV_SENSOR, OV_APPLET };

  // model
  void enter_dir(const string &dir);
  void enter_recent();
  void reload();
  void refresh_progress();
  bool go_back();  // false = leave the app
  void open_selected();
  void open_entry(const Entry &e);

  // layout
  void relayout();
  void ensure_visible();
  void move_sel(int dirs);
  int hit_test(float x, float y) const;
  int view_top() const { return HEADER_H; }
  int view_h() const { return H - HEADER_H - FOOTER_H; }
  int grid_cols() const;

  // covers
  void cancel_covers();
  void pump_covers();

  // dialogs
  void open_type_dialog();
  void on_type_choice(int i);
  void edit_notes();
  void build_settings();

  // input
  void library_input(u64 kDown, int dirs);
  void library_touch();
  void settings_input(u64 kDown, int dirs);
  void settings_touch();
  void overlay_input(u64 kDown, int dirs);

  // drawing
  void draw();
  void draw_header(const string &title, const string &sub, bool back);
  void draw_footer(const vector<HintDef> &hints, const string &left);
  void draw_library();
  void draw_folder_card(const Entry &e, SDL_Rect r, bool selected);
  void draw_continue_card(Entry &e, SDL_Rect r, bool selected);
  void draw_book_cell(Entry &e, SDL_Rect r, bool selected);
  void draw_settings();
  void draw_overlay();

  static const int HEADER_H = 88;
  static const int FOOTER_H = 64;
  static const int PAD = 24;
  static const int GAP = 18;

  int W = 1280, H = 720, rot = -1;
  string root = BOOKS_DIR;
  string path = BOOKS_DIR;
  bool in_recent = false;
  vector<Entry> entries;
  int num_cards = 0;  // folders + special cards (come first)
  vector<SDL_Rect> rects;  // content coordinates
  vector<int> section_y;   // y of section titles (content coordinates)
  vector<string> section_name;
  int content_h = 0, cover_w = 0, cover_h = 0;
  int sel = 0;
  float scroll = 0, fling = 0;
  bool layout_dirty = true;

  bool settings_open = false;
  vector<SettingCat> cats;
  int cat = 0, item = 0, focus = 0;  // focus 0 sidebar, 1 items
  vector<SDL_Rect> cat_rects, item_rects;

  Overlay overlay = OV_NONE;
  ListMenu menu;
  string notes_path, notes_text;
  function<void()> confirm_action;

  vector<Chip> chips;
  SDL_Rect back_rect = {0, 0, 0, 0};
  TouchTracker touch;
  KeyRepeat keys;
  PadState pad;
  bool quit = false;
  bool settings_dirty = false;
  // Cache size shown in Settings → Storage (re-scanned only when needed).
  bool cache_dirty = true;
  unsigned long long cache_bytes = 0;
  int cache_files = 0;
  string cache_usage_text() {
    if (cache_dirty) {
      Library_CacheUsage(&cache_bytes, &cache_files);
      cache_dirty = false;
    }
    char buf[64];
    double mb = cache_bytes / (1024.0 * 1024.0);
    if (mb >= 1024.0)
      snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    else if (mb >= 10.0)
      snprintf(buf, sizeof(buf), "%.0f MB", mb);
    else
      snprintf(buf, sizeof(buf), "%.1f MB", mb);
    return string(buf) + "  \xC2\xB7  " + std::to_string(cache_files) + (cache_files == 1 ? " file" : " files");
  }

  static const int N_WORKERS = 2;
  CoverLoader cl[N_WORKERS];
  uint32_t gen = 1;
};

Chooser::Chooser() {
  MuPDF_Context();  // PDF/EPUB thumbnails need it
  int cores[N_WORKERS] = {1, 2};
  for (int w = 0; w < N_WORKERS; w++) {
    mutexInit(&cl[w].mtx);
    if (R_SUCCEEDED(threadCreate(&cl[w].thread, cover_worker, &cl[w], nullptr, 512 * 1024, 0x2C,
                                 cores[w]))) {
      if (R_SUCCEEDED(threadStart(&cl[w].thread)))
        cl[w].started = true;
      else
        threadClose(&cl[w].thread);
    }
  }
  build_settings();
  if (App_IsAppletMode()) {
    menu = ListMenu();
    menu.title = "Applet mode";
    menu.subtitle = "BookReader has limited memory, so pages load slower";
    menu.min_w = 680;
    menu.items = {{"Got it", "", "For full speed, hold R while launching any game"}};
    overlay = OV_APPLET;
  }
  padInitializeDefault(&pad);
  padUpdate(&pad);
  enter_dir(root);
}

Chooser::~Chooser() {
  for (int w = 0; w < N_WORKERS; w++) __atomic_store_n(&cl[w].should_exit, 1, __ATOMIC_RELEASE);
  for (int w = 0; w < N_WORKERS; w++) {
    if (cl[w].started) {
      threadWaitForExit(&cl[w].thread);
      threadClose(&cl[w].thread);
    }
    free(cl[w].res_px);
    cl[w].res_px = nullptr;
  }
  g_cover_cache.clear();
  if (settings_dirty) Settings_Save();
}

// ── model ────────────────────────────────────────────────────────────────────

void Chooser::cancel_covers() {
  gen++;
  for (int w = 0; w < N_WORKERS; w++) {
    mutexLock(&cl[w].mtx);
    if (cl[w].has_job) {
      cl[w].has_job = false;
      cl[w].busy = false;
    }
    mutexUnlock(&cl[w].mtx);
  }
}

void Chooser::enter_dir(const string &dir) {
  cancel_covers();
  in_recent = false;
  path = dir;
  vector<Entry> folders, books;
  bool has_images = false;
  scan_dir(dir, folders, books, &has_images);
  entries.clear();
  if (dir == root && !Recent_Get().empty()) {
    // "Continue reading": the last book, or the next chapter if it was finished.
    string last = Recent_Get()[0];
    int lp = Progress_GetPage(last), lt = Progress_GetTotal(last);
    string target = last, label = "CONTINUE READING";
    if (lt > 0 && lp + 1 >= lt) {
      string nx = Library_Sibling(last, +1);
      if (!nx.empty()) {
        target = nx;
        label = "UP NEXT";
      }
    }
    struct stat st;
    bool isdir = stat(target.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    Entry c{target, isdir ? target.substr(target.find_last_of('/') + 1) : Library_DisplayName(target),
            E_CONTINUE, cover_kind_for(target)};
    c.label = label;
    entries.push_back(c);
  }
  if (dir == root && !Recent_Get().empty())
    entries.push_back({"", "Recently opened", E_RECENT, COVER_NONE});
  if (has_images)
    entries.push_back({dir, "Read the images in this folder", E_IMAGES, COVER_NONE});
  for (auto &f : folders) entries.push_back(f);
  num_cards = (int)entries.size();
  for (auto &b : books) entries.push_back(b);
  refresh_progress();
  sel = 0;
  scroll = fling = 0;
  layout_dirty = true;
}

void Chooser::enter_recent() {
  cancel_covers();
  in_recent = true;
  entries.clear();
  for (const auto &p : Recent_Get()) {
    struct stat st;
    bool dir = stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    string name = dir ? p.substr(p.find_last_of('/') + 1) : Library_DisplayName(p);
    entries.push_back({p, name, E_BOOK, cover_kind_for(p)});
  }
  num_cards = 0;
  refresh_progress();
  sel = 0;
  scroll = fling = 0;
  layout_dirty = true;
}

void Chooser::reload() {
  int keep = sel;
  float keep_scroll = scroll;
  if (in_recent)
    enter_recent();
  else
    enter_dir(path);
  if (!in_recent) {
    sel = std::min(keep, (int)entries.size() - 1);
    if (sel < 0) sel = 0;
    scroll = keep_scroll;
  }
}

void Chooser::refresh_progress() {
  for (auto &e : entries) {
    if (e.kind != E_BOOK && e.kind != E_CONTINUE) continue;
    e.last = Progress_GetPage(e.path);
    e.total = Progress_GetTotal(e.path);
  }
}

bool Chooser::go_back() {
  if (in_recent) {
    enter_dir(path);
    return true;
  }
  if (path == root) return false;
  string parent = path.substr(0, path.find_last_of('/'));
  if (parent.size() < root.size()) parent = root;
  string from = path;
  enter_dir(parent);
  for (int i = 0; i < (int)entries.size(); i++)
    if (entries[i].path == from) {
      sel = i;
      layout_dirty = true;
    }
  return true;
}

void Chooser::open_entry(const Entry &e) {
  cancel_covers();  // don't decode thumbnails in the background while reading
  for (auto &en : entries)
    if (en.cover_state == 1) en.cover_state = 0;
  Recent_Push(e.path);
  Menu_OpenBook(e.path.c_str());
  padUpdate(&pad);  // swallow the button that closed the reader
  touch = TouchTracker();
  refresh_progress();
  if (in_recent)
    enter_recent();
  else if (path == root)
    enter_dir(root);  // refresh the Continue reading card
  layout_dirty = true;
}

void Chooser::open_selected() {
  if (sel < 0 || sel >= (int)entries.size()) return;
  Entry e = entries[sel];
  switch (e.kind) {
    case E_RECENT: enter_recent(); break;
    case E_FOLDER: enter_dir(e.path); break;
    case E_CONTINUE:
    case E_IMAGES:
    case E_BOOK: open_entry(e); break;
  }
}

// ── layout ───────────────────────────────────────────────────────────────────

int Chooser::grid_cols() const {
  int c = g_settings.grid_cols;
  if (W < 1000) c = std::max(2, (int)lroundf(c * 0.6f));
  return c;
}

void Chooser::relayout() {
  layout_dirty = false;
  rects.assign(entries.size(), SDL_Rect{0, 0, 0, 0});
  section_y.clear();
  section_name.clear();
  int y = 12;
  int inner = W - 2 * PAD;
  bool both = num_cards > 0 && num_cards < (int)entries.size();

  int first_card = 0;
  if (!entries.empty() && entries[0].kind == E_CONTINUE) {
    int ch = W >= 1000 ? 176 : 200;
    rects[0] = {PAD, y, inner, ch};
    y += ch + 22;
    first_card = 1;
  }
  int ncards = num_cards - first_card;
  if (ncards > 0) {
    int fcols = W >= 1000 ? 3 : 2;
    int fw = (inner - (fcols - 1) * GAP) / fcols, fh = 68;
    if (both) {
      section_y.push_back(y);
      section_name.push_back("Folders");
      y += 34;
    }
    for (int i = 0; i < ncards; i++) {
      int r = i / fcols, c = i % fcols;
      rects[first_card + i] = {PAD + c * (fw + GAP), y + r * (fh + 14), fw, fh};
    }
    y += ((ncards + fcols - 1) / fcols) * (fh + 14) + 10;
  }

  int nb = (int)entries.size() - num_cards;
  if (nb > 0) {
    if (both) {
      section_y.push_back(y);
      section_name.push_back(in_recent ? "" : "Books");
      y += 34;
    }
    int cols = grid_cols();
    cover_w = (inner - (cols - 1) * GAP) / cols;
    cover_h = (int)(cover_w * 1.45f);
    int cell_h = cover_h + 64;
    for (int b = 0; b < nb; b++) {
      int r = b / cols, c = b % cols;
      rects[num_cards + b] = {PAD + c * (cover_w + GAP), y + r * (cell_h + 10), cover_w, cell_h};
    }
    y += ((nb + cols - 1) / cols) * (cell_h + 10);
  }
  content_h = y + 12;
  // Previously loaded thumbnails may be the wrong size now.
  for (auto &e : entries)
    if (e.cover_state == 3) e.cover_state = 0;
  ensure_visible();
}

void Chooser::ensure_visible() {
  if (sel < 0 || sel >= (int)rects.size()) return;
  const SDL_Rect &r = rects[sel];
  float top = (float)r.y - 12, bot = (float)(r.y + r.h + 12);
  if (top < scroll) scroll = top;
  if (bot > scroll + view_h()) scroll = bot - view_h();
  float max_s = std::max(0, content_h - view_h());
  scroll = std::min(std::max(scroll, 0.0f), max_s);
}

void Chooser::move_sel(int dirs) {
  if (entries.empty() || sel >= (int)rects.size()) return;
  const SDL_Rect &c = rects[sel];
  float cx = c.x + c.w / 2.0f, cy = c.y + c.h / 2.0f;
  int best = -1;
  float best_score = 1e9f;
  for (int i = 0; i < (int)rects.size(); i++) {
    if (i == sel) continue;
    const SDL_Rect &r = rects[i];
    float dx = r.x + r.w / 2.0f - cx, dy = r.y + r.h / 2.0f - cy;
    float primary, secondary;
    if (dirs & DIR_RIGHT) { primary = dx; secondary = dy; if (fabsf(dy) > c.h / 2.0f) continue; }
    else if (dirs & DIR_LEFT) { primary = -dx; secondary = dy; if (fabsf(dy) > c.h / 2.0f) continue; }
    else if (dirs & DIR_DOWN) { primary = dy; secondary = dx; }
    else if (dirs & DIR_UP) { primary = -dy; secondary = dx; }
    else return;
    if (primary <= 2) continue;
    float score = primary + 2.5f * fabsf(secondary);
    if (score < best_score) {
      best_score = score;
      best = i;
    }
  }
  if (best >= 0) {
    sel = best;
    fling = 0;
    ensure_visible();
  }
}

int Chooser::hit_test(float x, float y) const {
  if (y < view_top() || y > view_top() + view_h()) return -1;
  float cy = y - view_top() + scroll;
  for (int i = 0; i < (int)rects.size(); i++) {
    const SDL_Rect &r = rects[i];
    if (x >= r.x && x < r.x + r.w && cy >= r.y && cy < r.y + r.h) return i;
  }
  return -1;
}

// ── covers ───────────────────────────────────────────────────────────────────

void Chooser::pump_covers() {
  // Collect finished thumbnails.
  for (int w = 0; w < N_WORKERS; w++) {
    mutexLock(&cl[w].mtx);
    if (!cl[w].has_result) {
      mutexUnlock(&cl[w].mtx);
      continue;
    }
    unsigned char *px = cl[w].res_px;
    int iw = cl[w].res_w, ih = cl[w].res_h, idx = cl[w].res_index;
    uint32_t rg = cl[w].res_gen;
    string rpath = cl[w].res_path;
    cl[w].res_px = nullptr;
    cl[w].has_result = false;
    mutexUnlock(&cl[w].mtx);

    bool current = rg == gen && idx >= 0 && idx < (int)entries.size() && entries[idx].path == rpath;
    if (!px) {
      if (current) entries[idx].cover_state = 3;
      continue;
    }
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom(px, iw, ih, 32, iw * 4, 0x000000FF, 0x0000FF00,
                                              0x00FF0000, 0xFF000000);
    SDL_Texture *t = s ? SDL_CreateTextureFromSurface(RENDERER, s) : nullptr;
    if (s) SDL_FreeSurface(s);
    free(px);
    if (t) g_cover_cache.put(rpath, t);  // useful even if the user moved on
    if (current) entries[idx].cover_state = t ? 2 : 3;
  }

  // Only covers on screen (plus a little margin) are ever loaded.
  float win_top = scroll - view_h() * 0.5f, win_bot = scroll + view_h() * 1.5f;
  auto visible = [&](int i) {
    return i < (int)rects.size() && rects[i].y + rects[i].h >= win_top && rects[i].y <= win_bot;
  };
  auto needs = [&](int i) {
    Entry &e = entries[i];
    if ((e.kind != E_BOOK && e.kind != E_CONTINUE) || e.cover == COVER_NONE || e.cover_state == 1 ||
        e.cover_state == 3)
      return false;
    if (g_cover_cache.get(e.path)) {
      e.cover_state = 2;
      return false;
    }
    return visible(i);
  };
  for (int w = 0; w < N_WORKERS; w++) {
    if (!cl[w].started) continue;
    mutexLock(&cl[w].mtx);
    bool busy = cl[w].busy;
    mutexUnlock(&cl[w].mtx);
    if (busy) continue;
    int pick = -1;
    for (int i = 0; i < (int)entries.size() && pick < 0; i++)
      if (needs(i)) pick = i;
    if (pick < 0) break;
    entries[pick].cover_state = 1;
    mutexLock(&cl[w].mtx);
    cl[w].busy = true;
    cl[w].has_job = true;
    cl[w].job_path = entries[pick].path;
    cl[w].job_tw = cover_w;
    cl[w].job_th = cover_h;
    cl[w].job_index = pick;
    cl[w].job_kind = entries[pick].cover;
    cl[w].job_gen = gen;
    mutexUnlock(&cl[w].mtx);
  }
}

// ── dialogs ──────────────────────────────────────────────────────────────────

static const char *type_name(ContentType t, bool folder) {
  if (t == CONTENT_MANGA) return "Manga";
  if (t == CONTENT_BOOK) return folder ? "Books" : "Book";
  return "Not set";
}

void Chooser::open_type_dialog() {
  if (sel < 0 || sel >= (int)entries.size()) return;
  const Entry &e = entries[sel];
  if (e.kind == E_RECENT || e.kind == E_CONTINUE) return;
  bool folder = e.kind == E_FOLDER || e.kind == E_IMAGES;
  ContentType cur = Library_GetMark(e.path);
  ContentType inh = Library_InheritedMark(e.path);
  menu = ListMenu();
  menu.min_w = 640;
  if (folder) {
    menu.title = "Mark folder";
    menu.subtitle = e.kind == E_IMAGES ? path.substr(path.find_last_of('/') + 1) : e.name;
    menu.items = {
        {"Manga / comics", "", "Every file inside keeps its colours in dark mode"},
        {"Books", "", "Every file inside is inverted in dark mode"},
        {"Not set", "", inh != CONTENT_UNKNOWN ? string("Uses the parent folder: ") + type_name(inh, true)
                                               : "You'll be asked when opening a file"},
    };
  } else {
    menu.title = "Display as";
    menu.subtitle = e.name;
    menu.items = {
        {"Manga / comic", "", "Keeps colours, black around the page in dark mode"},
        {"Book", "", "White pages turn black, text turns white in dark mode"},
        {"Use folder setting", "", inh != CONTENT_UNKNOWN ? string("Folder: ") + type_name(inh, true)
                                                          : "No folder setting: you'll be asked"},
    };
  }
  menu.sel = cur == CONTENT_MANGA ? 0 : cur == CONTENT_BOOK ? 1 : 2;
  overlay = OV_TYPE;
}

void Chooser::on_type_choice(int i) {
  const Entry &e = entries[sel];
  ContentType t = i == 0 ? CONTENT_MANGA : i == 1 ? CONTENT_BOOK : CONTENT_UNKNOWN;
  Library_SetMark(e.path, t);
  overlay = OV_NONE;
}

void Chooser::edit_notes() {
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return;
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetType(&kbd, SwkbdType_Normal);
  swkbdConfigSetHeaderText(&kbd, "Notes");
  swkbdConfigSetStringLenMax(&kbd, 2000);
  swkbdConfigSetBlurBackground(&kbd, 1);
  swkbdConfigSetInitialText(&kbd, notes_text.c_str());
  vector<char> buf(8192, 0);
  Result rc = swkbdShow(&kbd, buf.data(), buf.size());
  swkbdClose(&kbd);
  if (R_SUCCEEDED(rc)) {
    notes_text = buf.data();
    Notes_Write(notes_path, notes_text);
  }
}

static string on_off(bool b) { return b ? "On" : "Off"; }

void Chooser::build_settings() {
  static Settings &s = g_settings;
  auto flag = [this](bool &b) {
    bool *pb = &b;
    return [this, pb](int) {
      *pb = !*pb;
      settings_dirty = true;
    };
  };
  auto cycle = [this](int &v, int lo, int hi) {
    int *pv = &v;
    return [this, pv, lo, hi](int d) {
      int n = hi - lo + 1;
      *pv = lo + ((*pv - lo + d) % n + n) % n;
      settings_dirty = true;
    };
  };
  static const char *topbar[] = {"Always hidden", "Tap to show", "Always shown"};
  static const char *orient[] = {"Auto", "Landscape", "Portrait (R on top)", "Portrait (L on top)"};
  static const char *tsize[] = {"Small", "Medium", "Large", "Extra large"};

  cats = {
      {"Appearance",
       {
           {"Theme", "Dark uses black backgrounds everywhere. Press - anywhere to switch.",
            [] { return string(s.dark_mode ? "Dark" : "Light"); }, flag(s.dark_mode)},
           {"Library columns", "Covers per row in landscape (portrait uses fewer). ZL / ZR also change it.",
            [] { return std::to_string(s.grid_cols); }, cycle(s.grid_cols, 3, 8)},
       }},
      {"Reading",
       {
           {"Top bar", "Title, battery and menu bar while reading. 'Tap to show': tap the middle of the page or press X.",
            [] { return string(topbar[s.top_bar]); }, cycle(s.top_bar, 0, 2)},
           {"Page & battery indicator", "Shows the page number and battery in its own strip at the side of the screen, never covering the page.",
            [] { return on_off(s.indicator); }, flag(s.indicator)},
           {"Indicator position", "Which edge the indicator sits on (in reading orientation).",
            [] { return string(s.indicator_pos == INDICATOR_TOP ? "Top left" : "Bottom left"); },
            cycle(s.indicator_pos, 0, 1)},
           {"Clock in indicator", "Adds the time next to the battery.",
            [] { return on_off(s.indicator_clock); }, flag(s.indicator_clock)},
           {"Default page fit", "Fit page shows the whole page. Fit width fills the screen width and scrolls. Y switches while reading.",
            [] { return string(s.fit_mode == FIT_WIDTH ? "Fit width" : "Fit page"); }, cycle(s.fit_mode, 0, 1)},
           {"Book text size", "Text size for EPUB, FB2, HTML and TXT books.",
            [] { return string(tsize[s.text_size]); }, cycle(s.text_size, 0, 3)},
           {"On-screen arrows", "Faint page-turn arrows at the screen edges that fade after 3 seconds.",
            [] { return on_off(s.screen_buttons); }, flag(s.screen_buttons)},
       }},
      {"Rotation",
       {
           {"Screen rotation", "Auto uses the motion sensor: hold the Switch upright like a book and the page turns with it.",
            [] { return string(orient[s.orientation]); }, cycle(s.orientation, 0, 3)},
           {"Auto-rotate direction", "If auto-rotate shows pages upside down, switch this to Reversed.",
            [] { return string(s.rotate_reverse ? "Reversed" : "Normal"); }, flag(s.rotate_reverse)},
           {"Sensor test", "Shows live motion sensor values and the detected rotation.",
            [] { return string(); }, [this](int) { overlay = OV_SENSOR; }},
       }},
      {"Manga & dark mode",
       {
           {"Ask manga or book", "When a file has no type yet, ask on first open. Off: archives and image folders are manga, documents are books.",
            [] { return on_off(s.ask_type); }, flag(s.ask_type)},
           {"Manga reading direction", "Right to left swaps tap zones, swipes, the D-pad and two-page spreads for manga.",
            [] { return string(s.manga_rtl ? "Right to left" : "Left to right"); }, flag(s.manga_rtl)},
       }},
      {"Controls",
       {
           {"Tap sides to turn pages", "Tap the left or right 30% of the screen to turn the page.",
            [] { return on_off(s.tap_zones); }, flag(s.tap_zones)},
           {"Swipe to turn pages", "Swipe sideways when the page fits the screen width.",
            [] { return on_off(s.swipe_turn); }, flag(s.swipe_turn)},
           {"Stick scroll speed", "How fast the left stick moves a zoomed page.",
            [] { return std::to_string(s.scroll_speed); }, cycle(s.scroll_speed, 1, 4)},
           {"Zoom speed", "How fast the right stick zooms.",
            [] { return std::to_string(s.zoom_step); }, cycle(s.zoom_step, 1, 4)},
       }},
      {"Storage",
       {
           {"Cache size", "Space on the SD card used by saved covers, page lists and converted text books.",
            [this] { return cache_usage_text(); }, [](int) {}, true},
           {"Clear cache", "Deletes all saved covers and page lists. Your books, reading progress, notes and settings are not touched.",
            [] { return string(); },
            [this](int) {
              confirm_action = [this] {
                Library_ClearCaches();
                g_cover_cache.clear();
                for (auto &e : entries) e.cover_state = 0;
                cache_dirty = true;
              };
              menu = ListMenu();
              menu.title = "Clear cache?";
              menu.subtitle = "This frees " + cache_usage_text();
              menu.min_w = 720;
              menu.items = {{"Clear cache", "", "Covers are rebuilt as you browse (slower the first time)"},
                            {"Cancel", "", "Books, progress, notes and settings are always kept"}};
              menu.sel = 1;
              overlay = OV_CONFIRM;
            }},
           {"Clear reading history", "Empties 'Recently opened'. Reading positions are kept.",
            [] { return string(); },
            [this](int) {
              confirm_action = [] { Recent_Clear(); };
              menu = ListMenu();
              menu.title = "Clear reading history?";
              menu.items = {{"Clear", "", ""}, {"Cancel", "", ""}};
              menu.sel = 1;
              overlay = OV_CONFIRM;
            }},
           {"Books folder", "Copy books, comics and folders here on your SD card.",
            [] { return string(BOOKS_DIR); }, [](int) {}, true},
           {"Version", "BookReader (based on WookReader)", [] { return string(APP_VERSION_STR); }, [](int) {}, true},
       }},
  };
}

// ── input ────────────────────────────────────────────────────────────────────

void Chooser::overlay_input(u64 kDown, int dirs) {
  if (overlay == OV_APPLET) {
    MenuResult r = menu.input(0, kDown, touch);
    if (r.activated >= 0 || r.closed) overlay = OV_NONE;
    return;
  }
  if (overlay == OV_TYPE || overlay == OV_CONFIRM) {
    MenuResult r = menu.input(dirs & (DIR_UP | DIR_DOWN), kDown, touch);
    if (r.activated >= 0) {
      if (overlay == OV_TYPE) {
        on_type_choice(r.activated);
      } else {
        if (r.activated == 0 && confirm_action) confirm_action();
        overlay = OV_NONE;
      }
    } else if (r.closed) {
      overlay = OV_NONE;
    }
  } else if (overlay == OV_NOTES) {
    if (kDown & HidNpadButton_A) edit_notes();
    if ((kDown & (HidNpadButton_B | HidNpadButton_Y)) || touch.tapped) overlay = OV_NONE;
  } else if (overlay == OV_SENSOR) {
    if ((kDown & (HidNpadButton_B | HidNpadButton_A)) || touch.tapped) overlay = OV_NONE;
  }
}

void Chooser::library_input(u64 kDown, int dirs) {
  if (dirs) move_sel(dirs);
  if (kDown & HidNpadButton_A) open_selected();
  if (kDown & HidNpadButton_B) {
    if (!go_back()) quit = true;
  }
  if (kDown & HidNpadButton_X) open_type_dialog();
  if ((kDown & HidNpadButton_Y) && sel < (int)entries.size() && entries[sel].kind == E_BOOK) {
    notes_path = entries[sel].path;
    notes_text = Notes_Read(notes_path);
    overlay = OV_NOTES;
  }
  if (kDown & HidNpadButton_Plus) {
    settings_open = true;
    cache_dirty = true;
    focus = 0;
    item = 0;
  }
  if (kDown & HidNpadButton_Minus) {
    g_settings.dark_mode = !g_settings.dark_mode;
    settings_dirty = true;
  }
  if (kDown & (HidNpadButton_ZL | HidNpadButton_ZR)) {
    int c = g_settings.grid_cols + ((kDown & HidNpadButton_ZL) ? 1 : -1);
    if (c >= 3 && c <= 8) {
      g_settings.grid_cols = c;
      settings_dirty = true;
      layout_dirty = true;
    }
  }
}

void Chooser::library_touch() {
  if (touch.count == 1 && touch.dragging && touch.prev_count == 1) {
    scroll -= touch.y - touch.prev_y;
    fling = 0;
  }
  if (touch.released && touch.dragging && !touch.multi) fling = -touch.vel_y;
  if (!touch.tapped) return;

  float x = touch.start_x, y = touch.start_y;
  for (auto &c : chips) {
    SDL_Point p = {(int)x, (int)y};
    if (SDL_PointInRect(&p, &c.r)) {
      u64 b = c.button;
      library_input(b, 0);
      return;
    }
  }
  SDL_Point p = {(int)x, (int)y};
  if (back_rect.w && SDL_PointInRect(&p, &back_rect)) {
    if (!go_back()) quit = true;
    return;
  }
  int i = hit_test(x, y);
  if (i >= 0) {
    sel = i;
    open_selected();
  }
}

void Chooser::settings_input(u64 kDown, int dirs) {
  int ncat = (int)cats.size();
  if (kDown & HidNpadButton_L) { cat = (cat + ncat - 1) % ncat; item = 0; }
  if (kDown & HidNpadButton_R) { cat = (cat + 1) % ncat; item = 0; }
  int nitems = (int)cats[cat].items.size();
  if (focus == 0) {
    if (dirs & DIR_UP) cat = (cat + ncat - 1) % ncat;
    if (dirs & DIR_DOWN) cat = (cat + 1) % ncat;
    if ((dirs & DIR_RIGHT) || (kDown & HidNpadButton_A)) { focus = 1; item = 0; }
    if (kDown & (HidNpadButton_B | HidNpadButton_Plus)) settings_open = false;
  } else {
    if (dirs & DIR_UP) item = (item + nitems - 1) % nitems;
    if (dirs & DIR_DOWN) item = (item + 1) % nitems;
    SettingDef &d = cats[cat].items[item];
    bool is_value = !d.value().empty() && !d.info;
    if (dirs & DIR_RIGHT) d.change(+1);
    if (dirs & DIR_LEFT) {
      if (is_value) d.change(-1); else focus = 0;
    }
    if (kDown & HidNpadButton_A) d.change(+1);
    if (kDown & HidNpadButton_B) focus = 0;
    if (kDown & HidNpadButton_Plus) settings_open = false;
  }
  if (kDown & HidNpadButton_Minus) {
    g_settings.dark_mode = !g_settings.dark_mode;
    settings_dirty = true;
  }
  if (!settings_open) {
    if (settings_dirty) Settings_Save();
    settings_dirty = false;
    layout_dirty = true;
  }
}

void Chooser::settings_touch() {
  if (!touch.tapped) return;
  SDL_Point p = {(int)touch.start_x, (int)touch.start_y};
  if (back_rect.w && SDL_PointInRect(&p, &back_rect)) {
    settings_input(HidNpadButton_Plus, 0);
    return;
  }
  for (int i = 0; i < (int)cat_rects.size(); i++)
    if (SDL_PointInRect(&p, &cat_rects[i])) {
      cat = i;
      item = 0;
      focus = 0;
      return;
    }
  for (int i = 0; i < (int)item_rects.size(); i++)
    if (SDL_PointInRect(&p, &item_rects[i])) {
      focus = 1;
      item = i;
      SDL_Rect r = item_rects[i];
      bool left_half = p.x < r.x + r.w / 2;
      SettingDef &d = cats[cat].items[i];
      d.change(!d.value().empty() && !d.info && left_half ? -1 : +1);
      return;
    }
  for (auto &c : chips)
    if (SDL_PointInRect(&p, &c.r)) {
      settings_input(c.button, 0);
      return;
    }
}

// ── drawing ──────────────────────────────────────────────────────────────────

static const SDL_Color WHITE_C = {255, 255, 255, 255};

void Chooser::draw_header(const string &title, const string &sub, bool back) {
  const Theme &t = UI_Theme();
  int x = PAD;
  int title_y = sub.empty() ? (HEADER_H - UI_TextH(FONT_32)) / 2 : 12;
  if (back) {
    UI_Text(FONT_40, x - 4, title_y + (UI_TextH(FONT_32) - UI_TextH(FONT_40)) / 2 - 3, t.text,
            "\xE2\x80\xB9");
    x += 30;
  }

  // Right: battery + clock
  int right = W - PAD;
  string clock = UI_Clock();
  int cw = UI_TextW(FONT_20, clock);
  int cy = HEADER_H / 2;
  UI_Text(FONT_20, right - cw, cy - UI_TextH(FONT_20) / 2, t.dim, clock);
  right -= cw + 18;
  int bw = UI_BatteryW(FONT_20, true);
  UI_Battery(right - bw, cy, t.dim, FONT_20, true);
  right -= bw + 24;
  if (App_IsAppletMode()) {
    int pw = UI_TextW(FONT_14, "APPLET MODE") + 16;
    UI_Pill(right - pw, cy - (UI_TextH(FONT_14) + 4) / 2, FONT_14, "APPLET MODE", t.manga, WHITE_C);
    right -= pw + 16;
  }

  string tt = UI_Ellipsize(FONT_32, title, right - x);
  UI_Text(FONT_32, x, title_y, t.text, tt);
  if (!sub.empty()) UI_Text(FONT_16, x, title_y + UI_TextH(FONT_32), t.dim, UI_Ellipsize(FONT_16, sub, right - x));
  if (back) back_rect = {0, 0, std::min(W / 2, x + UI_TextW(FONT_32, tt) + 24), HEADER_H};
  UI_Fill(PAD, HEADER_H - 1, W - 2 * PAD, 1, t.border);
}

void Chooser::draw_footer(const vector<HintDef> &hints, const string &left) {
  const Theme &t = UI_Theme();
  int y = H - FOOTER_H, cy = y + FOOTER_H / 2;
  UI_Fill(0, y, W, FOOTER_H, t.surface);
  UI_Fill(0, y, W, 1, t.border);
  int total = 0;
  for (auto &h : hints) total += UI_HintW(h.btn, h.label) + 24;
  int x = W - PAD - total + 24;
  int start = x;
  for (auto &h : hints) {
    int w = UI_Hint(x, cy, h.btn, h.label, t.text);
    chips.push_back({{x - 8, y, w + 16, FOOTER_H}, h.button});
    x += w + 24;
  }
  if (!left.empty() && start - PAD - 24 > UI_TextW(FONT_18, left))
    UI_Text(FONT_18, PAD, cy - UI_TextH(FONT_18) / 2, t.dim, left);
}

void Chooser::draw_continue_card(Entry &e, SDL_Rect r, bool selected) {
  const Theme &t = UI_Theme();
  if (selected) UI_RoundOutline(r.x - 4, r.y - 4, r.w + 8, r.h + 8, 22, 3, t.accent);
  UI_RoundRect(r.x, r.y, r.w, r.h, 18, selected ? t.surface2 : t.surface);

  int ch = r.h - 28, cw = (int)(ch / 1.45f);
  SDL_Rect cr = {r.x + 14, r.y + 14, cw, ch};
  SDL_Texture *tex = e.cover != COVER_NONE ? g_cover_cache.get(e.path) : nullptr;
  if (tex)
    SDL_RenderCopy(RENDERER, tex, nullptr, &cr);
  else
    UI_RoundRect(cr.x, cr.y, cr.w, cr.h, 8, t.surface2);

  int tx = cr.x + cw + 24, tw = r.x + r.w - 24 - tx;
  bool wide = W >= 1000;
  int hint_w = wide ? UI_HintW("A", e.label == "UP NEXT" ? "Start" : "Continue") + 8 : 0;
  int y = r.y + 18;
  UI_Text(FONT_16, tx, y, t.accent, e.label);
  y += UI_TextH(FONT_16) + 4;
  std::vector<string> title = UI_Wrap(FONT_28, e.name, tw - hint_w, wide ? 2 : 3);
  for (auto &l : title) {
    UI_Text(FONT_28, tx, y, t.text, l);
    y += UI_TextH(FONT_28);
  }
  string status = e.total > 0 ? "Page " + std::to_string(std::min(e.last + 1, e.total)) + " of " +
                                    std::to_string(e.total)
                              : "Not started";
  int by = r.y + r.h - 34;
  UI_Text(FONT_18, tx, by - UI_TextH(FONT_18) - 6, t.dim, status);
  int bar_w = tw - hint_w;
  UI_RoundRect(tx, by, bar_w, 6, 3, t.surface2);
  if (e.total > 0) {
    float f = std::min(1.0f, (e.last + 1) / (float)e.total);
    UI_RoundRect(tx, by, std::max(6, (int)(bar_w * f)), 6, 3, t.accent);
  }
  if (wide)
    UI_Hint(r.x + r.w - 24 - hint_w + 8, r.y + r.h / 2, "A", e.label == "UP NEXT" ? "Start" : "Continue",
            t.text);
}

void Chooser::draw_folder_card(const Entry &e, SDL_Rect r, bool selected) {
  const Theme &t = UI_Theme();
  if (selected) UI_RoundOutline(r.x - 4, r.y - 4, r.w + 8, r.h + 8, 18, 3, t.accent);
  UI_RoundRect(r.x, r.y, r.w, r.h, 14, selected ? t.surface2 : t.surface);

  SDL_Texture *icon = e.kind == E_IMAGES ? blue_book : blue_folder;
  if (icon) {
    if (e.kind == E_RECENT) SDL_SetTextureColorMod(icon, t.accent.r, t.accent.g, t.accent.b);
    SDL_DrawImageScale(RENDERER, icon, r.x + 16, r.y + (r.h - 36) / 2, 36, 36);
    SDL_SetTextureColorMod(icon, 255, 255, 255);
  }

  int pill_w = 0;
  ContentType mark = e.kind == E_RECENT ? CONTENT_UNKNOWN : Library_GetMark(e.path);
  const char *pill = mark == CONTENT_MANGA ? "MANGA" : mark == CONTENT_BOOK ? "BOOKS" : nullptr;
  if (pill) pill_w = UI_TextW(FONT_14, pill) + 16 + 12;
  SDL_Color fg = e.kind == E_RECENT ? t.accent : t.text;
  int tx = r.x + 66;
  UI_Text(FONT_22, tx, r.y + (r.h - UI_TextH(FONT_22)) / 2, fg,
          UI_Ellipsize(FONT_22, e.name, r.x + r.w - 16 - pill_w - tx));
  if (pill) {
    int pw = UI_TextW(FONT_14, pill) + 16;
    UI_Pill(r.x + r.w - 16 - pw, r.y + (r.h - UI_TextH(FONT_14) - 4) / 2, FONT_14, pill,
            mark == CONTENT_MANGA ? t.manga : t.book, WHITE_C);
  }
}

void Chooser::draw_book_cell(Entry &e, SDL_Rect r, bool selected) {
  const Theme &t = UI_Theme();
  SDL_Rect cr = {r.x, r.y, cover_w, cover_h};
  if (selected) UI_RoundOutline(cr.x - 6, cr.y - 6, cr.w + 12, cr.h + 12, 12, 4, t.accent);

  SDL_Texture *tex = e.cover != COVER_NONE ? g_cover_cache.get(e.path) : nullptr;
  if (tex) {
    SDL_RenderCopy(RENDERER, tex, nullptr, &cr);
  } else {
    UI_RoundRect(cr.x, cr.y, cr.w, cr.h, 8, t.surface2);
    int is = std::min(64, cr.w / 3);
    if (blue_book) SDL_DrawImageScale(RENDERER, blue_book, cr.x + (cr.w - is) / 2, cr.y + cr.h / 2 - is, is, is);
    string ext = e.cover == COVER_DIR ? "FOLDER" : Library_Ext(e.path);
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    for (char &c : ext) c = (char)toupper((unsigned char)c);
    UI_TextC(FONT_16, cr.x + cr.w / 2, cr.y + cr.h / 2 + 10, t.dim, ext);
  }

  // Reading progress along the bottom of the cover.
  if (e.total > 0) {
    float f = std::min(1.0f, (e.last + 1) / (float)e.total);
    UI_Fill(cr.x, cr.y + cr.h - 5, cr.w, 5, SDL_Color{0, 0, 0, 150});
    UI_Fill(cr.x, cr.y + cr.h - 5, (int)(cr.w * f), 5, t.accent);
  }

  ContentType ct = Library_Resolve(e.path);
  if (ct != CONTENT_UNKNOWN && cr.w >= 120)
    UI_Pill(cr.x + 6, cr.y + 6, FONT_14, ct == CONTENT_MANGA ? "MANGA" : "BOOK",
            ct == CONTENT_MANGA ? t.manga : t.book, WHITE_C);
  if (Notes_Exists(e.path)) {
    int nw = UI_TextW(FONT_14, "NOTE") + 16;
    UI_Pill(cr.x + cr.w - nw - 6, cr.y + 6, FONT_14, "NOTE", SDL_Color{0, 0, 0, 170}, WHITE_C);
  }

  int ty = cr.y + cr.h + 8;
  UI_Text(FONT_18, cr.x, ty, selected ? t.accent : t.text, UI_Ellipsize(FONT_18, e.name, cr.w));
  string status;
  if (e.total > 0) {
    int pct = (e.last + 1) * 100 / e.total;
    status = e.last + 1 >= e.total ? "Finished" : std::to_string(std::max(1, pct)) + "%  \xC2\xB7  page " +
                                                      std::to_string(e.last + 1) + " of " + std::to_string(e.total);
  } else {
    status = "New";
  }
  UI_Text(FONT_16, cr.x, ty + UI_TextH(FONT_18) + 2, t.dim, UI_Ellipsize(FONT_16, status, cr.w));
}

void Chooser::draw_library() {
  const Theme &t = UI_Theme();
  if (layout_dirty) relayout();

  string title, sub;
  bool back = in_recent || path != root;
  if (in_recent) {
    title = "Recently opened";
  } else if (path == root) {
    title = "Library";
  } else {
    title = path.substr(path.find_last_of('/') + 1);
    string rel = path.substr(root.size());
    sub = "Library";
    size_t pos = 1;
    while (pos < rel.size()) {
      size_t nx = rel.find('/', pos);
      if (nx == string::npos) nx = rel.size();
      sub += "  \xE2\x80\xBA  " + rel.substr(pos, nx - pos);
      pos = nx + 1;
    }
  }
  draw_header(title, sub, back);

  SDL_Rect clip = {0, HEADER_H, W, view_h()};
  SDL_RenderSetClipRect(RENDERER, &clip);
  for (size_t i = 0; i < section_y.size(); i++) {
    int y = HEADER_H + section_y[i] - (int)scroll;
    if (!section_name[i].empty()) UI_Text(FONT_20, PAD, y + 4, t.dim, section_name[i]);
  }
  for (int i = 0; i < (int)entries.size(); i++) {
    SDL_Rect r = rects[i];
    r.y = HEADER_H + r.y - (int)scroll;
    if (r.y + r.h < HEADER_H - 10 || r.y > H - FOOTER_H + 10) continue;
    if (i < num_cards)
      entries[i].kind == E_CONTINUE ? draw_continue_card(entries[i], r, i == sel)
                                    : draw_folder_card(entries[i], r, i == sel);
    else
      draw_book_cell(entries[i], r, i == sel);
  }
  if (entries.empty()) {
    int cy = HEADER_H + view_h() / 2;
    if (path == root && !in_recent) {
      UI_TextC(FONT_28, W / 2, cy - 50, t.text, "Your library is empty");
      UI_TextC(FONT_20, W / 2, cy, t.dim, "Copy books, comics or folders to");
      UI_TextC(FONT_20, W / 2, cy + 30, t.accent, BOOKS_DIR);
    } else {
      UI_TextC(FONT_25, W / 2, cy - 15, t.dim, "Nothing here yet");
    }
  }
  // Scrollbar
  if (content_h > view_h()) {
    int vh = view_h();
    int bar_h = std::max(40, vh * vh / content_h);
    int bar_y = HEADER_H + (int)((vh - bar_h) * (scroll / (content_h - vh)));
    UI_RoundRect(W - 8, bar_y, 4, bar_h, 2, t.border);
  }
  SDL_RenderSetClipRect(RENDERER, nullptr);

  vector<HintDef> hints;
  bool book = sel < (int)entries.size() && entries[sel].kind == E_BOOK;
  bool markable = sel < (int)entries.size() && entries[sel].kind != E_RECENT &&
                  entries[sel].kind != E_CONTINUE;
  hints.push_back({"A", "Open", HidNpadButton_A});
  hints.push_back({"B", back ? "Back" : "Exit", HidNpadButton_B});
  if (markable) hints.push_back({"X", "Manga / book", HidNpadButton_X});
  if (W >= 1000) {
    if (book) hints.push_back({"Y", "Notes", HidNpadButton_Y});
    hints.push_back({"-", "Theme", HidNpadButton_Minus});
  }
  hints.push_back({"+", "Settings", HidNpadButton_Plus});
  int nbooks = (int)entries.size() - num_cards;
  string left = nbooks > 0 ? std::to_string(nbooks) + (nbooks == 1 ? " book" : " books") : "";
  draw_footer(hints, left);
}

void Chooser::draw_settings() {
  const Theme &t = UI_Theme();
  draw_header("Settings", "", true);
  cat_rects.clear();
  item_rects.clear();

  int sb_w = W >= 1000 ? 300 : 236;
  int top = HEADER_H + 16;
  for (int i = 0; i < (int)cats.size(); i++) {
    SDL_Rect r = {PAD - 8, top + i * 58, sb_w - PAD, 50};
    cat_rects.push_back(r);
    bool selc = i == cat;
    if (selc) {
      UI_RoundRect(r.x, r.y, r.w, r.h, 12, focus == 0 ? t.surface2 : t.surface);
      if (focus == 0) UI_Fill(r.x, r.y + 12, 4, r.h - 24, t.accent);
    }
    UI_Text(FONT_22, r.x + 18, r.y + (r.h - UI_TextH(FONT_22)) / 2, selc ? t.text : t.dim,
            UI_Ellipsize(FONT_22, cats[i].name, r.w - 26));
  }

  int px = sb_w + 8, pw = W - px - PAD;
  int ph = H - FOOTER_H - top - 16;
  UI_RoundRect(px, top, pw, ph, 16, t.surface);
  const SettingCat &c = cats[cat];
  int row_h = 54;
  for (int j = 0; j < (int)c.items.size(); j++) {
    const SettingDef &d = c.items[j];
    SDL_Rect r = {px + 10, top + 10 + j * (row_h + 4), pw - 20, row_h};
    item_rects.push_back(r);
    bool selr = focus == 1 && j == item;
    if (selr) {
      UI_RoundRect(r.x, r.y, r.w, r.h, 12, t.surface2);
      UI_Fill(r.x, r.y + 14, 4, r.h - 28, t.accent);
    }
    int ty = r.y + (r.h - UI_TextH(FONT_22)) / 2;
    string v = d.value();
    int vx = r.x + r.w - 18;
    int used = 0;
    if (d.info) {
      string vv = UI_Ellipsize(FONT_18, v, r.w / 2);
      UI_TextR(FONT_18, vx, r.y + (r.h - UI_TextH(FONT_18)) / 2, t.dim, vv);
      used = UI_TextW(FONT_18, vv) + 24;
    } else if (!v.empty()) {
      SDL_Color vc = selr ? t.accent : t.dim;
      UI_TextR(FONT_22, vx, ty, vc, "\xE2\x80\xBA");
      int vw = UI_TextW(FONT_22, v);
      UI_Text(FONT_22, vx - 18 - vw, ty, selr ? t.text : t.dim, v);
      UI_TextR(FONT_22, vx - 26 - vw, ty, vc, "\xE2\x80\xB9");
      used = vw + 60;
    } else {
      UI_TextR(FONT_22, vx, ty, t.dim, "\xE2\x80\xBA");
      used = 30;
    }
    UI_Text(FONT_22, r.x + 18, ty, t.text, UI_Ellipsize(FONT_22, d.label, r.w - 36 - used));
  }
  // Description of the highlighted setting
  int di = focus == 1 ? item : -1;
  if (di >= 0 && di < (int)c.items.size()) {
    vector<string> lines = UI_Wrap(FONT_18, c.items[di].desc, pw - 48, 3);
    int lh = UI_TextH(FONT_18) + 4;
    int y = top + ph - 20 - (int)lines.size() * lh;
    UI_Fill(px + 24, y - 14, pw - 48, 1, t.border);
    for (auto &l : lines) {
      UI_Text(FONT_18, px + 24, y, t.dim, l);
      y += lh;
    }
  }

  vector<HintDef> hints;
  if (focus == 0) {
    hints.push_back({"A", "Select", HidNpadButton_A});
    hints.push_back({"B", "Close", HidNpadButton_B});
  } else {
    hints.push_back({"A", "Change", HidNpadButton_A});
    hints.push_back({"B", "Back", HidNpadButton_B});
  }
  if (W >= 1000) hints.push_back({"R", "Next category", HidNpadButton_R});
  draw_footer(hints, "");
}

void Chooser::draw_overlay() {
  const Theme &t = UI_Theme();
  if (overlay == OV_TYPE || overlay == OV_CONFIRM || overlay == OV_APPLET) {
    menu.draw();
  } else if (overlay == OV_NOTES) {
    UI_Fill(0, 0, W, H, t.scrim);
    int pw = std::min(W - 40, 820), ph = std::min(H - 80, 520);
    int px = (W - pw) / 2, py = (H - ph) / 2;
    UI_RoundRect(px, py, pw, ph, 18, t.surface);
    UI_Text(FONT_28, px + 28, py + 24, t.text, "Notes");
    UI_Text(FONT_18, px + 28, py + 28 + UI_TextH(FONT_28), t.dim,
            UI_Ellipsize(FONT_18, Library_DisplayName(notes_path), pw - 56));
    int body_y = py + 100, hint_y = py + ph - 44;
    if (notes_text.empty())
      UI_Text(FONT_22, px + 28, body_y, t.dim, "No notes yet.");
    else
      SDL_DrawTextWrapped(RENDERER, FONT_20, px + 28, body_y, pw - 56, hint_y - body_y - 12, t.text,
                          notes_text.c_str());
    int x = px + 28;
    x += UI_Hint(x, hint_y + 16, "A", "Edit", t.text) + 24;
    UI_Hint(x, hint_y + 16, "B", "Close", t.text);
  } else if (overlay == OV_SENSOR) {
    UI_Fill(0, 0, W, H, t.scrim);
    int pw = std::min(W - 40, 640), ph = 380;
    int px = (W - pw) / 2, py = (H - ph) / 2;
    UI_RoundRect(px, py, pw, ph, 18, t.surface);
    UI_Text(FONT_28, px + 28, py + 24, t.text, "Motion sensor");
    float ax, ay, az;
    bool ok = Orientation_Raw(&ax, &ay, &az);
    char buf[128];
    int y = py + 90;
    if (ok) {
      snprintf(buf, sizeof(buf), "x %+.2f    y %+.2f    z %+.2f", ax, ay, az);
      UI_Text(FONT_25, px + 28, y, t.text, buf);
    } else {
      UI_Text(FONT_22, px + 28, y, t.dim, "No reading (docked or Joy-Cons detached)");
    }
    snprintf(buf, sizeof(buf), "Detected rotation: %d\xC2\xB0", Orientation_Rotation());
    UI_Text(FONT_22, px + 28, y + 50, t.accent, buf);
    vector<string> lines = UI_Wrap(FONT_18,
                                   "Stand the Switch on its side: the rotation should change after a "
                                   "moment. If pages come out upside down, set Auto-rotate direction to "
                                   "Reversed.",
                                   pw - 56, 4);
    int ly = y + 100;
    for (auto &l : lines) {
      UI_Text(FONT_18, px + 28, ly, t.dim, l);
      ly += UI_TextH(FONT_18) + 4;
    }
    UI_Hint(px + 28, py + ph - 30, "B", "Close", t.text);
  }
}

void Chooser::draw() {
  const Theme &t = UI_Theme();
  Screen_Begin(rot);
  W = Screen_W();
  H = Screen_H();
  UI_Fill(0, 0, W, H, t.bg);
  chips.clear();
  back_rect = {0, 0, 0, 0};
  if (settings_open)
    draw_settings();
  else
    draw_library();
  draw_overlay();
  Screen_End();
}

// ── main loop ────────────────────────────────────────────────────────────────

void Chooser::run() {
  while (!quit && appletMainLoop()) {
    SDL_TextCache_NextFrame();
    SDL_PumpEvents();

    Orientation_Update();
    int r = Orientation_Rotation();
    if (r != rot) {
      rot = r;
      Screen_Begin(rot);  // touch mapping follows immediately
      W = Screen_W();
      H = Screen_H();
      layout_dirty = true;
    }
    if (layout_dirty) relayout();

    padUpdate(&pad);
    u64 kDown = padGetButtonsDown(&pad);
    u64 kHeld = padGetButtons(&pad);
    touch.update();
    int dirs = keys.update(kDown, kHeld);

    if (overlay != OV_NONE) {
      overlay_input(kDown, dirs);
    } else if (settings_open) {
      settings_input(kDown, dirs);
      settings_touch();
    } else {
      library_input(kDown, dirs);
      if (!quit && overlay == OV_NONE) library_touch();
    }
    if (quit) break;
    if (layout_dirty) relayout();

    if (fling != 0 && touch.count == 0) {
      scroll += fling;
      fling *= 0.92f;
      if (fabsf(fling) < 0.5f) fling = 0;
    }
    float max_s = std::max(0, content_h - view_h());
    scroll = std::min(std::max(scroll, 0.0f), max_s);

    pump_covers();
    draw();
  }
}

void Menu_StartChoosing() {
  Chooser *c = new Chooser();
  c->run();
  delete c;
}
