#ifndef WOOK_READER_ORIENTATION_H
#define WOOK_READER_ORIENTATION_H

// Reads the handheld motion sensor and decides how the screen should be
// rotated (0, 90 or 270 degrees clockwise) so content stays upright.
void Orientation_Init();
void Orientation_Exit();
void Orientation_Update();  // call once per frame
int Orientation_Rotation();  // honours the user's orientation setting

// Raw accelerometer reading (G), for the sensor test in Settings.
bool Orientation_Raw(float *x, float *y, float *z);

#endif
