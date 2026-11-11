#include "memory_mode.h"

#include <switch.h>

#include <algorithm>

bool App_IsAppletMode() {
  AppletType t = appletGetAppletType();
  return t != AppletType_Application && t != AppletType_SystemApplication;
}

size_t App_CacheBudget() {
  u64 total = 0, used = 0;
  if (R_FAILED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) ||
      R_FAILED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)) || total <= used)
    return 32u << 20;
  u64 free_mem = total - used;
  if (App_IsAppletMode())
    return (size_t)std::min<u64>(free_mem / 5, 48ull << 20);  // stay well clear of the limit
  return (size_t)std::min<u64>(free_mem * 45 / 100, 900ull << 20);
}
