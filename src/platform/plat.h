// The platform layer: everything the jelly needs from the OS, so the core (physics, rendering, UI, chat, calendar)
// is the same on every system. Implemented by platform/linux (X11 + GLX) and platform/windows (Win32 + WGL).
//
// Windows ("surfaces") are frameless, per-pixel transparent and share one OpenGL 3.3 context. Only their clickable
// area takes the mouse: everything else clicks through to the desktop.
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <time.h>

typedef struct PWin PWin;

enum {
  PW_KEYBOARD = 1, // can take the keyboard (text fields): a managed, chrome-less window
  PW_HAND = 2,     // hand cursor over it
  PW_MSAA = 4,     // multisampled (the jelly itself)
};

enum { PE_NONE, PE_MOVE, PE_LEAVE, PE_BUTTON, PE_WHEEL, PE_KEY, PE_TEXT, PE_PASTE, PE_FOCUS };
enum { PB_LEFT = 1, PB_MIDDLE = 2, PB_RIGHT = 3 };
enum { PM_CTRL = 1, PM_SHIFT = 2, PM_ALT = 4 };

typedef struct {
  int type;
  PWin *win;        // the surface it happened on (NULL: none of ours)
  float x, y;       // pointer, in the surface's pixels
  float rx, ry;     // pointer, on the screen
  int button, down; // PE_BUTTON: PB_*, pressed / released; PE_KEY: pressed / released; PE_FOCUS: gained / lost
  float wheel;      // PE_WHEEL: + up
  int key, mods;    // PE_KEY: an ImGuiKey, PM_* modifiers
  int tag;          // PE_PASTE: the tag given to pw_request_paste
  char text[32];    // PE_TEXT: UTF-8 characters typed
  char *paste;      // PE_PASTE: the clipboard text (malloc'd, freed by the next plat_poll), or NULL
} PEvent;

typedef struct { int x, y, w, h; } PRect;

/* ---- start-up */
int plat_main_guard(int argc, char **argv); // a crash watchdog where there is one: returns >= 0 to exit with that
int plat_init(int softGL);                  // display + GL; 0 on failure (the message is printed)
void plat_quit(void);
int plat_soft_gl(void);                     // running on software rendering
void plat_screen(int *w, int *h);
void *plat_gl_proc(const char *name);       // for the GL loader

/* ---- surfaces */
PWin *pw_create(const char *name, int x, int y, int w, int h, int flags);
void pw_move(PWin *w, int x, int y);
void pw_resize(PWin *w, int width, int height);
void pw_show(PWin *w);            // map and raise, above other windows
void pw_hide(PWin *w);
int pw_visible(PWin *w);
void pw_raise(PWin *w);
void pw_input_rects(PWin *w, const PRect *r, int n); // the clickable area (n = 0: all of it)
void pw_begin(PWin *w);           // draw into it: its GL target is bound, the viewport is the whole surface
void pw_present(PWin *w);         // show what was drawn (and return to the previous target)
void pw_vsync(PWin *w, int on);
unsigned pw_framebuffer(PWin *w); // its GL framebuffer (0 = the window itself): bind it again after drawing offscreen

/* ---- keyboard focus (for PW_KEYBOARD surfaces) */
void pw_focus_soon(PWin *w, double now); // ask for the keyboard once the surface is really on screen
void pw_release_focus(PWin *w);          // give it back to whoever had it
void pw_want_text(PWin *w, int on);      // keep the keyboard while a text field is active
void pw_clicked(PWin *w);                // a click inside: take the keyboard back if a field is still active
void pw_tick(PWin *w, double now);       // call every frame: carries out pw_focus_soon
int pw_focused(PWin *w);
void pw_request_paste(PWin *w, int tag); // the clipboard arrives as a PE_PASTE event with this tag

/* ---- events and the desktop */
int plat_poll(PEvent *e);                 // 0 when there are no more events
void plat_cursor(float *x, float *y);     // pointer position on the screen
double plat_idle_seconds(void);           // time since the last keyboard / mouse input anywhere, -1 if unknown
float plat_screen_luma(int x, int y, int w, int h); // mean brightness 0..1 of the desktop there, -1 if unknown
void plat_flush(void);

/* ---- time */
double plat_now(void);                    // monotonic seconds
void plat_sleep(double seconds);
void plat_localtime(time_t t, struct tm *out);
time_t plat_timegm(struct tm *tm);
int plat_zone_known(const char *iana);    // can times in this IANA zone be converted?
time_t plat_mktime_in(struct tm *tm, const char *iana); // local time in a zone (NULL: the local zone) -> UTC

/* ---- files and processes */
void plat_config_path(const char *name, char *out, size_t n); // ~/.config/jev-jelly/<name> or %APPDATA%\jev-jelly\<name>
void plat_private_file(const char *path);                      // readable by the user only
FILE *plat_temp_file(char *path, size_t n);                    // a new private temp file, open for writing
int plat_font(int cjk, char *out, size_t n);                   // a UI font (cjk = 0) or a CJK fallback font (1, 2 = bold)
int plat_find_exe(const char *name, char *out, size_t n);      // on PATH (or a known place), 1 if found
/* Runs a program with `in` on stdin (may be NULL) and returns its stdout (malloc'd, NULL if it couldn't start).
   *status is the exit code (-1 if killed). The program is found on PATH unless it's a path. */
char *plat_run(const char *prog, char *const argv[], const char *in, int *status);
