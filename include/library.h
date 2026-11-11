#ifndef WOOK_READER_LIBRARY_H
#define WOOK_READER_LIBRARY_H

#include <string>
#include <vector>

// How a file is shown in dark mode.
//   MANGA: colours untouched, black background around the page.
//   BOOK : luminance inverted (white paper -> black, text -> white).
enum ContentType { CONTENT_UNKNOWN = 0, CONTENT_MANGA = 1, CONTENT_BOOK = 2 };

void Library_Load();

// Explicit mark on exactly this path (file or folder).
ContentType Library_GetMark(const std::string &path);
void Library_SetMark(const std::string &path, ContentType t);
// Nearest marked ancestor folder (excluding the path itself).
ContentType Library_InheritedMark(const std::string &path);
// File mark, else inherited folder mark, else UNKNOWN.
ContentType Library_Resolve(const std::string &path);
// Fallback when the user is not asked: archives/image folders -> manga.
ContentType Library_DefaultType(const std::string &path);

bool Library_IsComicArchive(const std::string &path);
bool Library_IsImageFile(const std::string &name);
bool Library_IsSupportedBook(const std::string &name);
std::string Library_Ext(const std::string &name);  // lower-case, with dot
std::string Library_DisplayName(const std::string &path);  // no dir, no ext
// Next (dir = +1) or previous (dir = -1) book in the same folder, in library
// (natural) order. Empty when there is none.
std::string Library_Sibling(const std::string &path, int dir);

// Reading progress (0-based page) + total pages.
int Progress_GetPage(const std::string &path);
int Progress_GetTotal(const std::string &path);
void Progress_SetPage(const std::string &path, int page);
void Progress_SetTotal(const std::string &path, int total);
void Progress_Flush();

// Recently opened (most recent first).
const std::vector<std::string> &Recent_Get();
void Recent_Push(const std::string &path);
void Recent_Clear();

// Per-book notes.
bool Notes_Exists(const std::string &path);
std::string Notes_Read(const std::string &path);
void Notes_Write(const std::string &path, const std::string &text);

// Deletes cover/page-name caches.
void Library_ClearCaches();
// Disk space used by the caches (thumbnails, cover data, page lists).
void Library_CacheUsage(unsigned long long *bytes, int *files);

#endif
