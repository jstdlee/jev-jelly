// jelly — a little jelly friend that wanders your X11 desktop.
// Frameless ARGB override-redirect window + OpenGL 3.3, click-through outside the body.
// Left-drag to pull/throw, click to poke, hover-wiggle to pet, right-click for settings.

#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "jelly.h"

#define PI 3.14159265f
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* ------------------------------------------------------------------ settings */

const char *FLAVOR_NAMES[NFLAVORS] = {"Soda", "Strawberry", "Melon", "Lemon", "Grape", "Peach", "Mint"};
const float FLAVOR_COLS[NFLAVORS][3] = {
    {0.25f, 0.62f, 0.96f}, {0.95f, 0.26f, 0.44f}, {0.38f, 0.80f, 0.30f}, {0.98f, 0.80f, 0.22f},
    {0.64f, 0.52f, 0.96f}, {1.00f, 0.60f, 0.48f}, {0.36f, 0.90f, 0.76f}};

static Cfg cfg = {1, 0, {0.25f, 0.62f, 0.96f}, 0.5f, 60.0f, FACE_TINY, 0.6f};

static void cfg_path(char *out, size_t n) {
  const char *h = getenv("HOME");
  snprintf(out, n, "%s/.config/jev-jelly/jelly.conf", h ? h : ".");
}
static void cfg_load(void) {
  char p[512]; cfg_path(p, sizeof p);
  FILE *f = fopen(p, "r"); if (!f) return;
  char line[128];
  while (fgets(line, sizeof line, f)) {
    int i; float v, r, g, b;
    if (sscanf(line, "girl=%d", &i) == 1) cfg.girl = i != 0;
    else if (sscanf(line, "flavor=%d", &i) == 1) cfg.flavor = i;
    else if (sscanf(line, "flex=%f", &v) == 1) cfg.flex = v > 1.001f ? v / 4 : v;                          // old files: 0..4
    else if (sscanf(line, "size=%f", &v) == 1) cfg.size = v < 3 ? (float[]){44, 60, 80}[(int)v] : v; // old files: 0..2
    else if (sscanf(line, "face=%d", &i) == 1) cfg.face = i;
    else if (sscanf(line, "pull=%f", &v) == 1) cfg.pull = v;
    else if (sscanf(line, "color=%f %f %f", &r, &g, &b) == 3) { cfg.col[0] = r; cfg.col[1] = g; cfg.col[2] = b; }
  }
  fclose(f);
  cfg.flex = clampf(cfg.flex, 0, 1);
  cfg.size = clampf(cfg.size, 36, 110);
  cfg.pull = clampf(cfg.pull, 0, 1);
  if (cfg.face != FACE_CLASSIC) cfg.face = FACE_TINY;
  if (cfg.flavor >= NFLAVORS) cfg.flavor = 0;
  if (cfg.flavor >= 0) memcpy(cfg.col, FLAVOR_COLS[cfg.flavor], sizeof cfg.col);
}
static void cfg_save(void) {
  char p[512]; cfg_path(p, sizeof p);
  char d[512]; snprintf(d, sizeof d, "%s", p); *strrchr(d, '/') = 0;
  char parent[512]; snprintf(parent, sizeof parent, "%s", d); *strrchr(parent, '/') = 0;
  mkdir(parent, 0755); mkdir(d, 0755);
  FILE *f = fopen(p, "w"); if (!f) return;
  fprintf(f, "girl=%d\nflavor=%d\nflex=%.3f\nsize=%.0f\nface=%d\npull=%.3f\ncolor=%.3f %.3f %.3f\n", cfg.girl, cfg.flavor,
          cfg.flex, cfg.size, cfg.face, cfg.pull, cfg.col[0], cfg.col[1], cfg.col[2]);
  fclose(f);
}

/* ------------------------------------------------------------------ mesh */

#define MAXV 700
#define MAXT 1300
static int nv, ntri;
static float rest[MAXV][3], pos[MAXV][3], nrm[MAXV][3];
static float rdir[MAXV][3], rlen[MAXV], off[MAXV], offV[MAXV], offSnap[MAXV];
static int nbr[MAXV][6], nnbr[MAXV];
static float hgt[MAXV], scr[MAXV][2], nvz[MAXV];
static unsigned short tri[MAXT][3];
static float bodyH = 1.56f;

static int hk_a[4096], hk_b[4096], hk_v[4096];
static void add_nbr(int a, int b) {
  for (int i = 0; i < nnbr[a]; i++) if (nbr[a][i] == b) return;
  if (nnbr[a] < 6) nbr[a][nnbr[a]++] = b;
}
static int midpoint(int a, int b) {
  if (a > b) { int t = a; a = b; b = t; }
  unsigned h = ((unsigned)a * 73856093u ^ (unsigned)b * 19349663u) & 4095;
  while (hk_a[h] != -1) { if (hk_a[h] == a && hk_b[h] == b) return hk_v[h]; h = (h + 1) & 4095; }
  float m[3], l = 0;
  for (int k = 0; k < 3; k++) { m[k] = (rest[a][k] + rest[b][k]) * .5f; l += m[k] * m[k]; }
  l = sqrtf(l);
  for (int k = 0; k < 3; k++) rest[nv][k] = m[k] / l;
  hk_a[h] = a; hk_b[h] = b; hk_v[h] = nv;
  return nv++;
}
static void build_mesh(void) {
  float t = (1 + sqrtf(5)) / 2;
  float iv[12][3] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                     {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
  int it[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                   {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                   {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
  nv = 12;
  for (int i = 0; i < 12; i++) {
    float l = sqrtf(iv[i][0] * iv[i][0] + iv[i][1] * iv[i][1] + iv[i][2] * iv[i][2]);
    for (int k = 0; k < 3; k++) rest[i][k] = iv[i][k] / l;
  }
  ntri = 20;
  for (int i = 0; i < 20; i++) for (int k = 0; k < 3; k++) tri[i][k] = it[i][k];
  for (int lvl = 0; lvl < 3; lvl++) {
    memset(hk_a, -1, sizeof hk_a);
    static unsigned short nt[MAXT][3]; int n = 0;
    for (int i = 0; i < ntri; i++) {
      int a = tri[i][0], b = tri[i][1], c = tri[i][2];
      int ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
      unsigned short q[4][3] = {{a, ab, ca}, {b, bc, ab}, {c, ca, bc}, {ab, bc, ca}};
      memcpy(nt[n], q, sizeof q); n += 4;
    }
    memcpy(tri, nt, n * sizeof nt[0]); ntri = n;
  }
  // sculpt the sphere into a soft mochi dome sitting on y=0
  for (int i = 0; i < nv; i++) {
    float x = rest[i][0], y = rest[i][1], z = rest[i][2];
    const float cut = -0.5f;
    if (y < cut) y = cut + (y - cut) * 0.12f;
    float w = 1.08f + 0.10f * (0.5f - y * 0.5f);
    rest[i][0] = x * w; rest[i][2] = z * w; rest[i][1] = y + 0.56f;
    hgt[i] = clampf(rest[i][1] / bodyH, 0, 1);
    memcpy(pos[i], rest[i], sizeof pos[i]);
    float d[3] = {rest[i][0], rest[i][1] - 0.62f, rest[i][2]};
    rlen[i] = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
    for (int k = 0; k < 3; k++) rdir[i][k] = d[k] / rlen[i];
  }
  for (int i = 0; i < ntri; i++)
    for (int k = 0; k < 3; k++) { add_nbr(tri[i][k], tri[i][(k + 1) % 3]); add_nbr(tri[i][(k + 1) % 3], tri[i][k]); }
}

/* face anchors: a few rest-space vertices near a direction from the body center */
enum { A_EYE_L, A_EYE_R, A_MOUTH, A_CHEEK_L, A_CHEEK_R, A_BROW_L, A_BROW_R, A_TOP, A_BOW, NANCH };
static const float ANCH_DIR[NANCH][3] = {{-0.36f, 0.05f, 0.93f}, {0.36f, 0.05f, 0.93f}, {0, -0.14f, 1},
                                         {-0.58f, -0.10f, 0.80f}, {0.58f, -0.10f, 0.80f}, {-0.36f, 0.30f, 0.88f},
                                         {0.36f, 0.30f, 0.88f}, {0, 1, 0.12f}, {-0.42f, 0.88f, 0.22f}};
static int anch_v[NANCH][3];
static void build_anchors(void) {
  for (int a = 0; a < NANCH; a++) {
    float d[3] = {ANCH_DIR[a][0], ANCH_DIR[a][1], ANCH_DIR[a][2]};
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float best[3] = {-9, -9, -9};
    for (int i = 0; i < nv; i++) {
      float r[3] = {rest[i][0], rest[i][1] - 0.62f, rest[i][2]};
      float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]) + 1e-6f;
      float s = (r[0] * d[0] + r[1] * d[1] + r[2] * d[2]) / (rl * l);
      for (int k = 0; k < 3; k++)
        if (s > best[k]) {
          for (int j = 2; j > k; j--) { best[j] = best[j - 1]; anch_v[a][j] = anch_v[a][j - 1]; }
          best[k] = s; anch_v[a][k] = i; break;
        }
    }
  }
}

/* ------------------------------------------------------------------ state */

static Display *dpy;
static Window win;
static GLXContext ctx;
static GLXFBConfig fb;
static int SW, SH, W, H, winX = -99999, winY = -99999;
static float R, footY;
static const float TILT = 0.30f;
static float cT, sT;
static int running = 1;

enum { ST_IDLE, ST_WANDER, ST_DRAG, ST_FLING, ST_REST, ST_STAY }; // STAY: dropped somewhere, sits there quietly
enum { HOP_WAIT, HOP_PREP, HOP_AIR };
enum { EX_NORMAL, EX_HAPPY, EX_DIZZY, EX_SLEEP, EX_SURPRISE, EX_PULL,
       EX_QUESTION, EX_CURIOUS, EX_LAUGH, EX_SMILE, EX_HURRY, EX_LOOKBACK };

static int st = ST_WANDER, hopst = HOP_AIR;
static float stT, hopT, restAge;
static float Fx, Fy, Vx, Vy, tx, ty;
static float hopPx, hopV, hopG;
static float yawCur, pitchCur, pitch, pitchV;
static int mouseActive; // the mouse moved in the last few seconds: the head follows it
static float sq = 1, sqV, sqT = 1, yaw, yawV, yawT;
static float flex;
static int grabbed = -1, pressed, dragging, pressPick = -1;
static float pressX, pressY, grabOffX, grabOffY, curX, curY, pullAmt, flingMax;
static double pressT;
static int expr = EX_NORMAL;
static float exprT, blinkT = 2, blinkOn, lookX, lookY, lookTX, lookTY, glanceT;
static int pokeCount;
static float pokeWin, petDist, zT;
static float tilt, tiltV, tiltT, qTilt;      // sideways head tilt (question, curiosity, giggles)
static int hurry, questioned, tickles;
static float stillT, tickleWin, sweatT, lbX; // cursor-still time, tickle window, sweat timer, look-back direction
static const float STAY_SECONDS = 300;       // after you drag it somewhere, it stays put this long
static int current_expr(void);

typedef struct { int type; float x, y, vx, vy, life, max, size, rot; } Part;
#define MAXP 24
static Part parts[MAXP];
/* particle types match the overlay shader: 14 heart, 15 z, 17 ?, 19 sweat drop, 20 dust puff */
static void spawn(int type, float x, float y) {
  for (int i = 0; i < MAXP; i++)
    if (parts[i].life <= 0) {
      Part *p = &parts[i];
      p->type = type; p->x = x; p->y = y; p->rot = frand(-0.3f, 0.3f);
      switch (type) {
      case 14: p->vx = frand(-18, 18); p->vy = frand(-55, -40); p->life = 1.3f; p->size = R * frand(0.13f, 0.18f); break;
      case 17: p->vx = 0; p->vy = -14; p->life = 1.9f; p->size = R * 0.24f; p->rot = x < 0 ? 0.2f : -0.2f; break;
      case 19: p->vx = x < 0 ? -45 : 45; p->vy = -60; p->life = 0.9f; p->size = R * 0.10f; p->rot = x < 0 ? 0.4f : -0.4f; break;
      case 20: p->vx = x < 0 ? -30 : 30; p->vy = -6; p->life = 0.55f; p->size = R * 0.14f; break;
      default: p->vx = 12; p->vy = -22; p->life = 2.4f; p->size = R * frand(0.10f, 0.15f); break;
      }
      p->max = p->life;
      return;
    }
}
static void set_expr(int e, float t) { expr = e; exprT = t; }
static void do_question(void) {
  float sd = rand() % 2 ? 1.f : -1.f;
  qTilt = 0.26f * sd;
  set_expr(EX_QUESTION, 2.2f);
  spawn(17, -sd * 0.8f * R, -1.85f * R);
}

static float bx0, bx1, by0, by1; // foot bounds on screen
static void compute_bounds(void) {
  bx0 = R * 1.4f; bx1 = SW - R * 1.4f;
  by0 = R * 2.2f + 32; by1 = SH - R * 0.35f;
}
static void pick_target(void) {
  for (int k = 0; k < 8; k++) {
    tx = frand(bx0 + 40, bx1 - 40); ty = frand(by0 + 40, by1 - 20);
    if (hypotf(tx - Fx, ty - Fy) > 280) break;
  }
}
static int near_corner(void) {
  const float m = 260;
  return (Fx - bx0 < m || bx1 - Fx < m) && (Fy - by0 < m || by1 - Fy < m);
}
static void go_idle(float t) { st = ST_IDLE; stT = t; }
static void go_rest(float t) { st = ST_REST; stT = t; restAge = 0; zT = 1.5f; yawT = 0; }
static void go_stay(float t) { st = ST_STAY; stT = t; hurry = 0; }

/* ------------------------------------------------------------------ physics */

static void project(const float p[3], float *px, float *py) {
  float yv = p[1] * cT - p[2] * sT;
  *px = W * 0.5f + p[0] * R;
  *py = footY - hopPx - yv * R;
}

/* Stylized "flow" deformation: instead of simulating every vertex, the whole shape is driven by a few
   eased springs — squash, lean, and a signed stretch along an axis that follows the pull/push direction.
   Each vertex then glides toward that shape (the top a little later), so motion reads as smooth follow-through. */
/* Feel model, after cjxhaaa/slime's Blob:
   - the surface is a field of radial offsets, each sprung back to rest and coupled to its mesh neighbours,
     so a poke or an impact sends a ripple travelling across the body instead of just denting one spot;
   - global squash is its own volume-preserving spring;
   - a held jelly is locked to the hand (no lag) and stretches into a droplet along a smoothed velocity
     trail, which it keeps on release and unwinds from. */
static float trailX, trailY;             // smoothed screen velocity, px/s
/* pinch: the grabbed spot is pulled out toward the hand (P, body units, x / y-up), with a smooth patch falloff */
static float gw[MAXV], Px, Py, PVx, PVy;
static int pinchI = -1;
static float lastFx, lastFy;
static float rnd, rndV; // 0 = sitting on a flat bottom, 1 = a round ball (while held)
#define NTRK 16
static double trkT[NTRK]; static float trkX[NTRK], trkY[NTRK]; static int trkN, trkI;
static float dragVX, dragVY;

static void track(double t, float x, float y) {
  trkT[trkI] = t; trkX[trkI] = x; trkY[trkI] = y; trkI = (trkI + 1) % NTRK; if (trkN < NTRK) trkN++;
}
/* hand velocity fitted over the last ~80 ms, not the last event's delta */
static void tracked_velocity(double t, float *vx, float *vy) {
  *vx = *vy = 0;
  int newest = (trkI - 1 + NTRK) % NTRK, oldest = newest;
  for (int k = 1; k < trkN; k++) {
    int j = (newest - k + NTRK) % NTRK;
    if (t - trkT[j] > 0.08) break;
    oldest = j;
  }
  double dt = trkT[newest] - trkT[oldest];
  if (trkN < 2 || dt < 0.008 || t - trkT[newest] > 0.06) return;
  *vx = (float)((trkX[newest] - trkX[oldest]) / dt);
  *vy = (float)((trkY[newest] - trkY[oldest]) / dt);
}

/* dent (negative) or bulge around a direction, as a velocity kick that the ring carries as a wave */
static void surf_poke(float dx, float dy, float dz, float depth, float spread) {
  float l = sqrtf(dx * dx + dy * dy + dz * dz) + 1e-6f;
  dx /= l; dy /= l; dz /= l;
  for (int i = 0; i < nv; i++) {
    float c = clampf(rdir[i][0] * dx + rdir[i][1] * dy + rdir[i][2] * dz, -1, 1);
    float ang = acosf(c);
    if (ang > spread) continue;
    offV[i] += depth * cosf(ang / spread * PI * 0.5f);
  }
}
static void surf_pulse(float depth) { for (int i = 0; i < nv; i++) offV[i] += depth; }
static void squash(float amount) { sqV += -amount * 14; } // + flattens, - stretches up

/* screen directions -> body directions: screen x = x, screen up = away from the camera (-z) */
static void impact(float sx, float sy, float strength) {
  float n = fminf(1, strength / 1400);
  surf_poke(sx, 0, sy, -9.0f * n, 1.5f);
  squash(0.5f * n);
}

static void poke(int gi) {
  if (gi < 0) return;
  if (st == ST_REST) { // woken up, but it stays where you put it
    go_stay(stT); set_expr(EX_SURPRISE, 1.2f);
    surf_pulse(2.0f); squash(-0.3f);
    return;
  }
  surf_poke(rdir[gi][0], rdir[gi][1], rdir[gi][2], -5.6f * (0.6f + flex), 1.4f);
  squash(0.22f);
  pokeCount++; pokeWin = 1.6f;
  if (pokeCount >= 4) { set_expr(EX_DIZZY, 2.2f); pokeCount = 0; }
  else if (pokeCount >= 2) { set_expr(EX_LAUGH, 1.8f); spawn(14, frand(-0.4f, 0.4f) * R, -1.5f * R); }
  else { set_expr(EX_HAPPY, 1.9f); spawn(14, frand(-0.4f, 0.4f) * R, -1.5f * R); }
}

/* The grabbed half of the body comes along: full at the grabbed spot, about half at the equator, nothing on the
   far side. The neck then forms around the middle, like a mochi being pulled apart. */
static void set_pinch_weights(int gi, float sigma) {
  float edge = lerpf(0.55f, -0.55f, cfg.pull); // 0: a small pinch .. 1: a bit more than half the body
  for (int i = 0; i < nv; i++) {
    float d = rdir[i][0] * rdir[gi][0] + rdir[i][1] * rdir[gi][1] + rdir[i][2] * rdir[gi][2];
    float u = clampf((d - edge) / (1 - edge), 0, 1);
    float hemi = powf(u * u * (3 - 2 * u), 1.3f);
    float d2 = 0;
    for (int k = 0; k < 3; k++) { float e = rest[i][k] - rest[gi][k]; d2 += e * e; }
    float tip = expf(-d2 / (sigma * sigma));
    gw[i] = fmaxf(hemi, tip);
  }
}

static void release_grab(float kick) {
  grabbed = -1; (void)kick;
  // the pulled lump snaps back into the body and the whole jelly rings from it
  float pl = hypotf(Px, Py);
  if (pinchI >= 0) surf_poke(rdir[pinchI][0], rdir[pinchI][1], rdir[pinchI][2], -3.0f * fminf(pl, 1.5f), 1.5f);
  squash(0.18f * fminf(pl, 1.5f));
}

static void step(float dt, double t) {
  float f = flex;

  /* ---- body on the screen ---- */
  switch (st) {
  case ST_IDLE:
    Vx = Vy = 0; sqT = current_expr() == EX_CURIOUS ? 1.06f : 1; // stretches up to have a look
    stT -= dt;
    if (stT <= 0) {
      if (rand() % 4 == 0) go_idle(frand(2, 6));
      else {
        pick_target(); st = ST_WANDER; hopst = HOP_WAIT; hopT = 0.1f;
        hurry = hypotf(tx - Fx, ty - Fy) > 600 && rand() % 100 < 35;
      }
    }
    break;
  case ST_WANDER:
    if (hopst == HOP_WAIT) {
      Vx = Vy = 0; sqT = 1; hopT -= dt;
      if (hopT <= 0) { hopst = HOP_PREP; hopT = hurry ? 0.09f : 0.16f; squash(hurry ? 0.22f : 0.30f); } // anticipation
    } else if (hopst == HOP_PREP) {
      hopT -= dt;
      if (hopT <= 0) {
        float dx = tx - Fx, dy = ty - Fy, d = hypotf(dx, dy);
        if (d < R * 0.4f) { // arrived: sometimes a little feeling about it
          hurry = 0; go_idle(frand(3, 10));
          int r = rand() % 100;
          if (r < 15) do_question();
          else if (r < 35) set_expr(EX_SMILE, 2.5f);
          break;
        }
        float hp = hurry ? R * frand(0.14f, 0.22f) : R * frand(0.24f, 0.40f);
        hopG = R * 16; hopV = sqrtf(2 * hopG * hp);
        float tair = 2 * hopV / hopG, dist = fminf(d, hurry ? R * frand(0.9f, 1.15f) : R * frand(0.55f, 0.85f));
        Vx = dx / d * dist / tair; Vy = dy / d * dist / tair;
        hopst = HOP_AIR; lbX = Vx;
        squash(-0.36f); surf_pulse(1.5f); // spring up: stretch tall
        float yt = atan2f(dx, dy);
        yawT = clampf(yt, -0.55f, 0.55f);
        if (fabsf(yt) > 2.4f) yawT = 0;
      }
    } else { // HOP_AIR (also the first drop-in)
      hopV -= hopG * dt; hopPx += hopV * dt;
      if (hopPx <= 0) {
        float speed = -hopV / R * 46; // in the reference's px/s for a 46px body
        hopPx = 0; hopV = 0; Vx = Vy = 0;
        surf_poke(0, -1, 0, -9.0f * fminf(1, speed / 700), 1.5f);
        squash(0.5f * fminf(1, speed / 700));
        if (hurry) { spawn(20, -0.7f * R, -0.05f * R); spawn(20, 0.7f * R, -0.05f * R); }
        hopst = HOP_WAIT; hopT = hurry ? frand(0.04f, 0.12f) : frand(0.35f, 1.1f);
        if (!hurry && rand() % 100 < 12) { // stop and look back over its shoulder
          float dir = lbX;
          lbX = dir < 0 ? 1 : -1;
          yawT = clampf(lbX * 1.0f, -1.0f, 1.0f);
          set_expr(EX_LOOKBACK, 1.2f); hopT += 1.2f; mouseActive = 0;
        }
      }
    }
    break;
  case ST_DRAG: { // pinched: the spot follows the hand; the body is towed once the stretch reaches its leash
    float Lmax = lerpf(0.9f, 1.9f, f) * R;
    float dx = curX - grabOffX - Fx, dy = curY - grabOffY - (Fy - hopPx), L = hypotf(dx, dy);
    if (L > Lmax) { float k = (L - Lmax) / L; Fx += dx * k; Fy += dy * k; dx -= dx * k; dy -= dy * k; }
    Vx = (Fx - lastFx) / dt; Vy = (Fy - lastFy) / dt;
    // pinch target in body units (screen up is +y in the view plane)
    float tx_ = dx / R, ty_ = -dy / (R * cT);
    float w = 2 * PI * 7, z = 0.75f;
    PVx += (w * w * (tx_ - Px) - 2 * z * w * PVx) * dt;
    PVy += (w * w * (ty_ - Py) - 2 * z * w * PVy) * dt;
    hopPx += (R * 0.28f - hopPx) * fminf(1, dt * 10); hopV = 0; // lifted off the desk
    sqT = 1;
    break;
  }
  case ST_FLING: {
    float drag = expf(-dt / 0.34f);
    Vx *= drag; Vy *= drag; sqT = 1;
    hopPx += (0 - hopPx) * fminf(1, dt * 12);
    if (hypotf(Vx, Vy) < 6) {
      Vx = Vy = 0;
      if (near_corner()) go_rest(STAY_SECONDS);
      else {
        go_stay(STAY_SECONDS);
        if (flingMax > 1200) set_expr(EX_DIZZY, 1.6f);
        else if (flingMax < 300) set_expr(EX_SMILE, 2.0f);
      }
    }
    break;
  }
  case ST_STAY: // quiet: no wandering, no spontaneous antics; still breathes and reacts to you
    Vx = Vy = 0;
    sqT = current_expr() == EX_CURIOUS ? 1.06f : 1 + 0.015f * sinf((float)t * 1.4f);
    stT -= dt;
    if (stT <= 0) go_idle(frand(1, 3));
    break;
  case ST_REST:
    Vx = Vy = 0; restAge += dt;
    sqT = 1 + 0.03f * sinf((float)t * 1.6f);
    stT -= dt;
    if (stT <= 0) { go_idle(frand(1.5f, 3)); squash(-0.3f); surf_pulse(1.5f); set_expr(EX_HAPPY, 1.0f); }
    break;
  }

  if (st != ST_DRAG) {
    Fx += Vx * dt; Fy += Vy * dt;
    // walls: bounce a throw and splat against it
    if (Fx < bx0) { Fx = bx0; if (st == ST_FLING) impact(-1, 0, fabsf(Vx)); Vx = fabsf(Vx) * 0.45f; }
    if (Fx > bx1) { Fx = bx1; if (st == ST_FLING) impact(1, 0, fabsf(Vx)); Vx = -fabsf(Vx) * 0.45f; }
    if (Fy < by0) { Fy = by0; if (st == ST_FLING) impact(0, -1, fabsf(Vy)); Vy = fabsf(Vy) * 0.45f; }
    if (Fy > by1) { Fy = by1; if (st == ST_FLING) impact(0, 1, fabsf(Vy)); Vy = -fabsf(Vy) * 0.45f; }
    if (st == ST_WANDER && hopst == HOP_AIR && (Fx == bx0 || Fx == bx1 || Fy == by0 || Fy == by1)) pick_target();
  } else {
    Fx = clampf(Fx, R, SW - R); Fy = clampf(Fy, R * 2, SH);
  }
  lastFx = Fx; lastFy = Fy;
  if (st != ST_DRAG) {
    float w = 2 * PI * lerpf(4.5f, 2.6f, f), z = lerpf(0.22f, 0.10f, f);
    PVx += (-w * w * Px - 2 * z * w * PVx) * dt;
    PVy += (-w * w * Py - 2 * z * w * PVy) * dt;
  }
  Px += PVx * dt; Py += PVy * dt;
  float tr = fminf(1, dt * (st == ST_DRAG ? 8 : 6));
  trailX += (Vx - trailX) * tr; trailY += (Vy - trailY) * tr;

  /* ---- round up into a ball while held (a little while flying), settle flat when put down ---- */
  float rT = st == ST_DRAG ? 1.0f : st == ST_FLING ? 0.6f : (st == ST_WANDER && hopst == HOP_AIR) ? 0.35f : 0.0f;
  float wr = 2 * PI * lerpf(3.4f, 2.2f, f), zr = lerpf(0.38f, 0.26f, f);
  rndV += (-wr * wr * (rnd - rT) - 2 * zr * wr * rndV) * dt;
  rnd = clampf(rnd + rndV * dt, -0.25f, 1.25f);

  if (current_expr() == EX_LAUGH) sqT = 1 + 0.07f * sinf((float)t * 24); // giggle bounce
  { float w = 2 * PI * 2.2f, z = 0.45f;
    tiltV += (-w * w * (tilt - tiltT) - 2 * z * w * tiltV) * dt;
    tilt += tiltV * dt; }

  /* ---- squash spring (volume preserving) and yaw ---- */
  float kS = lerpf(190, 110, f), dS = lerpf(18, 11, f);
  sqV += (-kS * (sq - sqT) - dS * sqV) * dt;
  sq = clampf(sq + sqV * dt, 0.45f, 1.7f);
  if (mouseActive) yawT = yawCur;
  else if (st != ST_WANDER) yawT = (st == ST_IDLE || st == ST_DRAG || st == ST_STAY) ? yawCur : 0;
  float pitchT = mouseActive ? pitchCur : 0;
  pitchV += (-30 * (pitch - pitchT) - 2 * 0.8f * 5.5f * pitchV) * dt;
  pitch += pitchV * dt;
  yawV += (-30 * (yaw - yawT) - 2 * 0.8f * 5.5f * yawV) * dt;
  yaw += yawV * dt;

  /* ---- the surface ring: radial springs + neighbour coupling ---- */
  float stiff = lerpf(300, 150, f), damp = lerpf(9.0f, 5.5f, f), coup = lerpf(480, 320, f);
  float ceil = 0.16f * sqrtf(stiff) * 1.6f;
  memcpy(offSnap, off, sizeof(float) * nv);
  for (int i = 0; i < nv; i++) {
    float mean = 0;
    for (int j = 0; j < nnbr[i]; j++) mean += offSnap[nbr[i][j]];
    mean /= nnbr[i];
    offV[i] += (-stiff * off[i] - damp * offV[i] + coup * (mean - off[i])) * dt;
    offV[i] = clampf(offV[i], -ceil * 6, ceil * 6);
  }
  for (int i = 0; i < nv; i++) {
    off[i] += offV[i] * dt;
    if (off[i] > 0.55f) { off[i] = 0.55f; offV[i] *= 0.4f; }
    else if (off[i] < -0.55f) { off[i] = -0.55f; offV[i] *= 0.4f; }
  }

  /* ---- droplet stretch along the trail ---- */
  float sp = hypotf(trailX, trailY) / R * 46; // reference px/s
  float stretch = fminf(0.32f, sp / 3600) * lerpf(0.8f, 1.3f, f);
  float ux = 0, uy = 0;
  if (sp > 1) { ux = trailX / hypotf(trailX, trailY); uy = -trailY / hypotf(trailX, trailY); }
  float pl = hypotf(Px, Py);
  pullAmt = st == ST_DRAG ? pl / lerpf(0.9f, 1.9f, f) : 0;
  float pux = pl > 1e-4f ? Px / pl : 0, puy = pl > 1e-4f ? Py / pl : 0, thin = fminf(0.5f, 0.32f * pl);

  /* ---- assemble the shape ---- */
  float sx = 1 / sqrtf(sq), cy = cosf(yaw), sy = sinf(yaw), cpi = cosf(pitch), spi = sinf(pitch);
  for (int i = 0; i < nv; i++) {
    float rr = rlen[i] + (0.98f - rlen[i]) * rnd, cyl = 0.62f + 0.36f * rnd; // sitting dome <-> ball of radius 0.98
    float r = rr * (1 + off[i] * (0.35f + 0.65f * hgt[i])); // base touches the floor, moves less
    float lx0 = rdir[i][0] * r, ly0 = rdir[i][1] * r + cyl, lz0 = rdir[i][2] * r;
    float gx = lx0 * sx, gy = ly0 * sq, gz = lz0 * sx;
    gx += tilt * gy; // head tilt
    float g[3] = {gx * cy + gz * sy, gy, -gx * sy + gz * cy};
    if (fabsf(pitch) > 1e-4f) { // tip the head up / down toward the mouse
      float c0 = (0.62f + 0.36f * rnd) * sq, yy = g[1] - c0, zz = g[2];
      g[1] = c0 + yy * cpi + zz * spi; g[2] = -yy * spi + zz * cpi;
    }
    if (stretch > 1e-4f) { // elongate toward the travel direction, in the view plane
      float cyc = (0.62f + 0.36f * rnd) * sq;
      float vx = g[0], vy = (g[1] - cyc) * cT - g[2] * sT;
      float l = hypotf(vx, vy) + 1e-6f, al = (vx * ux + vy * uy) / l;
      float k = 1 + stretch * al;
      g[0] *= k; g[1] = cyc + (g[1] - cyc) * k; g[2] *= k;
    }
    if (pinchI >= 0 && pl > 1e-3f) { // pulled lump with a thinning neck
      float w = gw[i], cyc = (0.62f + 0.36f * rnd) * sq;
      float rx = g[0], ry = g[1] - cyc;
      float al = rx * pux + ry * puy, th = 1 - thin * 4 * w * (1 - w);
      g[0] = al * pux + (rx - al * pux) * th;
      g[1] = cyc + al * puy + (ry - al * puy) * th;
      g[2] *= th;
      g[0] += Px * w; g[1] += Py * w;
    }
    float floorY = (pinchI >= 0 && Py < 0) ? Py * gw[i] : 0;
    if (g[1] < floorY) g[1] = floorY;
    memcpy(pos[i], g, sizeof g);
  }
}

static void compute_normals(void) {
  memset(nrm, 0, sizeof(float) * 3 * nv);
  for (int t = 0; t < ntri; t++) {
    float *a = pos[tri[t][0]], *b = pos[tri[t][1]], *c = pos[tri[t][2]];
    float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    for (int k = 0; k < 3; k++) for (int j = 0; j < 3; j++) nrm[tri[t][k]][j] += n[j];
  }
  for (int i = 0; i < nv; i++) {
    float l = sqrtf(nrm[i][0] * nrm[i][0] + nrm[i][1] * nrm[i][1] + nrm[i][2] * nrm[i][2]) + 1e-9f;
    for (int j = 0; j < 3; j++) nrm[i][j] /= l;
    project(pos[i], &scr[i][0], &scr[i][1]);
    nvz[i] = nrm[i][2] * cT + nrm[i][1] * sT;
  }
}

static int pick_vertex(float wx, float wy) {
  int best = -1; float bd = 1e18f;
  for (int i = 0; i < nv; i++) {
    if (nvz[i] < 0.05f) continue;
    float dx = scr[i][0] - wx, dy = scr[i][1] - wy, d = dx * dx + dy * dy;
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

/* ------------------------------------------------------------------ GL */

static const char *JELLY_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos; layout(location=1) in vec3 aN;\n"
    "uniform vec3 uProj; uniform vec2 uTilt;\n"
    "out vec3 vN; out float vY;\n"
    "void main(){\n"
    "  float yv = aPos.y*uTilt.x - aPos.z*uTilt.y;\n"
    "  float zv = aPos.z*uTilt.x + aPos.y*uTilt.y;\n"
    "  vN = vec3(aN.x, aN.y*uTilt.x - aN.z*uTilt.y, aN.z*uTilt.x + aN.y*uTilt.y);\n"
    "  vY = aPos.y;\n"
    "  gl_Position = vec4(aPos.x*uProj.x, uProj.z + yv*uProj.y, -zv*0.2, 1.0);\n"
    "}\n";

static const char *JELLY_FS =
    "#version 330 core\n"
    "in vec3 vN; in float vY;\n"
    "uniform vec3 uColor;\n"
    "out vec4 frag;\n"
    "float box(vec2 p, vec2 b, float s){ vec2 d=abs(p)-b; float o=length(max(d,0.))+min(max(d.x,d.y),0.);"
    "  return 1.0-smoothstep(-s,s,o); }\n"
    "float windows(vec3 r){\n" // bright room windows behind the viewer -> glossy highlights
    "  if(r.z < 0.08) return 0.0;\n"
    "  vec2 uv = r.xy / r.z;\n"
    "  vec2 q = uv - vec2(-0.80, 1.05);\n"
    "  float w = box(q, vec2(0.44, 0.36), 0.05);\n"
    "  float mull = max(1.0-smoothstep(0.02,0.045,abs(q.x)), 1.0-smoothstep(0.02,0.045,abs(q.y)));\n"
    "  float m = w*(1.0-mull);\n"
    "  m += 0.55*box(uv - vec2(1.05, 0.85), vec2(0.13, 0.30), 0.06);\n"
    "  return m;\n"
    "}\n"
    "vec3 env(vec3 d){ float t=clamp(d.y*0.5+0.5,0.,1.); return mix(vec3(0.72,0.72,0.76), vec3(1.0), t); }\n"
    "void main(){\n"
    "  vec3 N = normalize(vN);\n"
    "  float ndv = clamp(N.z, 0.0, 1.0);\n"
    "  vec3 R = reflect(vec3(0,0,-1), N);\n"
    "  vec3 T = refract(vec3(0,0,-1), N, 1.0/1.38);\n"
    "  float thick = pow(ndv, 0.7);\n"
    "  vec3 absorb = pow(max(uColor, vec3(0.001)), vec3(1.0 + 1.5*thick));\n" // Beer-Lambert-ish depth tint
    "  vec3 body = absorb * (0.78 + 0.30*env(T).r + 0.18*N.y) + vec3(0.05);\n"
    "  body += uColor * 0.30 * smoothstep(0.45, 0.0, vY);\n"                  // light pooling at the base
    "  float F = 0.03 + 0.97*pow(1.0-ndv, 5.0);\n"
    "  float win = windows(R) * (0.55 + 0.45*F);\n"
    "  float spec = pow(max(dot(R, normalize(vec3(-0.45,0.65,0.62))),0.0), 220.0);\n"
    "  float rim = pow(1.0-ndv, 2.5);\n"
    "  float aB = mix(0.60, 0.93, thick);\n"
    "  vec3 rgb = body*aB*(1.0-F) + env(R)*F*0.75 + uColor*rim*0.30 + vec3(win*0.95 + spec*1.2);\n"
    "  float a = clamp(aB*(1.0-F) + F*0.75 + rim*0.15 + win*0.9 + spec, 0.0, 1.0);\n"
    "  frag = vec4(min(rgb, vec3(a)), a);\n"
    "}\n";

static const char *QUAD_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 aC;\n"
    "uniform vec2 uCenter, uHalf, uWin; uniform float uRot;\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "  vUV = aC;\n"
    "  vec2 p = aC*uHalf; float c=cos(uRot), s=sin(uRot);\n"
    "  p = vec2(c*p.x - s*p.y, s*p.x + c*p.y);\n"
    "  vec2 w = uCenter + vec2(p.x, -p.y);\n"
    "  gl_Position = vec4(w.x/uWin.x*2.0-1.0, 1.0-w.y/uWin.y*2.0, 0.0, 1.0);\n"
    "}\n";

static const char *QUAD_FS =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform int uType; uniform vec4 uCol; uniform vec4 uP;\n"
    "out vec4 frag;\n"
    "const vec3 INK = vec3(0.11,0.11,0.19);\n"
    "float seg(vec2 p, vec2 a, vec2 b){ vec2 pa=p-a, ba=b-a; float h=clamp(dot(pa,ba)/dot(ba,ba),0.,1.); return length(pa-ba*h); }\n"
    "float fill(float d){ float w=fwidth(d)*0.75+1e-4; return 1.0-smoothstep(-w,w,d); }\n"
    "float heart(vec2 p){ p.x=abs(p.x); if(p.y+p.x>1.0) return length(p-vec2(0.25,0.75))-0.35355;"
    "  return sqrt(min(dot(p-vec2(0,1),p-vec2(0,1)), dot(p-0.5*max(p.x+p.y,0.0),p-0.5*max(p.x+p.y,0.0))))*sign(p.x-p.y); }\n"
    "void main(){\n"
    "  vec2 p = vUV; vec3 c = INK; float a = 0.0;\n"
    "  if(uType==0){\n" // floor shadow + colored caustic
    "    float r2 = dot(p,p); a = exp(-r2*2.6)*0.30*uP.x;\n"
    "    c = mix(vec3(0.04,0.04,0.07), uCol.rgb*0.45, 0.55);\n"
    "    vec2 q = p - vec2(0.0,-0.22); float g = exp(-(q.x*q.x*5.0 + q.y*q.y*28.0))*uP.x;\n"
    "    frag = vec4(c*a + uCol.rgb*g*0.28, a + g*0.22); return;\n"
    "  }\n"
    "  if(uType==1){\n" // open eye (+ lashes)
    "    float d = (length(p/vec2(uP.z*0.46, uP.z*0.58)) - 1.0)*0.46*uP.z; a = fill(d);\n"
    "    float hl = fill(length(p - vec2(-0.13,0.20)*uP.z) - 0.15*uP.z); c = mix(INK, vec3(1), hl);\n"
    "    if(uP.y>0.5){ float s=uP.x; float dl=min(seg(p,vec2(0.30*s,0.42),vec2(0.66*s,0.72)), seg(p,vec2(0.42*s,0.22),vec2(0.84*s,0.40)))-0.07;"
    "      float la=fill(dl); c=mix(c,INK,la*(1.0-a)); a=max(a,la); }\n"
    "  } else if(uType==2){\n" // happy ^
    "    float d = abs(length(p-vec2(0,-0.35))-0.55)-0.09; a = fill(d)*smoothstep(-0.32,-0.12,p.y);\n"
    "  } else if(uType==3){\n" // sleepy closed
    "    float d = abs(length(p-vec2(0,0.35))-0.50)-0.08; a = fill(d)*smoothstep(0.28,0.10,p.y);\n"
    "    if(uP.y>0.5){ float s=uP.x; a=max(a, fill(seg(p,vec2(0.36*s,-0.05),vec2(0.62*s,-0.22))-0.06)); }\n"
    "  } else if(uType==4){\n" // blink line
    "    a = fill(seg(p,vec2(-0.45,0.0),vec2(0.45,0.0))-0.08);\n"
    "  } else if(uType==5){\n" // > <
    "    vec2 q = vec2(p.x*-uP.x, p.y);\n"
    "    a = fill(min(seg(q,vec2(-0.40,0.40),vec2(0.35,0.0)), seg(q,vec2(0.35,0.0),vec2(-0.40,-0.40)))-0.09);\n"
    "  } else if(uType==6){\n" // small smile
    "    float d = abs(length(p-vec2(0,0.55))-0.72)-0.11; a = fill(d)*smoothstep(0.1,-0.05,p.y);\n"
    "  } else if(uType==7){\n" // open happy mouth
    "    float d = max(length(p-vec2(0,0.25))-0.78, p.y-0.25); a = fill(d);\n"
    "    float tg = fill(length(p-vec2(0,-0.45))-0.36); c = mix(vec3(0.45,0.07,0.12), vec3(0.98,0.47,0.52), tg);\n"
    "  } else if(uType==8){\n" // 'o' mouth
    "    float d = (length(p/vec2(0.42*uP.x,0.52*uP.x))-1.0)*0.42*uP.x; a = fill(d);\n"
    "    c = mix(INK, vec3(0.45,0.07,0.12), fill(d+0.09));\n"
    "  } else if(uType==9){\n" // blush
    "    a = exp(-dot(p,p)*2.8); c = uCol.rgb;\n"
    "  } else if(uType==11){\n" // ribbon bow
    "    float e1 = length((p-vec2(-0.47,0.0))/vec2(0.50,0.40))-1.0; float e2 = length((p-vec2(0.47,0.0))/vec2(0.50,0.40))-1.0;\n"
    "    float d = min(min(e1,e2)*0.40, length(p)-0.20); a = fill(d);\n"
    "    c = uCol.rgb*(0.78+0.35*p.y); c = mix(c, uCol.rgb*0.55, smoothstep(-0.07,-0.01,d));\n"
    "    float fold = min(seg(p,vec2(-0.2,0.0),vec2(-0.62,0.14)), seg(p,vec2(0.2,0.0),vec2(0.62,0.14)));\n"
    "    c = mix(c, uCol.rgb*0.6, fill(fold-0.035)*0.8);\n"
    "    c = mix(c, vec3(1), fill(length(p-vec2(-0.55,0.18))-0.09)*0.8);\n"
    "  } else if(uType==12){\n" // hair curl
    "    vec2 q = p-vec2(0.12,0.28); float ang = atan(q.y,q.x);\n"
    "    float arc = abs(length(q)-0.32)-0.10; arc = (ang > -1.2 || ang < -2.9) ? arc : 1.0;\n"
    "    float stem = seg(p, vec2(0.02,-0.95), vec2(-0.20,0.26))-0.11;\n"
    "    a = fill(min(arc,stem)); c = uCol.rgb;\n"
    "  } else if(uType==13){\n" // eyebrow
    "    a = fill(seg(p,vec2(-0.5,0.0),vec2(0.5,0.0))-0.14);\n"
    "  } else if(uType==16){\n" // arched brow, uP.x = curvature radius
    "    float c = uP.x; float d = abs(length(p - vec2(0.0, 0.15 - c)) - c) - 0.16;\n"
    "    a = fill(d) * smoothstep(0.75, 0.55, abs(p.x));\n"
    "  } else if(uType==17){\n" // question mark
    "    vec2 q = p - vec2(0.0, 0.32); float ang = atan(q.y, q.x);\n"
    "    float hook = abs(length(q) - 0.34) - 0.1; hook = (ang > -1.2 || ang < -2.6) ? hook : 1.0;\n"
    "    float stem = seg(p, vec2(0.12, 0.02), vec2(0.0, -0.3)) - 0.1;\n"
    "    float dot_ = length(p - vec2(0.0, -0.66)) - 0.12;\n"
    "    a = fill(min(min(hook, stem), dot_)); c = vec3(0.42, 0.40, 0.72);\n"
    "  } else if(uType==19){\n" // sweat drop
    "    float r = 0.5 * clamp(1.0 - (p.y + 0.2) * 0.95, 0.04, 1.0);\n"
    "    float d = p.y > -0.2 ? abs(p.x) - r : length(p - vec2(0.0, -0.2)) - 0.5;\n"
    "    a = fill(d * 0.8); c = mix(vec3(0.55, 0.80, 1.0), vec3(1.0), fill(length(p - vec2(-0.15, -0.3)) - 0.12));\n"
    "  } else if(uType==20){\n" // dust puff
    "    a = exp(-dot(p, p) * 3.0) * 0.55; c = vec3(0.88, 0.88, 0.92);\n"
    "  } else if(uType==14){\n" // heart
    "    a = fill(heart(vec2(p.x*0.72, p.y*0.72+0.48))); c = vec3(1.0,0.42,0.58);\n"
    "    c = mix(c, vec3(1), fill(length(p-vec2(-0.3,0.3))-0.12)*0.7);\n"
    "  } else if(uType==15){\n" // z
    "    a = fill(min(min(seg(p,vec2(-0.5,0.5),vec2(0.5,0.5)), seg(p,vec2(0.5,0.5),vec2(-0.5,-0.5))), seg(p,vec2(-0.5,-0.5),vec2(0.5,-0.5)))-0.12);\n"
    "    c = vec3(0.35,0.38,0.55);\n"
    "  }\n"
    "  a *= uCol.a;\n"
    "  frag = vec4(c*a, a);\n"
    "}\n";

static GLuint jprog, qprog, jvao, jvbo, jibo, qvao, qvbo;
static GLint uJProj, uJTilt, uJColor, uQCenter, uQHalf, uQWin, uQRot, uQType, uQCol, uQP;

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL); glCompileShader(s);
  GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "shader: %s\n", log); exit(1); }
  return s;
}
static GLuint link_prog(const char *vs, const char *fs) {
  GLuint p = glCreateProgram();
  glAttachShader(p, compile(GL_VERTEX_SHADER, vs)); glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
  glLinkProgram(p);
  GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "link: %s\n", log); exit(1); }
  return p;
}
static void gl_init(void) {
  jprog = link_prog(JELLY_VS, JELLY_FS);
  qprog = link_prog(QUAD_VS, QUAD_FS);
  uJProj = glGetUniformLocation(jprog, "uProj"); uJTilt = glGetUniformLocation(jprog, "uTilt");
  uJColor = glGetUniformLocation(jprog, "uColor");
  uQCenter = glGetUniformLocation(qprog, "uCenter"); uQHalf = glGetUniformLocation(qprog, "uHalf");
  uQWin = glGetUniformLocation(qprog, "uWin"); uQRot = glGetUniformLocation(qprog, "uRot");
  uQType = glGetUniformLocation(qprog, "uType"); uQCol = glGetUniformLocation(qprog, "uCol");
  uQP = glGetUniformLocation(qprog, "uP");

  glGenVertexArrays(1, &jvao); glBindVertexArray(jvao);
  glGenBuffers(1, &jvbo); glBindBuffer(GL_ARRAY_BUFFER, jvbo);
  glBufferData(GL_ARRAY_BUFFER, nv * 6 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0); glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12); glEnableVertexAttribArray(1);
  glGenBuffers(1, &jibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, jibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, ntri * 3 * sizeof(unsigned short), tri, GL_STATIC_DRAW);

  float q[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  glGenVertexArrays(1, &qvao); glBindVertexArray(qvao);
  glGenBuffers(1, &qvbo); glBindBuffer(GL_ARRAY_BUFFER, qvbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0); glEnableVertexAttribArray(0);
}

static void quad(int type, float cx, float cy, float hx, float hy, float rot, float r, float g, float b, float a,
                 float p0, float p1, float p2, float p3) {
  glUniform1i(uQType, type);
  glUniform2f(uQCenter, cx, cy); glUniform2f(uQHalf, hx, hy); glUniform1f(uQRot, rot);
  glUniform4f(uQCol, r, g, b, a); glUniform4f(uQP, p0, p1, p2, p3);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

typedef struct { float x, y, nx, ny, nz; } Anch;
static Anch anchor(int a) {
  Anch o = {0};
  float p[3] = {0, 0, 0}, n[3] = {0, 0, 0};
  for (int k = 0; k < 3; k++) for (int c = 0; c < 3; c++) { p[c] += pos[anch_v[a][k]][c] / 3; n[c] += nrm[anch_v[a][k]][c]; }
  float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-9f;
  project(p, &o.x, &o.y);
  o.nx = n[0] / l; o.ny = (n[1] * cT - n[2] * sT) / l; o.nz = (n[2] * cT + n[1] * sT) / l;
  return o;
}

static int current_expr(void) {
  if (exprT > 0) return expr;
  switch (st) {
  case ST_REST: return restAge > 1.2f ? EX_SLEEP : EX_NORMAL;
  case ST_DRAG: return EX_PULL;
  case ST_FLING: return EX_SURPRISE;
  case ST_WANDER: return hurry ? EX_HURRY : EX_NORMAL;
  default: return EX_NORMAL;
  }
}

static void render(void) {
  glViewport(0, 0, W, H);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied alpha for the ARGB visual
  const float *col = cfg.col;

  /* shadow */
  glDisable(GL_DEPTH_TEST);
  glUseProgram(qprog); glBindVertexArray(qvao);
  glUniform2f(uQWin, (float)W, (float)H);
  float shrink = 1.0f / (1.0f + hopPx / (R * 0.8f));
  quad(0, W * 0.5f, footY + 0.04f * R, 1.30f * R * shrink / sqrtf(sq), 0.36f * R * shrink, 0, col[0], col[1], col[2], 1,
       shrink, 0, 0, 0);

  /* jelly body: depth pre-pass so only the nearest surface is blended */
  static float vbuf[MAXV * 6];
  for (int i = 0; i < nv; i++) { memcpy(vbuf + i * 6, pos[i], 12); memcpy(vbuf + i * 6 + 3, nrm[i], 12); }
  glUseProgram(jprog); glBindVertexArray(jvao);
  glBindBuffer(GL_ARRAY_BUFFER, jvbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, nv * 6 * sizeof(float), vbuf);
  glUniform3f(uJProj, 2 * R / W, 2 * R / H, 1 - 2 * (footY - hopPx) / H);
  glUniform2f(uJTilt, cT, sT);
  glUniform3f(uJColor, col[0], col[1], col[2]);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS); glColorMask(0, 0, 0, 0);
  glDrawElements(GL_TRIANGLES, ntri * 3, GL_UNSIGNED_SHORT, 0);
  glDepthFunc(GL_LEQUAL); glColorMask(1, 1, 1, 1);
  glDrawElements(GL_TRIANGLES, ntri * 3, GL_UNSIGNED_SHORT, 0);
  glDisable(GL_DEPTH_TEST);

  /* face */
  glUseProgram(qprog); glBindVertexArray(qvao);
  int ex = current_expr();
  Anch eL = anchor(A_EYE_L), eR = anchor(A_EYE_R);
  float roll = atan2f(-(eR.y - eL.y), eR.x - eL.x);
  float fsy = sqrtf(sq), fsx = 1 / sqrtf(sqrtf(sq));
  float lookPx = (cfg.face == FACE_TINY ? 0.09f : 0.06f) * R;
  float dark[3] = {col[0] * 0.55f, col[1] * 0.55f, col[2] * 0.55f};

  // hair curl (boy) / ribbon (girl) on top
  if (cfg.girl) {
    Anch b = anchor(A_BOW);
    quad(11, b.x, b.y - 0.02f * R, 0.30f * R, 0.30f * R, roll + 0.35f, 1.0f, 0.42f, 0.58f, 1, 0, 0, 0, 0);
  } else {
    Anch tp = anchor(A_TOP);
    quad(12, tp.x, tp.y - 0.16f * R, 0.20f * R, 0.24f * R, roll - 0.1f, dark[0], dark[1], dark[2], 0.92f, 0, 0, 0, 0);
  }

  Anch eyes[2] = {eL, eR};
  int face = cfg.face;
  for (int s = 0; s < 2; s++) {
    Anch e = eyes[s];
    if (e.nz < 0.12f) continue;
    float side = s ? 1.f : -1.f;
    float fx = sqrtf(fmaxf(0.1f, 1 - e.nx * e.nx));
    float cx = e.x + lookX * lookPx, cy = e.y + lookY * lookPx * 0.8f;
    { // eyes: tiny dots, or classic
      float k = face == FACE_TINY ? 0.5f : 1.0f;
      float hx = 0.22f * R * fsx * fx * k, hy = 0.22f * R * fsy * k;
      int type = 1; float size = face == FACE_TINY ? 0.95f : (cfg.girl ? 1.08f : 0.94f);
      switch (ex) {
      case EX_NORMAL: case EX_LOOKBACK: type = blinkOn > 0 ? 4 : 1; break;
      case EX_HAPPY: type = 2; break;
      case EX_SMILE: type = 2; hx *= 0.85f; hy *= 0.85f; break;
      case EX_LAUGH: type = 2; hx *= 1.2f; hy *= 1.2f; cy -= 0.02f * R; break;
      case EX_SLEEP: type = 3; break;
      case EX_DIZZY: type = 5; break;
      case EX_SURPRISE: type = 1; size *= 0.8f; break;
      case EX_CURIOUS: type = blinkOn > 0 ? 4 : 1; size *= 1.3f; break;        // big shiny eyes
      case EX_QUESTION: type = 1; if ((qTilt > 0) == (side > 0)) size *= 0.78f; break; // one eye squints a little
      case EX_HURRY: type = 1; size *= 0.9f; break;
      case EX_PULL: type = pullAmt > 0.9f ? 5 : 1; break;
      }
      int lashes = face == FACE_CLASSIC && cfg.girl;
      quad(type, cx, cy, hx, hy, roll, 1, 1, 1, 1, side, (float)lashes, size, 0);
    }
    if (face == FACE_CLASSIC && !cfg.girl) { // boy's eyebrows
      Anch b = anchor(s ? A_BROW_R : A_BROW_L);
      float tilt_ = (ex == EX_DIZZY || ex == EX_PULL || ex == EX_HURRY) ? -0.35f * side
                    : ex == EX_QUESTION ? (side > 0 ? 0.35f : -0.1f)
                    : (ex == EX_SLEEP ? 0.15f * side : 0.12f * side);
      float lift = (ex == EX_CURIOUS || ex == EX_SURPRISE) ? -0.05f * R : 0;
      quad(13, b.x + lookX * lookPx * 0.5f, b.y - 0.02f * R + lift, 0.13f * R * fsx * fx, 0.07f * R, roll + tilt_, 1, 1, 1, 0.85f, 0, 0, 0, 0);
    }
  }
  // blush
  float blushA = (ex == EX_SMILE || ex == EX_LAUGH) ? 0.7f : cfg.girl ? 0.55f : (ex == EX_HAPPY ? 0.35f : 0.0f);
  if (blushA > 0)
    for (int s = 0; s < 2; s++) {
      Anch c = anchor(s ? A_CHEEK_R : A_CHEEK_L);
      if (c.nz < 0.1f) continue;
      quad(9, c.x, c.y, 0.17f * R * sqrtf(fmaxf(0.1f, 1 - c.nx * c.nx)), 0.10f * R, roll, 1.0f, 0.45f, 0.55f, blushA * c.nz, 0,
           0, 0, 0);
    }
  // mouth
  Anch m = anchor(A_MOUTH);
  if (face == FACE_CLASSIC && m.nz > 0.12f) { // the abstract faces have no mouth
    float mk = face == FACE_TINY ? 0.85f : 1.0f, mhx = 0.12f * R * fsx * mk, mhy = 0.12f * R * fsy * mk;
    switch (ex) {
    case EX_NORMAL: quad(6, m.x + lookX * lookPx * 0.6f, m.y, mhx, mhy, roll, 1, 1, 1, 1, 0, 0, 0, 0); break;
    case EX_HAPPY: quad(7, m.x, m.y, mhx * 1.1f, mhy * 1.1f, roll, 1, 1, 1, 1, 0, 0, 0, 0); break;
    case EX_SLEEP: quad(8, m.x, m.y, mhx, mhy, roll, 1, 1, 1, 1, 0.55f + 0.1f * sq, 0, 0, 0); break;
    case EX_DIZZY: quad(8, m.x, m.y, mhx, mhy, roll + 0.3f, 1, 1, 1, 1, 0.7f, 0, 0, 0); break;
    case EX_SURPRISE: quad(8, m.x, m.y, mhx, mhy, roll, 1, 1, 1, 1, 0.95f, 0, 0, 0); break;
    case EX_PULL: quad(8, m.x, m.y, mhx, mhy, roll, 1, 1, 1, 1, 0.6f + 0.3f * fminf(pullAmt, 1), 0, 0, 0); break;
    case EX_LAUGH: quad(7, m.x, m.y, mhx * 1.25f, mhy * 1.25f, roll, 1, 1, 1, 1, 0, 0, 0, 0); break;
    case EX_SMILE: case EX_LOOKBACK: quad(6, m.x, m.y, mhx, mhy, roll, 1, 1, 1, 1, 0, 0, 0, 0); break;
    case EX_CURIOUS: case EX_HURRY: quad(8, m.x, m.y, mhx, mhy, roll, 1, 1, 1, 1, 0.5f, 0, 0, 0); break;
    case EX_QUESTION: quad(8, m.x + 0.03f * R, m.y, mhx, mhy, roll + 0.3f, 1, 1, 1, 1, 0.55f, 0, 0, 0); break;
    }
  }
  // particles
  for (int i = 0; i < MAXP; i++) {
    Part *p = &parts[i];
    if (p->life <= 0) continue;
    float age = p->max - p->life, a = fminf(1, age * 5) * fminf(1, p->life * 2.5f);
    quad(p->type, W * 0.5f + p->x, footY + p->y - hopPx, p->size, p->size, p->rot, 1, 1, 1, a, 0, 0, 0, 0);
  }
}

/* ------------------------------------------------------------------ window */

static void update_input_shape(void) {
  XRectangle rs[40]; int n = 0;
  if (pressed) { rs[0] = (XRectangle){0, 0, (unsigned short)W, (unsigned short)H}; n = 1; }
  else {
    enum { NB = 28 };
    float y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < nv; i++) { if (scr[i][1] < y0) y0 = scr[i][1]; if (scr[i][1] > y1) y1 = scr[i][1]; }
    y0 -= (cfg.girl ? 0.30f : 0.36f) * R; // include bow / hair on top
    float lo[NB], hi[NB], bh = (y1 - y0) / NB + 1e-3f;
    for (int b = 0; b < NB; b++) { lo[b] = 1e9f; hi[b] = -1e9f; }
    for (int i = 0; i < nv; i++) {
      int b = (int)((scr[i][1] - y0) / bh); if (b < 0) b = 0; if (b >= NB) b = NB - 1;
      if (scr[i][0] < lo[b]) lo[b] = scr[i][0];
      if (scr[i][0] > hi[b]) hi[b] = scr[i][0];
    }
    for (int b = 0; b < NB; b++) {
      if (lo[b] > hi[b]) { lo[b] = W * 0.5f - 0.25f * R; hi[b] = W * 0.5f + 0.25f * R; }
      int x = (int)lo[b] - 2, w = (int)(hi[b] - lo[b]) + 4;
      rs[n++] = (XRectangle){(short)x, (short)(y0 + b * bh - 1), (unsigned short)(w > 0 ? w : 1), (unsigned short)(bh + 2)};
    }
  }
  XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, rs, n, ShapeSet, Unsorted);
}

static void apply_size(int first) {
  R = cfg.size;
  W = (int)(R * 8.0f); H = (int)(R * 8.0f);
  footY = H * 0.62f;
  compute_bounds();
  if (!first) { XResizeWindow(dpy, win, W, H); winX = winY = -99999; }
  Fx = clampf(Fx, bx0, bx1); Fy = clampf(Fy, by0, by1);
}

static void create_window(void) {
  int scr_n = DefaultScreen(dpy);
  Window root = RootWindow(dpy, scr_n);
  SW = DisplayWidth(dpy, scr_n); SH = DisplayHeight(dpy, scr_n);
  int attr[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, True, GLX_SAMPLE_BUFFERS, 1,
                GLX_SAMPLES, 4, None};
  XVisualInfo *vi = NULL;
  for (int pass = 0; pass < 2 && !vi; pass++) {
    if (pass == 1) { attr[20] = GLX_SAMPLE_BUFFERS; attr[21] = 0; attr[22] = GLX_SAMPLES; attr[23] = 0; }
    int n = 0; GLXFBConfig *fbs = glXChooseFBConfig(dpy, scr_n, attr, &n);
    for (int i = 0; i < n; i++) {
      XVisualInfo *v = glXGetVisualFromFBConfig(dpy, fbs[i]);
      if (v && v->depth == 32) { fb = fbs[i]; vi = v; break; }
      if (v) XFree(v);
    }
    if (fbs) XFree(fbs);
  }
  if (!vi) { fprintf(stderr, "no 32-bit ARGB GLX visual (is a compositor running?)\n"); exit(1); }

  XSetWindowAttributes swa = {0};
  swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
  swa.border_pixel = 0; swa.background_pixel = 0; swa.override_redirect = True;
  swa.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask;
  win = XCreateWindow(dpy, root, (int)Fx - W / 2, (int)(Fy - footY), W, H, 0, vi->depth, InputOutput, vi->visual,
                      CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect | CWEventMask, &swa);
  XStoreName(dpy, win, "jelly");
  XClassHint ch = {"jelly", "Jelly"}; XSetClassHint(dpy, win, &ch);
  XDefineCursor(dpy, win, XCreateFontCursor(dpy, XC_hand2));

  typedef GLXContext (*CtxFn)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
  CtxFn mk = (CtxFn)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
  int cattr[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3, GLX_CONTEXT_PROFILE_MASK_ARB,
                 GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
  if (mk) ctx = mk(dpy, fb, NULL, True, cattr);
  if (!ctx) ctx = glXCreateNewContext(dpy, fb, GLX_RGBA_TYPE, NULL, True);
  XFree(vi);
  XMapRaised(dpy, win);
  opt_init(dpy, fb);
  glXMakeCurrent(dpy, win, ctx);
  typedef void (*SwapFn)(Display *, GLXDrawable, int);
  SwapFn si = (SwapFn)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
  if (si) si(dpy, win, 1);
}

static void on_sig(int s) { (void)s; running = 0; }

int main(void) {
  srand((unsigned)time(NULL));
  signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
  cfg_load();
  flex = cfg.flex;
  cT = cosf(TILT); sT = sinf(TILT);
  build_mesh(); build_anchors();

  dpy = XOpenDisplay(NULL);
  if (!dpy) { fprintf(stderr, "cannot open X display\n"); return 1; }
  SW = DisplayWidth(dpy, DefaultScreen(dpy)); SH = DisplayHeight(dpy, DefaultScreen(dpy));
  apply_size(1);
  Fx = SW * frand(0.35f, 0.65f); Fy = by1 - frand(0, SH * 0.25f);
  // arrive by dropping in from above
  st = ST_WANDER; hopst = HOP_AIR; hopG = R * 16; hopPx = R * 1.2f; hopV = 0;
  create_window();
  gl_init();
  compute_normals();

  double last = now(), acc = 0, shapeT = 0, raiseT = 0;
  const double DT = 1.0 / 240.0;
  while (running) {
    double frameStart = now();
    while (XPending(dpy)) {
      XEvent e; XNextEvent(dpy, &e);
      if (opt_owns(e.xany.window)) { opt_event(&e); continue; }
      if (e.type == ButtonPress && e.xbutton.button == Button1) {
        pressed = 1; dragging = 0; pressT = frameStart;
        pressX = curX = e.xbutton.x_root; pressY = curY = e.xbutton.y_root;
        pressPick = pick_vertex((float)e.xbutton.x, (float)e.xbutton.y);
        grabOffX = curX - Fx; grabOffY = curY - (Fy - hopPx);
        trkN = 0; track(frameStart, curX, curY);
        update_input_shape();
      } else if (e.type == ButtonPress && e.xbutton.button == Button3) {
        if (opt_is_open()) { opt_close(); cfg_save(); }
        else opt_open(winX + (int)(W * 0.5f + R * 1.6f), winY + (int)(footY - R * 3.0f));
      } else if (e.type == MotionNotify) {
        float nx = e.xmotion.x_root, ny = e.xmotion.y_root;
        if (pressed && !dragging && hypotf(nx - pressX, ny - pressY) > 6) {
          dragging = 1; st = ST_DRAG; exprT = 0; flingMax = 0; Vx = Vy = 0;
          grabbed = pressPick;
          if (grabbed >= 0) { pinchI = grabbed; set_pinch_weights(grabbed, lerpf(0.35f, 0.5f, cfg.pull)); Px = Py = PVx = PVy = 0; }
          lastFx = Fx; lastFy = Fy;
          surf_pulse(-1.3f); // a little squeeze as it's picked up
        } else if (!pressed) {
          // brushing: the cursor pushes the surface in where it passes, and the ripple spreads
          float spd = fminf(hypotf(nx - curX, ny - curY), 40);
          int hv = pick_vertex(nx - winX, ny - winY);
          if (hv >= 0 && spd > 0.5f)
            surf_poke(rdir[hv][0], rdir[hv][1], rdir[hv][2], -fminf(spd * 0.05f, 1.0f) * (0.6f + flex), 0.9f);
          petDist += hypotf(nx - curX, ny - curY);
          if (petDist > R * 7) {
            petDist = 0; squash(-0.12f);
            tickles = tickleWin > 0 ? tickles + 1 : 1; tickleWin = 2.5f;
            if (tickles >= 3) { set_expr(EX_LAUGH, 2.0f); spawn(14, -0.4f * R, -1.5f * R); spawn(14, 0.4f * R, -1.6f * R); }
            else { set_expr(EX_SMILE, 1.6f); spawn(14, frand(-0.4f, 0.4f) * R, -1.5f * R); }
          }
        }
        curX = nx; curY = ny;
        if (pressed) track(frameStart, nx, ny);
      } else if (e.type == ButtonRelease && e.xbutton.button == Button1) {
        if (dragging) {
          // a slime doesn't leave with the hand's full speed: some goes into wobble
          float vx, vy; tracked_velocity(frameStart, &vx, &vy);
          Vx = clampf(vx * 0.6f, -1400, 1400); Vy = clampf(vy * 0.6f, -1400, 1400);
          trailX = Vx; trailY = Vy;
          flingMax = hypotf(Vx, Vy);
          release_grab(0);
          st = ST_FLING; set_expr(EX_SURPRISE, 0.7f);
          surf_pulse(1.2f);
        } else if (frameStart - pressT < 0.4) {
          poke(pressPick);
        }
        pressed = dragging = 0;
      }
    }

    double t = now();
    double fdt = t - last; last = t;
    if (dragging) { track(t, curX, curY); tracked_velocity(t, &dragVX, &dragVY); }
    if (fdt > 0.1) fdt = 0.1;

    // eyes follow the cursor when it's near
    if (!pressed) {
      Window r, c; int rx, ry, wx, wy; unsigned int mk;
      if (XQueryPointer(dpy, DefaultRootWindow(dpy), &r, &c, &rx, &ry, &wx, &wy, &mk)) { curX = rx; curY = ry; }
    }
    float cdx = curX - Fx, cdy = curY - (Fy - R), cd = hypotf(cdx, cdy);
    // eyes follow the mouse anywhere on screen while it's moving; after a few idle seconds it looks around on its own
    static double mouseMovedAt; static float mlx, mly;
    if (hypotf(curX - mlx, curY - mly) > 1) mouseMovedAt = t;
    mlx = curX; mly = curY;
    mouseActive = 0;
    if (st != ST_REST && current_expr() != EX_LOOKBACK && t - mouseMovedAt < 4.0) {
      float d = cd + 1e-3f, m = fminf(1, d / 160);
      lookTX = cdx / d * m; lookTY = cdy / d * m;
      yawCur = 0.75f * tanhf(cdx / 450);    // the whole head turns toward the mouse
      pitchCur = 0.40f * tanhf(-cdy / 500); // and tips up / down to it
      mouseActive = 1;
    }
    else if (st == ST_WANDER && hopst == HOP_AIR) { float d = hypotf(Vx, Vy) + 1e-3f; lookTX = Vx / d; lookTY = Vy / d * 0.5f; }
    else if ((glanceT -= (float)fdt) <= 0) {
      glanceT = frand(1.5f, 4);
      lookTX = frand(-0.8f, 0.8f); lookTY = frand(-0.3f, 0.3f);
      yawCur = frand(-0.25f, 0.25f);
      if (st == ST_REST) { lookTX = lookTY = 0; }
    }
    // curiosity: when you come close it stops, turns to you and stares; linger and it wonders what you want
    {
      static float lcx, lcy;
      float moved = hypotf(curX - lcx, curY - lcy); lcx = curX; lcy = curY;
      int nearby = cd < 300 && !pressed && (st == ST_IDLE || st == ST_STAY || (st == ST_WANDER && hopst != HOP_AIR));
      if (nearby) {
        if (st == ST_WANDER && !hurry) go_idle(2);
        if (st == ST_IDLE && stT < 1.5f) stT = 1.5f;
        stillT = moved < 2 ? stillT + (float)fdt : 0;
        if (stillT > 2.0f && !questioned) { do_question(); questioned = 1; }
        if (exprT <= 0 || expr == EX_CURIOUS) set_expr(EX_CURIOUS, 0.25f);
      } else if (cd > 420) { questioned = 0; stillT = 0; }
    }
    int exNow = current_expr();
    if (exNow == EX_LOOKBACK) { lookTX = lbX; lookTY = -0.2f; }
    if (exNow == EX_QUESTION) tiltT = qTilt;
    else if (exNow == EX_CURIOUS) tiltT = clampf(-cdx / 1500, -0.12f, 0.12f);
    else if (exNow == EX_LAUGH) tiltT = 0.07f * sinf((float)t * 7);
    else if (hurry && st == ST_WANDER) tiltT = clampf(Vx / (R * 20), -0.15f, 0.15f);
    else tiltT = 0;
    if (tickleWin > 0) tickleWin -= (float)fdt;
    if (hurry && st == ST_WANDER && (sweatT -= (float)fdt) <= 0) { sweatT = 0.8f; spawn(19, (rand() % 2 ? 0.55f : -0.55f) * R, -1.45f * R); }

    float lk = 1 - expf(-(float)fdt * 10);
    lookX += (lookTX - lookX) * lk; lookY += (lookTY - lookY) * lk;

    acc += fdt;
    while (acc >= DT) { step((float)DT, t); acc -= DT; }

    // timers
    if (exprT > 0) exprT -= (float)fdt;
    if (pokeWin > 0 && (pokeWin -= (float)fdt) <= 0) pokeCount = 0;
    if (blinkOn > 0) blinkOn -= (float)fdt;
    else if ((blinkT -= (float)fdt) <= 0) { blinkOn = 0.12f; blinkT = frand(2, 5.5f); }
    if (st == ST_REST && restAge > 1.5f && (zT -= (float)fdt) <= 0) { zT = 1.7f; spawn(15, 0.45f * R, -1.35f * R); }
    for (int i = 0; i < MAXP; i++) if (parts[i].life > 0) {
      Part *p = &parts[i];
      p->life -= (float)fdt; p->x += p->vx * (float)fdt; p->y += p->vy * (float)fdt;
      if (p->type == 15) p->vx = 14 * sinf(p->life * 3);
      if (p->type == 19) p->vy += 260 * (float)fdt;             // sweat falls
      if (p->type == 20) p->size *= 1 + (float)fdt * 1.8f;       // dust puffs spread
    }

    compute_normals();

    int nx = (int)lroundf(Fx - W * 0.5f), ny = (int)lroundf(Fy - footY);
    if (nx != winX || ny != winY) { XMoveWindow(dpy, win, nx, ny); winX = nx; winY = ny; }
    if (t - raiseT > 2 && !opt_is_open()) { XRaiseWindow(dpy, win); raiseT = t; }

    static int dbg = -1; static double dbgT;
    if (dbg < 0) dbg = getenv("JELLY_DEBUG") != NULL;
    if (dbg && t - dbgT > 0.2) {
      dbgT = t;
      fprintf(stderr, "st=%d hop=%d F=%.0f,%.0f V=%.0f,%.0f hopPx=%.1f sq=%.2f trail=%.0f,%.0f yaw=%.2f pull=%.2f press=%d grab=%d cur=%.0f,%.0f\n",
              st, hopst, Fx, Fy, Vx, Vy, hopPx, sq, trailX, trailY, yaw, pullAmt, pressed, grabbed, curX, curY);
    }
    render();
    glXSwapBuffers(dpy, win);

    // right-click panel: edits apply live
    float oldSize = cfg.size; int oldGirl = cfg.girl, oldFlavor = cfg.flavor;
    int of = opt_frame(&cfg, fdt);
    static double saveAt = 0;
    if (saveAt > 0 && t > saveAt) { cfg_save(); saveAt = 0; }
    if (of & OPT_CHANGED) {
      saveAt = t + 0.8; // saved shortly after the last change
      flex = cfg.flex;
      if (fabsf(cfg.size - oldSize) > 0.5f) apply_size(0);
      if (cfg.girl != oldGirl || cfg.flavor != oldFlavor) {
        squash(-0.35f); surf_pulse(2.0f); set_expr(EX_HAPPY, 1.5f);
        spawn(14, -0.3f * R, -1.5f * R); spawn(14, 0.3f * R, -1.6f * R);
      }
    }
    if (of & OPT_NAP) { go_rest(frand(40, 90)); opt_close(); cfg_save(); }
    if (of & OPT_CLOSED) cfg_save();
    if (of & OPT_QUIT) { cfg_save(); running = 0; }
    if (t - shapeT > 1.0 / 30) { update_input_shape(); shapeT = t; }
    XFlush(dpy);

    // cap to ~60 fps, and idle lighter while napping
    double target = (st == ST_REST && restAge > 3) ? 1.0 / 30 : 1.0 / 60;
    double spent = now() - frameStart;
    if (spent < target) usleep((useconds_t)((target - spent) * 1e6));
  }
  glXMakeCurrent(dpy, None, NULL);
  glXDestroyContext(dpy, ctx);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  return 0;
}
