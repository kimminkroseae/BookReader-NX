#ifndef WOOK_READER_MUPDF_CTX_H
#define WOOK_READER_MUPDF_CTX_H

#include <mupdf/fitz.h>

// Shared MuPDF context, created on first use WITH lock callbacks so that
// fz_clone_context() works for background rendering threads.
extern fz_context *ctx;
fz_context *MuPDF_Context();
void MuPDF_Shutdown();

#endif
