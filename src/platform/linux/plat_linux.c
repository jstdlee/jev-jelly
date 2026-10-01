// The Linux platform layer: X11 windows with a 32-bit ARGB visual (a compositor blends them), one GLX context shared
// by every surface, XShape input regions for click-through, the X input method for typing (ibus / fcitx: Japanese,
// Chinese and Korean work), the clipboard, the screensaver extension for idle time, and a crash watchdog.

#define _GNU_SOURCE
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "../plat.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct PWin {
  Window win;
  int flags, w, h, mapped;
  XIC ic;
  int focused, prevWant, grabTries, pasteTag;
  double grabAt;
  GLXDrawable prevDraw;
  GLXContext prevCtx;
};

static Display *dpy;
static GLXFBConfig fbc;
static GLXContext ctx;
static XIM im;
static int softGL, SW, SH;
#define MAXWIN 8
static PWin *wins[MAXWIN];
static int nwins;

static PWin *find(Window w) {
  for (int i = 0; i < nwins; i++) if (wins[i]->win == w) return wins[i];
  return NULL;
}

/* ------------------------------------------------------------------ start-up */

double plat_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
void plat_sleep(double s) { if (s > 0) usleep((useconds_t)(s * 1e6)); }

/* Watchdog: the jelly runs as a child process. On the GB10, NVIDIA GL crashes new windows in their first frames
   while a big model holds most of the memory (e.g. TensorFold's ~90 GB): then it restarts on Mesa software GL.
   A later crash just restarts it (at most a few times a minute). */
int plat_main_guard(int argc, char **argv) {
  (void)argc;
  if (getenv("JELLY_CHILD") || getenv("JELLY_NO_WATCHDOG")) return -1;
  int software = getenv("LIBGL_ALWAYS_SOFTWARE") != NULL, restarts = 0;
  double windowStart = plat_now();
  for (;;) {
    char *args[] = {argv[0], NULL};
    if (software) { // one render thread: llvmpipe's worker threads spin and cost 4x the CPU for a window this small
      setenv("__GLX_VENDOR_LIBRARY_NAME", "mesa", 1); setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
      setenv("LP_NUM_THREADS", "1", 0);
    }
    setenv("JELLY_CHILD", "1", 1);
    pid_t pid;
    if (posix_spawn(&pid, "/proc/self/exe", NULL, NULL, args, environ) != 0) return 1;
    double t0 = plat_now();
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {}
    if (WIFEXITED(st)) return WEXITSTATUS(st); // quit on purpose
    int early = plat_now() - t0 < 20 && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV; // the driver's crash, not ours
    if (early && !software) { // the GPU driver can't give us a window right now
      fprintf(stderr, "jelly: GPU GL crashed at startup (memory pressure?); using software rendering\n");
      software = 1;
      continue;
    }
    if (plat_now() - windowStart > 60) { windowStart = plat_now(); restarts = 0; }
    if (++restarts > 4) { fprintf(stderr, "jelly: crashed too often, giving up\n"); return 1; }
    fprintf(stderr, "jelly: crashed (signal %d), restarting\n", WIFSIGNALED(st) ? WTERMSIG(st) : 0);
  }
}

static int on_xerror(Display *d, XErrorEvent *e) {
  if (getenv("JELLY_DEBUG")) { char msg[128]; XGetErrorText(d, e->error_code, msg, sizeof msg); fprintf(stderr, "X error (ignored): %s\n", msg); }
  return 0;
}

int plat_init(int wantSoft) {
  softGL = wantSoft || getenv("LIBGL_ALWAYS_SOFTWARE") != NULL;
  setlocale(LC_CTYPE, ""); // for the input method (CJK typing); numbers stay in the C locale
  dpy = XOpenDisplay(NULL);
  if (!dpy) { fprintf(stderr, "cannot open X display\n"); return 0; }
  XSetErrorHandler(on_xerror);
  int scr = DefaultScreen(dpy);
  SW = DisplayWidth(dpy, scr); SH = DisplayHeight(dpy, scr);
  int attr[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, True, GLX_SAMPLE_BUFFERS, 1,
                GLX_SAMPLES, 4, None};
  int found = 0;
  for (int pass = softGL; pass < 2 && !found; pass++) { // (software GL: skip multisampling, it's costly on the CPU)
    if (pass == 1) { attr[20] = GLX_SAMPLE_BUFFERS; attr[21] = 0; attr[22] = GLX_SAMPLES; attr[23] = 0; }
    int n = 0; GLXFBConfig *fbs = glXChooseFBConfig(dpy, scr, attr, &n);
    for (int i = 0; i < n && !found; i++) {
      XVisualInfo *v = glXGetVisualFromFBConfig(dpy, fbs[i]);
      if (v && v->depth == 32) { fbc = fbs[i]; found = 1; }
      if (v) XFree(v);
    }
    if (fbs) XFree(fbs);
  }
  if (!found) { fprintf(stderr, "no 32-bit ARGB GLX visual (is a compositor running?)\n"); return 0; }
  typedef GLXContext (*CtxFn)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
  CtxFn mk = (CtxFn)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
  int cattr[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3, GLX_CONTEXT_PROFILE_MASK_ARB,
                 GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
  if (mk) ctx = mk(dpy, fbc, NULL, True, cattr);
  if (!ctx) ctx = glXCreateNewContext(dpy, fbc, GLX_RGBA_TYPE, NULL, True);
  if (!ctx) { fprintf(stderr, "cannot create an OpenGL context\n"); return 0; }
  XSetLocaleModifiers("");
  im = XOpenIM(dpy, NULL, NULL, NULL);
  if (!im) { XSetLocaleModifiers("@im=none"); im = XOpenIM(dpy, NULL, NULL, NULL); }
  return 1;
}

void plat_quit(void) {
  if (!dpy) return;
  glXMakeCurrent(dpy, None, NULL);
  if (ctx) glXDestroyContext(dpy, ctx);
  for (int i = 0; i < nwins; i++) XDestroyWindow(dpy, wins[i]->win);
  XCloseDisplay(dpy);
  dpy = NULL;
}
int plat_soft_gl(void) { return softGL; }
void plat_screen(int *w, int *h) { *w = SW; *h = SH; }
void *plat_gl_proc(const char *name) { return (void *)glXGetProcAddressARB((const GLubyte *)name); }
void plat_flush(void) { XFlush(dpy); }

/* ------------------------------------------------------------------ surfaces */

/* GNOME's window manager won't let an override-redirect (unmanaged) window keep the keyboard, so windows you type
   in are managed but look unmanaged: no decorations, above everything, not in the taskbar or pager. The window
   manager drops "above" when a window is withdrawn, so this runs before every map. */
static void prepare_managed(Window w) {
  Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), util = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  XChangeProperty(dpy, w, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&util, 1);
  Atom states[3] = {XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False), XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
                    XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False)};
  XChangeProperty(dpy, w, XInternAtom(dpy, "_NET_WM_STATE", False), XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 3);
  struct { unsigned long flags, functions, decorations; long input; unsigned long status; } mwm = {2, 0, 0, 0, 0};
  Atom motif = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
  XChangeProperty(dpy, w, motif, motif, 32, PropModeReplace, (unsigned char *)&mwm, 5); // no title bar or border
  XWMHints wh = {0}; wh.flags = InputHint; wh.input = True;
  XSetWMHints(dpy, w, &wh);
  XSizeHints sh = {0}; sh.flags = USPosition | PPosition; // keep the position we give it
  XSetWMNormalHints(dpy, w, &sh);
}

PWin *pw_create(const char *name, int x, int y, int w, int h, int flags) {
  if (nwins == MAXWIN) return NULL;
  PWin *p = calloc(1, sizeof *p);
  p->flags = flags; p->w = w; p->h = h;
  XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbc);
  Window root = DefaultRootWindow(dpy);
  XSetWindowAttributes swa = {0};
  swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
  swa.override_redirect = !(flags & PW_KEYBOARD); // typing needs a managed window (see prepare_managed)
  swa.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | ExposureMask;
  if (flags & PW_KEYBOARD) swa.event_mask |= KeyPressMask | KeyReleaseMask | FocusChangeMask;
  p->win = XCreateWindow(dpy, root, x, y, w, h, 0, vi->depth, InputOutput, vi->visual,
                         CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
  XFree(vi);
  XStoreName(dpy, p->win, name);
  XClassHint ch = {(char *)"jelly", (char *)"Jelly"}; XSetClassHint(dpy, p->win, &ch);
  if (flags & PW_HAND) XDefineCursor(dpy, p->win, XCreateFontCursor(dpy, XC_hand2));
  if ((flags & PW_KEYBOARD) && im)
    p->ic = XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, p->win, XNFocusWindow, p->win, NULL);
  wins[nwins++] = p;
  return p;
}

void pw_move(PWin *p, int x, int y) { XMoveWindow(dpy, p->win, x, y); }
void pw_resize(PWin *p, int w, int h) { p->w = w; p->h = h; XResizeWindow(dpy, p->win, w, h); }
void pw_show(PWin *p) {
  if (p->mapped) { XRaiseWindow(dpy, p->win); return; }
  if (p->flags & PW_KEYBOARD) {
    prepare_managed(p->win);
    XWindowAttributes wa; XGetWindowAttributes(dpy, p->win, &wa);
    XMapRaised(dpy, p->win);
    XSync(dpy, False);
    XMoveWindow(dpy, p->win, wa.x, wa.y); // the window manager may have placed it elsewhere
  } else XMapRaised(dpy, p->win);
  p->mapped = 1;
}
void pw_hide(PWin *p) {
  if (!p->mapped) return;
  pw_release_focus(p);
  XUnmapWindow(dpy, p->win);
  p->mapped = 0;
}
int pw_visible(PWin *p) { return p->mapped; }
void pw_raise(PWin *p) { XRaiseWindow(dpy, p->win); }
void pw_input_rects(PWin *p, const PRect *r, int n) {
  XRectangle rs[64];
  if (n <= 0) { rs[0] = (XRectangle){0, 0, (unsigned short)p->w, (unsigned short)p->h}; n = 1; }
  else {
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++)
      rs[i] = (XRectangle){(short)r[i].x, (short)r[i].y, (unsigned short)(r[i].w > 0 ? r[i].w : 1), (unsigned short)(r[i].h > 0 ? r[i].h : 1)};
  }
  XShapeCombineRectangles(dpy, p->win, ShapeInput, 0, 0, rs, n, ShapeSet, Unsorted);
}

/* Every surface draws with the one context. It's made current on a surface only while drawing it, and put back
   afterwards: making the context current on a window that was never mapped corrupts later draws on this NVIDIA
   driver, so GL objects are created while the first (jelly) surface is current. */
void pw_begin(PWin *p) {
  p->prevCtx = glXGetCurrentContext(); p->prevDraw = glXGetCurrentDrawable();
  glXMakeCurrent(dpy, p->win, ctx);
  glViewport(0, 0, p->w, p->h);
}
void pw_present(PWin *p) {
  glXSwapBuffers(dpy, p->win);
  if (p->prevDraw && p->prevDraw != p->win) glXMakeCurrent(dpy, p->prevDraw, p->prevCtx);
}
void pw_vsync(PWin *p, int on) {
  typedef void (*SwapFn)(Display *, GLXDrawable, int);
  static SwapFn si;
  if (!si) si = (SwapFn)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
  if (si) si(dpy, p->win, on);
}
unsigned pw_framebuffer(PWin *p) { (void)p; return 0; }

/* ------------------------------------------------------------------ keyboard focus */

/* Focus is tracked from FocusIn / FocusOut, not assumed: if the user clicks another window, the text field is
   deactivated and we stop asking for the keyboard until they click back into it. */
static void grab(PWin *p) {
  XSync(dpy, False); // the window must be viewable before it can take focus
  // ask the window manager to activate us (as a pager would: it's the user's own click that opened this)
  XEvent ev = {0};
  ev.xclient.type = ClientMessage; ev.xclient.window = p->win; ev.xclient.format = 32;
  ev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
  ev.xclient.data.l[0] = 2; ev.xclient.data.l[1] = CurrentTime;
  XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  XSetInputFocus(dpy, p->win, RevertToParent, CurrentTime);
  if (p->ic) XSetICFocus(p->ic);
  p->focused = 1;
  if (getenv("JELLY_DEBUG")) fprintf(stderr, "kb: grab 0x%lx\n", p->win);
}
/* managed windows are mapped by the window manager a moment after we ask: activate once it's really up, and check
   that it stuck (try once more if not) */
void pw_focus_soon(PWin *p, double now) { p->grabAt = now + 0.2; p->grabTries = 2; }
void pw_tick(PWin *p, double now) {
  if (!p->grabTries || now < p->grabAt) return;
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, p->win, &wa) || wa.map_state != IsViewable) { p->grabAt = now + 0.1; return; }
  Window f; int rv;
  XGetInputFocus(dpy, &f, &rv);
  if (f == p->win) { p->grabTries = 0; p->focused = 1; return; }
  grab(p);
  p->grabTries--; p->grabAt = now + 0.35;
}
void pw_release_focus(PWin *p) { // managed windows: the window manager hands focus on when we unmap or the user clicks away
  if (!p->focused) return;
  if (p->ic) XUnsetICFocus(p->ic);
  p->focused = 0;
}
/* after each frame: grab when a text field has just been activated (never re-grab in a loop after the user moved
   focus elsewhere), release when editing ends */
void pw_want_text(PWin *p, int want) {
  if (want && !p->prevWant && !p->focused) grab(p);
  else if (!want && p->focused) pw_release_focus(p);
  p->prevWant = want;
}
void pw_clicked(PWin *p) { if (p->prevWant && !p->focused) grab(p); }
int pw_focused(PWin *p) { return p->focused; }
void pw_request_paste(PWin *p, int tag) {
  p->pasteTag = tag;
  XConvertSelection(dpy, XInternAtom(dpy, "CLIPBOARD", False), XInternAtom(dpy, "UTF8_STRING", False),
                    XInternAtom(dpy, "JELLY_PASTE", False), p->win, CurrentTime);
  XFlush(dpy);
}

/* ------------------------------------------------------------------ events */

static int map_key(KeySym s) {
  switch (s) {
  case XK_Return: return ImGuiKey_Enter;
  case XK_KP_Enter: return ImGuiKey_KeypadEnter;
  case XK_BackSpace: return ImGuiKey_Backspace;
  case XK_Delete: return ImGuiKey_Delete;
  case XK_Tab: case XK_ISO_Left_Tab: return ImGuiKey_Tab;
  case XK_Escape: return ImGuiKey_Escape;
  case XK_Left: return ImGuiKey_LeftArrow;
  case XK_Right: return ImGuiKey_RightArrow;
  case XK_Up: return ImGuiKey_UpArrow;
  case XK_Down: return ImGuiKey_DownArrow;
  case XK_Home: return ImGuiKey_Home;
  case XK_End: return ImGuiKey_End;
  case XK_Page_Up: return ImGuiKey_PageUp;
  case XK_Page_Down: return ImGuiKey_PageDown;
  case XK_Insert: return ImGuiKey_Insert;
  }
  if (s >= XK_a && s <= XK_z) return ImGuiKey_A + (int)(s - XK_a);
  if (s >= XK_A && s <= XK_Z) return ImGuiKey_A + (int)(s - XK_A);
  if (s >= XK_0 && s <= XK_9) return ImGuiKey_0 + (int)(s - XK_0);
  return ImGuiKey_None;
}

static PEvent queue[8]; // one X event can become two (a key and the text it typed)
static int qn, qi;
static char *lastPaste;

static void fill_pointer(PEvent *e, int x, int y, int rx, int ry) { e->x = (float)x; e->y = (float)y; e->rx = (float)rx; e->ry = (float)ry; }

static void translate(XEvent *xe) {
  PWin *p = find(xe->xany.window);
  PEvent e = {0};
  e.win = p;
  switch (xe->type) {
  case MotionNotify:
    e.type = PE_MOVE; fill_pointer(&e, xe->xmotion.x, xe->xmotion.y, xe->xmotion.x_root, xe->xmotion.y_root);
    queue[qn++] = e; break;
  case LeaveNotify:
    e.type = PE_LEAVE; queue[qn++] = e; break;
  case ButtonPress: case ButtonRelease: {
    int b = xe->xbutton.button, down = xe->type == ButtonPress;
    fill_pointer(&e, xe->xbutton.x, xe->xbutton.y, xe->xbutton.x_root, xe->xbutton.y_root);
    unsigned st = xe->xbutton.state;
    e.mods = (st & ControlMask ? PM_CTRL : 0) | (st & ShiftMask ? PM_SHIFT : 0) | (st & Mod1Mask ? PM_ALT : 0);
    if (b == Button4 || b == Button5) { if (!down) break; e.type = PE_WHEEL; e.wheel = b == Button4 ? 1.f : -1.f; }
    else if (b >= 1 && b <= 3) { e.type = PE_BUTTON; e.button = b == 1 ? PB_LEFT : b == 2 ? PB_MIDDLE : PB_RIGHT; e.down = down; }
    else break;
    queue[qn++] = e; break;
  }
  case FocusIn: case FocusOut:
    if (!p) break;
    if (getenv("JELLY_DEBUG"))
      fprintf(stderr, "kb: %s 0x%lx mode %d detail %d\n", xe->type == FocusIn ? "FocusIn" : "FocusOut", p->win, xe->xfocus.mode, xe->xfocus.detail);
    if (xe->type == FocusOut) {
      if (xe->xfocus.detail == NotifyInferior || xe->xfocus.mode == NotifyGrab) break;
      p->focused = 0; // someone else has the keyboard now: stop editing
    }
    e.type = PE_FOCUS; e.down = xe->type == FocusIn; queue[qn++] = e; break;
  case KeyPress: case KeyRelease: {
    XKeyEvent *ke = &xe->xkey;
    int down = xe->type == KeyPress;
    e.mods = (ke->state & ControlMask ? PM_CTRL : 0) | (ke->state & ShiftMask ? PM_SHIFT : 0) | (ke->state & Mod1Mask ? PM_ALT : 0);
    KeySym sym = ke->keycode ? XLookupKeysym(ke, 0) : NoSymbol;
    // X reports the modifiers as they were *before* this event: pressing or releasing a modifier changes its own bit
    int bit = sym == XK_Control_L || sym == XK_Control_R ? PM_CTRL : sym == XK_Shift_L || sym == XK_Shift_R ? PM_SHIFT
            : sym == XK_Alt_L || sym == XK_Alt_R || sym == XK_Meta_L || sym == XK_Meta_R ? PM_ALT : 0;
    if (bit) e.mods = down ? (e.mods | bit) : (e.mods & ~bit);
    e.type = PE_KEY; e.down = down; e.key = map_key(sym);
    queue[qn++] = e;
    if (!down || (ke->state & ControlMask)) break;
    char buf[256]; KeySym ks; Status st = XLookupNone; int n;
    if (p && p->ic) n = Xutf8LookupString(p->ic, ke, buf, sizeof buf - 1, &ks, &st);
    else { n = XLookupString(ke, buf, sizeof buf - 1, &ks, NULL); st = n > 0 ? XLookupChars : XLookupNone; }
    if ((st == XLookupChars || st == XLookupBoth) && n > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f) {
      PEvent t = {0}; t.type = PE_TEXT; t.win = p;
      buf[n] = 0; snprintf(t.text, sizeof t.text, "%.31s", buf);
      queue[qn++] = t;
    }
    break;
  }
  case SelectionNotify: {
    if (!p) break;
    e.type = PE_PASTE; e.tag = p->pasteTag;
    if (xe->xselection.property != None) {
      Atom type; int fmt; unsigned long n, left; unsigned char *data = NULL;
      if (XGetWindowProperty(dpy, p->win, xe->xselection.property, 0, 1 << 16, True, AnyPropertyType, &type, &fmt, &n, &left, &data) == Success && data) {
        e.paste = strdup((const char *)data);
        XFree(data);
      }
    }
    lastPaste = e.paste;
    queue[qn++] = e; break;
  }
  }
}

int plat_poll(PEvent *out) {
  if (qi == 0 && lastPaste) { free(lastPaste); lastPaste = NULL; } // the previous paste has been handled
  for (;;) {
    if (qi < qn) { *out = queue[qi++]; return 1; }
    qi = qn = 0;
    if (lastPaste) { free(lastPaste); lastPaste = NULL; }
    if (!XPending(dpy)) return 0;
    XEvent xe; XNextEvent(dpy, &xe);
    if (XFilterEvent(&xe, None)) continue; // the input method takes what it needs first
    translate(&xe);
  }
}

void plat_cursor(float *x, float *y) {
  Window r, c; int rx, ry, wx, wy; unsigned int mk;
  if (XQueryPointer(dpy, DefaultRootWindow(dpy), &r, &c, &rx, &ry, &wx, &wy, &mk)) { *x = (float)rx; *y = (float)ry; }
}

/* keyboard / mouse idle time from the X screensaver extension (loaded at runtime: it may not be installed) */
double plat_idle_seconds(void) {
  static int tried; static void *(*xssAlloc)(void); static int (*xssQuery)(Display *, Drawable, void *);
  static void *info;
  typedef struct { Window window; int state, kind; unsigned long til_or_since, idle, eventMask; } XSSInfo;
  if (!tried) {
    tried = 1;
    void *h = dlopen("libXss.so.1", RTLD_NOW | RTLD_LOCAL);
    if (h) { xssAlloc = (void *(*)(void))dlsym(h, "XScreenSaverAllocInfo"); xssQuery = (int (*)(Display *, Drawable, void *))dlsym(h, "XScreenSaverQueryInfo"); }
    if (xssAlloc && xssQuery) info = xssAlloc();
  }
  if (info && xssQuery(dpy, DefaultRootWindow(dpy), info)) return ((XSSInfo *)info)->idle / 1000.0;
  return -1;
}

float plat_screen_luma(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SW) w = SW - x;
  if (y + h > SH) h = SH - y;
  if (w < 8 || h < 8) return -1;
  XImage *im_ = XGetImage(dpy, DefaultRootWindow(dpy), x, y, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
  if (!im_) return -1;
  double sum = 0; int n = 0;
  for (int j = 0; j < h; j += 6)
    for (int i = 0; i < w; i += 6) {
      unsigned long px = XGetPixel(im_, i, j);
      sum += 0.2126 * ((px >> 16) & 255) + 0.7152 * ((px >> 8) & 255) + 0.0722 * (px & 255);
      n++;
    }
  XDestroyImage(im_);
  return n ? (float)(sum / n / 255.0) : -1;
}

/* ------------------------------------------------------------------ time zones */

/* mktime in another zone: switch TZ for the call. Everything that reads or switches the zone holds tzmx. */
static pthread_mutex_t tzmx = PTHREAD_MUTEX_INITIALIZER;
void plat_localtime(time_t t, struct tm *out) { pthread_mutex_lock(&tzmx); localtime_r(&t, out); pthread_mutex_unlock(&tzmx); }
time_t plat_timegm(struct tm *tm) { return timegm(tm); }
int plat_zone_known(const char *z) {
  if (!z || !*z || strstr(z, "..")) return 0;
  char p[300]; snprintf(p, sizeof p, "/usr/share/zoneinfo/%s", z);
  return access(p, R_OK) == 0;
}
time_t plat_mktime_in(struct tm *tm, const char *zone) {
  pthread_mutex_lock(&tzmx);
  char saved[256]; const char *o = getenv("TZ"); int had = o != NULL;
  if (zone) {
    if (had) snprintf(saved, sizeof saved, "%s", o);
    char z[300]; snprintf(z, sizeof z, ":%s", zone);
    setenv("TZ", z, 1); tzset();
  }
  time_t t = mktime(tm);
  if (zone) { if (had) setenv("TZ", saved, 1); else unsetenv("TZ"); tzset(); }
  pthread_mutex_unlock(&tzmx);
  return t;
}

/* ------------------------------------------------------------------ files and processes */

void plat_config_path(const char *name, char *out, size_t n) {
  const char *h = getenv("HOME");
  char d[512]; snprintf(d, sizeof d, "%s/.config", h ? h : ".");
  mkdir(d, 0755);
  strncat(d, "/jev-jelly", sizeof d - strlen(d) - 1);
  mkdir(d, 0755);
  snprintf(out, n, "%s/%s", d, name);
}
void plat_private_file(const char *path) { chmod(path, 0600); }
FILE *plat_temp_file(char *path, size_t n) {
  const char *dir = getenv("XDG_RUNTIME_DIR");
  snprintf(path, n, "%s/jev-jelly-XXXXXX", dir && *dir ? dir : "/tmp");
  int fd = mkstemp(path); // 0600
  return fd < 0 ? NULL : fdopen(fd, "w");
}
int plat_font(int cjk, char *out, size_t n) {
  static const char *ui[] = {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf", NULL};
  static const char *cj[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", NULL};
  static const char *cjb[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                              "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", NULL};
  const char **list = cjk == 0 ? ui : cjk == 1 ? cj : cjb;
  for (int i = 0; list[i]; i++) if (access(list[i], R_OK) == 0) { snprintf(out, n, "%s", list[i]); return 1; }
  return 0;
}
int plat_find_exe(const char *name, char *out, size_t n) {
  const char *h = getenv("HOME");
  char c[1024];
  snprintf(c, sizeof c, "%s/.bun/bin/%s", h ? h : "", name); // bun-installed tools (oh-my-pi)
  if (access(c, X_OK) == 0) { snprintf(out, n, "%s", c); return 1; }
  const char *path = getenv("PATH");
  char buf[4096]; snprintf(buf, sizeof buf, "%s", path ? path : "");
  for (char *save, *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
    snprintf(c, sizeof c, "%s/%s", d, name);
    if (access(c, X_OK) == 0) { snprintf(out, n, "%s", c); return 1; }
  }
  return 0;
}

void plat_path_prepend(const char *dir) {
  const char *p = getenv("PATH");
  if (p && !strncmp(p, dir, strlen(dir)) && (p[strlen(dir)] == ':' || !p[strlen(dir)])) return; // already first
  char *np = malloc(strlen(dir) + (p ? strlen(p) : 0) + 2);
  sprintf(np, "%s%s%s", dir, p && *p ? ":" : "", p ? p : "");
  setenv("PATH", np, 1);
  free(np);
}

/* posix_spawn, not fork: forking a process that holds a GL context is unsafe */
char *plat_run(const char *prog, char *const argv[], const char *in, int *status) {
  int pin[2], pout[2];
  *status = -1;
  if (pipe(pin)) return NULL;
  if (pipe(pout)) { close(pin[0]); close(pin[1]); return NULL; }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
  posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
  posix_spawn_file_actions_addclose(&fa, pin[1]);
  posix_spawn_file_actions_addclose(&fa, pout[0]);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid;
  int rc = strchr(prog, '/') ? posix_spawn(&pid, prog, &fa, NULL, argv, environ) : posix_spawnp(&pid, prog, &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(pin[0]); close(pout[1]);
  if (rc) { close(pin[1]); close(pout[0]); return NULL; }
  signal(SIGPIPE, SIG_IGN);
  if (in && write(pin[1], in, strlen(in)) < 0) {}
  close(pin[1]);
  size_t cap = 1 << 14, len = 0;
  char *buf = malloc(cap);
  ssize_t k;
  while ((k = read(pout[0], buf + len, cap - len - 1)) > 0) {
    len += (size_t)k;
    if (cap - len < 4096) { cap *= 2; buf = realloc(buf, cap); }
  }
  close(pout[0]);
  buf[len] = 0;
  int st = 0;
  waitpid(pid, &st, 0);
  *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return buf;
}
