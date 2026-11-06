// The window and GL context for gsplay: firmware SDL2 and GLES through dlopen, as bench/ does.
#pragma once

bool gsplayGlInit();
void gsplayGlSwap();
// Milliseconds in glFinish and in the swap since the last call.
void gsplayGlTimes(double &finishMs, double &swapMs);
int gsplayScreenWidth();
int gsplayScreenHeight();
