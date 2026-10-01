// The Windows platform layer. Every surface is a layered popup window (WS_EX_LAYERED | WS_EX_TOPMOST |
// WS_EX_TOOLWINDOW): OpenGL draws into an offscreen framebuffer, the pixels are read back and handed to
// UpdateLayeredWindow with per-pixel alpha. Fully transparent pixels click through to the desktop, which is how
// Windows does what XShape input regions do on Linux; the clickable rectangles are kept at alpha >= 1 so they
// catch clicks even where nothing is drawn. One hidden window owns the GL 3.3 context, current for the whole run.
// Typing arrives as WM_CHAR (IMEs work), the clipboard through Win32, idle time from GetLastInputInfo, and time
// zones from the ICU library that ships with Windows 10 1903+.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "../plat.h"
#include "../../core/gl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

struct PWin {
  HWND hwnd;
  int flags, w, h, x, y, shown;
  // GL target: fbo (a color renderbuffer read back each frame), drawn through msaa when multisampled
  GLuint fbo, color, depth, msFbo, msColor, msDepth;
  int fw, fh;
  GLint prevFbo;
  // the pixels handed to the window
  HDC memDC;
  HBITMAP dib;
  unsigned char *bits;
  int bw, bh;
  PRect rects[64];
  int nrects;
  // keyboard
  int focused, prevWant, grabTries, pasteTag, tracking, buttons;
  double grabAt;
  WCHAR hiSurrogate;
};

static HINSTANCE inst;
static HWND glWnd;
static HDC glDC;
static HGLRC glRC;
static int softGL, SW, SH, msaaSamples = 4;
#define MAXWIN 8
static PWin *wins[MAXWIN];
static int nwins;

static PWin *find(HWND h) {
  for (int i = 0; i < nwins; i++) if (wins[i]->hwnd == h) return wins[i];
  return NULL;
}

/* ------------------------------------------------------------------ events queue */

#define QCAP 256
static PEvent q[QCAP];
static int qhead, qtail;
static char *lastPaste;
static void push(const PEvent *e) {
  int n = (qtail + 1) % QCAP;
  if (n == qhead) return; // full: drop
  q[qtail] = *e; qtail = n;
}

/* ------------------------------------------------------------------ strings */

static void to_utf8(const WCHAR *w, char *out, int n) {
  int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
  if (k <= 0) out[0] = 0;
}
static WCHAR *to_wide(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  WCHAR *w = malloc(sizeof(WCHAR) * (size_t)(n > 0 ? n : 1));
  if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
  return w;
}

/* ------------------------------------------------------------------ time */

static LARGE_INTEGER qpf;
double plat_now(void) {
  if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf); // (usable before plat_init)
  LARGE_INTEGER c; QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)qpf.QuadPart;
}
void plat_sleep(double s) { if (s > 0) Sleep((DWORD)(s * 1000 + 0.5)); }
void plat_localtime(time_t t, struct tm *out) { localtime_s(out, &t); }
time_t plat_timegm(struct tm *tm) { return _mkgmtime(tm); }

/* IANA zones through ICU (icu.dll, Windows 10 1903+), loaded at runtime: without it, times are taken as local */
typedef int (*ucal_open_t)(void);
static void *(*u_open)(const WCHAR *, int, const char *, int, int *);
static void (*u_close)(void *);
static void (*u_setDateTime)(void *, int, int, int, int, int, int, int *);
static void (*u_set)(void *, int, int);
static double (*u_getMillis)(void *, int *);
static int (*u_canonical)(const WCHAR *, int, WCHAR *, int, signed char *, int *);
static int icuTried;
static void icu_load(void) {
  if (icuTried) return;
  icuTried = 1;
  HMODULE h = LoadLibraryW(L"icu.dll");
  if (!h) return;
  u_open = (void *(*)(const WCHAR *, int, const char *, int, int *))(void *)GetProcAddress(h, "ucal_open");
  u_close = (void (*)(void *))(void *)GetProcAddress(h, "ucal_close");
  u_setDateTime = (void (*)(void *, int, int, int, int, int, int, int *))(void *)GetProcAddress(h, "ucal_setDateTime");
  u_set = (void (*)(void *, int, int))(void *)GetProcAddress(h, "ucal_set");
  u_getMillis = (double (*)(void *, int *))(void *)GetProcAddress(h, "ucal_getMillis");
  u_canonical = (int (*)(const WCHAR *, int, WCHAR *, int, signed char *, int *))(void *)GetProcAddress(h, "ucal_getCanonicalTimeZoneID");
  if (!u_open || !u_close || !u_setDateTime || !u_set || !u_getMillis) u_open = NULL;
}
static void *zone_cal(const char *iana) {
  icu_load();
  if (!u_open) return NULL;
  WCHAR *w = to_wide(iana);
  int err = 0;
  void *c = u_open(w, -1, "en_US", 1 /* UCAL_GREGORIAN */, &err);
  free(w);
  if (err > 0 && c) { u_close(c); c = NULL; }
  return c;
}
int plat_zone_known(const char *z) {
  if (!z || !*z) return 0;
  if (!strcmp(z, "UTC")) return 1;
  icu_load();
  if (!u_open || !u_canonical) return 0;
  // ICU quietly falls back to "Etc/Unknown" for a name it doesn't know: ask whether it's a real zone
  WCHAR *w = to_wide(z), out[128];
  signed char isSystem = 0; int err = 0;
  u_canonical(w, -1, out, 128, &isSystem, &err);
  free(w);
  return err <= 0 && isSystem;
}
time_t plat_mktime_in(struct tm *tm, const char *zone) {
  if (!zone) return mktime(tm);
  if (!strcmp(zone, "UTC")) return _mkgmtime(tm);
  void *c = zone_cal(zone);
  if (!c) return mktime(tm);
  int err = 0;
  u_setDateTime(c, tm->tm_year + 1900, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, &err);
  u_set(c, 14 /* UCAL_MILLISECOND */, 0);
  double ms = u_getMillis(c, &err);
  u_close(c);
  return err > 0 ? mktime(tm) : (time_t)floor(ms / 1000.0);
}

/* ------------------------------------------------------------------ keys */

static int map_key(WPARAM vk) {
  switch (vk) {
  case VK_RETURN: return ImGuiKey_Enter;
  case VK_BACK: return ImGuiKey_Backspace;
  case VK_DELETE: return ImGuiKey_Delete;
  case VK_TAB: return ImGuiKey_Tab;
  case VK_ESCAPE: return ImGuiKey_Escape;
  case VK_LEFT: return ImGuiKey_LeftArrow;
  case VK_RIGHT: return ImGuiKey_RightArrow;
  case VK_UP: return ImGuiKey_UpArrow;
  case VK_DOWN: return ImGuiKey_DownArrow;
  case VK_HOME: return ImGuiKey_Home;
  case VK_END: return ImGuiKey_End;
  case VK_PRIOR: return ImGuiKey_PageUp;
  case VK_NEXT: return ImGuiKey_PageDown;
  case VK_INSERT: return ImGuiKey_Insert;
  }
  if (vk >= 'A' && vk <= 'Z') return ImGuiKey_A + (int)(vk - 'A');
  if (vk >= '0' && vk <= '9') return ImGuiKey_0 + (int)(vk - '0');
  return ImGuiKey_None;
}
static int mods_now(void) {
  return (GetKeyState(VK_CONTROL) < 0 ? PM_CTRL : 0) | (GetKeyState(VK_SHIFT) < 0 ? PM_SHIFT : 0) |
         (GetKeyState(VK_MENU) < 0 ? PM_ALT : 0);
}

/* ------------------------------------------------------------------ window procedure */

static void pointer_event(PWin *p, int type, LPARAM lp, PEvent *e) {
  memset(e, 0, sizeof *e);
  e->type = type; e->win = p;
  POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  e->x = (float)pt.x; e->y = (float)pt.y;
  ClientToScreen(p->hwnd, &pt);
  e->rx = (float)pt.x; e->ry = (float)pt.y;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  PWin *p = find(h);
  if (!p) return DefWindowProcW(h, msg, wp, lp);
  PEvent e;
  switch (msg) {
  case WM_MOUSEMOVE:
    if (!p->tracking) { TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); p->tracking = 1; }
    pointer_event(p, PE_MOVE, lp, &e); push(&e); return 0;
  case WM_MOUSELEAVE:
    p->tracking = 0;
    memset(&e, 0, sizeof e); e.type = PE_LEAVE; e.win = p; push(&e); return 0;
  case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
  case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
    int down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
    int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? PB_LEFT : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? PB_RIGHT : PB_MIDDLE;
    // keep getting the pointer while a button is held, even outside (dragging the jelly across the screen)
    if (down) { if (!p->buttons++) SetCapture(h); }
    else if (p->buttons > 0 && !--p->buttons) ReleaseCapture();
    pointer_event(p, PE_BUTTON, lp, &e); e.button = b; e.down = down; push(&e);
    return 0;
  }
  case WM_CAPTURECHANGED: p->buttons = 0; return 0;
  case WM_MOUSEWHEEL: {
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; // screen coordinates
    memset(&e, 0, sizeof e); e.type = PE_WHEEL; e.win = p;
    e.rx = (float)pt.x; e.ry = (float)pt.y;
    ScreenToClient(h, &pt); e.x = (float)pt.x; e.y = (float)pt.y;
    e.wheel = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
    push(&e); return 0;
  }
  case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    memset(&e, 0, sizeof e); e.type = PE_KEY; e.win = p;
    e.down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    e.key = map_key(wp); e.mods = mods_now();
    push(&e);
    if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) break; // Alt+F4 etc. still work
    return 0;
  case WM_CHAR: {
    WCHAR c = (WCHAR)wp;
    if (c >= 0xD800 && c <= 0xDBFF) { p->hiSurrogate = c; return 0; } // the first half of a character
    WCHAR w[3] = {0};
    if (c >= 0xDC00 && c <= 0xDFFF && p->hiSurrogate) { w[0] = p->hiSurrogate; w[1] = c; }
    else w[0] = c;
    p->hiSurrogate = 0;
    if (w[0] < 0x20 || w[0] == 0x7f) return 0; // Enter, Tab, Backspace come as keys
    if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) return 0; // Ctrl+letter shortcuts
    memset(&e, 0, sizeof e); e.type = PE_TEXT; e.win = p;
    to_utf8(w, e.text, sizeof e.text);
    push(&e); return 0;
  }
  case WM_SETFOCUS: case WM_KILLFOCUS:
    p->focused = msg == WM_SETFOCUS;
    memset(&e, 0, sizeof e); e.type = PE_FOCUS; e.win = p; e.down = p->focused; push(&e); return 0;
  case WM_MOUSEACTIVATE: // only the windows you type in take the focus when clicked
    return (p->flags & PW_KEYBOARD) ? MA_ACTIVATE : MA_NOACTIVATE;
  case WM_SETCURSOR:
    if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(NULL, (p->flags & PW_HAND) ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW)); return TRUE; }
    break;
  case WM_CLOSE: return 0; // Alt+F4 on a panel: ignore (it closes with ×)
  }
  return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ start-up */

/* One jelly at a time: a second start (another click on the shortcut) just exits. */
int plat_main_guard(int argc, char **argv) {
  (void)argc; (void)argv;
  if (getenv("JELLY_SMOKE")) return -1;
  CreateMutexW(NULL, TRUE, L"Local\\jev-jelly");
  return GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

void *plat_gl_proc(const char *name) {
  void *f = (void *)wglGetProcAddress(name);
  if (!f || f == (void *)1 || f == (void *)2 || f == (void *)3 || f == (void *)-1) {
    static HMODULE gl32;
    if (!gl32) gl32 = LoadLibraryW(L"opengl32.dll");
    f = (void *)GetProcAddress(gl32, name);
  }
  return f;
}

int plat_init(int wantSoft) {
  softGL = wantSoft;
  inst = GetModuleHandleW(NULL);
  QueryPerformanceFrequency(&qpf);
  timeBeginPeriod(1); // Sleep() at millisecond precision, for a steady 60 fps
  // real pixels: the jelly and the panels are drawn at the screen's own resolution
  typedef BOOL(WINAPI * SetCtx)(HANDLE);
  SetCtx sc = (SetCtx)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
  if (!sc || !sc((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
  SW = GetSystemMetrics(SM_CXSCREEN); SH = GetSystemMetrics(SM_CYSCREEN);

  WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc;
  wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"JellySurface";
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  RegisterClassExW(&wc);
  WNDCLASSEXW gc; memset(&gc, 0, sizeof gc); gc.cbSize = sizeof gc;
  gc.lpfnWndProc = DefWindowProcW; gc.hInstance = inst; gc.lpszClassName = L"JellyGL"; gc.style = CS_OWNDC;
  RegisterClassExW(&gc);

  // the GL context lives on a hidden window; surfaces draw into framebuffers, never into it
  glWnd = CreateWindowExW(0, L"JellyGL", L"jelly gl", WS_POPUP, 0, 0, 16, 16, NULL, NULL, inst, NULL);
  glDC = GetDC(glWnd);
  PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA,
                               32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0};
  int pf = ChoosePixelFormat(glDC, &pfd);
  if (!pf || !SetPixelFormat(glDC, pf, &pfd)) { fprintf(stderr, "no OpenGL pixel format\n"); return 0; }
  HGLRC tmp = wglCreateContext(glDC);
  if (!tmp || !wglMakeCurrent(glDC, tmp)) { fprintf(stderr, "cannot create an OpenGL context\n"); return 0; }
  typedef HGLRC(WINAPI * MkCtx)(HDC, HGLRC, const int *);
  MkCtx mk = (MkCtx)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
  int attr[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3, WGL_CONTEXT_PROFILE_MASK_ARB,
                WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
  glRC = mk ? mk(glDC, NULL, attr) : NULL;
  if (!glRC) {
    MessageBoxW(NULL, L"Jelly needs OpenGL 3.3. Please update your graphics driver.", L"jelly", MB_ICONERROR);
    return 0;
  }
  wglMakeCurrent(glDC, glRC);
  wglDeleteContext(tmp);
  if (!gl_load()) return 0;
  if (softGL) msaaSamples = 0;
  return 1;
}

void plat_quit(void) {
  for (int i = 0; i < nwins; i++) DestroyWindow(wins[i]->hwnd);
  wglMakeCurrent(NULL, NULL);
  if (glRC) wglDeleteContext(glRC);
  if (glWnd) DestroyWindow(glWnd);
  timeEndPeriod(1);
}
int plat_soft_gl(void) { return softGL; }
void plat_screen(int *w, int *h) { *w = SW; *h = SH; }
void plat_flush(void) {}

/* ------------------------------------------------------------------ surfaces */

PWin *pw_create(const char *name, int x, int y, int w, int h, int flags) {
  if (nwins == MAXWIN) return NULL;
  PWin *p = calloc(1, sizeof *p);
  p->flags = flags; p->w = w; p->h = h; p->x = x; p->y = y;
  DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW; // no taskbar button
  if (!(flags & PW_KEYBOARD)) ex |= WS_EX_NOACTIVATE;         // clicking the jelly doesn't steal the focus
  WCHAR *title = to_wide(name);
  wins[nwins++] = p; // before CreateWindow: wndproc looks it up
  p->hwnd = CreateWindowExW(ex, L"JellySurface", title, WS_POPUP, x, y, w, h, NULL, NULL, inst, NULL);
  free(title);
  return p;
}

void pw_move(PWin *p, int x, int y) {
  p->x = x; p->y = y;
  SetWindowPos(p->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void pw_resize(PWin *p, int w, int h) { p->w = w; p->h = h; }
void pw_show(PWin *p) {
  ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
  SetWindowPos(p->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  p->shown = 1;
}
void pw_hide(PWin *p) {
  if (!p->shown) return;
  ShowWindow(p->hwnd, SW_HIDE);
  p->shown = 0; p->focused = 0;
}
int pw_visible(PWin *p) { return p->shown; }
void pw_raise(PWin *p) { SetWindowPos(p->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
void pw_input_rects(PWin *p, const PRect *r, int n) {
  if (n > 64) n = 64;
  p->nrects = n > 0 ? n : 0;
  for (int i = 0; i < p->nrects; i++) p->rects[i] = r[i];
}

static void targets(PWin *p) { // (re)create the GL targets and the pixel buffer at the surface's size
  if (p->fbo && p->fw == p->w && p->fh == p->h) return;
  if (p->fbo) {
    glDeleteFramebuffers(1, &p->fbo); glDeleteRenderbuffers(1, &p->color); glDeleteRenderbuffers(1, &p->depth);
    if (p->msFbo) { glDeleteFramebuffers(1, &p->msFbo); glDeleteRenderbuffers(1, &p->msColor); glDeleteRenderbuffers(1, &p->msDepth); }
    p->msFbo = 0;
  }
  p->fw = p->w; p->fh = p->h;
  glGenFramebuffers(1, &p->fbo); glBindFramebuffer(GL_FRAMEBUFFER, p->fbo);
  glGenRenderbuffers(1, &p->color); glBindRenderbuffer(GL_RENDERBUFFER, p->color);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, p->w, p->h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, p->color);
  glGenRenderbuffers(1, &p->depth); glBindRenderbuffer(GL_RENDERBUFFER, p->depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, p->w, p->h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, p->depth);
  if ((p->flags & PW_MSAA) && msaaSamples) {
    glGenFramebuffers(1, &p->msFbo); glBindFramebuffer(GL_FRAMEBUFFER, p->msFbo);
    glGenRenderbuffers(1, &p->msColor); glBindRenderbuffer(GL_RENDERBUFFER, p->msColor);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaaSamples, GL_RGBA8, p->w, p->h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, p->msColor);
    glGenRenderbuffers(1, &p->msDepth); glBindRenderbuffer(GL_RENDERBUFFER, p->msDepth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaaSamples, GL_DEPTH24_STENCIL8, p->w, p->h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, p->msDepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { // no multisampling here: draw plainly
      glDeleteFramebuffers(1, &p->msFbo); p->msFbo = 0;
    }
  }
  if (p->dib) { DeleteObject(p->dib); DeleteDC(p->memDC); }
  BITMAPINFO bi; memset(&bi, 0, sizeof bi); // bottom-up rows, like glReadPixels
  bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = p->w; bi.bmiHeader.biHeight = p->h;
  bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
  HDC screen = GetDC(NULL);
  p->memDC = CreateCompatibleDC(screen);
  p->dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&p->bits, NULL, 0);
  SelectObject(p->memDC, p->dib);
  ReleaseDC(NULL, screen);
  p->bw = p->w; p->bh = p->h;
}

unsigned pw_framebuffer(PWin *p) { return p->msFbo ? p->msFbo : p->fbo; }
void pw_begin(PWin *p) {
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &p->prevFbo);
  targets(p);
  glBindFramebuffer(GL_FRAMEBUFFER, pw_framebuffer(p));
  glViewport(0, 0, p->w, p->h);
}
void pw_present(PWin *p) {
  if (p->msFbo) { // resolve the multisampled drawing
    glBindFramebuffer(GL_READ_FRAMEBUFFER, p->msFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p->fbo);
    glBlitFramebuffer(0, 0, p->w, p->h, 0, 0, p->w, p->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, p->fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, p->w, p->h, GL_BGRA, GL_UNSIGNED_BYTE, p->bits); // premultiplied, as UpdateLayeredWindow wants
  // the clickable area: fully transparent pixels there get alpha 1 (invisible) so they still catch the pointer
  for (int i = 0; i < p->nrects; i++) {
    PRect r = p->rects[i];
    int x0 = r.x < 0 ? 0 : r.x, x1 = r.x + r.w > p->w ? p->w : r.x + r.w;
    int y0 = r.y < 0 ? 0 : r.y, y1 = r.y + r.h > p->h ? p->h : r.y + r.h;
    for (int y = y0; y < y1; y++) {
      unsigned char *row = p->bits + (size_t)(p->h - 1 - y) * (size_t)p->w * 4;
      for (int x = x0; x < x1; x++) if (!row[x * 4 + 3]) row[x * 4 + 3] = 1;
    }
  }
  POINT pos = {p->x, p->y}, src = {0, 0};
  SIZE size = {p->w, p->h};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(p->hwnd, NULL, &pos, &size, p->memDC, &src, 0, &bf, ULW_ALPHA);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)p->prevFbo);
}
void pw_vsync(PWin *p, int on) { (void)p; (void)on; } // frames are paced by the main loop

/* ------------------------------------------------------------------ keyboard focus */

static void grab(PWin *p) {
  if (!p->shown) return;
  // the user's own click opened this window, so Windows lets us bring it to the front
  SetForegroundWindow(p->hwnd);
  SetFocus(p->hwnd);
}
void pw_focus_soon(PWin *p, double now) { p->grabAt = now + 0.05; p->grabTries = 2; }
void pw_tick(PWin *p, double now) {
  if (!p->grabTries || now < p->grabAt) return;
  if (GetForegroundWindow() == p->hwnd) { p->grabTries = 0; p->focused = 1; return; }
  grab(p);
  p->grabTries--; p->grabAt = now + 0.3;
}
void pw_release_focus(PWin *p) { p->focused = 0; }
void pw_want_text(PWin *p, int want) {
  if (want && !p->prevWant && !p->focused) grab(p);
  p->prevWant = want;
}
void pw_clicked(PWin *p) { if (p->prevWant && !p->focused) grab(p); }
int pw_focused(PWin *p) { return p->focused; }
void pw_request_paste(PWin *p, int tag) {
  PEvent e; memset(&e, 0, sizeof e);
  e.type = PE_PASTE; e.win = p; e.tag = tag;
  if (OpenClipboard(p->hwnd)) {
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *w = h ? (const WCHAR *)GlobalLock(h) : NULL;
    if (w) {
      int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
      if (n > 0) { e.paste = malloc((size_t)n); WideCharToMultiByte(CP_UTF8, 0, w, -1, e.paste, n, NULL, NULL); }
      GlobalUnlock(h);
    }
    CloseClipboard();
  }
  if (e.paste) { char *o = e.paste; for (char *s = e.paste; *s; s++) if (*s != '\r') *o++ = *s; *o = 0; } // CRLF -> LF
  push(&e);
}

/* ------------------------------------------------------------------ events and the desktop */

int plat_poll(PEvent *out) {
  if (lastPaste) { free(lastPaste); lastPaste = NULL; } // the previous paste has been handled
  MSG m;
  while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m); // WM_KEYDOWN -> WM_CHAR (and the IME's composed text)
    DispatchMessageW(&m);
  }
  if (qhead == qtail) return 0;
  *out = q[qhead]; qhead = (qhead + 1) % QCAP;
  if (out->type == PE_PASTE) lastPaste = out->paste;
  return 1;
}

void plat_cursor(float *x, float *y) { POINT p; if (GetCursorPos(&p)) { *x = (float)p.x; *y = (float)p.y; } }
double plat_idle_seconds(void) {
  LASTINPUTINFO li = {sizeof li, 0};
  if (!GetLastInputInfo(&li)) return -1;
  return (GetTickCount() - li.dwTime) / 1000.0;
}
float plat_screen_luma(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SW) w = SW - x;
  if (y + h > SH) h = SH - y;
  if (w < 8 || h < 8) return -1;
  HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
  BITMAPINFO bi; memset(&bi, 0, sizeof bi); // top-down rows
  bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
  unsigned char *px = NULL;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
  float out = -1;
  if (bmp) {
    HGDIOBJ old = SelectObject(mem, bmp);
    if (BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY)) { // (without CAPTUREBLT: our own layered windows aren't in it)
      double sum = 0; int n = 0;
      for (int j = 0; j < h; j += 6)
        for (int i = 0; i < w; i += 6) {
          unsigned char *c = px + ((size_t)j * (size_t)w + (size_t)i) * 4;
          sum += 0.0722 * c[0] + 0.7152 * c[1] + 0.2126 * c[2];
          n++;
        }
      if (n) out = (float)(sum / n / 255.0);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
  }
  DeleteDC(mem); ReleaseDC(NULL, screen);
  return out;
}

/* ------------------------------------------------------------------ files and processes */

void plat_config_path(const char *name, char *out, size_t n) {
  char d[MAX_PATH * 3] = "";
  const char *a = getenv("APPDATA"); // UTF-8: the manifest sets the process code page to UTF-8
  snprintf(d, sizeof d, "%s\\jev-jelly", a && *a ? a : ".");
  CreateDirectoryA(d, NULL);
  snprintf(out, n, "%s\\%s", d, name);
}
void plat_private_file(const char *path) { (void)path; } // %APPDATA% is already private to the user
FILE *plat_temp_file(char *path, size_t n) {
  char dir[MAX_PATH], name[MAX_PATH];
  if (!GetTempPathA(sizeof dir, dir) || !GetTempFileNameA(dir, "jly", 0, name)) return NULL;
  snprintf(path, n, "%s", name);
  return fopen(path, "wb");
}
int plat_font(int cjk, char *out, size_t n) {
  char win[MAX_PATH]; GetWindowsDirectoryA(win, sizeof win);
  static const char *ui[] = {"segoeui.ttf", "arial.ttf", NULL};
  static const char *cj[] = {"msyh.ttc", "YuGothR.ttc", "meiryo.ttc", "malgun.ttf", "simsun.ttc", NULL};
  static const char *cjb[] = {"msyhbd.ttc", "YuGothB.ttc", "msyh.ttc", "meiryo.ttc", "malgunbd.ttf", "malgun.ttf", NULL};
  const char **list = cjk == 0 ? ui : cjk == 1 ? cj : cjb;
  for (int i = 0; list[i]; i++) {
    char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\Fonts\\%s", win, list[i]);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) { snprintf(out, n, "%s", p); return 1; }
  }
  return 0;
}
static int exists(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
int plat_find_exe(const char *name, char *out, size_t n) {
  static const char *ext[] = {".exe", ".cmd", ".bat", ""};
  char c[2048];
  const char *home = getenv("USERPROFILE");
  for (int e = 0; ext[e][0]; e++) { // bun-installed tools (oh-my-pi)
    snprintf(c, sizeof c, "%s\\.bun\\bin\\%s%s", home ? home : "", name, ext[e]);
    if (exists(c)) { snprintf(out, n, "%s", c); return 1; }
  }
  const char *path = getenv("PATH");
  char *buf = _strdup(path ? path : "");
  int found = 0;
  for (char *save, *d = strtok_s(buf, ";", &save); d && !found; d = strtok_s(NULL, ";", &save))
    for (int e = 0; ext[e][0] && !found; e++) {
      snprintf(c, sizeof c, "%s\\%s%s", d, name, ext[e]);
      if (exists(c)) { snprintf(out, n, "%s", c); found = 1; }
    }
  free(buf);
  return found;
}

void plat_path_prepend(const char *dir) {
  const char *p = getenv("PATH");
  size_t n = strlen(dir) + (p ? strlen(p) : 0) + 2;
  char *np = malloc(n);
  snprintf(np, n, "%s;%s", dir, p ? p : "");
  _putenv_s("PATH", np);
  SetEnvironmentVariableA("PATH", np); // what child processes inherit
  free(np);
}

/* Windows command-line quoting (the rules CommandLineToArgvW and the C runtime parse by) */
static void quote_arg(char **buf, size_t *len, size_t *cap, const char *a) {
#define PUT(ch) do { if (*len + 2 > *cap) { *cap *= 2; *buf = realloc(*buf, *cap); } (*buf)[(*len)++] = (ch); } while (0)
  if (*len) PUT(' ');
  if (*a && !strpbrk(a, " \t\n\v\"")) { for (; *a; a++) PUT(*a); (*buf)[*len] = 0; return; }
  PUT('"');
  for (const char *s = a;; s++) {
    int bs = 0;
    while (*s == '\\') { s++; bs++; }
    if (!*s) { for (int i = 0; i < bs * 2; i++) PUT('\\'); break; }
    if (*s == '"') { for (int i = 0; i < bs * 2 + 1; i++) PUT('\\'); PUT('"'); }
    else { for (int i = 0; i < bs; i++) PUT('\\'); PUT(*s); }
  }
  PUT('"');
  (*buf)[*len] = 0;
#undef PUT
}

char *plat_run(const char *prog, char *const argv[], const char *in, int *status) {
  *status = -1;
  char exe[1024];
  if (strchr(prog, '\\') || strchr(prog, '/')) snprintf(exe, sizeof exe, "%s", prog);
  else if (!plat_find_exe(prog, exe, sizeof exe)) return NULL;
  size_t len = 0, cap = 1024;
  char *cmd = malloc(cap); cmd[0] = 0;
  const char *dot = strrchr(exe, '.');
  int script = dot && (!_stricmp(dot, ".cmd") || !_stricmp(dot, ".bat"));
  if (script) { quote_arg(&cmd, &len, &cap, "cmd.exe"); quote_arg(&cmd, &len, &cap, "/d"); quote_arg(&cmd, &len, &cap, "/c"); }
  quote_arg(&cmd, &len, &cap, exe);
  for (int i = 1; argv[i]; i++) quote_arg(&cmd, &len, &cap, argv[i]);

  SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
  HANDLE inR, inW, outR, outW;
  if (!CreatePipe(&inR, &inW, &sa, 0)) { free(cmd); return NULL; }
  if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR); CloseHandle(inW); free(cmd); return NULL; }
  SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
  STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = nul;
  PROCESS_INFORMATION pi;
  WCHAR *wcmd = to_wide(cmd);
  free(cmd);
  BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  free(wcmd);
  CloseHandle(inR); CloseHandle(outW); CloseHandle(nul);
  if (!ok) { CloseHandle(inW); CloseHandle(outR); return NULL; }
  if (in) { DWORD wr; WriteFile(inW, in, (DWORD)strlen(in), &wr, NULL); }
  CloseHandle(inW);
  size_t blen = 0, bcap = 1 << 14;
  char *buf = malloc(bcap);
  DWORD got;
  while (ReadFile(outR, buf + blen, (DWORD)(bcap - blen - 1), &got, NULL) && got > 0) {
    blen += got;
    if (bcap - blen < 4096) { bcap *= 2; buf = realloc(buf, bcap); }
  }
  buf[blen] = 0;
  CloseHandle(outR);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = (DWORD)-1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  *status = (int)code;
  return buf;
}
