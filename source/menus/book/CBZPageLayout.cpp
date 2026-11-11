#include "CBZPageLayout.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>

#include <archive.h>
#include <archive_entry.h>
#include <SDL2/SDL_image.h>

// stb_image: used for JPEG decoding because devkitPro libjpeg lacks progressive JPEG support
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NEON   // ARM NEON SIMD for faster JPEG IDCT + YCbCr conversion on Cortex-A57
#include "stb_image.h"

#include <turbojpeg.h>

#include "memory_mode.h"
#include "natsort.h"
#include "paths.h"

extern "C"
{
#include <switch/kernel/mutex.h>
#include "common.h"
#include "SDL_helper.h"
}

extern void Log_Write(const std::string &msg);
extern void Log_Error(const std::string &msg);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool is_image_ext(const char *filename);

// macOS resource-fork junk that looks like images but is not.
static bool is_page_entry(const char *name)
{
    if (!name || !is_image_ext(name)) return false;
    if (strstr(name, "__MACOSX/")) return false;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    return !(base[0] == '.' && base[1] == '_');
}

// Scan a RAR3 archive's file headers WITHOUT decompression.
// Jumps between blocks using PACK_SIZE fseek — works for solid and non-solid RAR3.
// Populates out_names with ALL image entry names. Returns true if RAR3 format detected.
static bool enumerate_rar3_headers(const char *path, std::vector<std::string> &out_names,
                                    volatile int *out_count = nullptr)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t magic[7];
    if (fread(magic, 1, 7, f) != 7 ||
        memcmp(magic, "\x52\x61\x72\x21\x1a\x07\x00", 7) != 0)
    {
        fclose(f);
        return false;  // not RAR3
    }

    long pos = 7;
    for (;;)
    {
        if (fseek(f, pos, SEEK_SET) != 0) break;

        uint8_t h[7];
        if (fread(h, 1, 7, f) != 7) break;

        const uint8_t  btype = h[2];
        const uint16_t flags = (uint16_t)(h[3] | (h[4] << 8));
        const uint16_t hsize = (uint16_t)(h[5] | (h[6] << 8));
        if (hsize < 7) break;
        if (btype == 0x7B) break;  // end-of-archive

        uint32_t add_size = 0;
        if (flags & 0x8000)
        {
            uint8_t s[4];
            if (fseek(f, pos + 7, SEEK_SET) != 0) break;
            if (fread(s, 1, 4, f) != 4) break;
            add_size = (uint32_t)s[0] | (uint32_t)s[1]<<8 |
                       (uint32_t)s[2]<<16 | (uint32_t)s[3]<<24;
        }

        if (btype == 0x74)  // file block
        {
            uint8_t ns[2];
            if (fseek(f, pos + 26, SEEK_SET) == 0 && fread(ns, 1, 2, f) == 2)
            {
                const uint16_t name_size = (uint16_t)(ns[0] | (ns[1] << 8));
                if (name_size > 0 && name_size < 512)
                {
                    const long name_off = pos + 32 + ((flags & 0x0100) ? 8 : 0);
                    char namebuf[513];
                    if (fseek(f, name_off, SEEK_SET) == 0 &&
                        (size_t)fread(namebuf, 1, name_size, f) == name_size)
                    {
                        namebuf[name_size] = '\0';
                        for (int i = 0; namebuf[i]; i++)
                            if (namebuf[i] == '\\') namebuf[i] = '/';
                        if (is_page_entry(namebuf))
                        {
                            out_names.push_back(namebuf);
                            if (out_count)
                                __atomic_store_n(out_count, (int)out_names.size(), __ATOMIC_RELEASE);
                        }
                    }
                }
            }
        }

        const long next = pos + (long)hsize + (long)add_size;
        if (next <= pos) break;
        pos = next;
    }
    fclose(f);
    return true;  // was RAR3 format (even if no images found)
}

static bool is_image_ext(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot)
        return false;
    char ext[16] = {};
    for (int i = 0; i < 15 && dot[i]; i++)
        ext[i] = (char)tolower((unsigned char)dot[i]);

    return strcmp(ext, ".jpg")  == 0 || strcmp(ext, ".jpeg") == 0 ||
           strcmp(ext, ".png")  == 0 || strcmp(ext, ".webp") == 0 ||
           strcmp(ext, ".gif")  == 0 || strcmp(ext, ".bmp")  == 0;
}

// ---------------------------------------------------------------------------
// Page-name disk cache
// ---------------------------------------------------------------------------
// Caches the sorted list of image filenames from an archive to disk so that
// re-opening the same comic skips the full archive scan (especially important
// for large solid RAR (CBR) archives where enumeration decompresses every page).
//
// Cache file: <APP_DIR>/.pagecache/<16-hex-digit key>.lst2 (natural sort order)
// Cache key : FNV-1a hash of (path + file-size + mtime) — invalidates if the
//             file is replaced or modified.

static const char CACHE_DIR[] = PAGECACHE_DIR;

static uint64_t compute_cache_key(const char *path)
{
    struct stat st;
    uint64_t h = 14695981039346656037ULL;  // FNV-1a 64-bit offset basis
    if (stat(path, &st) == 0) {
        for (const char *p = path; *p; p++) {
            h ^= (uint8_t)*p;
            h *= 1099511628211ULL;
        }
        h ^= (uint64_t)(uint32_t)st.st_size;  h *= 1099511628211ULL;
        h ^= (uint64_t)(uint32_t)st.st_mtime; h *= 1099511628211ULL;
    }
    return h;
}

static bool load_page_cache(const char *path, std::vector<std::string> &names)
{
    uint64_t key = compute_cache_key(path);
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%016llx.lst2",
             CACHE_DIR, (unsigned long long)key);
    FILE *f = fopen(cache_path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
        if (len > 0) names.emplace_back(line, len);
    }
    fclose(f);
    if (names.empty()) return false;
    Log_Write(std::string("page_cache: hit ") + path);
    return true;
}

static void save_page_cache(const char *path, const std::vector<std::string> &names)
{
    if (names.empty()) return;
    mkdir(CACHE_DIR, 0755);  // create dir if it doesn't exist (ignores EEXIST)
    uint64_t key = compute_cache_key(path);
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%016llx.lst2",
             CACHE_DIR, (unsigned long long)key);
    FILE *f = fopen(cache_path, "w");
    if (!f) { Log_Error(std::string("save_page_cache: cannot write ") + cache_path); return; }
    for (const auto &name : names)
        fprintf(f, "%s\n", name.c_str());
    fclose(f);
}

// ---------------------------------------------------------------------------
// LRU raw image byte cache
// ---------------------------------------------------------------------------
// Thread-safe cache of recently-extracted compressed image bytes.
// Eliminates redundant archive opens for back-navigation and for prefetch
// threads that would otherwise race to extract the same page independently.
// Capacity: 10 MB; evicts the least-recently-used entry when full.

struct RawCacheEntry {
    std::string key;          // archive_path + "|" + entry_name
    uint8_t    *data = nullptr;
    size_t      size = 0;
    uint64_t    seq  = 0;     // higher = more recently used
};

struct RawImageCache {
    Mutex                      mtx;          // zero-init = unlocked on Switch
    std::vector<RawCacheEntry> entries;
    size_t                     total_bytes = 0;
    uint64_t                   seq_counter = 0;
    static constexpr size_t    MAX_BYTES   = 10 * 1024 * 1024;  // 10 MB

    // Returns a malloc'd copy of cached data (caller must SDL_free). Returns false on miss.
    bool get(const std::string &key, void **out_data, size_t *out_size)
    {
        mutexLock(&mtx);
        for (auto &e : entries) {
            if (e.key == key) {
                void *copy = SDL_malloc(e.size);
                if (copy) {
                    memcpy(copy, e.data, e.size);
                    *out_data = copy;
                    *out_size = e.size;
                    e.seq = ++seq_counter;
                }
                mutexUnlock(&mtx);
                return copy != nullptr;
            }
        }
        mutexUnlock(&mtx);
        return false;
    }

    void put(const std::string &key, const void *data, size_t size)
    {
        if (size == 0 || size > MAX_BYTES) return;
        mutexLock(&mtx);
        // If already cached, just refresh the LRU sequence
        for (auto &e : entries) {
            if (e.key == key) { e.seq = ++seq_counter; mutexUnlock(&mtx); return; }
        }
        // Evict LRU entries until we have room
        while (total_bytes + size > MAX_BYTES && !entries.empty()) {
            auto oldest = entries.begin();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->seq < oldest->seq) oldest = it;
            total_bytes -= oldest->size;
            SDL_free(oldest->data);
            entries.erase(oldest);
        }
        // Insert new entry
        RawCacheEntry e;
        e.key  = key;
        e.data = (uint8_t *)SDL_malloc(size);
        if (e.data) {
            memcpy(e.data, data, size);
            e.size = size;
            e.seq  = ++seq_counter;
            total_bytes += size;
            entries.push_back(std::move(e));
        }
        mutexUnlock(&mtx);
    }
};

static RawImageCache g_raw_cache;

// ---------------------------------------------------------------------------
// Open archive and enumerate image files into out_names (unsorted).
// Optional (first_name / first_data / first_size): when all three are non-null,
// the raw bytes of the FIRST image entry found are returned via SDL_malloc'd
// buffer (caller must SDL_free). This allows the caller to display the cover
// immediately while the rest of the scan continues in the background.
// Returns true on success.
static bool enumerate_images(const char *path, std::vector<std::string> &out_names,
                              std::string *first_name = nullptr,
                              void       **first_data = nullptr,
                              size_t      *first_size = nullptr,
                              volatile int *out_count = nullptr,
                              volatile int *cancel = nullptr)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, path, 4194304) != ARCHIVE_OK)
    {
        Log_Error(std::string("enumerate_images: archive_read_open_filename failed: ") +
                  archive_error_string(a) + " path=" + path);
        archive_read_free(a);
        return false;
    }

    bool want_first     = (first_name && first_data && first_size);
    bool first_extracted = false;

    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK)
    {
        if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE))
            break;
        if (archive_entry_filetype(entry) == AE_IFDIR)
        {
            archive_read_data_skip(a);
            continue;
        }

        const char *name = archive_entry_pathname(entry);
        if (name && is_page_entry(name))
        {
            out_names.push_back(name);
            if (out_count)
                __atomic_store_n(out_count, (int)out_names.size(), __ATOMIC_RELEASE);

            if (want_first && !first_extracted)
            {
                // Read this entry's raw bytes (no extra decompression cost for
                // solid RAR — we'd decompress it anyway during the skip).
                la_int64_t entry_size = archive_entry_size(entry);
                if (entry_size > 0)
                {
                    *first_data = SDL_malloc((size_t)entry_size);
                    if (*first_data)
                    {
                        uint8_t *dst      = (uint8_t *)*first_data;
                        size_t  remaining = (size_t)entry_size;
                        while (remaining > 0)
                        {
                            la_ssize_t n = archive_read_data(a, dst, remaining);
                            if (n <= 0) break;
                            dst       += n;
                            remaining -= (size_t)n;
                        }
                        *first_size = (size_t)entry_size - remaining;
                        *first_name = name;
                        first_extracted = true;
                        continue;  // data consumed; skip the data_skip below
                    }
                }
            }
        }
        archive_read_data_skip(a);
    }

    archive_read_free(a);
    return true;
}

// Open archive and extract image data for the entry whose pathname == target_name.
// Returns heap-allocated buffer in out_data / out_size, caller must SDL_free.
// Optional cancel_flag: if set to 1 by another thread, extraction aborts early.
static bool extract_image(const char *path, const std::string &target_name,
                          void **out_data, size_t *out_size,
                          volatile int *cancel_flag = nullptr)
{
    *out_data = nullptr;
    *out_size = 0;

    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, path, 1048576) != ARCHIVE_OK)
    {
        Log_Error(std::string("extract_image: open failed: ") + archive_error_string(a));
        archive_read_free(a);
        return false;
    }

    struct archive_entry *entry;
    bool found = false;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK)
    {
        // Check cancel flag each iteration — allows fast abort when scanning solid archives
        if (cancel_flag && __atomic_load_n(cancel_flag, __ATOMIC_ACQUIRE))
        {
            archive_read_free(a);
            return false;
        }

        if (archive_entry_filetype(entry) == AE_IFDIR)
        {
            archive_read_data_skip(a);
            continue;
        }

        const char *name = archive_entry_pathname(entry);

        if (name && target_name == name)
        {
            la_int64_t entry_size = archive_entry_size(entry);

            if (entry_size > 0)
            {
                // Known size: allocate once, read directly — no intermediate copies
                *out_size = (size_t)entry_size;
                *out_data = SDL_malloc(*out_size);
                if (*out_data)
                {
                    uint8_t *dst = (uint8_t *)*out_data;
                    size_t remaining = *out_size;
                    while (remaining > 0)
                    {
                        if (cancel_flag && __atomic_load_n(cancel_flag, __ATOMIC_ACQUIRE))
                        {
                            SDL_free(*out_data); *out_data = nullptr; *out_size = 0;
                            archive_read_free(a);
                            return false;
                        }
                        la_ssize_t n = archive_read_data(a, dst, remaining);
                        if (n <= 0) break;
                        dst += n;
                        remaining -= (size_t)n;
                    }
                    *out_size -= remaining;
                }
            }
            else
            {
                // Unknown size (some TAR/streaming formats): chunked fallback
                std::vector<uint8_t> buf;
                buf.reserve(8 * 1024 * 1024);  // pre-allocate 8 MB to avoid O(N log N) reallocations
                uint8_t chunk[65536];
                la_ssize_t n;
                while ((n = archive_read_data(a, chunk, sizeof(chunk))) > 0)
                {
                    if (cancel_flag && __atomic_load_n(cancel_flag, __ATOMIC_ACQUIRE))
                    {
                        archive_read_free(a);
                        return false;
                    }
                    buf.insert(buf.end(), chunk, chunk + n);
                }
                if (!buf.empty())
                {
                    *out_size = buf.size();
                    *out_data = SDL_malloc(*out_size);
                    if (*out_data) memcpy(*out_data, buf.data(), *out_size);
                }
            }

            if (!*out_data || *out_size == 0)
                Log_Error("extract_image: got 0 bytes for " + target_name);
            found = true;
            break;
        }
        archive_read_data_skip(a);
    }

    archive_read_free(a);

    if (!found && !(cancel_flag && __atomic_load_n(cancel_flag, __ATOMIC_ACQUIRE)))
        Log_Error("extract_image: entry not found: " + target_name + " in " + path);

    return found && *out_data != nullptr;
}

// Wrapper that checks/populates g_raw_cache before calling extract_image.
// Eliminates archive re-opens for recently-visited pages (back navigation,
// prefetch thread races, etc.). Caller must SDL_free the returned data.
// Folder-of-images mode: read <dir>/<name> straight from the SD card.
static bool read_dir_image(const char *dir, const std::string &name,
                           void **out_data, size_t *out_size)
{
    *out_data = nullptr;
    *out_size = 0;
    std::string full = std::string(dir) + "/" + name;
    FILE *f = fopen(full.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    void *buf = SDL_malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { SDL_free(buf); return false; }
    *out_data = buf;
    *out_size = got;
    return true;
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// A libarchive reader that stays open between pages. Reading pages in archive
// order (the normal case) just continues forward instead of reopening and
// re-scanning from the start, which matters hugely for solid RAR / 7z.
struct ArchiveCursor
{
    struct archive *a = nullptr;
    std::string path;
    ArchiveMem *mem = nullptr;  // whole archive in RAM, once loaded
    bool on_mem = false;

    ~ArchiveCursor() { close(); }
    void close()
    {
        if (a) archive_read_free(a);
        a = nullptr;
    }
    bool open(const char *p)
    {
        close();
        a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        on_mem = mem && __atomic_load_n(&mem->ready, __ATOMIC_ACQUIRE);
        int r = on_mem ? archive_read_open_memory(a, mem->buf, mem->size)
                       : archive_read_open_filename(a, p, 1048576);
        if (r != ARCHIVE_OK)
        {
            close();
            return false;
        }
        path = p;
        return true;
    }
    bool read(const char *p, const std::string &target, void **out, size_t *out_size,
              volatile int *cancel)
    {
        *out = nullptr;
        *out_size = 0;
        // Switch to the in-RAM copy as soon as it is available.
        if (a && !on_mem && mem && __atomic_load_n(&mem->ready, __ATOMIC_ACQUIRE)) close();
        bool fresh = false;
        for (int pass = 0; pass < 2; pass++)
        {
            if (!a || path != p || pass == 1)
            {
                if (!open(p)) return false;
                fresh = true;
            }
            struct archive_entry *e;
            int r;
            while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK)
            {
                if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE)) return false;
                if (archive_entry_filetype(e) == AE_IFDIR) continue;
                const char *name = archive_entry_pathname(e);
                if (!name || target != name) continue;
                std::vector<uint8_t> buf;
                la_int64_t sz = archive_entry_size(e);
                if (sz > 0) buf.reserve((size_t)sz);
                uint8_t chunk[65536];
                la_ssize_t n;
                while ((n = archive_read_data(a, chunk, sizeof(chunk))) > 0)
                {
                    if (cancel && __atomic_load_n(cancel, __ATOMIC_ACQUIRE)) return false;
                    buf.insert(buf.end(), chunk, chunk + n);
                }
                if (n < 0 || buf.empty()) { close(); return false; }
                *out = SDL_malloc(buf.size());
                if (!*out) return false;
                memcpy(*out, buf.data(), buf.size());
                *out_size = buf.size();
                return true;
            }
            if (r != ARCHIVE_EOF) close();  // broken stream: reopen next time
            if (fresh) break;               // scanned the whole archive already
        }
        return false;
    }
};

static bool extract_image_cached(const char *archive_path,
                                 const std::string &target_name,
                                 void **out_data, size_t *out_size,
                                 volatile int *cancel_flag = nullptr,
                                 bool is_dir = false,
                                 ArchiveCursor *cursor = nullptr)
{
    std::string key = std::string(archive_path) + "|" + target_name;
    if (g_raw_cache.get(key, out_data, out_size))
        return true;

    bool ok;
    if (is_dir)
        ok = read_dir_image(archive_path, target_name, out_data, out_size);
    else if (cursor)
        ok = cursor->read(archive_path, target_name, out_data, out_size, cancel_flag);
    else
        ok = extract_image(archive_path, target_name, out_data, out_size, cancel_flag);
    if (!ok)
        return false;

    if (*out_data && *out_size)
        g_raw_cache.put(key, *out_data, *out_size);

    return true;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// libjpeg-turbo: 2-3x faster than stb_image on the Switch's Cortex-A57.
static unsigned char *decode_jpeg_turbo(const void *data, size_t dsize, int *out_w, int *out_h)
{
    tjhandle tj = tjInitDecompress();
    if (!tj) return nullptr;
    int w = 0, h = 0, subsamp = 0, cs = 0;
    if (tjDecompressHeader3(tj, (const unsigned char *)data, (unsigned long)dsize,
                            &w, &h, &subsamp, &cs) != 0)
    {
        tjDestroy(tj);
        return nullptr;
    }
    unsigned char *pixels = (unsigned char *)malloc((size_t)w * h * 4);
    if (!pixels) { tjDestroy(tj); return nullptr; }
    if (tjDecompress2(tj, (const unsigned char *)data, (unsigned long)dsize,
                      pixels, w, 0, h, TJPF_RGBA, TJFLAG_FASTDCT) != 0)
    {
        free(pixels);
        tjDestroy(tj);
        return nullptr;
    }
    tjDestroy(tj);
    *out_w = w;
    *out_h = h;
    return pixels;
}

// Decodes any supported image into a malloc'd RGBA buffer (caller free()s).
// Thread-safe: no renderer access.
static unsigned char *decode_rgba(const void *data, size_t size, int *w, int *h)
{
    const uint8_t *d = (const uint8_t *)data;
    if (size >= 2 && d[0] == 0xFF && d[1] == 0xD8)
    {
        unsigned char *px = decode_jpeg_turbo(data, size, w, h);
        if (px) return px;
    }
    int ch = 0;
    unsigned char *px = stbi_load_from_memory(d, (int)size, w, h, &ch, 4);
    if (px) return px;

    // WebP and anything else SDL_image understands.
    SDL_RWops *rw = SDL_RWFromConstMem(data, (int)size);
    if (!rw) return nullptr;
    SDL_Surface *s = IMG_Load_RW(rw, 1);
    if (!s) return nullptr;
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(s);
    if (!c) return nullptr;
    px = (unsigned char *)malloc((size_t)c->w * c->h * 4);
    if (px)
    {
        for (int y = 0; y < c->h; y++)
            memcpy(px + (size_t)y * c->w * 4, (uint8_t *)c->pixels + (size_t)y * c->pitch,
                   (size_t)c->w * 4);
        *w = c->w;
        *h = c->h;
    }
    SDL_FreeSurface(c);
    return px;
}

// Uploads RGBA pixels; very tall pages (webtoons) are scaled to fit the GPU limit.
static SDL_Texture *rgba_to_texture(unsigned char *px, int w, int h)
{
    static int max_tex = 0;
    if (!max_tex)
    {
        SDL_RendererInfo info;
        max_tex = (SDL_GetRendererInfo(RENDERER, &info) == 0 && info.max_texture_height > 0)
                      ? std::min(info.max_texture_width, info.max_texture_height) : 8192;
    }
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom(px, w, h, 32, w * 4,
                                              0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!s) return nullptr;
    SDL_Texture *t = nullptr;
    if (w > max_tex || h > max_tex)
    {
        float sc = (float)max_tex / (float)std::max(w, h);
        SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, std::max(1, (int)(w * sc)),
                                                        std::max(1, (int)(h * sc)), 32,
                                                        SDL_PIXELFORMAT_RGBA32);
        if (d)
        {
            SDL_BlitScaled(s, nullptr, d, nullptr);
            t = SDL_CreateTextureFromSurface(RENDERER, d);
            SDL_FreeSurface(d);
        }
    }
    else
    {
        t = SDL_CreateTextureFromSurface(RENDERER, s);
    }
    SDL_FreeSurface(s);
    return t;
}


// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

void CBZPageLayout::enum_thread_entry(void *arg)
{
    static_cast<CBZPageLayout *>(arg)->do_enumerate();
}

void CBZPageLayout::do_enumerate()
{
    std::vector<std::string> &names = _enum_names;
    auto publish_first = [&](void) {
        if (names.empty()) return;
        void *fd = nullptr;
        size_t fs = 0;
        if (extract_image_cached(_archive_path.c_str(), names[0], &fd, &fs, &_enum_cancel, _is_dir))
        {
            _first_image_raw = fd;
            _first_image_size = fs;
            _first_image_name = names[0];
            __atomic_store_n(&_first_image_ready, 1, __ATOMIC_RELEASE);
        }
    };

    if (_is_dir)
    {
        DIR *d = opendir(_archive_path.c_str());
        if (d)
        {
            while (struct dirent *e = readdir(d))
                if (e->d_type != DT_DIR && is_page_entry(e->d_name))
                    names.push_back(e->d_name);
            closedir(d);
        }
        std::sort(names.begin(), names.end(), NaturalLess());
        __atomic_store_n(&_enum_count, (int)names.size(), __ATOMIC_RELEASE);
        publish_first();
    }
    else if (load_page_cache(_archive_path.c_str(), names))
    {
        __atomic_store_n(&_enum_count, (int)names.size(), __ATOMIC_RELEASE);
        publish_first();
    }
    else if (enumerate_rar3_headers(_archive_path.c_str(), names, &_enum_count))
    {
        // RAR3: header-only scan, no decompression.
        std::sort(names.begin(), names.end(), NaturalLess());
        __atomic_store_n(&_enum_count, (int)names.size(), __ATOMIC_RELEASE);
        publish_first();
        save_page_cache(_archive_path.c_str(), names);
    }
    else
    {
        // Generic libarchive scan; grabs the first image's bytes on the way.
        void *fd = nullptr;
        size_t fs = 0;
        enumerate_images(_archive_path.c_str(), names, &_first_image_name, &fd, &fs,
                         &_enum_count, &_enum_cancel);
        if (fd)
        {
            _first_image_raw = fd;
            _first_image_size = fs;
            __atomic_store_n(&_first_image_ready, 1, __ATOMIC_RELEASE);
        }
        std::sort(names.begin(), names.end(), NaturalLess());
        if (!__atomic_load_n(&_enum_cancel, __ATOMIC_ACQUIRE))
            save_page_cache(_archive_path.c_str(), names);
    }
    __atomic_store_n(&_enum_done, 1, __ATOMIC_RELEASE);
}

void CBZPageLayout::apply_first_image()
{
    if (!_first_image_raw || !_first_image_size) return;
    int w = 0, h = 0;
    unsigned char *px = decode_rgba(_first_image_raw, _first_image_size, &w, &h);
    SDL_free(_first_image_raw);
    _first_image_raw = nullptr;
    _first_image_size = 0;
    if (!px) return;
    if (_invert) InvertLuminance(px, w, h, w * 4, 4);
    _tex = rgba_to_texture(px, w, h);
    free(px);
    if (!_tex) return;
    _tex_w = _tex_cur_w = w;
    _tex_h = h;
    _current_page = 0;
    apply_dims(ANCHOR_TOP);
    _valid = true;
}

void CBZPageLayout::finish_enumeration()
{
    if (!_enumerating) return;
    if (_enum_threaded)
    {
        threadWaitForExit(&_enum_thread);
        threadClose(&_enum_thread);
    }
    _enumerating = false;
    _page_names = std::move(_enum_names);
    _enum_names.clear();
    _page_count = (int)_page_names.size();

    if (_page_count == 0)
    {
        Log_Error("CBZPageLayout: no images in " + _archive_path);
        FreeTextureIfNeeded(&_tex);
        _valid = false;
        return;
    }
    Log_Write("CBZPageLayout: pages=" + std::to_string(_page_count) + " path=" + _archive_path);

    int start = std::max(0, std::min(_start_page, _page_count - 1));
    bool cover_on_screen = _valid && _tex && start == 0 && !_spread_mode &&
                           _first_image_name == _page_names[0];
    if (_first_image_raw) { SDL_free(_first_image_raw); _first_image_raw = nullptr; }
    if (cover_on_screen)
    {
        _current_page = 0;
        pf_schedule();
    }
    else
    {
        _valid = true;  // load_page_texture/prefetch need a valid layout
        FreeTextureIfNeeded(&_tex);
        _current_page = -1;
        load_page_texture(start, ANCHOR_TOP);
        _valid = _tex != nullptr;
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CBZPageLayout::CBZPageLayout(const char *path, int start_page, SDL_Rect vp, int fit,
                             bool invert, bool spread, bool rtl)
    : PageLayout()
{
    _archive_path = path;
    while (_archive_path.size() > 1 && _archive_path.back() == '/') _archive_path.pop_back();
    _is_dir = path_is_dir(_archive_path.c_str());
    _start_page = start_page;
    _invert = invert;
    _spread_mode = spread;
    _rtl = rtl;
    _view.fit = fit;
    _view.set_viewport(vp.x, vp.y, vp.w, vp.h);
    _budget = App_CacheBudget();
    _main_cursor = new ArchiveCursor();
    _main_cursor->mem = &_mem;
    for (int i = 0; i < N_PF; i++)
    {
        _pf[i].cursor = new ArchiveCursor();
        _pf[i].cursor->mem = &_mem;
    }

    // Copy the whole archive into RAM in the background (lowest priority).
    if (!_is_dir &&
        R_SUCCEEDED(threadCreate(&_mem_thread, mem_thread_entry, this, nullptr, 0x10000, 0x3B, 2)))
    {
        if (R_SUCCEEDED(threadStart(&_mem_thread)))
            _mem_threaded = true;
        else
            threadClose(&_mem_thread);
    }

    _enumerating = true;
    if (R_SUCCEEDED(threadCreate(&_enum_thread, enum_thread_entry, this, nullptr, 0x40000, 0x30, 2)))
    {
        if (R_SUCCEEDED(threadStart(&_enum_thread)))
        {
            _enum_threaded = true;
            return;
        }
        threadClose(&_enum_thread);
    }
    // Fallback: enumerate synchronously.
    do_enumerate();
    finish_enumeration();
}

CBZPageLayout::~CBZPageLayout()
{
    if (_mem_threaded)
    {
        __atomic_store_n(&_mem_cancel, 1, __ATOMIC_RELEASE);
        threadWaitForExit(&_mem_thread);
        threadClose(&_mem_thread);
        _mem_threaded = false;
    }
    if (_enumerating && _enum_threaded)
    {
        __atomic_store_n(&_enum_cancel, 1, __ATOMIC_RELEASE);
        threadWaitForExit(&_enum_thread);
        threadClose(&_enum_thread);
        _enumerating = false;
    }
    if (_first_image_raw) { SDL_free(_first_image_raw); _first_image_raw = nullptr; }
    cancel_prefetch();
    // Cursors may read from the in-RAM archive: close them before freeing it.
    for (int i = 0; i < N_PF; i++) { delete _pf[i].cursor; _pf[i].cursor = nullptr; }
    delete _main_cursor;
    _main_cursor = nullptr;
    ram_clear();
    free(_mem.buf);
    _mem.buf = nullptr;
    FreeTextureIfNeeded(&_tex);
    FreeTextureIfNeeded(&_tex_r);
}

void CBZPageLayout::mem_thread_entry(void *arg) { static_cast<CBZPageLayout *>(arg)->do_load_mem(); }

void CBZPageLayout::do_load_mem()
{
    struct stat st;
    if (stat(_archive_path.c_str(), &st) != 0 || st.st_size <= 0) return;
    size_t size = (size_t)st.st_size;
    // Only when it leaves at least half of the budget for decoded pages.
    if (size > _budget / 2 || size > (1ull << 30)) return;
    unsigned char *buf = (unsigned char *)malloc(size);
    if (!buf) return;
    FILE *f = fopen(_archive_path.c_str(), "rb");
    if (!f) { free(buf); return; }
    size_t got = 0;
    while (got < size)
    {
        if (__atomic_load_n(&_mem_cancel, __ATOMIC_ACQUIRE)) break;
        size_t want = std::min<size_t>(4u << 20, size - got);
        size_t n = fread(buf + got, 1, want, f);
        if (n == 0) break;
        got += n;
    }
    fclose(f);
    if (got != size) { free(buf); return; }
    _mem.buf = buf;
    _mem.size = size;
    __atomic_store_n(&_mem.ready, 1, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// Page loading
// ---------------------------------------------------------------------------

int CBZPageLayout::page_count() const
{
    return _enumerating ? __atomic_load_n(&_enum_count, __ATOMIC_ACQUIRE) : _page_count;
}

int CBZPageLayout::nav_limit() const { return _enumerating ? 0 : _page_count; }

void CBZPageLayout::apply_dims(int anchor)
{
    _view.set_content((float)_tex_w, (float)_tex_h);
    _view.reset(anchor);
}

// ── RAM page cache ───────────────────────────────────────────────────────────

size_t CBZPageLayout::page_budget() const
{
    size_t archive = __atomic_load_n(&_mem.ready, __ATOMIC_ACQUIRE) ? _mem.size : 0;
    return _budget > archive ? _budget - archive : 0;
}

// Pages to have decoded, nearest first with a forward bias: +1 +2 -1 +3 +4 -2 ...
int CBZPageLayout::fill_order(int *out, int max) const
{
    if (_enumerating || !_valid) return 0;
    int n = 0, f = _current_page + 1, b = _current_page - 1;
    while (n < max && (f < _page_count || b >= 0))
    {
        for (int k = 0; k < 2 && n < max && f < _page_count; k++) out[n++] = f++;
        if (n < max && b >= 0) out[n++] = b--;
    }
    return n;
}

static int page_dist(int a, int b) { return a > b ? a - b : b - a; }

bool CBZPageLayout::ram_insert(int page, unsigned char *px, int w, int h)
{
    if (_ram.count(page)) { free(px); return true; }
    size_t bytes = (size_t)w * h * 4;
    int d = page_dist(page, _current_page);
    while (_ram_bytes + bytes > page_budget())
    {
        auto far = _ram.end();
        for (auto it = _ram.begin(); it != _ram.end(); ++it)
            if (far == _ram.end() || page_dist(it->first, _current_page) > page_dist(far->first, _current_page))
                far = it;
        if (far == _ram.end() || page_dist(far->first, _current_page) <= d) { free(px); return false; }
        _ram_bytes -= far->second.bytes;
        free(far->second.px);
        _ram.erase(far);
    }
    _ram[page] = {px, w, h, bytes};
    _ram_bytes += bytes;
    return true;
}

void CBZPageLayout::ram_prune()
{
    while (_ram_bytes > page_budget() && !_ram.empty())
    {
        auto far = _ram.begin();
        for (auto it = _ram.begin(); it != _ram.end(); ++it)
            if (page_dist(it->first, _current_page) > page_dist(far->first, _current_page)) far = it;
        _ram_bytes -= far->second.bytes;
        free(far->second.px);
        _ram.erase(far);
    }
}

void CBZPageLayout::ram_clear()
{
    for (auto &kv : _ram) free(kv.second.px);
    _ram.clear();
    _ram_bytes = 0;
}

SDL_Texture *CBZPageLayout::ram_texture(int page, int *w, int *h)
{
    auto it = _ram.find(page);
    if (it == _ram.end()) return nullptr;
    SDL_Texture *t = rgba_to_texture(it->second.px, it->second.w, it->second.h);
    if (t) { *w = it->second.w; *h = it->second.h; }
    return t;
}

// Decodes a page on the calling (main) thread and keeps a copy in RAM.
SDL_Texture *CBZPageLayout::decode_now(int page, int *w, int *h)
{
    void *data = nullptr;
    size_t size = 0;
    if (!extract_image_cached(_archive_path.c_str(), _page_names[page], &data, &size, nullptr, _is_dir,
                              _main_cursor))
        return nullptr;
    unsigned char *px = decode_rgba(data, size, w, h);
    SDL_free(data);
    if (!px) { Log_Error("CBZPageLayout: cannot decode " + _page_names[page]); return nullptr; }
    if (_invert) InvertLuminance(px, *w, *h, *w * 4, 4);
    SDL_Texture *t = rgba_to_texture(px, *w, *h);
    ram_insert(page, px, *w, *h);  // takes ownership
    return t;
}

// ── GPU-ready neighbours (N+1, N+2, N-1) ─────────────────────────────────────

int CBZPageLayout::wanted_pages(int *out) const
{
    if (_enumerating || _spread_mode || !_valid) return 0;
    int n = 0;
    const int cand[] = {_current_page + 1, _current_page + 2, _current_page - 1};
    for (int p : cand)
        if (p >= 0 && p < _page_count) out[n++] = p;
    return n;
}

bool CBZPageLayout::is_wanted(int page) const
{
    int w[3];
    int n = wanted_pages(w);
    for (int i = 0; i < n; i++)
        if (w[i] == page) return true;
    return false;
}

int CBZPageLayout::ready_find(int page) const
{
    for (int i = 0; i < N_READY; i++)
        if (_ready[i].tex && _ready[i].page == page) return i;
    return -1;
}

void CBZPageLayout::ready_put(int page, SDL_Texture *tex, int w, int h)
{
    int slot = ready_find(page);
    if (slot < 0)
        for (int i = 0; i < N_READY && slot < 0; i++)
            if (!_ready[i].tex) slot = i;
    if (slot < 0)
        for (int i = 0; i < N_READY && slot < 0; i++)
            if (!is_wanted(_ready[i].page)) slot = i;
    if (slot < 0) { SDL_DestroyTexture(tex); return; }
    FreeTextureIfNeeded(&_ready[slot].tex);
    _ready[slot] = {page, tex, w, h};
}

void CBZPageLayout::ready_prune()
{
    for (int i = 0; i < N_READY; i++)
        if (_ready[i].tex && !is_wanted(_ready[i].page))
        {
            FreeTextureIfNeeded(&_ready[i].tex);
            _ready[i].page = -1;
        }
}

void CBZPageLayout::free_ready_textures()
{
    for (int i = 0; i < N_READY; i++)
    {
        FreeTextureIfNeeded(&_ready[i].tex);
        _ready[i].page = -1;
    }
}

// Uploads at most one decoded neighbour per frame so the UI never stutters.
void CBZPageLayout::fill_ready()
{
    int want[3];
    int n = wanted_pages(want);
    for (int k = 0; k < n; k++)
    {
        if (ready_find(want[k]) >= 0) continue;
        int w = 0, h = 0;
        if (SDL_Texture *t = ram_texture(want[k], &w, &h))
        {
            ready_put(want[k], t, w, h);
            return;
        }
    }
}

// Texture for `page`, fastest source first. Ready textures are handed over.
SDL_Texture *CBZPageLayout::take_page(int page, int *w, int *h)
{
    int r = ready_find(page);
    if (r >= 0)
    {
        SDL_Texture *t = _ready[r].tex;
        *w = _ready[r].w;
        *h = _ready[r].h;
        _ready[r].tex = nullptr;
        _ready[r].page = -1;
        return t;
    }
    if (SDL_Texture *t = ram_texture(page, w, h)) return t;
    for (int i = 0; i < N_PF; i++)
    {
        PfSlot &s = _pf[i];
        if (!s.active || s.page != page || __atomic_load_n(&s.cancel, __ATOMIC_ACQUIRE)) continue;
        threadWaitForExit(&s.thread);  // already decoding exactly this page
        threadClose(&s.thread);
        s.active = false;
        reap_slot(i);
        return ram_texture(page, w, h);
    }
    return nullptr;
}

void CBZPageLayout::load_page_texture(int page_num, int anchor)
{
    int limit = nav_limit();
    if (limit <= 0) return;
    int target = std::max(0, std::min(page_num, limit - 1));
    bool do_spread = _spread_mode && target + 1 < limit;

    int w = 0, h = 0;
    SDL_Texture *t = take_page(target, &w, &h);
    if (!t) t = decode_now(target, &w, &h);
    if (!t)
    {
        Log_Error("CBZPageLayout: failed to load page " + std::to_string(target));
        return;
    }

    // Keep the page we are leaving on the GPU: turning back is then instant.
    int old_page = _current_page;
    _current_page = target;
    if (_tex && !_tex_r && !do_spread && old_page != target && is_wanted(old_page))
        ready_put(old_page, _tex, _tex_cur_w, _tex_h);
    else
        FreeTextureIfNeeded(&_tex);
    _tex = nullptr;
    FreeTextureIfNeeded(&_tex_r);

    _tex = t;
    _tex_w = _tex_cur_w = w;
    _tex_h = h;
    if (do_spread)
    {
        int rw = 0, rh = 0;
        _tex_r = ram_texture(target + 1, &rw, &rh);
        if (!_tex_r) _tex_r = decode_now(target + 1, &rw, &rh);
        if (_tex_r && rh > 0 && h > 0)
        {
            int H = std::max(h, rh);
            _tex_cur_w = (int)lroundf(w * (float)H / h);
            _tex_w = _tex_cur_w + (int)lroundf(rw * (float)H / rh);
            _tex_h = H;
        }
    }
    apply_dims(anchor);
    ram_prune();
    ready_prune();
    pf_schedule();
}

void CBZPageLayout::reload_current()
{
    if (_enumerating || !_valid) return;
    cancel_prefetch();
    ram_clear();
    FreeTextureIfNeeded(&_tex);
    FreeTextureIfNeeded(&_tex_r);
    int p = _current_page;
    _current_page = -1;
    load_page_texture(p, ANCHOR_TOP);
}

// ---------------------------------------------------------------------------
// Background decoders: fill the RAM cache outward from the current page
// ---------------------------------------------------------------------------

void CBZPageLayout::prefetch_entry_0(void *arg) { ((CBZPageLayout *)arg)->do_prefetch(0); }
void CBZPageLayout::prefetch_entry_1(void *arg) { ((CBZPageLayout *)arg)->do_prefetch(1); }

void CBZPageLayout::do_prefetch(int i)
{
    PfSlot &s = _pf[i];
    void *data = nullptr;
    size_t size = 0;
    if (!__atomic_load_n(&s.cancel, __ATOMIC_ACQUIRE) &&
        extract_image_cached(_archive_path.c_str(), s.name, &data, &size, &s.cancel, _is_dir,
                             s.cursor))
    {
        if (!__atomic_load_n(&s.cancel, __ATOMIC_ACQUIRE))
        {
            int w = 0, h = 0;
            unsigned char *px = decode_rgba(data, size, &w, &h);
            if (px && s.invert) InvertLuminance(px, w, h, w * 4, 4);
            s.px = px;
            s.w = w;
            s.h = h;
        }
        SDL_free(data);
    }
    __atomic_store_n(&s.done, 1, __ATOMIC_RELEASE);
}

// Moves a finished (already joined) slot's pixels into the RAM cache.
void CBZPageLayout::reap_slot(int i)
{
    PfSlot &s = _pf[i];
    if (s.px && s.invert == _invert && !__atomic_load_n(&s.cancel, __ATOMIC_ACQUIRE))
        ram_insert(s.page, s.px, s.w, s.h);  // takes ownership
    else
        free(s.px);
    s.px = nullptr;
    s.page = -1;
}

void CBZPageLayout::pf_reap()
{
    for (int i = 0; i < N_PF; i++)
    {
        PfSlot &s = _pf[i];
        if (!s.active || !__atomic_load_n(&s.done, __ATOMIC_ACQUIRE)) continue;
        threadWaitForExit(&s.thread);
        threadClose(&s.thread);
        s.active = false;
        reap_slot(i);
    }
}

void CBZPageLayout::pf_schedule()
{
    static void (*const entries[N_PF])(void *) = {prefetch_entry_0, prefetch_entry_1};
    static const int cores[N_PF] = {1, 2};

    for (int i = 0; i < N_PF; i++)
        if (_pf[i].active && _pf[i].invert != _invert)
            __atomic_store_n(&_pf[i].cancel, 1, __ATOMIC_RELEASE);

    static int order[4096];
    int n = fill_order(order, 4096);
    size_t avg = _ram.empty() ? (size_t)8 << 20 : _ram_bytes / _ram.size();
    int in_flight = 0;
    for (int i = 0; i < N_PF; i++)
        if (_pf[i].active) in_flight++;

    // Distance of the farthest cached page: anything nearer may replace it.
    int far = -1;
    for (auto &kv : _ram) far = std::max(far, page_dist(kv.first, _current_page));

    for (int k = 0; k < n; k++)
    {
        int page = order[k];
        if (_ram.count(page) || ready_find(page) >= 0) continue;
        bool busy = false;
        for (int i = 0; i < N_PF; i++)
            if (_pf[i].active && _pf[i].page == page) busy = true;
        if (busy) continue;
        bool room = _ram_bytes + (size_t)(in_flight + 1) * avg <= page_budget();
        if (!room && page_dist(page, _current_page) >= far) return;  // cache is full of nearer pages
        int idle = -1;
        for (int i = 0; i < N_PF && idle < 0; i++)
            if (!_pf[i].active) idle = i;
        if (idle < 0) return;
        PfSlot &s = _pf[idle];
        s.page = page;
        s.name = _page_names[page];
        s.invert = _invert;
        s.px = nullptr;
        s.w = s.h = 0;
        __atomic_store_n(&s.cancel, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s.done, 0, __ATOMIC_RELEASE);
        if (R_FAILED(threadCreate(&s.thread, entries[idle], this, nullptr, 0x80000, 0x30, cores[idle])))
            return;
        if (R_FAILED(threadStart(&s.thread)))
        {
            threadClose(&s.thread);
            return;
        }
        s.active = true;
        in_flight++;
    }
}

// Blocking: used on teardown and when every decoded page becomes invalid.
void CBZPageLayout::cancel_prefetch()
{
    for (int i = 0; i < N_PF; i++)
        if (_pf[i].active) __atomic_store_n(&_pf[i].cancel, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < N_PF; i++)
    {
        PfSlot &s = _pf[i];
        if (s.active)
        {
            threadWaitForExit(&s.thread);
            threadClose(&s.thread);
            s.active = false;
        }
        free(s.px);
        s.px = nullptr;
        s.page = -1;
    }
    free_ready_textures();
}

bool CBZPageLayout::poll()
{
    bool changed = false;
    if (_enumerating)
    {
        if (!_valid && __atomic_load_n(&_first_image_ready, __ATOMIC_ACQUIRE) && _first_image_raw)
        {
            apply_first_image();
            changed = true;
        }
        if (__atomic_load_n(&_enum_done, __ATOMIC_ACQUIRE))
        {
            finish_enumeration();
            changed = true;
        }
        return changed;
    }
    pf_reap();
    ram_prune();
    ready_prune();
    fill_ready();
    pf_schedule();
    return changed;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void CBZPageLayout::next_page(int n)
{
    if (!_valid || _enumerating) return;
    int target = _current_page + (_spread_mode ? 2 * n : n);
    if (target >= _page_count) return;
    load_page_texture(target, ANCHOR_TOP);
}

void CBZPageLayout::previous_page(int n)
{
    if (!_valid || _enumerating) return;
    int target = std::max(0, _current_page - (_spread_mode ? 2 * n : n));
    if (target == _current_page) return;
    load_page_texture(target, ANCHOR_TOP);
}

void CBZPageLayout::goto_page(int num)
{
    if (_enumerating) { _start_page = num; return; }
    if (!_valid || _page_count <= 0) return;
    int target = std::max(0, std::min(num, _page_count - 1));
    if (target == _current_page) { apply_dims(ANCHOR_TOP); return; }
    load_page_texture(target, ANCHOR_TOP);
}

void CBZPageLayout::set_viewport(SDL_Rect vp)
{
    _view.set_viewport(vp.x, vp.y, vp.w, vp.h);
    if (_tex) apply_dims(ANCHOR_TOP);
}

void CBZPageLayout::set_fit(int fit)
{
    _view.fit = fit;
    if (_tex) apply_dims(ANCHOR_TOP);
}

void CBZPageLayout::set_invert(bool invert)
{
    if (invert == _invert) return;
    _invert = invert;
    reload_current();
}

void CBZPageLayout::set_spread(bool on)
{
    if (on == _spread_mode) return;
    _spread_mode = on;
    reload_current();
}

void CBZPageLayout::draw()
{
    if (!_tex) return;
    SDL_Rect r = _view.rect();
    if (!_tex_r)
    {
        SDL_RenderCopy(RENDERER, _tex, nullptr, &r);
        return;
    }
    // Two-page spread: page order follows the reading direction.
    float sc = (float)r.w / (float)_tex_w;
    int cur_w = (int)lroundf(_tex_cur_w * sc);
    SDL_Texture *left = _rtl ? _tex_r : _tex;
    SDL_Texture *right = _rtl ? _tex : _tex_r;
    int left_w = _rtl ? r.w - cur_w : cur_w;
    SDL_Rect dl = {r.x, r.y, left_w, r.h};
    SDL_Rect dr = {r.x + left_w, r.y, r.w - left_w, r.h};
    SDL_RenderCopy(RENDERER, left, nullptr, &dl);
    SDL_RenderCopy(RENDERER, right, nullptr, &dr);
}
