// Chat box: triple-click the jelly and a line to type in fades in beside it, keyboard ready. Enter sends the message
// to the model in the background (the box fades away while the jelly thinks); the reply comes back in the same spot
// as a game-style dialogue: Enter to reply, Esc to close. More questions can be asked while one is being answered
// (triple-click again): they're answered in order, and each answer is shown under its question. One conversation
// at a time.
// No window chrome: a translucent glass backdrop whose edges feather out into the desktop, themed like the options
// panel, with a faint glow in the jelly's color.

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
#include <string.h>
#include <unistd.h>

#define CWW 540   // canvas; only the glass takes clicks
#define CWH 600
#define OX 24     // glass inside the canvas (room for the feathered edge)
#define OY 24
#define BW (CWW - 2 * OX)
#define PAD 20    // text inset
#define HEADER 16 // the glass's top margin doubles as a drag handle

static Display *dpy;
static GLXFBConfig fbc;
static GLXContext ctx;
static Window win;
static ImGuiContext *ig;
static Kbd kb;
static const Theme *styled; // theme the ImGui style was built for
static int mode = CH_HIDDEN, mapped, focusInput, wantClose, grabNow;
static int winX, winY, dragging, dragRX, dragRY, dragWX, dragWY;
static float bodyH = 180;
static char input[4000];
static char shown[16384]; // the reply (or error) on display
static char shownQ[1200], shownHow[80]; // ... the question it answers, and how it was answered
static int isError;
static double clk;

void chat_init(Display *d, GLXFBConfig fb, GLXContext shared) { dpy = d; fbc = fb; ctx = shared; }
int chat_mode(void) { return mode; }
int chat_busy(void) { return mode != CH_HIDDEN && llm_waiting() > 0; }
int chat_owns(Window w) { return win && w == win; }

static void restyle(void) {
  ImGuiStyle *s = igGetStyle();
  theme_style(s, 0.0001f); // the window itself is invisible; the cloud is its background
  s->WindowPadding = (ImVec2_c){0, 0};
  s->ItemSpacing = (ImVec2_c){8, 8};
  s->FrameRounding = 10;
  styled = theme();
}

static void create(void) {
  XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbc);
  Window root = DefaultRootWindow(dpy);
  XSetWindowAttributes swa = {0};
  swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
  swa.override_redirect = False; // managed (see kb_prepare_window) so it can take the keyboard
  swa.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | KeyPressMask |
                   KeyReleaseMask | FocusChangeMask;
  win = XCreateWindow(dpy, root, 0, 0, CWW, CWH, 0, vi->depth, InputOutput, vi->visual,
                      CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
  kb_prepare_window(win);
  XStoreName(dpy, win, "jelly chat");
  XFree(vi);
  kb_attach(&kb, win);

  // GL objects belong to the (shared) context, so they're created while the jelly's window is current: making the
  // context current on a window that was never mapped corrupts later draws on this NVIDIA driver
  GLXContext pc = glXGetCurrentContext(); GLXDrawable pd = glXGetCurrentDrawable();
  ig = igCreateContext(NULL);
  igSetCurrentContext(ig);
  ImGuiIO *io = igGetIO_Nil();
  io->IniFilename = NULL;
  if (access("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", R_OK) == 0)
    ImFontAtlas_AddFontFromFileTTF(io->Fonts, "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", 18.0f, NULL, NULL);
  const char *cjk = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  if (access(cjk, R_OK) == 0) {
    ImFontConfig *fc = ImFontConfig_ImFontConfig();
    fc->MergeMode = true;
    ImFontAtlas_AddFontFromFileTTF(io->Fonts, cjk, 18.0f, fc, NULL);
    ImFontConfig_destroy(fc);
  }
  restyle();
  ImGui_ImplOpenGL3_Init("#version 330 core");
  glXMakeCurrent(dpy, pd, pc);
}

static void place_near(float ax, float ay, float jr) {
  int sw = DisplayWidth(dpy, DefaultScreen(dpy)), sh = DisplayHeight(dpy, DefaultScreen(dpy));
  int side = ax + jr + CWW + 30 < sw ? 1 : -1;
  winX = (int)(side > 0 ? ax + jr * 0.9f : ax - jr * 0.9f - CWW);
  winY = (int)(ay - bodyH - 90);
  if (winX < 8) winX = 8;
  if (winX + CWW > sw) winX = sw - CWW - 8;
  if (winY < 8) winY = 8;
  if (winY + CWH > sh) winY = sh - CWH - 8;
}

/* Each time the box appears: the window manager drops "always on top" when a window is withdrawn, and without it
   a new window may be placed behind the active one. So re-apply the properties, raise it, and ask to be activated
   (which also makes Enter / Esc work straight away). */
static void show(void) {
  if (!mapped) {
    kb_prepare_window(win);
    XMoveWindow(dpy, win, winX, winY);
    XMapRaised(dpy, win);
    XSync(dpy, False);
    XMoveWindow(dpy, win, winX, winY);
    mapped = 1;
  } else XRaiseWindow(dpy, win);
  kb_grab_soon(&kb, clk);
}
static void hide(void) {
  if (mapped) { kb_release(&kb); XUnmapWindow(dpy, win); mapped = 0; }
}

static int autoSend; // debugging aid: send the preset input on the next frame
void chat_debug_send(const char *text) { snprintf(input, sizeof input, "%s", text); autoSend = 1; }

void chat_open(float ax, float ay, float jr) {
  if (!win) create();
  if (!mapped) place_near(ax, ay, jr);
  mode = CH_INPUT; focusInput = 1; // even while earlier questions are being answered: this one joins the queue
  show();
  grabNow = 1;
}

void chat_event(XEvent *e) {
  if (!win) return;
  igSetCurrentContext(ig);
  ImGuiIO *io = igGetIO_Nil();
  if (kb_paste_arrived(&kb, e, io, 0)) return;
  if (kb_event(&kb, e, io)) { if (kb.wantPaste) kb_request_paste(&kb); return; }
  switch (e->type) {
  case MotionNotify:
    if (dragging) {
      winX = dragWX + e->xmotion.x_root - dragRX; winY = dragWY + e->xmotion.y_root - dragRY;
      XMoveWindow(dpy, win, winX, winY);
      return;
    }
    ImGuiIO_AddMousePosEvent(io, (float)e->xmotion.x, (float)e->xmotion.y);
    break;
  case LeaveNotify: if (!dragging) ImGuiIO_AddMousePosEvent(io, -3.4e38f, -3.4e38f); break;
  case ButtonPress:
  case ButtonRelease: {
    int down = e->type == ButtonPress, b = e->xbutton.button;
    if (b == Button1 && down && e->xbutton.y < OY + HEADER && e->xbutton.x < OX + BW - 50) {
      dragging = 1; dragRX = e->xbutton.x_root; dragRY = e->xbutton.y_root; dragWX = winX; dragWY = winY;
      return;
    }
    if (b == Button1 && !down && dragging) { dragging = 0; return; }
    if (down) kb_clicked(&kb); // clicking back into the cloud takes the keyboard again
    ImGuiIO_AddMousePosEvent(io, (float)e->xbutton.x, (float)e->xbutton.y);
    if (b == Button1) ImGuiIO_AddMouseButtonEvent(io, 0, down);
    else if (b == Button3) ImGuiIO_AddMouseButtonEvent(io, 1, down);
    else if (down && b == Button4) ImGuiIO_AddMouseWheelEvent(io, 0, 1);
    else if (down && b == Button5) ImGuiIO_AddMouseWheelEvent(io, 0, -1);
    break;
  }
  }
}

static unsigned hexf(const float t[3]) { return ((unsigned)(t[0] * 255) << 16) | ((unsigned)(t[1] * 255) << 8) | (unsigned)(t[2] * 255); }

/* translucent glass with feathered edges: no border, it just fades into the desktop */
static void draw_glass(ImDrawList *dl, float h, const float tint[3], float fade) {
  const Theme *t = theme();
  float x0 = OX, y0 = OY, x1 = OX + BW, y1 = OY + h, base = (t->light ? 0.72f : 0.62f) * fade;
  for (int i = 12; i >= 1; i--) { // the feather: wider, fainter layers
    float k = i / 12.f, a = base * 0.30f * (1 - k) * (1 - k);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){x0 - i * 1.8f, y0 - i * 1.8f}, (ImVec2_c){x1 + i * 1.8f, y1 + i * 1.8f}, th_u32(t->bg, a), 18 + i * 1.8f, 0);
  }
  ImDrawList_AddRectFilled(dl, (ImVec2_c){x0, y0}, (ImVec2_c){x1, y1}, th_u32(t->bg, base), 18, 0);
  // a faint sheen along the top, and a glow in the jelly's color along the bottom that fades out at both ends
  ImDrawList_AddLine(dl, (ImVec2_c){x0 + 18, y0 + 1}, (ImVec2_c){x1 - 18, y0 + 1}, th_u32(0xffffff, (t->light ? 0.55f : 0.10f) * fade), 1);
  unsigned glow = hexf(tint);
  float mid = (x0 + x1) / 2, gy = y1 - 2;
  ImDrawList_AddRectFilledMultiColor(dl, (ImVec2_c){x0 + 30, gy}, (ImVec2_c){mid, gy + 2}, th_u32(glow, 0), th_u32(glow, 0.8f * fade), th_u32(glow, 0.8f * fade), th_u32(glow, 0));
  ImDrawList_AddRectFilledMultiColor(dl, (ImVec2_c){mid, gy}, (ImVec2_c){x1 - 30, gy + 2}, th_u32(glow, 0.8f * fade), th_u32(glow, 0), th_u32(glow, 0), th_u32(glow, 0.8f * fade));
}

static int take_next(void) { // the next answer, if one is ready
  char q[1200], a[16384], how[80];
  int k = llm_take_reply(q, sizeof q, a, sizeof a, how, sizeof how);
  if (!k) return 0;
  snprintf(shown, sizeof shown, "%s", a); snprintf(shownQ, sizeof shownQ, "%s", q); snprintf(shownHow, sizeof shownHow, "%s", how);
  isError = k < 0;
  return k;
}

/* a small clickable key hint, e.g. "↵ send" */
static int hint(const char *key, const char *what) {
  const Theme *t = theme();
  char id[64]; snprintf(id, sizeof id, "%s %s##h", key, what);
  ImVec2_c sz = igCalcTextSize(id, NULL, true, -1);
  ImVec2_c p = igGetCursorScreenPos();
  int hit = igInvisibleButton(id, (ImVec2_c){sz.x + 12, sz.y + 4}, 0), hov = igIsItemHovered(0);
  ImDrawList *dl = igGetWindowDrawList();
  ImDrawList_AddText_Vec2(dl, (ImVec2_c){p.x + 6, p.y + 2}, th_u32(hov ? t->accent : t->muted, 1), key, NULL);
  ImVec2_c ks = igCalcTextSize(key, NULL, false, -1);
  ImDrawList_AddText_Vec2(dl, (ImVec2_c){p.x + 6 + ks.x + 5, p.y + 2}, th_u32(hov ? t->text : t->muted, hov ? 1 : 0.8f), what, NULL);
  return hit;
}

int chat_frame(double dt, float ax, float ay, float jr, const float tint[3]) {
  (void)jr;
  if (!win || mode == CH_HIDDEN) return CHAT_NONE;
  int out = CHAT_NONE;
  clk += dt;

  // waiting for the model: nothing on screen, the jelly does the thinking
  if (mode == CH_WAIT) {
    if (take_next()) {
      mode = CH_REPLY;
      show();
      out = isError ? CHAT_ERROR : CHAT_REPLIED;
    } else if (!llm_waiting()) { mode = CH_HIDDEN; return CHAT_NONE; } // nothing left (the conversation was reset)
    else return CHAT_NONE;
  }

  GLXContext pc = glXGetCurrentContext(); GLXDrawable pd = glXGetCurrentDrawable();
  glXMakeCurrent(dpy, win, ctx);
  static int swapSet; // no vsync wait here: the jelly's own swap paces the loop
  if (!swapSet) {
    typedef void (*SwapFn)(Display *, GLXDrawable, int);
    SwapFn si = (SwapFn)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    if (si) si(dpy, win, 0);
    swapSet = 1;
  }
  igSetCurrentContext(ig);
  if (styled != theme()) restyle();
  const Theme *t = theme();
  ImGuiIO *io = igGetIO_Nil();
  io->DisplaySize = (ImVec2_c){CWW, CWH};
  io->DeltaTime = dt > 0 ? (float)dt : 1.f / 60;
  ImGui_ImplOpenGL3_NewFrame();
  igNewFrame();

  static float appear;
  appear = fminf(1, appear + (float)dt * 5); // fade in
  draw_glass(igGetBackgroundDrawList(NULL), bodyH, tint, appear);

  float full = BW - 2 * PAD;
  igSetNextWindowPos((ImVec2_c){OX + PAD, OY + 14}, ImGuiCond_Always, (ImVec2_c){0, 0});
  igSetNextWindowSize((ImVec2_c){full, 0}, ImGuiCond_Always);
  igPushStyleColor_Vec4(ImGuiCol_FrameBg, (ImVec4_c){0, 0, 0, 0}); // the text sits right on the glass
  igPushStyleColor_Vec4(ImGuiCol_FrameBgHovered, (ImVec4_c){0, 0, 0, 0});
  igPushStyleColor_Vec4(ImGuiCol_FrameBgActive, (ImVec4_c){0, 0, 0, 0});
  igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, (ImVec2_c){0, 2});
  igBegin("##chat", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
  ImVec4_c accent = {((t->accent >> 16) & 255) / 255.f, ((t->accent >> 8) & 255) / 255.f, (t->accent & 255) / 255.f, 1};
  int turns = llm_turns(), waiting = llm_waiting(), readyN = llm_ready();
  int enter = igIsKeyPressed_Bool(ImGuiKey_Enter, false) || igIsKeyPressed_Bool(ImGuiKey_KeypadEnter, false);
  int esc = igIsKeyPressed_Bool(ImGuiKey_Escape, false);

  if (mode == CH_INPUT) {
    // an answer came back: show it, unless they're in the middle of typing the next question
    if (readyN && !*input && !autoSend && take_next()) { mode = CH_REPLY; out |= isError ? CHAT_ERROR : CHAT_REPLIED; focusInput = 1; }
  }
  if (mode == CH_INPUT) {
    // questions still being answered, oldest first: the current one thinks, the rest wait their turn
    char plist[6][200]; int np = llm_pending_list(plist, 6);
    for (int i = 0; i < np && i < 4; i++) {
      char one[120]; snprintf(one, sizeof one, "%.90s%s", plist[i], strlen(plist[i]) > 90 ? "…" : "");
      for (char *c = one; *c; c++) if (*c == '\n') *c = ' ';
      igTextColored((ImVec4_c){accent.x, accent.y, accent.z, 0.55f}, "You");
      igSameLine(0, 8);
      float w = full - 110;
      ImVec2_c ts = igCalcTextSize(one, NULL, false, -1);
      if (ts.x > w) { int k = (int)strlen(one) * (int)w / (int)ts.x; if (k > 3) { one[k - 3] = 0; strcat(one, "…"); } }
      igTextDisabled("%s", one);
      igSameLine(full - 88, 0);
      if (i == 0) { static const char *dots[] = {"thinking.", "thinking..", "thinking..."}; igTextColored(accent, "%s", dots[(int)(clk * 3) % 3]); }
      else igTextDisabled("queued");
    }
    if (np > 4) igTextDisabled("+ %d more", np - 4);
    if (np) igDummy((ImVec2_c){0, 4});

    if (focusInput) { igSetKeyboardFocusHere(0); focusInput = 0; }
    ImVec2_c before = igGetCursorScreenPos();
    int send = igInputTextMultiline("##in", input, sizeof input, (ImVec2_c){full, 70},
                                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine |
                                        ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_NoHorizontalScroll, NULL, NULL);
    if (!*input) // placeholder
      ImDrawList_AddText_Vec2(igGetWindowDrawList(), (ImVec2_c){before.x, before.y + 2}, th_u32(t->muted, 0.75f),
                              np ? "Ask more while I think…" : turns ? "Say more…" : "Say something to Jelly…", NULL);
    igDummy((ImVec2_c){0, 2});
    int view = 0;
    if (readyN) { view = hint("answer ready", "· view"); igSameLine(0, 0); } // they're typing: don't pull the text away
    igSetCursorPosX(full - 196);
    if (hint("enter", "send")) send = 1;
    igSameLine(0, 4);
    if (hint("esc", np ? "hide" : "close")) esc = 1;
    if (view && take_next()) mode = CH_REPLY;
    else if (esc) {
      if (readyN && take_next()) mode = CH_REPLY;
      else if (waiting) mode = CH_WAIT; // hide and keep thinking; the answer pops up when it's ready
      else if (*shown && turns) mode = CH_REPLY;
      else wantClose = 1;
    }
    if (autoSend) { send = 1; autoSend = 0; }
    char *p = input; while (*p == ' ' || *p == '\n') p++;
    if (send && *p && llm_send(p)) { input[0] = 0; out = CHAT_SENT; focusInput = 1; } // stay open: ask more right away
  } else if (mode == CH_REPLY) {
    if (*shownQ) { // the question on top, so answers to queued questions are easy to tell apart
      igPushTextWrapPos(full - 12);
      ImVec2_c qs = igCalcTextSize(shownQ, NULL, false, full - 12);
      if (qs.y > igGetTextLineHeight() * 3.2f) { // long question: the first lines only
        char cut[400]; snprintf(cut, sizeof cut, "%.300s…", shownQ);
        igTextColored((ImVec4_c){accent.x, accent.y, accent.z, 0.55f}, "You");
        igSameLine(0, 8); igTextDisabled("%s", cut);
      } else {
        igTextColored((ImVec4_c){accent.x, accent.y, accent.z, 0.55f}, "You");
        igSameLine(0, 8); igTextDisabled("%s", shownQ);
      }
      igPopTextWrapPos();
      igDummy((ImVec2_c){0, 2});
    }
    igTextColored(accent, "Jelly"); // a name plate, like a game dialogue
    if (*shownHow && !isError) { igSameLine(0, 8); igTextDisabled("· %s", shownHow); } // e.g. "searched the web"
    ImVec2_c ts = igCalcTextSize(shown, NULL, false, full - 12);
    float h = fminf(ts.y + 8, 360);
    igBeginChild_Str("##reply", (ImVec2_c){full, h}, 0, 0);
    igPushTextWrapPos(full - 12);
    if (isError) igTextColored((ImVec4_c){1, 0.45f, 0.42f, 1}, "Oops, I couldn't reach my brain~ (%s)", shown);
    else igTextUnformatted(shown, NULL);
    igPopTextWrapPos();
    igEndChild();
    if (waiting && !readyN) { igTextDisabled(waiting == 1 ? "still answering 1 more~" : "still answering %d more~", waiting); igSameLine(0, 0); }
    igSetCursorPosX(full - 196);
    char nextLbl[32]; snprintf(nextLbl, sizeof nextLbl, "next (%d)", readyN);
    int reply = hint("enter", readyN ? nextLbl : isError ? "retry" : waiting ? "ask more" : "reply");
    igSameLine(0, 4);
    if (hint("esc", "close")) esc = 1;
    if ((reply || enter) && readyN) take_next(); // the next answer in the queue
    else if (reply || enter) { mode = CH_INPUT; focusInput = 1; grabNow = 1; }
    else if (esc) { if (waiting || readyN) mode = CH_WAIT; else wantClose = 1; } // unanswered questions: keep them coming
  }
  ImVec2_c ws = igGetWindowSize();
  igEnd();
  igPopStyleVar(1);
  igPopStyleColor(3);
  igRender();
  glViewport(0, 0, CWW, CWH);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
  glXSwapBuffers(dpy, win);
  glXMakeCurrent(dpy, pd, pc);

  if (grabNow && mapped) { kb_grab_soon(&kb, clk); grabNow = 0; } // opening the box / Reply: the keyboard is yours
  kb_tick(&kb, clk);
  kb_sync(&kb, io->WantTextInput || mode == CH_INPUT || mode == CH_REPLY); // keys work while the box is up

  // the cloud grows with its content; only the cloud (and its tail) takes clicks
  float nh = ws.y + 24;
  if (fabsf(nh - bodyH) > 1) {
    bodyH = nh;
    XRectangle r = {OX, OY, BW, (unsigned short)bodyH};
    XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, &r, 1, ShapeSet, Unsorted);
  }
  if (mode == CH_WAIT) hide();
  if (wantClose) {
    wantClose = 0;
    hide();
    mode = CH_HIDDEN; shown[0] = shownQ[0] = shownHow[0] = 0;
    llm_end_session(); // one conversation at a time: closing ends it
    out |= CHAT_CLOSED;
  }
  return out;
}
