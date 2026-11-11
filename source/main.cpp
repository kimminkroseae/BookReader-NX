#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>

#include <cstring>
#include <exception>
#include <strings.h>
#include <fstream>
#include <string>
#include <vector>

#include "library.h"
#include "menu_book_reader.h"
#include "mupdf_ctx.h"
#include "orientation.h"
#include "paths.h"
#include "settings.h"
#include "ui.h"

extern "C" {
#include "MenuChooser.h"
#include "SDL_helper.h"
#include "common.h"
#include "fs.h"
#include "textures.h"
}

// ── Logging ──────────────────────────────────────────────────────────────────

static std::ofstream logFile;

void Log_Init() {
  logFile.open(LOG_FILE, std::ios::out | std::ios::trunc);
  if (logFile.is_open()) logFile << "=== BookReader log ===" << std::endl;
}

void Log_Write(const std::string &msg) {
  if (logFile.is_open()) logFile << "[LOG] " << msg << std::endl;
}

void Log_Error(const std::string &msg) {
  if (logFile.is_open()) logFile << "[ERROR] " << msg << std::endl;
}

void Log_Close() {
  if (logFile.is_open()) {
    logFile << "=== end ===" << std::endl;
    logFile.close();
  }
}

// ── Crash trace ──────────────────────────────────────────────────────────────

extern "C" void _start(void);

struct BtState {
  void **cur;
  void **end;
};

static _Unwind_Reason_Code bt_cb(struct _Unwind_Context *c, void *arg) {
  BtState *s = static_cast<BtState *>(arg);
  if (s->cur == s->end) return _URC_END_OF_STACK;
  *s->cur++ = reinterpret_cast<void *>(_Unwind_GetIP(c));
  return _URC_NO_REASON;
}

static void terminate_handler() {
  Log_Error("TERMINATE: uncaught exception");
  void *buf[32];
  BtState st = {buf, buf + 32};
  _Unwind_Backtrace(bt_cb, &st);
  char line[64];
  snprintf(line, sizeof(line), "load_base=0x%lx", (unsigned long)(uintptr_t)_start);
  Log_Error(line);
  for (void **p = buf; p < st.cur; p++) {
    snprintf(line, sizeof(line), "  0x%016lx", (unsigned long)(uintptr_t)*p);
    Log_Error(line);
  }
  Log_Close();
  exit(1);
}

// ── Globals ──────────────────────────────────────────────────────────────────

SDL_Renderer *RENDERER;
SDL_Window *WINDOW;
TTF_Font *FONT_14, *FONT_16, *FONT_18, *FONT_20, *FONT_22, *FONT_25, *FONT_28, *FONT_32,
    *FONT_40;

static bool s_time = false, s_psm = false, s_pl = false, s_romfs = false;

// ── Data folder migration ────────────────────────────────────────────────────
// Older versions kept everything in /switch/WookReader (shown by hbmenu as an
// entry) or /switch/.WookReader. Move it into the hidden /switch/.BookReader.

static bool is_dir(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ends_with(const std::string &s, const char *suf) {
  size_t n = strlen(suf);
  if (s.size() < n) return false;
  return strcasecmp(s.c_str() + s.size() - n, suf) == 0;
}

static void move_children(const std::string &from, const std::string &to) {
  DIR *d = opendir(from.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (struct dirent *e = readdir(d))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
  closedir(d);
  for (auto &n : names) rename((from + "/" + n).c_str(), (to + "/" + n).c_str());
}

static void migrate_old_data() {
  if (is_dir(APP_DIR)) return;
  // Same layout, just renamed: move the whole folder.
  if (is_dir(WOOK_APP_DIR) && rename(WOOK_APP_DIR, APP_DIR) == 0) return;
  FS_RecursiveMakeDir(BOOKS_DIR);
  const char *sources[] = {OLD_APP_DIR, OLDER_APP_DIR};
  for (const char *src : sources) {
    if (!is_dir(src)) continue;
    DIR *d = opendir(src);
    if (!d) continue;
    std::vector<std::string> names;
    while (struct dirent *e = readdir(d))
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    for (auto &n : names) {
      std::string from = std::string(src) + "/" + n;
      if (ends_with(n, ".nro")) continue;  // leave the app where it is
      if (n == "books") {
        move_children(from, BOOKS_DIR);
        rmdir(from.c_str());
      } else if (n[0] == '.' || ends_with(n, ".cfg") || ends_with(n, ".txt") ||
                 ends_with(n, ".lst")) {
        rename(from.c_str(), (std::string(APP_DIR) + "/" + n).c_str());  // app data
      } else {
        rename(from.c_str(), (std::string(BOOKS_DIR) + "/" + n).c_str());  // user books
      }
    }
    rmdir(src);  // only succeeds if nothing (e.g. the .nro) is left
    break;
  }
}

// ── Fonts ────────────────────────────────────────────────────────────────────

static TTF_Font *open_font(const PlFontData *sys, int size) {
  TTF_Font *f = nullptr;
  if (sys) f = TTF_OpenFontRW(SDL_RWFromConstMem(sys->address, sys->size), 1, size);
  if (!f) f = TTF_OpenFont("romfs:/resources/font/Roboto-Light.ttf", size);
  if (f) TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
  return f;
}

static bool load_fonts() {
  // The Switch system font covers Latin + Japanese, so manga titles render.
  PlFontData sys;
  bool have_sys = s_pl && R_SUCCEEDED(plGetSharedFontByType(&sys, PlSharedFontType_Standard));
  const PlFontData *p = have_sys ? &sys : nullptr;
  FONT_14 = open_font(p, 14);
  FONT_16 = open_font(p, 16);
  FONT_18 = open_font(p, 18);
  FONT_20 = open_font(p, 20);
  FONT_22 = open_font(p, 22);
  FONT_25 = open_font(p, 25);
  FONT_28 = open_font(p, 28);
  FONT_32 = open_font(p, 32);
  FONT_40 = open_font(p, 40);
  return FONT_14 && FONT_16 && FONT_18 && FONT_20 && FONT_22 && FONT_25 && FONT_28 &&
         FONT_32 && FONT_40;
}

static void close_fonts() {
  TTF_Font **fonts[] = {&FONT_14, &FONT_16, &FONT_18, &FONT_20, &FONT_22,
                        &FONT_25, &FONT_28, &FONT_32, &FONT_40};
  for (auto f : fonts) {
    if (*f) TTF_CloseFont(*f);
    *f = nullptr;
  }
}

// ── Services ─────────────────────────────────────────────────────────────────

static void Term_Services() {
  Log_Write("Terminate services");
  Progress_Flush();
  Orientation_Exit();
  SDL_TextCache_Clear();
  close_fonts();
  TTF_Quit();
  MuPDF_Shutdown();
  Textures_Free();
  Screen_Shutdown();
  if (RENDERER) SDL_DestroyRenderer(RENDERER);
  if (WINDOW) SDL_DestroyWindow(WINDOW);
  IMG_Quit();
  SDL_Quit();
  if (s_pl) plExit();
  if (s_psm) psmExit();
  if (s_time) timeExit();
  if (s_romfs) romfsExit();
  Log_Close();
}

static bool Init_Services() {
  migrate_old_data();
  FS_RecursiveMakeDir(BOOKS_DIR);
  Log_Init();

  s_romfs = R_SUCCEEDED(romfsInit());
  s_time = R_SUCCEEDED(timeInitialize());
  s_psm = R_SUCCEEDED(psmInitialize());
  s_pl = R_SUCCEEDED(plInitialize(PlServiceType_User));

  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    Log_Error(std::string("SDL_Init: ") + SDL_GetError());
    return false;
  }
  WINDOW = SDL_CreateWindow("BookReader", 0, 0, 1280, 720, 0);
  if (!WINDOW) {
    Log_Error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    return false;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  // VSync: the old build redrew as fast as possible and drained the battery.
  RENDERER = SDL_CreateRenderer(WINDOW, -1,
                                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC |
                                    SDL_RENDERER_TARGETTEXTURE);
  if (!RENDERER) RENDERER = SDL_CreateRenderer(WINDOW, -1, SDL_RENDERER_ACCELERATED);
  if (!RENDERER) {
    Log_Error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
    return false;
  }
  SDL_SetRenderDrawBlendMode(RENDERER, SDL_BLENDMODE_BLEND);

  if (!IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP))
    Log_Error(std::string("IMG_Init: ") + IMG_GetError());

  if (TTF_Init() == -1) {
    Log_Error(std::string("TTF_Init: ") + TTF_GetError());
    return false;
  }
  if (!load_fonts()) {
    Log_Error("Failed to load fonts");
    return false;
  }

  Screen_Init();
  Textures_Load();
  Settings_Load();
  Library_Load();

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  hidInitializeTouchScreen();
  Orientation_Init();

  Log_Write("Services initialised");
  return true;
}

int main(int argc, char *argv[]) {
  std::set_terminate(terminate_handler);

  if (!Init_Services()) {
    Term_Services();
    return 1;
  }

  // Launched with a file (e.g. via a file association): open it directly.
  if (argc >= 2 && argv[1] && argv[1][0] == '/' && FS_FileExists(argv[1]))
    Menu_OpenBook(argv[1]);
  else
    Menu_StartChoosing();

  Settings_Save();
  Term_Services();
  return 0;
}
