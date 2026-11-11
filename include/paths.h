#ifndef WOOK_READER_PATHS_H
#define WOOK_READER_PATHS_H

// All app data lives in a dot-folder so hbmenu does not list it as an entry.
// The .nro itself goes in /switch/BookReader.nro.
#define APP_DIR        "/switch/.BookReader"
#define BOOKS_DIR      APP_DIR "/books"
#define LOG_FILE       APP_DIR "/log.txt"
#define OPTIONS_FILE   APP_DIR "/options.cfg"
#define PROGRESS_FILE  APP_DIR "/saved_pages.cfg"
#define LIBRARY_FILE   APP_DIR "/library.txt"
#define RECENT_FILE    APP_DIR "/.recent.lst"
#define NOTES_DIR      APP_DIR "/.notes"
#define PAGECACHE_DIR  APP_DIR "/.pagecache"

// Older locations, migrated on first start.
#define WOOK_APP_DIR   "/switch/.WookReader"  // WookReader rework builds
#define OLD_APP_DIR    "/switch/WookReader"
#define OLDER_APP_DIR  "/switch/eBookReader"

#endif
