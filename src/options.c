// Right-click options panel: a borderless ARGB window next to the jelly, drawn with cimgui + the OpenGL3 backend.
// Mouse input comes straight from X events; there's no keyboard because every control works by mouse.

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#define CIMGUI_USE_OPENGL3
#include "cimgui_impl.h"
#include "jelly.h"
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <GL/gl.h>
#include <math.h>
#include <stdio.h>

static Display *dpy;
static GLXFBConfig fbc;
static Window win;
static GLXContext ctx;
static ImGuiContext *ig;
static int open_, W = 340, H = 760, panelH = 640; // fixed canvas; clicks limited to the panel via XShape
static int wantClose;

static const char *FLEX_NAMES[] = {"Firm", "Bouncy", "Jiggly", "Wobbly", "Gooey"};
static const char *PULL_NAMES[] = {"Pinch", "Small", "Half body", "Big", "Most"};
static int showWheel;

static ImVec4_c RGB(unsigned hex, float a) {
  return (ImVec4_c){((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a};
}
static ImU32 U32(unsigned hex, float a) {
  return ((ImU32)(a * 255) << 24) | ((hex & 255) << 16) | (((hex >> 8) & 255) << 8) | ((hex >> 16) & 255);
}
static ImU32 U32f(float r, float g, float b) {
  return (255u << 24) | ((ImU32)(b * 255) << 16) | ((ImU32)(g * 255) << 8) | (ImU32)(r * 255);
}
static void label(const char *t) { // muted section label, Primer style
  igDummy((ImVec2_c){0, 2});
  igPushFont(NULL, 14.5f); igTextDisabled("%s", t); igPopFont();
}
/* Primer-like segmented control: returns 1 when the selection changed */
static int segmented(const char *id, const char **items, int n, int *val, float width) {
  int changed = 0;
  ImDrawList *dl = igGetWindowDrawList();
  ImVec2_c p = igGetCursorScreenPos();
  float h = 32, pad = 3, w = (width - pad * 2) / n;
  ImDrawList_AddRectFilled(dl, p, (ImVec2_c){p.x + width, p.y + h}, U32(0x0d1117, 1), 6, 0);
  ImDrawList_AddRect(dl, p, (ImVec2_c){p.x + width, p.y + h}, U32(0x30363d, 1), 6, 1, 0);
  igPushID_Str(id);
  for (int i = 0; i < n; i++) {
    ImVec2_c a = {p.x + pad + w * i, p.y + pad}, b = {a.x + w, p.y + h - pad};
    igSetCursorScreenPos(a);
    igPushID_Int(i);
    if (igInvisibleButton("##seg", (ImVec2_c){w, h - pad * 2}, 0) && *val != i) { *val = i; changed = 1; }
    int hov = igIsItemHovered(0);
    igPopID();
    if (*val == i) {
      ImDrawList_AddRectFilled(dl, a, b, U32(0x30363d, 1), 5, 0);
      ImDrawList_AddRect(dl, a, b, U32(0x484f58, 1), 5, 1, 0);
    } else if (hov) ImDrawList_AddRectFilled(dl, a, b, U32(0x21262d, 1), 5, 0);
    ImVec2_c ts = igCalcTextSize(items[i], NULL, false, -1);
    ImDrawList_AddText_Vec2(dl, (ImVec2_c){a.x + (w - ts.x) / 2, a.y + (h - pad * 2 - ts.y) / 2},
                            *val == i ? U32(0xe6edf3, 1) : U32(0x7d8590, 1), items[i], NULL);
  }
  igPopID();
  igSetCursorScreenPos((ImVec2_c){p.x, p.y + h});
  igDummy((ImVec2_c){width, 0});
  return changed;
}

void opt_init(Display *d, GLXFBConfig fb) { dpy = d; fbc = fb; }
int opt_is_open(void) { return open_; }
int opt_owns(Window w) { return open_ && w == win; }

static void create(int x, int y) {
  XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbc);
  Window root = DefaultRootWindow(dpy);
  XSetWindowAttributes swa = {0};
  swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
  swa.override_redirect = True;
  swa.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | ExposureMask;
  win = XCreateWindow(dpy, root, x, y, W, H, 0, vi->depth, InputOutput, vi->visual,
                      CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
  XStoreName(dpy, win, "jelly options");
  XFree(vi);

  typedef GLXContext (*CtxFn)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
  CtxFn mk = (CtxFn)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
  int cattr[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3, GLX_CONTEXT_PROFILE_MASK_ARB,
                 GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
  if (mk) ctx = mk(dpy, fbc, NULL, True, cattr);
  if (!ctx) ctx = glXCreateNewContext(dpy, fbc, GLX_RGBA_TYPE, NULL, True);
  XMapRaised(dpy, win);

  GLXContext prevCtx = glXGetCurrentContext();
  GLXDrawable prevDraw = glXGetCurrentDrawable();
  glXMakeCurrent(dpy, win, ctx);
  typedef void (*SwapFn)(Display *, GLXDrawable, int);
  SwapFn si = (SwapFn)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
  if (si) si(dpy, win, 0); // the jelly's swap already paces the loop

  ig = igCreateContext(NULL);
  igSetCurrentContext(ig);
  ImGuiIO *io = igGetIO_Nil();
  io->IniFilename = NULL;
  const char *fonts[] = {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
  for (int i = 0; i < 2; i++) {
    FILE *f = fopen(fonts[i], "rb");
    if (f) { fclose(f); ImFontAtlas_AddFontFromFileTTF(io->Fonts, fonts[i], 17.0f, NULL, NULL); break; }
  }
  ImGuiStyle *s = igGetStyle();
  igStyleColorsDark(s);
  // GitHub Primer (dark) palette
  s->WindowRounding = 12; s->FrameRounding = 6; s->GrabRounding = 6; s->PopupRounding = 6; s->ChildRounding = 6;
  s->WindowPadding = (ImVec2_c){16, 14}; s->FramePadding = (ImVec2_c){10, 6}; s->ItemSpacing = (ImVec2_c){8, 8};
  s->WindowBorderSize = 1; s->FrameBorderSize = 1; s->GrabMinSize = 14;
  ImVec4_c *c = s->Colors;
  c[ImGuiCol_WindowBg] = RGB(0x161b22, 0.98f);
  c[ImGuiCol_Border] = RGB(0x30363d, 1);
  c[ImGuiCol_Text] = RGB(0xe6edf3, 1);
  c[ImGuiCol_TextDisabled] = RGB(0x7d8590, 1);
  c[ImGuiCol_FrameBg] = RGB(0x0d1117, 1);
  c[ImGuiCol_FrameBgHovered] = RGB(0x0d1117, 1);
  c[ImGuiCol_FrameBgActive] = RGB(0x0d1117, 1);
  c[ImGuiCol_Button] = RGB(0x21262d, 1);
  c[ImGuiCol_ButtonHovered] = RGB(0x30363d, 1);
  c[ImGuiCol_ButtonActive] = RGB(0x282e33, 1);
  c[ImGuiCol_SliderGrab] = RGB(0x2f81f7, 1);
  c[ImGuiCol_SliderGrabActive] = RGB(0x58a6ff, 1);
  c[ImGuiCol_Separator] = RGB(0x21262d, 1);
  c[ImGuiCol_PopupBg] = RGB(0x161b22, 1);
  ImGui_ImplOpenGL3_Init("#version 330 core");
  glXMakeCurrent(dpy, prevDraw, prevCtx);
}

void opt_open(int x, int y) {
  int sw = DisplayWidth(dpy, DefaultScreen(dpy)), sh = DisplayHeight(dpy, DefaultScreen(dpy));
  if (x + W > sw) x = sw - W - 8;
  if (y + panelH > sh) y = sh - panelH - 8;
  if (x < 8) x = 8;
  if (y < 8) y = 8;
  if (!win) create(x, y);
  else { XMoveWindow(dpy, win, x, y); XMapRaised(dpy, win); }
  open_ = 1; wantClose = 0;
}

void opt_close(void) {
  if (!open_) return;
  XUnmapWindow(dpy, win);
  open_ = 0;
}

void opt_event(XEvent *e) {
  if (!open_) return;
  igSetCurrentContext(ig);
  ImGuiIO *io = igGetIO_Nil();
  switch (e->type) {
  case MotionNotify: ImGuiIO_AddMousePosEvent(io, (float)e->xmotion.x, (float)e->xmotion.y); break;
  case LeaveNotify: ImGuiIO_AddMousePosEvent(io, -3.4e38f, -3.4e38f); break;
  case ButtonPress:
  case ButtonRelease: {
    int down = e->type == ButtonPress, b = e->xbutton.button;
    ImGuiIO_AddMousePosEvent(io, (float)e->xbutton.x, (float)e->xbutton.y);
    if (b == Button1) ImGuiIO_AddMouseButtonEvent(io, 0, down);
    else if (b == Button3) ImGuiIO_AddMouseButtonEvent(io, 1, down);
    else if (b == Button2) ImGuiIO_AddMouseButtonEvent(io, 2, down);
    else if (down && b == Button4) ImGuiIO_AddMouseWheelEvent(io, 0, 1);
    else if (down && b == Button5) ImGuiIO_AddMouseWheelEvent(io, 0, -1);
    break;
  }
  }
}

static int flex_index(float f) { int i = (int)lroundf(f * 4); return i < 0 ? 0 : i > 4 ? 4 : i; }

int opt_frame(Cfg *c, double dt) {
  if (!open_) return OPT_NONE;
  int out = 0;
  GLXContext prevCtx = glXGetCurrentContext();
  GLXDrawable prevDraw = glXGetCurrentDrawable();
  glXMakeCurrent(dpy, win, ctx);
  igSetCurrentContext(ig);
  ImGuiIO *io = igGetIO_Nil();
  io->DisplaySize = (ImVec2_c){(float)W, (float)H};
  io->DeltaTime = dt > 0 ? (float)dt : 1.0f / 60;
  ImGui_ImplOpenGL3_NewFrame();
  igNewFrame();

  igSetNextWindowPos((ImVec2_c){0, 0}, ImGuiCond_Always, (ImVec2_c){0, 0});
  igSetNextWindowSize((ImVec2_c){(float)W, 0}, ImGuiCond_Always);
  igBegin("##jelly", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings);
  float full = (float)W - 16 * 2;
  // header
  ImDrawList *dl = igGetWindowDrawList();
  ImVec2_c hp = igGetCursorScreenPos();
  ImDrawList_AddCircleFilled(dl, (ImVec2_c){hp.x + 9, hp.y + 11}, 8, U32f(c->col[0], c->col[1], c->col[2]), 24);
  igSetCursorScreenPos((ImVec2_c){hp.x + 24, hp.y});
  igText("Jelly friend");
  igSameLine(full - 14, 0);
  igPushStyleColor_Vec4(ImGuiCol_Button, RGB(0x161b22, 0));
  igPushStyleVar_Float(ImGuiStyleVar_FrameBorderSize, 0);
  if (igButton("x", (ImVec2_c){26, 0})) wantClose = 1;
  igPopStyleVar(1); igPopStyleColor(1);
  igSeparator();

  label("CHARACTER");
  static const char *CH[] = {"Boy", "Girl"};
  if (segmented("ch", CH, 2, &c->girl, full)) out |= OPT_CHANGED;

  label("FACE");
  static const char *FC[] = {"Tiny", "Classic"};
  int fi = c->face == FACE_CLASSIC;
  if (segmented("face", FC, 2, &fi, full)) { c->face = fi ? FACE_CLASSIC : FACE_TINY; out |= OPT_CHANGED; }

  label("FLAVOR");
  {
    float d = 30, gap = (full - d * 8) / 7;
    ImVec2_c p = igGetCursorScreenPos();
    for (int i = 0; i < 8; i++) {
      ImVec2_c a = {p.x + i * (d + gap), p.y};
      ImVec2_c ctr = {a.x + d / 2, a.y + d / 2};
      igSetCursorScreenPos(a);
      igPushID_Int(i);
      int clicked = igInvisibleButton("##sw", (ImVec2_c){d, d}, 0), hov = igIsItemHovered(0);
      igPopID();
      int sel = i < NFLAVORS ? c->flavor == i : (c->flavor < 0 || showWheel);
      if (i < NFLAVORS) {
        ImDrawList_AddCircleFilled(dl, ctr, d / 2 - 3, U32f(FLAVOR_COLS[i][0], FLAVOR_COLS[i][1], FLAVOR_COLS[i][2]), 32);
        if (hov) igSetTooltip("%s", FLAVOR_NAMES[i]);
        if (clicked) { c->flavor = i; c->col[0] = FLAVOR_COLS[i][0]; c->col[1] = FLAVOR_COLS[i][1]; c->col[2] = FLAVOR_COLS[i][2]; out |= OPT_CHANGED; }
      } else { // custom: rainbow ring that opens the wheel
        for (int k = 0; k < 12; k++) {
          float a0 = k / 12.f * 6.2832f, a1 = (k + 1) / 12.f * 6.2832f, hr, hg, hb;
          igColorConvertHSVtoRGB(k / 12.f, 0.75f, 1, &hr, &hg, &hb);
          ImDrawList_PathClear(dl);
          ImDrawList_PathArcTo(dl, ctr, d / 2 - 6, a0, a1, 6);
          ImDrawList_PathStroke(dl, U32f(hr, hg, hb), 5, 0);
        }
        if (c->flavor < 0) ImDrawList_AddCircleFilled(dl, ctr, d / 2 - 10, U32f(c->col[0], c->col[1], c->col[2]), 24);
        if (hov) igSetTooltip("Any color");
        if (clicked) showWheel = !showWheel;
      }
      if (sel) ImDrawList_AddCircle(dl, ctr, d / 2, U32(0x2f81f7, 1), 32, 2);
      else if (hov) ImDrawList_AddCircle(dl, ctr, d / 2, U32(0x484f58, 1), 32, 1.5f);
    }
    igSetCursorScreenPos((ImVec2_c){p.x, p.y + d});
    igDummy((ImVec2_c){full, 0});
  }
  if (showWheel) {
    igSetCursorPosX(((float)W - 180) / 2);
    igSetNextItemWidth(180);
    if (igColorPicker3("##custom", c->col, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview |
                                             ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha)) {
      c->flavor = -1; out |= OPT_CHANGED;
    }
  }

  char fmt[32];
  label("STRETCH");
  igSetNextItemWidth(full);
  snprintf(fmt, sizeof fmt, "%s", FLEX_NAMES[flex_index(c->flex)]);
  if (igSliderFloat("##flex", &c->flex, 0, 1, fmt, 0)) out |= OPT_CHANGED;

  label("PULL REACH");
  igSetNextItemWidth(full);
  snprintf(fmt, sizeof fmt, "%s", PULL_NAMES[flex_index(c->pull)]);
  if (igSliderFloat("##pull", &c->pull, 0, 1, fmt, 0)) out |= OPT_CHANGED;

  label("SIZE");
  igSetNextItemWidth(full);
  if (igSliderFloat("##size", &c->size, 36, 110, "%.0f px", 0)) out |= OPT_CHANGED;

  igDummy((ImVec2_c){0, 4});
  igSeparator();
  float half = (full - 8) / 2;
  if (igButton("Take a nap", (ImVec2_c){half, 32})) out |= OPT_NAP;
  igSameLine(0, 8);
  igPushStyleColor_Vec4(ImGuiCol_Text, RGB(0xf85149, 1));
  igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, RGB(0xda3633, 1));
  if (igButton("Quit jelly", (ImVec2_c){half, 32})) out |= OPT_QUIT;
  igPopStyleColor(2);

  ImVec2_c ws = igGetWindowSize();
  igEnd();
  igRender();

  glViewport(0, 0, W, H);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
  glXSwapBuffers(dpy, win);
  glXMakeCurrent(dpy, prevDraw, prevCtx);

  // only the panel itself takes clicks; the rest of the canvas is transparent and click-through
  int nh = (int)ceilf(ws.y);
  if (nh > 50 && nh != panelH) {
    panelH = nh;
    XRectangle r = {0, 0, (unsigned short)W, (unsigned short)panelH};
    XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, &r, 1, ShapeSet, Unsorted);
  }

  if (wantClose) { opt_close(); out |= OPT_CLOSED; }
  return out;
}
