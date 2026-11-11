#pragma once

#include "PageLayout.hpp"
extern "C" {
#include <switch/kernel/thread.h>
#include <switch/result.h>
#include <switch/types.h>
}
#include <string>
#include <vector>

#include <map>

struct ArchiveCursor;

// Whole archive file copied into RAM (read-only once `ready` is set).
struct ArchiveMem {
  void *buf = nullptr;
  size_t size = 0;
  volatile int ready = 0;
};

// Image-based books: comic archives (CBZ/CBR/CBT/CB7/ZIP/RAR/7Z/TAR) and plain
// folders of images. Pages are decoded on background threads.
class CBZPageLayout : public PageLayout {
 public:
  CBZPageLayout(const char *path, int start_page, SDL_Rect viewport, int fit, bool invert,
                bool spread, bool rtl);
  ~CBZPageLayout();

  int page_count() const override;
  bool is_ready() const override { return _valid && _tex; }
  bool is_spread() const override { return _spread_mode; }
  int pages_shown() const override { return (_spread_mode && _tex_r) ? 2 : 1; }

  bool poll() override;
  void next_page(int n) override;
  void previous_page(int n) override;
  void goto_page(int num) override;
  void set_viewport(SDL_Rect vp) override;
  void set_fit(int fit) override;
  void set_invert(bool invert) override;
  void draw() override;

  void set_spread(bool on);
  void set_rtl(bool rtl) { _rtl = rtl; }

  bool is_enumerating() const { return _enumerating; }
  bool failed() const { return !_enumerating && !_valid; }
  int final_page_count() const { return _enumerating ? 0 : _page_count; }

 private:
  void load_page_texture(int page_num, int anchor);
  void apply_dims(int anchor);
  void free_ready_textures();
  void finish_enumeration();
  void apply_first_image();
  int nav_limit() const;
  void reload_current();

  std::string _archive_path;
  bool _is_dir = false;
  std::vector<std::string> _page_names;
  int _page_count = 0;
  bool _valid = false;
  bool _rtl = false;

  // Background enumeration
  Thread _enum_thread;
  bool _enumerating = false;
  bool _enum_threaded = false;
  std::vector<std::string> _enum_names;  // owned by the enum thread until done
  volatile int _enum_cancel = 0;
  volatile int _enum_done = 0;
  volatile int _enum_count = 0;
  int _start_page = 0;
  static void enum_thread_entry(void *arg);
  void do_enumerate();

  volatile int _first_image_ready = 0;
  void *_first_image_raw = nullptr;
  size_t _first_image_size = 0;
  std::string _first_image_name;

  SDL_Texture *_tex = nullptr;    // current page
  SDL_Texture *_tex_r = nullptr;  // following page (spread mode)
  int _tex_w = 1, _tex_h = 1;     // combined size in spread mode
  int _tex_cur_w = 1;             // width of the current page inside a spread
  bool _spread_mode = false;

  // Decoded neighbour pages waiting on the GPU (N+1, N+2, N-1)
  struct ReadyTex {
    int page = -1;
    SDL_Texture *tex = nullptr;
    int w = 0, h = 0;
  };
  static constexpr int N_READY = 3;
  ReadyTex _ready[N_READY];
  int wanted_pages(int *out) const;
  bool is_wanted(int page) const;
  int ready_find(int page) const;
  void ready_put(int page, SDL_Texture *tex, int w, int h);
  void ready_prune();
  SDL_Texture *take_page(int page, int *w, int *h);
  void fill_ready();

  // RAM cache of decoded pages, filled outward from the current page and
  // bounded by a memory budget (freed when the book closes).
  struct RamPage {
    unsigned char *px;
    int w, h;
    size_t bytes;
  };
  std::map<int, RamPage> _ram;
  size_t _ram_bytes = 0;
  size_t _budget = 0;
  size_t page_budget() const;
  int fill_order(int *out, int max) const;
  bool ram_insert(int page, unsigned char *px, int w, int h);
  void ram_prune();
  void ram_clear();
  SDL_Texture *ram_texture(int page, int *w, int *h);
  SDL_Texture *decode_now(int page, int *w, int *h);

  // Archive copied into RAM by a low-priority thread
  ArchiveMem _mem;
  Thread _mem_thread;
  bool _mem_threaded = false;
  volatile int _mem_cancel = 0;
  static void mem_thread_entry(void *arg);
  void do_load_mem();

  // Background decoders, each with its own forward-reading archive cursor
  static constexpr int N_PF = 2;
  struct PfSlot {
    Thread thread;
    bool active = false;
    int page = -1;
    std::string name;
    bool invert = false;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    volatile int cancel = 0;
    volatile int done = 0;
    ArchiveCursor *cursor = nullptr;
  };
  PfSlot _pf[N_PF];
  ArchiveCursor *_main_cursor = nullptr;
  static void prefetch_entry_0(void *arg);
  static void prefetch_entry_1(void *arg);
  void do_prefetch(int slot);
  void pf_reap();
  void reap_slot(int i);
  void pf_schedule();
  void cancel_prefetch();
};
