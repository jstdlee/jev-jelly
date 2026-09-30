// Reminder words: the event appears beside the jelly as solid words, each word in its own color, with the colors
// slowly shifting. The text is drawn into an offscreen texture so it can pixel-dissolve in and out (3 px blocks
// appear one by one) when it shows up, and when it's clicked or after ten minutes. Bright colors on dark desktops,
// deeper ones on light desktops (sampled from what's behind), with a faint shadow.
// Japanese, Chinese and Korean titles use the matching Noto Sans CJK face.
// Its own borderless ARGB window and ImGui context, like the options panel.

#define GL_GLEXT_PROTOTYPES
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#define CIMGUI_USE_OPENGL3
#include "cimgui_impl.h"
#include "jelly.h"
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LIFETIME (10 * 60.0) // a real reminder stays up to ten minutes (a preview only a few seconds)
#define WW 600 // window: room for a few lines of words
#define WH 280
#define CELL 3.0f // LED pitch in pixels
#define MAXCH 160

static Display *dpy;
static GLXFBConfig fbc;
static Window win;
static GLXContext ctx;
static ImGuiContext *ig;
static ImFont *fonts[3], *font; // CJK faces: JP, KR, SC (their Latin letters are Noto Sans)
static int visible, leaving, mapped, clicked;
static CalEvent ev;
static double age, leaveT;
static float bx = -1, by, bvx, bvy; // center of the words on screen; they float after a target beside the jelly
static int wx, wy;
static float ink = 0.15f, inkT = 0.15f; // 0.15 = on a light desktop, 0.97 = on a dark one; eases between them
static float hue0;                       // the rainbow starts at the jelly's own hue
static GLuint fbo, tex, prog, vao;
static GLint uTime, uFade, uOn, uBright, uHue;
static double sampleAt;
static int needLayout; // text is measured inside a frame: measuring before one returns undersized widths

/* one laid-out character: byte range in its string, line, and x offset within the line */
typedef struct { const char *s; int len, line; float x, w, size; } Glyph;
static Glyph gl[MAXCH];
static int ngl;
static float lineW[6], lineY[6], lineSize[6];
static int nlines;
static char whenStr[64], whereStr[100];
static float boxW, boxH;

void bub_init(Display *d, GLXFBConfig fb, GLXContext shared) { dpy = d; fbc = fb; ctx = shared; }
int bub_visible(void) { return visible; }
int bub_owns(Window w) { return mapped && w == win; }
void bub_center(float *x, float *y) { *x = bx; *y = by; }

static void create(void) {
  XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbc);
  Window root = DefaultRootWindow(dpy);
  XSetWindowAttributes swa = {0};
  swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
  swa.override_redirect = True;
  swa.event_mask = ButtonPressMask | ButtonReleaseMask;
  win = XCreateWindow(dpy, root, 0, 0, WW, WH, 0, vi->depth, InputOutput, vi->visual,
                      CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
  XStoreName(dpy, win, "jelly reminder");
  XFree(vi);
  // drawn with the jelly's own GL context (same visual): no new context is ever created

  // GL objects belong to the (shared) context, so they're created while the jelly's window is current: making the
  // context current on a window that was never mapped corrupts later draws on this NVIDIA driver
  GLXContext pc = glXGetCurrentContext(); GLXDrawable pd = glXGetCurrentDrawable();
  ig = igCreateContext(NULL);
  igSetCurrentContext(ig);
  ImGuiIO *io = igGetIO_Nil();
  io->IniFilename = NULL;
  // glyphs are rasterized on demand (Dear ImGui 1.92), so the big CJK faces cost nothing until used
  const char *cjk = "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc";
  const char *cjk2 = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
  const char *latin = "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf";
  const char *path = access(cjk, R_OK) == 0 ? cjk : access(cjk2, R_OK) == 0 ? cjk2 : NULL;
  for (int i = 0; i < 3; i++) {
    if (path) {
      ImFontConfig *fc = ImFontConfig_ImFontConfig();
      fc->FontNo = (ImU32)i; // collection faces: 0 JP, 1 KR, 2 SC
      fonts[i] = ImFontAtlas_AddFontFromFileTTF(io->Fonts, path, 20.0f, fc, NULL);
      ImFontConfig_destroy(fc);
    } else if (access(latin, R_OK) == 0) {
      fonts[i] = ImFontAtlas_AddFontFromFileTTF(io->Fonts, latin, 20.0f, NULL, NULL);
    } else {
      fonts[i] = ImFontAtlas_AddFontDefault(io->Fonts, NULL);
    }
  }
  ImGui_ImplOpenGL3_Init("#version 330 core");

  // offscreen target for the text, and the LED shader that redraws it as dots
  glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, WW, WH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  static const char *vs = "#version 330 core\n"
      "void main(){ vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0, (gl_VertexID >> 1) * 4.0 - 1.0); gl_Position = vec4(p, 0, 1); }\n";
  static const char *fs = "#version 330 core\n"
      "uniform sampler2D uTex; uniform float uTime, uFade, uOn, uBright, uHue;\n"
      "out vec4 frag;\n"
      "float hash(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
      "void main(){\n"
      "  vec4 c = texelFetch(uTex, ivec2(gl_FragCoord.xy), 0);\n"   // premultiplied text color
      "  float h = hash(floor(gl_FragCoord.xy / 3.0));\n"           // 3 px blocks switch on / off one by one
      "  frag = c * smoothstep(h - 0.06, h + 0.06, uOn) * uFade;\n"
      "}\n";
  GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
  glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
  GLint ok; glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[1024]; glGetShaderInfoLog(f, sizeof log, NULL, log); fprintf(stderr, "led shader: %s\n", log); }
  prog = glCreateProgram(); glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog);
  uTime = glGetUniformLocation(prog, "uTime"); uFade = glGetUniformLocation(prog, "uFade");
  uOn = glGetUniformLocation(prog, "uOn"); uBright = glGetUniformLocation(prog, "uBright");
  uHue = glGetUniformLocation(prog, "uHue");
  glGenVertexArrays(1, &vao);
  glXMakeCurrent(dpy, pd, pc);
}

/* next UTF-8 character: returns its byte length and codepoint */
static int utf8(const char *s, unsigned *cp) {
  const unsigned char *u = (const unsigned char *)s;
  if (u[0] < 0x80) { *cp = u[0]; return 1; }
  if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((u[0] & 31u) << 6) | (u[1] & 63u); return 2; }
  if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) { *cp = ((u[0] & 15u) << 12) | ((u[1] & 63u) << 6) | (u[2] & 63u); return 3; }
  if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
    *cp = ((u[0] & 7u) << 18) | ((u[1] & 63u) << 12) | ((u[2] & 63u) << 6) | (u[3] & 63u); return 4;
  }
  *cp = '?'; return 1;
}
static int is_cjk(unsigned c) {
  return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

/* Hangul -> Korean face, kana -> Japanese, other Han -> Simplified Chinese; Latin-only uses the JP face */
static ImFont *pick_font(const char *s) {
  int han = 0;
  for (unsigned cp; *s;) {
    s += utf8(s, &cp);
    if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F)) return fonts[1];
    if (cp >= 0x3040 && cp <= 0x30FF) return fonts[0];
    if (cp >= 0x4E00 && cp <= 0x9FFF) han = 1;
  }
  return han ? fonts[2] : fonts[0];
}

static float adv(const char *s, int len, float size) {
  return ImFont_CalcTextSizeA(font, size, 1e9f, 0, s, s + len, NULL).x;
}

/* Lays out one string into lines no wider than maxw: breaks at spaces in Latin text and between any two characters
   in CJK text. The last allowed line ends with an ellipsis if the text doesn't fit. */
static void layout_text(const char *s, float size, float maxw, int maxLines) {
  if (nlines >= 5) return;
  int line = nlines, first = ngl, lastSpace = -1, used = 1;
  float x = 0;
  lineSize[line] = size;
  static const char ell[] = "…";
  for (const char *p = s; *p && ngl < MAXCH - 1;) {
    unsigned cp; int n = utf8(p, &cp);
    float w = adv(p, n, size);
    if (x + w > maxw && ngl > first) {
      if (used >= maxLines || line >= 5) { // out of room: finish with an ellipsis
        float ew = adv(ell, 3, size);
        while (ngl > first && x + ew > maxw) x = gl[--ngl].x;
        gl[ngl++] = (Glyph){ell, 3, line, x, ew, size};
        x += ew;
        break;
      }
      int cut = (!is_cjk(cp) && lastSpace > first) ? lastSpace + 1 : ngl; // carry the unfinished word down
      lineW[line] = cut > first ? (cut == lastSpace + 1 ? gl[lastSpace].x : gl[cut - 1].x + gl[cut - 1].w) : 0;
      line++; used++; lineSize[line] = size;
      float shift = cut < ngl ? gl[cut].x : x;
      for (int k = cut; k < ngl; k++) { gl[k].line = line; gl[k].x -= shift; }
      x -= shift; first = cut; lastSpace = -1;
    }
    if (cp == ' ') lastSpace = ngl;
    gl[ngl++] = (Glyph){p, n, line, x, w, size};
    x += w; p += n;
  }
  lineW[line] = x;
  nlines = line + 1;
}

static void relayout(void) {
  ngl = 0; nlines = 0;
  layout_text(whenStr, 20, 540, 1);
  layout_text(ev.title, 30, 540, 3);
  if (*whereStr) layout_text(whereStr, 18, 540, 1);
  float y = 0; boxW = 0;
  for (int i = 0; i < nlines; i++) {
    lineY[i] = y;
    y += lineSize[i] * (i == 0 ? 1.45f : 1.3f);
    if (lineW[i] > boxW) boxW = lineW[i];
  }
  boxH = y;
  if (getenv("JELLY_DEBUG"))
    for (int i = 0; i < nlines; i++) {
      char b[512]; int o = 0;
      for (int k = 0; k < ngl && o < 500; k++) if (gl[k].line == i) { memcpy(b + o, gl[k].s, (size_t)gl[k].len); o += gl[k].len; }
      b[o] = 0; fprintf(stderr, "layout %d: w=%.0f '%s'\n", i, lineW[i], b);
    }
  // only the words take clicks
  XRectangle r = {(short)((WW - boxW) / 2 - 10), (short)((WH - boxH) / 2 - 8), (unsigned short)(boxW + 20), (unsigned short)(boxH + 16)};
  XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, &r, 1, ShapeSet, Unsorted);
}

static double lifetime = LIFETIME;

void bub_show(const CalEvent *e, float t[3], double life) {
  lifetime = life > 0 ? life : LIFETIME;
  float sat, v;
  igColorConvertRGBtoHSV(t[0], t[1], t[2], &hue0, &sat, &v);
  if (!win) create();
  ev = *e;
  font = pick_font(ev.title);
  snprintf(whereStr, sizeof whereStr, "%s", ev.where);
  cal_when(&ev, time(NULL), whenStr, sizeof whenStr);
  needLayout = 1;
  age = 0; leaving = 0; clicked = 0; bx = -1; sampleAt = 0;
  visible = 1;
}

void bub_event(XEvent *e) {
  if (visible && e->type == ButtonPress && e->xbutton.button == Button1) clicked = 1;
}

/* is the desktop behind the words light or dark? pick the ink accordingly */
static void sample_background(void) {
  int sw = DisplayWidth(dpy, DefaultScreen(dpy)), sh = DisplayHeight(dpy, DefaultScreen(dpy));
  int x = wx + (int)((WW - boxW) / 2), y = wy + (int)((WH - boxH) / 2), w = (int)boxW, h = (int)boxH;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > sw) w = sw - x;
  if (y + h > sh) h = sh - y;
  if (w < 8 || h < 8) return;
  XImage *im = XGetImage(dpy, DefaultRootWindow(dpy), x, y, (unsigned)w, (unsigned)h, AllPlanes, ZPixmap);
  if (!im) return;
  double sum = 0; int n = 0;
  for (int j = 0; j < h; j += 6)
    for (int i = 0; i < w; i += 6) {
      unsigned long p = XGetPixel(im, i, j);
      sum += 0.2126 * ((p >> 16) & 255) + 0.7152 * ((p >> 8) & 255) + 0.0722 * (p & 255);
      n++;
    }
  XDestroyImage(im);
  float lum = n ? (float)(sum / n / 255) : 1;
  inkT = lum > 0.5f ? 0.15f : 0.97f;
}


int bub_frame(double dt, float ax, float ay, float jr) {
  if (!visible) return BUB_NONE;
  int out = BUB_NONE;
  age += dt;
  int sw = DisplayWidth(dpy, DefaultScreen(dpy));

  // float beside and a little above the jelly's head, on whichever side has room
  float side = ax + jr + boxW + 60 < sw ? 1.f : -1.f;
  float tx = ax + side * (jr * 0.8f + boxW / 2 + 24), ty = ay - boxH * 0.4f - 10;
  if (ty < boxH / 2 + 12) ty = boxH / 2 + 12;
  if (bx < 0) { bx = ax; by = ay; } // they rise out of the jelly
  float w = 2 * 3.14159f * 0.9f;
  bvx += (w * w * (tx - bx) - 2 * 0.7f * w * bvx) * (float)dt;
  bvy += (w * w * (ty - by) - 2 * 0.7f * w * bvy) * (float)dt;
  bx += bvx * (float)dt; by += bvy * (float)dt;

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

  // the countdown ticks on
  char when[64]; cal_when(&ev, time(NULL), when, sizeof when);
  if (strcmp(when, whenStr) && !leaving) { snprintf(whenStr, sizeof whenStr, "%s", when); needLayout = 1; }

  if (clicked && !leaving) { leaving = 1; leaveT = 0; out = BUB_POPPED; }
  if (!leaving && age > lifetime) { leaving = 1; leaveT = 0; out = BUB_EXPIRED; }
  float leaveDur = 0.6f;
  if (leaving) leaveT += dt;
  if (leaving && leaveT > leaveDur) {
    visible = 0;
    if (mapped) { XUnmapWindow(dpy, win); mapped = 0; }
    glXMakeCurrent(dpy, pd, pc);
    return out;
  }

  int nx = (int)lroundf(bx - WW / 2.f), ny = (int)lroundf(by - WH / 2.f);
  if (!mapped) { XMoveWindow(dpy, win, nx, ny); XMapRaised(dpy, win); mapped = 1; }
  else if (nx != wx || ny != wy) XMoveWindow(dpy, win, nx, ny);
  wx = nx; wy = ny;
  if (age - sampleAt > 1.2 || sampleAt == 0) { sampleAt = age; sample_background(); }
  ink += (inkT - ink) * fminf(1, (float)dt * 3);

  ImGuiIO *io = igGetIO_Nil();
  io->DisplaySize = (ImVec2_c){WW, WH};
  io->DeltaTime = dt > 0 ? (float)dt : 1.f / 60;
  ImGui_ImplOpenGL3_NewFrame();
  igNewFrame();
  if (needLayout) { relayout(); needLayout = 0; }
  ImDrawList *dl = igGetForegroundDrawList_ViewportPtr(NULL);

  float ox = WW / 2.f, oy = (WH - boxH) / 2.f, t = (float)age;
  float val = ink > 0.5f ? 1.0f : 0.58f; // bright on dark desktops, deeper on light ones
  ImU32 shadow = ink > 0.5f ? 0x000000 : 0xffffff;
  int word = 0, run = 0, prevLine = -1;
  for (int k = 0; k < ngl; k++) {
    Glyph *g = &gl[k];
    unsigned cp; utf8(g->s, &cp);
    if (g->line != prevLine) { word++; run = 0; prevLine = g->line; }
    else if (cp == ' ' || (is_cjk(cp) && ++run % 4 == 0)) word++; // a new color per word (per few characters in CJK)
    float a = g->line == 0 || (*whereStr && g->line == nlines - 1) ? 0.85f : 1.0f;
    float r, gg, b;
    igColorConvertHSVtoRGB(fmodf(hue0 + word * 0.14f + t * 0.04f, 1), 0.72f, val, &r, &gg, &b);
    float x = floorf(ox - lineW[g->line] / 2 + g->x), y = floorf(oy + lineY[g->line]);
    ImDrawList_AddText_FontPtr(dl, font, g->size, (ImVec2_c){x + 1, y + 1}, ((ImU32)(a * 0.35f * 255) << 24) | shadow, g->s, g->s + g->len, 0, NULL);
    ImU32 c = ((ImU32)(a * 255) << 24) | ((ImU32)(b * 255) << 16) | ((ImU32)(gg * 255) << 8) | (ImU32)(r * 255);
    ImDrawList_AddText_FontPtr(dl, font, g->size, (ImVec2_c){x, y}, c, g->s, g->s + g->len, 0, NULL);
  }

  igRender();
  glViewport(0, 0, WW, WH);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(prog); glBindVertexArray(vao);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
  float on = leaving ? 1 - (float)leaveT / leaveDur : fminf(1, (float)age / 0.7f);
  glUniform1f(uTime, (float)age); glUniform1f(uOn, on * 1.1f - 0.05f); glUniform1f(uFade, 1);
  glUniform1f(uHue, hue0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glXSwapBuffers(dpy, win);
  glXMakeCurrent(dpy, pd, pc);
  return out;
}
