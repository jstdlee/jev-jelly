// Chat box: triple-click the jelly and a line to type in fades in beside it, keyboard ready. Enter sends the message
// to the model in the background and the box stays open, so more questions can follow: they're listed while they
// wait and answered in order. Each answer comes back as a game-style dialogue under its question: r to reply, n / p
// to page through the answers, Esc to close. Drag the box from anywhere that isn't a control. One conversation at a
// time.
// No window chrome: a translucent glass backdrop whose edges feather out into the desktop, themed like the options
// panel, with a faint glow in the jelly's color.

#include "ui.h"
#include "jelly.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CWW 540   // canvas; only the glass takes clicks
#define CWH 600
#define OX 24     // glass inside the canvas (room for the feathered edge)
#define OY 24
#define BW (CWW - 2 * OX)
#define PAD 20    // text inset
#define HEADER 16 // the glass's top margin doubles as a drag handle

static Ui ui;
static int created;
static const Theme *styled; // theme the ImGui style was built for
static int mode = CH_HIDDEN, focusInput, wantClose, grabNow;
static float bodyH = 180;
static char input[4000];
static char shown[16384]; // the reply (or error) on display
static char shownQ[1200], shownHow[80]; // ... the question it answers, and how it was answered
static int overItem;   // the pointer is over a control (last frame): a press there isn't a drag
static int userPlaced; // they moved the box: keep it where they put it for the rest of the conversation

/* every answer seen in this conversation, so n / p can page through them */
#define SEEN 64
static struct { char *q, *a; char how[80]; int err; } seen[SEEN];
static int nseen, cur = -1;
static int isError;
static double clk;

int chat_mode(void) { return mode; }
int chat_busy(void) { return mode != CH_HIDDEN && llm_waiting() > 0; }

static void restyle(void) {
  ImGuiStyle *s = igGetStyle();
  theme_style(s, 0.0001f); // the window itself is invisible; the cloud is its background
  s->WindowPadding = (ImVec2_c){0, 0};
  s->ItemSpacing = (ImVec2_c){8, 8};
  s->FrameRounding = 10;
  styled = theme();
}

static void create(void) {
  ui_init(&ui, "jelly chat", CWW, CWH, PW_KEYBOARD, 18.0f);
  restyle();
  created = 1;
}

static void place_near(float ax, float ay, float jr) {
  if (userPlaced) return;
  int sw, sh; plat_screen(&sw, &sh);
  int winX, winY, side = ax + jr + CWW + 30 < sw ? 1 : -1;
  winX = (int)(side > 0 ? ax + jr * 0.9f : ax - jr * 0.9f - CWW);
  winY = (int)(ay - bodyH - 90);
  if (winX < 8) winX = 8;
  if (winX + CWW > sw) winX = sw - CWW - 8;
  if (winY < 8) winY = 8;
  if (winY + CWH > sh) winY = sh - CWH - 8;
  ui_move(&ui, winX, winY);
}

/* Each time the box appears it's raised above everything and asks for the keyboard (so Enter / Esc work straight
   away). */
static void show(void) {
  pw_show(ui.win);
  pw_focus_soon(ui.win, ui.clock);
}
static void hide(void) { pw_hide(ui.win); }

static int autoSend; // debugging aid: send the preset input on the next frame
void chat_debug_send(const char *text) { snprintf(input, sizeof input, "%s", text); autoSend = 1; }

void chat_open(float ax, float ay, float jr) {
  if (!created) create();
  if (!pw_visible(ui.win)) place_near(ax, ay, jr);
  mode = CH_INPUT; focusInput = 1; // even while earlier questions are being answered: this one joins the queue
  show();
  grabNow = 1;
}

int chat_event(const PEvent *e) {
  if (!created || e->win != ui.win) return 0;
  if (ui_drag(&ui, e)) return 1;
  // drag from anywhere on the glass that isn't a control (a text field, a hint, the scroll bar)
  if (e->type == PE_BUTTON && e->button == PB_LEFT && e->down && (!overItem || e->y < OY + HEADER)) {
    ui_drag_start(&ui, e); userPlaced = 1;
    return 1;
  }
  return ui_event(&ui, e);
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

static void show_at(int i) {
  cur = i;
  snprintf(shown, sizeof shown, "%s", seen[i].a); snprintf(shownQ, sizeof shownQ, "%s", seen[i].q);
  snprintf(shownHow, sizeof shownHow, "%s", seen[i].how); isError = seen[i].err;
}
static int take_next(void) { // the next answer, if one is ready
  char q[1200], a[16384], how[80];
  int k = llm_take_reply(q, sizeof q, a, sizeof a, how, sizeof how);
  if (!k) return 0;
  if (nseen == SEEN) { free(seen[0].q); free(seen[0].a); memmove(seen, seen + 1, sizeof seen[0] * (SEEN - 1)); nseen--; }
  seen[nseen].q = strdup(q); seen[nseen].a = strdup(a); seen[nseen].err = k < 0;
  snprintf(seen[nseen].how, sizeof seen[nseen].how, "%s", how);
  show_at(nseen++);
  return k;
}
static void forget_seen(void) {
  for (int i = 0; i < nseen; i++) { free(seen[i].q); free(seen[i].a); }
  nseen = 0; cur = -1;
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
  if (!created || mode == CH_HIDDEN) return CHAT_NONE;
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

  ui_frame_begin(&ui, dt);
  if (styled != theme()) restyle();
  const Theme *t = theme();
  ImGuiIO *io = igGetIO_Nil();

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
    if (nseen > 1 || readyN) { // where we are among this conversation's answers
      char pos[32]; snprintf(pos, sizeof pos, "%d / %d", cur + 1, nseen + readyN);
      igSameLine(full - igCalcTextSize(pos, NULL, false, -1).x - 12, 0); igTextDisabled("%s", pos);
    }
    ImVec2_c ts = igCalcTextSize(shown, NULL, false, full - 12);
    float h = fminf(ts.y + 8, 360);
    igBeginChild_Str("##reply", (ImVec2_c){full, h}, 0, 0);
    igPushTextWrapPos(full - 12);
    if (isError) igTextColored((ImVec4_c){1, 0.45f, 0.42f, 1}, "Oops, I couldn't reach my brain~ (%s)", shown);
    else igTextUnformatted(shown, NULL);
    igPopTextWrapPos();
    igEndChild();
    if (waiting && !readyN) { igTextDisabled(waiting == 1 ? "still answering 1 more~" : "still answering %d more~", waiting); igSameLine(0, 0); }
    // r reply · n next · p previous (the keys, or click the hints)
    int canNext = cur < nseen - 1 || readyN, canPrev = cur > 0;
    igSetCursorPosX(full - (canPrev ? 300 : 238));
    int kr = hint("r", isError ? "retry" : waiting ? "ask more" : "reply");
    igSameLine(0, 2);
    int kn = 0, kp = 0;
    if (canNext) { char nl[32]; snprintf(nl, sizeof nl, "next%s", readyN ? " ●" : ""); kn = hint("n", nl); igSameLine(0, 2); }
    if (canPrev) { kp = hint("p", "prev"); igSameLine(0, 2); }
    if (hint("esc", "close")) esc = 1;
    if (!io->WantTextInput) {
      kr |= igIsKeyPressed_Bool(ImGuiKey_R, false);
      kn |= canNext && igIsKeyPressed_Bool(ImGuiKey_N, false);
      kp |= canPrev && igIsKeyPressed_Bool(ImGuiKey_P, false);
    }
    if (enter) { if (canNext) kn = 1; else kr = 1; } // Enter: the next answer if there is one, else reply
    if (kn) { if (cur < nseen - 1) show_at(cur + 1); else take_next(); }
    else if (kp) show_at(cur - 1);
    else if (kr) { mode = CH_INPUT; focusInput = 1; grabNow = 1; }
    else if (esc) { if (waiting || readyN) mode = CH_WAIT; else wantClose = 1; } // unanswered questions: keep them coming
  }
  ImVec2_c ws = igGetWindowSize();
  overItem = igGetHoveredID() != 0 || igIsAnyItemActive() || igIsAnyItemHovered();
  igEnd();
  igPopStyleVar(1);
  igPopStyleColor(3);
  ui_frame_end(&ui);

  if (grabNow && pw_visible(ui.win)) { pw_focus_soon(ui.win, ui.clock); grabNow = 0; } // opening the box / Reply: the keyboard is yours
  pw_want_text(ui.win, io->WantTextInput || mode == CH_INPUT || mode == CH_REPLY); // keys work while the box is up

  // the cloud grows with its content; only the cloud (and its tail) takes clicks
  float nh = ws.y + 24;
  if (fabsf(nh - bodyH) > 1) {
    bodyH = nh;
    PRect r = {OX, OY, BW, (int)bodyH};
    pw_input_rects(ui.win, &r, 1);
  }
  if (mode == CH_WAIT) hide();
  if (wantClose) {
    wantClose = 0;
    hide();
    mode = CH_HIDDEN; shown[0] = shownQ[0] = shownHow[0] = 0;
    forget_seen(); userPlaced = 0; // a new conversation starts next to the jelly again
    llm_end_session(); // one conversation at a time: closing ends it
    out |= CHAT_CLOSED;
  }
  return out;
}
