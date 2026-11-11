#include "library.h"

#include <dirent.h>
#include <libconfig.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "natsort.h"
#include "paths.h"

extern "C" {
#include "fs.h"
}

// ── helpers ──────────────────────────────────────────────────────────────────

std::string Library_Ext(const std::string &name) {
  size_t slash = name.find_last_of('/');
  size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return "";
  std::string e = name.substr(dot);
  for (char &c : e) c = (char)tolower((unsigned char)c);
  return e;
}

std::string Library_DisplayName(const std::string &path) {
  std::string n = path.substr(path.find_last_of('/') + 1);
  size_t dot = n.find_last_of('.');
  if (dot != std::string::npos && dot > 0) n = n.substr(0, dot);
  return n;
}

bool Library_IsComicArchive(const std::string &path) {
  static const char *exts[] = {".cbz", ".cbr", ".cbt", ".cb7", ".zip",
                               ".rar", ".7z",  ".tar", nullptr};
  std::string e = Library_Ext(path);
  for (int i = 0; exts[i]; i++)
    if (e == exts[i]) return true;
  return false;
}

bool Library_IsImageFile(const std::string &name) {
  static const char *exts[] = {".jpg", ".jpeg", ".png", ".webp",
                               ".gif", ".bmp",  nullptr};
  std::string e = Library_Ext(name);
  for (int i = 0; exts[i]; i++)
    if (e == exts[i]) return true;
  return false;
}

bool Library_IsSupportedBook(const std::string &name) {
  static const char *exts[] = {".pdf", ".epub", ".xps", ".oxps", ".fb2",
                               ".xhtml", ".html", ".htm", ".txt", nullptr};
  if (Library_IsComicArchive(name)) return true;
  std::string e = Library_Ext(name);
  for (int i = 0; exts[i]; i++)
    if (e == exts[i]) return true;
  return false;
}

std::string Library_Sibling(const std::string &path, int dir) {
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return "";
  std::string folder = path.substr(0, slash), self = path.substr(slash + 1);
  DIR *d = opendir(folder.c_str());
  if (!d) return "";
  std::vector<std::string> names;
  while (struct dirent *e = readdir(d)) {
    if (e->d_name[0] == '.' || e->d_type == DT_DIR) continue;
    if (Library_IsSupportedBook(e->d_name)) names.push_back(e->d_name);
  }
  closedir(d);
  // Same ordering as the library grid (by display name, natural order).
  std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
    return natural_less(Library_DisplayName(a), Library_DisplayName(b));
  });
  for (size_t i = 0; i < names.size(); i++) {
    if (names[i] != self) continue;
    long j = (long)i + dir;
    if (j < 0 || j >= (long)names.size()) return "";
    return folder + "/" + names[j];
  }
  return "";
}

static std::string parent_dir(const std::string &p) {
  size_t s = p.find_last_of('/');
  if (s == std::string::npos || s == 0) return "";
  return p.substr(0, s);
}

static std::string strip_slash(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

// ── content marks ────────────────────────────────────────────────────────────

static std::map<std::string, ContentType> g_marks;
static bool g_marks_loaded = false;

static void marks_save() {
  FILE *f = fopen(LIBRARY_FILE, "w");
  if (!f) return;
  fprintf(f, "# BookReader content types: M = manga (keep colours), B = book (invert in dark mode)\n");
  for (const auto &kv : g_marks)
    fprintf(f, "%c %s\n", kv.second == CONTENT_MANGA ? 'M' : 'B', kv.first.c_str());
  fclose(f);
}

void Library_Load() {
  g_marks.clear();
  FILE *f = fopen(LIBRARY_FILE, "r");
  if (f) {
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
      size_t len = strlen(line);
      while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
      if (len < 3 || line[1] != ' ') continue;
      ContentType t = line[0] == 'M' ? CONTENT_MANGA
                      : line[0] == 'B' ? CONTENT_BOOK
                                       : CONTENT_UNKNOWN;
      if (t != CONTENT_UNKNOWN) g_marks[strip_slash(line + 2)] = t;
    }
    fclose(f);
  }
  g_marks_loaded = true;
}

ContentType Library_GetMark(const std::string &path) {
  if (!g_marks_loaded) Library_Load();
  auto it = g_marks.find(strip_slash(path));
  return it == g_marks.end() ? CONTENT_UNKNOWN : it->second;
}

void Library_SetMark(const std::string &path, ContentType t) {
  if (!g_marks_loaded) Library_Load();
  std::string p = strip_slash(path);
  if (t == CONTENT_UNKNOWN)
    g_marks.erase(p);
  else
    g_marks[p] = t;
  marks_save();
}

ContentType Library_InheritedMark(const std::string &path) {
  for (std::string d = parent_dir(strip_slash(path)); !d.empty(); d = parent_dir(d)) {
    ContentType t = Library_GetMark(d);
    if (t != CONTENT_UNKNOWN) return t;
  }
  return CONTENT_UNKNOWN;
}

ContentType Library_Resolve(const std::string &path) {
  ContentType t = Library_GetMark(path);
  return t != CONTENT_UNKNOWN ? t : Library_InheritedMark(path);
}

ContentType Library_DefaultType(const std::string &path) {
  struct stat st;
  bool is_dir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  return (is_dir || Library_IsComicArchive(path)) ? CONTENT_MANGA : CONTENT_BOOK;
}

// ── progress ─────────────────────────────────────────────────────────────────

static config_t *g_progress = nullptr;
static bool g_progress_dirty = false;

static void progress_open() {
  if (g_progress) return;
  g_progress = (config_t *)malloc(sizeof(config_t));
  config_init(g_progress);
  config_read_file(g_progress, PROGRESS_FILE);
}

// Key format kept compatible with older versions where it was valid.
static std::string book_key(const std::string &path) {
  std::string name = path.substr(path.find_last_of('/') + 1);
  std::string k = name;
  static const std::string bad = " :/?#[]@!$&'()*+,;=.";
  for (char c : bad) k.erase(std::remove(k.begin(), k.end(), c), k.end());

  bool valid = !k.empty() && isalpha((unsigned char)k[0]);
  for (unsigned char c : k)
    if (!(isalnum(c) || c == '-' || c == '_' || c == '*')) valid = false;
  if (valid) return k;

  // Not a legal libconfig name (starts with a digit, non-ASCII, ...).
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : name) { h ^= c; h *= 1099511628211ULL; }
  std::string out = "k_";
  for (unsigned char c : k)
    out += (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
  char hex[20];
  snprintf(hex, sizeof(hex), "_%016llx", (unsigned long long)h);
  return out + hex;
}

static int progress_get(const std::string &key) {
  progress_open();
  int v = 0;
  config_setting_t *s = config_setting_get_member(config_root_setting(g_progress), key.c_str());
  if (s && config_setting_type(s) == CONFIG_TYPE_INT) v = config_setting_get_int(s);
  return v;
}

static void progress_set(const std::string &key, int v) {
  progress_open();
  config_setting_t *root = config_root_setting(g_progress);
  config_setting_t *s = config_setting_get_member(root, key.c_str());
  if (s && config_setting_type(s) != CONFIG_TYPE_INT) {
    config_setting_remove(root, key.c_str());
    s = nullptr;
  }
  if (!s) s = config_setting_add(root, key.c_str(), CONFIG_TYPE_INT);
  if (s && config_setting_get_int(s) != v) {
    config_setting_set_int(s, v);
    g_progress_dirty = true;
  }
}

int Progress_GetPage(const std::string &path) { return progress_get(book_key(path)); }
int Progress_GetTotal(const std::string &path) { return progress_get(book_key(path) + "_T"); }
void Progress_SetPage(const std::string &path, int page) { progress_set(book_key(path), page); }
void Progress_SetTotal(const std::string &path, int total) {
  progress_set(book_key(path) + "_T", total);
}

void Progress_Flush() {
  if (g_progress && g_progress_dirty) {
    config_write_file(g_progress, PROGRESS_FILE);
    g_progress_dirty = false;
  }
}

// ── recent ───────────────────────────────────────────────────────────────────

static std::vector<std::string> g_recent;
static bool g_recent_loaded = false;
static const size_t RECENT_MAX = 20;

static void recent_load() {
  g_recent.clear();
  g_recent_loaded = true;
  FILE *f = fopen(RECENT_FILE, "r");
  if (!f) return;
  char line[1024];
  while (g_recent.size() < RECENT_MAX && fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    std::string p(line, len);
    struct stat st;
    if (!p.empty() && stat(p.c_str(), &st) == 0) g_recent.push_back(p);  // drop moved/deleted
  }
  fclose(f);
}

static void recent_save() {
  FILE *f = fopen(RECENT_FILE, "w");
  if (!f) return;
  for (const auto &p : g_recent) fprintf(f, "%s\n", p.c_str());
  fclose(f);
}

const std::vector<std::string> &Recent_Get() {
  if (!g_recent_loaded) recent_load();
  return g_recent;
}

void Recent_Push(const std::string &path) {
  if (!g_recent_loaded) recent_load();
  if (!g_recent.empty() && g_recent[0] == path) return;
  g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), path), g_recent.end());
  g_recent.insert(g_recent.begin(), path);
  if (g_recent.size() > RECENT_MAX) g_recent.resize(RECENT_MAX);
  recent_save();
}

void Recent_Clear() {
  g_recent.clear();
  g_recent_loaded = true;
  remove(RECENT_FILE);
}

// ── notes ────────────────────────────────────────────────────────────────────

static std::set<std::string> g_note_files;  // file names in NOTES_DIR
static bool g_notes_scanned = false;

static std::string note_file(const std::string &path) {
  std::string name = path.substr(path.find_last_of('/') + 1);
  static const std::string bad = " :/?#[]@!$&'()*+,;=.";
  for (char c : bad) name.erase(std::remove(name.begin(), name.end(), c), name.end());
  return name + ".txt";
}

static void notes_scan() {
  g_notes_scanned = true;
  g_note_files.clear();
  DIR *d = opendir(NOTES_DIR);
  if (!d) return;
  while (struct dirent *e = readdir(d))
    if (e->d_name[0] != '.') g_note_files.insert(e->d_name);
  closedir(d);
}

bool Notes_Exists(const std::string &path) {
  if (!g_notes_scanned) notes_scan();
  return g_note_files.count(note_file(path)) != 0;
}

std::string Notes_Read(const std::string &path) {
  std::string p = std::string(NOTES_DIR) + "/" + note_file(path);
  FILE *f = fopen(p.c_str(), "r");
  if (!f) return "";
  std::string text;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  fclose(f);
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

void Notes_Write(const std::string &path, const std::string &text) {
  if (!g_notes_scanned) notes_scan();
  FS_RecursiveMakeDir(NOTES_DIR);
  std::string fn = note_file(path);
  std::string p = std::string(NOTES_DIR) + "/" + fn;
  if (text.empty()) {
    remove(p.c_str());
    g_note_files.erase(fn);
    return;
  }
  FILE *f = fopen(p.c_str(), "w");
  if (!f) return;
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);
  g_note_files.insert(fn);
}

// ── caches ───────────────────────────────────────────────────────────────────

void Library_CacheUsage(unsigned long long *bytes, int *files) {
  *bytes = 0;
  *files = 0;
  DIR *d = opendir(PAGECACHE_DIR);
  if (!d) return;
  while (struct dirent *e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    struct stat st;
    std::string f = std::string(PAGECACHE_DIR) + "/" + e->d_name;
    if (stat(f.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
      *bytes += (unsigned long long)st.st_size;
      (*files)++;
    }
  }
  closedir(d);
}

void Library_ClearCaches() {
  DIR *d = opendir(PAGECACHE_DIR);
  if (!d) return;
  std::vector<std::string> files;
  while (struct dirent *e = readdir(d))
    if (e->d_name[0] != '.') files.push_back(std::string(PAGECACHE_DIR) + "/" + e->d_name);
  closedir(d);
  for (const auto &f : files) unlink(f.c_str());
}
