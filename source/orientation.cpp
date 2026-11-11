#include "orientation.h"

#include <switch.h>

#include <cmath>

#include "settings.h"

static HidSixAxisSensorHandle s_handle;
static bool s_started = false;
static int s_auto_rot = 0;       // committed auto rotation
static int s_candidate = 0;      // rotation currently being observed
static int s_candidate_frames = 0;
static float s_ax = 0, s_ay = 0, s_az = 0;
static bool s_have_sample = false;

static const int STABLE_FRAMES = 15;  // ~250 ms at 60 fps

void Orientation_Init() {
  if (s_started) return;
  if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&s_handle, 1, HidNpadIdType_Handheld,
                                             HidNpadStyleTag_NpadHandheld)) &&
      R_SUCCEEDED(hidStartSixAxisSensor(s_handle)))
    s_started = true;
}

void Orientation_Exit() {
  if (s_started) hidStopSixAxisSensor(s_handle);
  s_started = false;
}

bool Orientation_Raw(float *x, float *y, float *z) {
  *x = s_ax;
  *y = s_ay;
  *z = s_az;
  return s_have_sample;
}

void Orientation_Update() {
  if (!s_started) return;

  // Docked: the TV never rotates.
  if (appletGetOperationMode() == AppletOperationMode_Console) {
    s_auto_rot = 0;
    s_candidate_frames = 0;
    return;
  }
  if (!(hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld))
    return;  // Joy-Cons detached: no reliable console orientation, keep last

  HidSixAxisSensorState st;
  if (hidGetSixAxisSensorStates(s_handle, &st, 1) == 0) return;
  s_ax = st.acceleration.x;
  s_ay = st.acceleration.y;
  s_az = st.acceleration.z;
  s_have_sample = true;

  float ax = fabsf(s_ax), ay = fabsf(s_ay), az = fabsf(s_az);
  int want;
  if (ax > 0.55f && ax > ay * 1.5f && ax > az * 0.8f)
    want = s_ax > 0 ? 270 : 90;  // standing on a short edge (sign verified on hardware)
  else if (ay > 0.55f && ay > ax * 1.5f)
    want = 0;                    // upright landscape
  else
    want = s_auto_rot;           // lying flat or in between: keep current

  if (want == s_candidate) {
    if (s_candidate_frames < STABLE_FRAMES) s_candidate_frames++;
  } else {
    s_candidate = want;
    s_candidate_frames = 0;
  }
  if (s_candidate_frames >= STABLE_FRAMES) s_auto_rot = s_candidate;
}

int Orientation_Rotation() {
  switch (g_settings.orientation) {
    case ORIENT_LANDSCAPE: return 0;
    case ORIENT_PORTRAIT_R_TOP: return 90;
    case ORIENT_PORTRAIT_L_TOP: return 270;
    default: break;
  }
  int r = s_auto_rot;
  if (g_settings.rotate_reverse && r != 0) r = 360 - r;
  return r;
}
