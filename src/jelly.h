// Shared between the jelly (jelly.c) and its right-click options panel (options.c).
#pragma once
#include <X11/Xlib.h>
#include <GL/glx.h>

#define NFLAVORS 7
extern const char *FLAVOR_NAMES[NFLAVORS];
extern const float FLAVOR_COLS[NFLAVORS][3];

typedef struct {
  int girl;
  int flavor;   // index into FLAVOR_*, or -1 for a custom color
  float col[3]; // current body color
  float flex;   // 0 = firm .. 1 = gooey
  float size;   // body radius in pixels
  int face;     // FACE_*
  float pull;   // how much of the body a pull drags along: 0 = small pinch .. 1 = more than half
} Cfg;

enum { FACE_TINY = 0, FACE_CLASSIC = 2 }; // 1 was the retired brows-only face

enum { OPT_NONE = 0, OPT_CHANGED = 1, OPT_NAP = 2, OPT_QUIT = 4, OPT_CLOSED = 8 };

void opt_init(Display *dpy, GLXFBConfig fb);
void opt_open(int x, int y); // screen position of the panel's top-left
void opt_close(void);
int opt_is_open(void);
int opt_owns(Window w);
void opt_event(XEvent *e);
int opt_frame(Cfg *cfg, double dt); // draws the panel, edits cfg live, returns OPT_* flags
