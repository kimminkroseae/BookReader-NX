#include "mupdf_ctx.h"

#include <switch.h>

fz_context *ctx = nullptr;

static Mutex s_locks_mutex[FZ_LOCK_MAX];

static void lock_cb(void *, int lock) { mutexLock(&s_locks_mutex[lock]); }
static void unlock_cb(void *, int lock) { mutexUnlock(&s_locks_mutex[lock]); }

static fz_locks_context s_locks = {nullptr, lock_cb, unlock_cb};

fz_context *MuPDF_Context() {
  if (ctx) return ctx;
  for (int i = 0; i < FZ_LOCK_MAX; i++) mutexInit(&s_locks_mutex[i]);
  // Bounded store: the Switch applet mode only has a few hundred MB.
  ctx = fz_new_context(nullptr, &s_locks, 96 << 20);
  if (ctx) {
    fz_try(ctx) { fz_register_document_handlers(ctx); }
    fz_catch(ctx) {
      fz_drop_context(ctx);
      ctx = nullptr;
    }
  }
  return ctx;
}

void MuPDF_Shutdown() {
  if (ctx) fz_drop_context(ctx);
  ctx = nullptr;
}
