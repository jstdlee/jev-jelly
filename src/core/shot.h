// Transparent screenshots of the surfaces (see shot.c): JELLY_SHOT_DIR=<dir> [JELLY_SHOT_AT=<seconds>].
#pragma once
int shot_png(const char *path, const unsigned char *rgba, int w, int h);
void shot_maybe(const char *name, int w, int h, double age); // call with the surface's drawing still bound
