#ifndef WOOK_READER_MEMORY_MODE_H
#define WOOK_READER_MEMORY_MODE_H

#include <stddef.h>

// True when launched from the album (applet mode): only a few hundred MB of
// RAM and less CPU time. Title-override (launch a game while holding R) gives
// the app nearly the whole console.
bool App_IsAppletMode();

// Bytes the reader may use for its in-RAM book cache (archive bytes + decoded
// or rendered pages). Computed from the memory that is actually free.
size_t App_CacheBudget();

#endif
