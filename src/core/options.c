// Right-click options panel: a borderless transparent surface next to the jelly, drawn with cimgui + the OpenGL3
// backend. Drag either panel by its header to move it. The events / history / services pane opens beside the
// settings.

#include "ui.h"
#include "jelly.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 340        // settings panel width
#define EW 470       // events pane width
#define GAP 10
#define CW (W + GAP + EW) // canvas: both panels; everything outside them is transparent and click-through
#define H 980
#define EH 720       // events pane height
#define HEADER 40    // drag handle height

static Ui ui;
static int created;
static int open_, panelH = 800, wantClose, wantPreview, showWheel, showEvents, showHistory, showServices, shapeDirty = 1;
static const Theme *styled; // the theme the ImGui style was built for
static double deleteArmed; // "Delete history" needs a second click
static double evAt = -1; // when the events list was last copied
#define SIDE (showEvents || showHistory || showServices) // the pane beside the settings is open
static void side_off(void) { showEvents = showHistory = showServices = 0; shapeDirty = 1; }
static char calNote[160];
static double calNoteAt;

#define TH theme()
#define ACCENT RGB(TH->accent, 1)
#define C_TEXT(a) U32(TH->text, a)
#define C_MUTED(a) U32(TH->muted, a)
#define C_ACC(a) U32(TH->accent, a)
static const char *FLEX_NAMES[] = {"Firm", "Bouncy", "Jiggly", "Wobbly", "Gooey"};
static const char *PULL_NAMES[] = {"Pinch", "Small", "Half body", "Big", "Most"};
static const unsigned CAL_COLORS[CAL_MAX_URLS] = {0x2f81f7, 0x3fb950, 0xd29922, 0xf778ba, 0xa371f7,
                                                  0xdb6d28, 0x39c5cf, 0xf85149, 0x8b949e, 0x56d364};

/* "Paste link": the clipboard arrives as a PE_PASTE event with tag 1 (tag 0 is typing into a text field) */
enum { PASTE_CALENDAR = 1 };
static void request_clipboard(void) { pw_request_paste(ui.win, PASTE_CALENDAR); }
static void got_clipboard(const char *data) {
  snprintf(calNote, sizeof calNote, "The clipboard has no calendar link");
  if (data) {
    if (cal_add_url(data)) snprintf(calNote, sizeof calNote, "Added — reading it now…");
    else if (!strncmp(data, "http", 4) || !strncmp(data, "webcal", 6))
      snprintf(calNote, sizeof calNote, "Already added (or %d calendars max)", CAL_MAX_URLS);
  }
  calNoteAt = 0;
}

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
  float h = 36, pad = 4, w = (width - pad * 2) / n;
  ImDrawList_AddRectFilled(dl, p, (ImVec2_c){p.x + width, p.y + h}, U32(TH->frame, TH->frameAlpha), h * 0.42f, 0);
  igPushID_Str(id);
  for (int i = 0; i < n; i++) {
    ImVec2_c a = {p.x + pad + w * i, p.y + pad}, b = {a.x + w, p.y + h - pad};
    igSetCursorScreenPos(a);
    igPushID_Int(i);
    if (igInvisibleButton("##seg", (ImVec2_c){w, h - pad * 2}, 0) && *val != i) { *val = i; changed = 1; }
    int hov = igIsItemHovered(0), act = igIsItemActive();
    if (*val == i || hov || act) th_jelly_item(dl, a.x + 1, a.y, b.x - 1, b.y, *val == i ? TH_BTN_ON : TH_BTN_QUIET);
    igPopID();
    ImVec2_c ts = igCalcTextSize(items[i], NULL, false, -1);
    ImDrawList_AddText_Vec2(dl, (ImVec2_c){a.x + (w - ts.x) / 2, a.y + (h - pad * 2 - ts.y) / 2},
                            *val == i ? (TH->light ? C_TEXT(1) : U32(0xffffff, 1)) : C_MUTED(1), items[i], NULL);
  }
  igPopID();
  igSetCursorScreenPos((ImVec2_c){p.x, p.y + h});
  igDummy((ImVec2_c){width, 0});
  return changed;
}

int opt_is_open(void) { return open_; }

static void create(void) {
  ui_init(&ui, "jelly options", CW, H, PW_KEYBOARD, 17.0f);
  theme_style(igGetStyle(), 0); styled = theme();
  created = 1;
}

static void clamp_to_screen(void) {
  int sw, sh; plat_screen(&sw, &sh);
  int wide = SIDE ? CW : W;
  if (ui.x + wide > sw) ui.x = sw - wide - 8;
  if (ui.y + panelH > sh) ui.y = sh - panelH - 8;
  if (ui.x < 8) ui.x = 8;
  if (ui.y < 8) ui.y = 8;
}

void opt_open(int x, int y) {
  if (!created) create();
  ui.x = x; ui.y = y;
  clamp_to_screen();
  ui_move(&ui, ui.x, ui.y);
  pw_show(ui.win); // above everything (the window manager forgets that when a window is hidden: set every time)
  pw_raise(ui.win);
  pw_focus_soon(ui.win, ui.clock); // activate it, so it comes up above whatever window has the focus
  open_ = 1; wantClose = 0; shapeDirty = 1;
}

void opt_show_events(int on) { side_off(); showEvents = on; evAt = -1; }

void opt_close(void) {
  if (!open_) return;
  pw_hide(ui.win);
  open_ = 0; ui.dragging = 0;
  llm_cfg_save();
}

/* over a header (a drag handle), away from its close button? */
static int on_header(int x, int y) {
  if (y > HEADER) return 0;
  if (x < W - 84) return 1; // (leaves the Test all icon and × clickable)
  return SIDE && x > W + GAP && x < CW - 48;
}

int opt_event(const PEvent *e) {
  if (!open_ || e->win != ui.win) return 0;
  if (e->type == PE_PASTE && e->tag == PASTE_CALENDAR) { got_clipboard(e->paste); return 1; }
  if (ui_drag(&ui, e)) return 1;
  if (e->type == PE_BUTTON && e->button == PB_LEFT && e->down && on_header((int)e->x, (int)e->y)) { ui_drag_start(&ui, e); return 1; }
  return ui_event(&ui, e);
}

/* debugging aid: JELLY_OPT_TABS="LLM,jev" opens those tabs for the first frames (screenshots without input) */
static int dbgFrames;
static int tabflag(const char *name) {
  const char *want = getenv("JELLY_OPT_TABS");
  if (!want || dbgFrames > 20) return 0;
  char list[200]; snprintf(list, sizeof list, ",%s,", want);
  char key[64]; snprintf(key, sizeof key, ",%s,", name);
  return strstr(list, key) ? ImGuiTabItemFlags_SetSelected : 0;
}

static int flex_index(float f) { int i = (int)lroundf(f * 4); return i < 0 ? 0 : i > 4 ? 4 : i; }

/* header icons (× and the Test-all check) share one look: same size, centered on the header line, a themed hover
   circle. kind 0 = ×, 1 = check; `col` (0 = none) colors the check when it has a status to show. */
static int icon_button(const char *id, int kind, unsigned col, float pulse) {
  ImVec2_c p = igGetCursorScreenPos();
  float h = igGetTextLineHeight() + 4;
  int hit = igInvisibleButton(id, (ImVec2_c){26, h}, 0), hov = igIsItemHovered(0);
  ImDrawList *dl = igGetWindowDrawList();
  float cx = p.x + 13, cy = p.y + h / 2;
  unsigned c = col ? col : hov ? TH->text : TH->muted;
  if (hov) ImDrawList_AddCircleFilled(dl, (ImVec2_c){cx, cy}, 12, U32(TH->text, 0.10f), 24);
  if (kind == 0) {
    ImDrawList_AddLine(dl, (ImVec2_c){cx - 4.5f, cy - 4.5f}, (ImVec2_c){cx + 4.5f, cy + 4.5f}, U32(c, 1), 1.8f);
    ImDrawList_AddLine(dl, (ImVec2_c){cx - 4.5f, cy + 4.5f}, (ImVec2_c){cx + 4.5f, cy - 4.5f}, U32(c, 1), 1.8f);
  } else {
    float a = pulse > 0 ? 0.45f + 0.55f * pulse : 1;
    if (col) ImDrawList_AddCircleFilled(dl, (ImVec2_c){cx, cy}, 9, U32(col, 0.18f * a), 20);
    ImDrawList_AddCircle(dl, (ImVec2_c){cx, cy}, 8.5f, U32(c, a), 20, 1.6f);
    ImDrawList_PathClear(dl); // a tick
    ImDrawList_PathLineTo(dl, (ImVec2_c){cx - 4, cy + 0.5f});
    ImDrawList_PathLineTo(dl, (ImVec2_c){cx - 1, cy + 3.5f});
    ImDrawList_PathLineTo(dl, (ImVec2_c){cx + 4.5f, cy - 3});
    ImDrawList_PathStroke(dl, U32(c, a), 1.8f, 0);
  }
  return hit;
}
static int close_button(float right) { // the × at the right end of a header
  igSameLine(right, 0);
  return icon_button("##close", 0, 0, 0);
}

/* ------------------------------------------------------------------ events pane */

static CalEvent evbuf[800];
static int nevbuf;
static char openKey[200]; // the event whose description is unfolded

static void events_pane(double now) {
  if (now - evAt > 2 || evAt < 0) { nevbuf = cal_events(evbuf, 800); evAt = now; }
  igSetNextWindowPos((ImVec2_c){W + GAP, 0}, ImGuiCond_Always, (ImVec2_c){0, 0});
  igSetNextWindowSize((ImVec2_c){EW, EH}, ImGuiCond_Always);
  igBegin("##events", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  float full = EW - 32;
  igText("Upcoming");
  igSameLine(0, 8);
  igTextDisabled("next 3 months");
  if (close_button(full - 14)) side_off();
  igSeparator();

  igBeginChild_Str("##list", (ImVec2_c){0, 0}, 0, 0);
  time_t t = time(NULL);
  struct tm today; cal_localtime(t, &today);
  int lastDay = -1, lastYear = -1, shown = 0, lastAllday = -1;
  time_t lastStart = 0;
  ImFont *font = igGetFont();
  float fs = igGetFontSize();
  for (int i = 0; i < nevbuf; i++) {
    CalEvent *e = &evbuf[i];
    if (e->end <= t && !(e->end == e->start && e->start > t - 3600)) continue; // already over
    struct tm s; cal_localtime(e->start > t || !e->allday ? e->start : t, &s);
    if (s.tm_yday != lastDay || s.tm_year != lastYear) { // date header
      lastDay = s.tm_yday; lastYear = s.tm_year;
      char d[64]; strftime(d, sizeof d, "%a, %d %b", &s);
      long days = (long)((s.tm_year - today.tm_year) * 366 + s.tm_yday - today.tm_yday);
      igDummy((ImVec2_c){0, shown ? 6 : 0});
      igPushFont(NULL, 14.5f);
      if (days == 0) igTextColored(ACCENT, "TODAY  ·  %s", d);
      else if (days == 1) igTextColored(RGB(TH->text, 1), "TOMORROW  ·  %s", d);
      else igTextColored(RGB(TH->muted, 1), "%s", d);
      igPopFont();
    }
    // events starting together share one row: time and month once, titles stacked under each other
    int same = shown && e->start == lastStart && e->allday == lastAllday;
    lastStart = e->start; lastAllday = e->allday;
    if (same) igSetCursorPosY(igGetCursorPosY() - igGetStyle()->ItemSpacing.y);
    shown++;
    char key[200]; snprintf(key, sizeof key, "%.150s@%ld", e->uid, (long)e->start);
    int open = !strcmp(key, openKey);

    // row: time | colored bar | title + place, all drawn by hand over one selectable
    char when[32];
    if (e->allday) snprintf(when, sizeof when, "All day");
    else strftime(when, sizeof when, "%H:%M", &s);
    float tx = 70, tw = full - tx - 96; // right side: month label, then the +/− for descriptions
    ImVec2_c ts = ImFont_CalcTextSizeA(font, fs, 1e9f, tw, e->title, NULL, NULL);
    float rowH = ts.y + (*e->where ? fs * 0.95f : 0) + 10;
    ImVec2_c p = igGetCursorScreenPos();
    igPushID_Int(i);
    if (igSelectable_Bool("##row", open, 0, (ImVec2_c){0, rowH}))
      snprintf(openKey, sizeof openKey, "%s", open ? "" : key);
    igPopID();
    ImDrawList *dl = igGetWindowDrawList();
    unsigned col = CAL_COLORS[(unsigned)e->cal % CAL_MAX_URLS];
    if (!same) ImDrawList_AddText_FontPtr(dl, font, fs * 0.92f, (ImVec2_c){p.x + 4, p.y + 5}, C_MUTED(1), when, NULL, 0, NULL);
    ImDrawList_AddRectFilled(dl, (ImVec2_c){p.x + tx - 10, p.y + 5}, (ImVec2_c){p.x + tx - 7, p.y + rowH - 5}, U32(col, 1), 2, 0);
    ImDrawList_AddText_FontPtr(dl, font, fs, (ImVec2_c){p.x + tx, p.y + 4}, C_TEXT(1), e->title, NULL, tw, NULL);
    if (*e->where)
      ImDrawList_AddText_FontPtr(dl, font, fs * 0.85f, (ImVec2_c){p.x + tx, p.y + 4 + ts.y + 1}, C_MUTED(1), e->where, NULL, tw, NULL);
    char mon[16], mname[8]; strftime(mname, sizeof mname, "%b", &s); // month label, e.g. "Oct 2" (%e isn't portable)
    snprintf(mon, sizeof mon, "%s %d", mname, s.tm_mday);
    ImVec2_c ms = ImFont_CalcTextSizeA(font, fs * 0.85f, 1e9f, 0, mon, NULL, NULL);
    if (!same) ImDrawList_AddText_FontPtr(dl, font, fs * 0.85f, (ImVec2_c){p.x + full - 34 - ms.x, p.y + 6}, C_MUTED(1), mon, NULL, 0, NULL);
    if (*e->desc) ImDrawList_AddText_FontPtr(dl, font, fs, (ImVec2_c){p.x + full - 24, p.y + 4}, C_MUTED(1), open ? "−" : "+", NULL, 0, NULL);
    if (open) {
      igIndent(tx);
      igPushTextWrapPos(full - 8);
      if (*e->desc) igTextColored(RGB(TH->text, 0.85f), "%s", e->desc);
      else igTextDisabled("No description");
      char range[96], a[16], b[16];
      struct tm en; cal_localtime(e->end, &en);
      strftime(a, sizeof a, "%H:%M", &s); strftime(b, sizeof b, "%H:%M", &en);
      if (!e->allday) { snprintf(range, sizeof range, "%s – %s  ·  %ld min", a, b, (long)(e->end - e->start) / 60); igTextDisabled("%s", range); }
      igPopTextWrapPos();
      igUnindent(tx);
      igDummy((ImVec2_c){0, 4});
    }
  }
  if (!shown) {
    igDummy((ImVec2_c){0, 20});
    igTextDisabled(cal_count() ? "Nothing on your calendars in the next 3 months." : "No calendars yet — use Paste link.");
  }
  igEndChild();
  igEnd();
}

/* ------------------------------------------------------------------ chat history pane */

static LlmHist hist[400];
static int nhist;
static double histAt = -1;

static void history_pane(double now) {
  if (now - histAt > 3 || histAt < 0) { nhist = llm_history(hist, 400); histAt = now; }
  igSetNextWindowPos((ImVec2_c){W + GAP, 0}, ImGuiCond_Always, (ImVec2_c){0, 0});
  igSetNextWindowSize((ImVec2_c){EW, EH}, ImGuiCond_Always);
  igBegin("##history", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  float full = EW - 32;
  igText("Chat history");
  igSameLine(0, 8);
  igTextDisabled("%d messages%s", nhist, llm_cfg()->saveHistory ? "" : " · saving is off");
  if (close_button(full - 14)) side_off();
  igSeparator();
  igBeginChild_Str("##hist", (ImVec2_c){0, 0}, 0, 0);
  long lastSession = -1;
  for (int i = nhist - 1; i >= 0; i--) { // newest conversation first, each read top to bottom
    if (hist[i].session == lastSession) continue;
    lastSession = hist[i].session;
    int first = i; while (first > 0 && hist[first - 1].session == lastSession) first--;
    time_t st = (time_t)(hist[first].session ? hist[first].session : hist[first].t);
    struct tm lt; cal_localtime(st, &lt);
    char d[64]; strftime(d, sizeof d, "%a, %d %b %Y  %H:%M", &lt);
    igDummy((ImVec2_c){0, 4});
    igPushFont(NULL, 14.5f); igTextColored(RGB(TH->muted, 1), "%s", d); igPopFont();
    for (int j = first; j <= i; j++) {
      igTextColored(hist[j].user ? RGB(TH->muted, 1) : ACCENT, "%s", hist[j].user ? "You" : "Jelly");
      igSameLine(56, 0);
      igPushTextWrapPos(full - 8);
      igTextUnformatted(hist[j].text, NULL);
      igPopTextWrapPos();
    }
    igSeparator();
  }
  if (!nhist) { igDummy((ImVec2_c){0, 20}); igTextDisabled("No saved chats."); }
  igEndChild();
  igEnd();
}

static void status_dot(int state, double t) {
  ImDrawList *dl = igGetWindowDrawList();
  ImVec2_c p = igGetCursorScreenPos();
  unsigned c = state == TEST_OK ? TH->accent : state == TEST_FAIL ? TH->danger : state == TEST_RUNNING ? TH->warn : TH->muted;
  float a = state == TEST_RUNNING ? 0.55f + 0.45f * sinf((float)t * 8) : 1;
  ImDrawList_AddCircleFilled(dl, (ImVec2_c){p.x + 6, p.y + 10}, 5.5f, U32(c, a), 16);
  igDummy((ImVec2_c){16, 0});
  igSameLine(0, 4);
}

/* ------------------------------------------------------------------ services pane ("Test all") */

enum { SV_GRAY, SV_YELLOW, SV_GREEN };
static void service_row(const char *name, const char *usedFor, int level, int busy, const char *detail, double t) {
  float full = EW - 32;
  ImDrawList *dl = igGetWindowDrawList();
  ImVec2_c p = igGetCursorScreenPos();
  unsigned col = level == SV_GREEN ? 0x3fb950 : level == SV_YELLOW ? 0xd29922 : 0x8b949e;
  float a = busy ? 0.45f + 0.55f * (0.5f + 0.5f * sinf((float)t * 8)) : 1;
  ImDrawList_AddCircleFilled(dl, (ImVec2_c){p.x + 8, p.y + 11}, 6.5f, U32(col, a), 20);
  if (level == SV_GREEN && !busy) ImDrawList_AddCircle(dl, (ImVec2_c){p.x + 8, p.y + 11}, 9.5f, U32(col, 0.35f), 20, 1.5f);
  igSetCursorScreenPos((ImVec2_c){p.x + 24, p.y});
  igText("%s", name);
  igSameLine(0, 8);
  igTextDisabled("%s", usedFor);
  igSetCursorScreenPos((ImVec2_c){p.x + 24, igGetCursorScreenPos().y});
  igPushTextWrapPos(full);
  igPushFont(NULL, 14.5f);
  if (level == SV_YELLOW && !busy) igTextColored(RGB(TH->warn, 1), "%s", detail); else igTextDisabled("%s", detail);
  igPopFont();
  igPopTextWrapPos();
  igDummy((ImVec2_c){0, 6});
}
static int level_of(int testState) { return testState == TEST_OK ? SV_GREEN : testState == TEST_NONE ? SV_GRAY : SV_YELLOW; }

/* all the services that are switched on: TEST_RUNNING while any test runs, TEST_FAIL if any failed, TEST_OK if
   they all passed, TEST_NONE before the first test */
static int services_overall(void) {
  LlmCfg *lc = llm_cfg();
  char m[600];
  int st[5], n = 0, any = 0, fail = 0, run = 0;
  if (*lc->base) st[n++] = llm_test_state(m, sizeof m);
  if (lc->actionOn) st[n++] = llm_router_state(m, sizeof m);
  if (lc->routerOn) st[n++] = llm_jev_state(m, sizeof m);
  if (lc->wsProvider != WS_NONE) st[n++] = llm_search_state(m, sizeof m);
  if (llm_agent_available()) st[n++] = llm_agent_state(m, sizeof m);
  for (int i = 0; i < n; i++) { run |= st[i] == TEST_RUNNING; fail |= st[i] == TEST_FAIL; any |= st[i] != TEST_NONE; }
  return run ? TEST_RUNNING : fail ? TEST_FAIL : any ? TEST_OK : TEST_NONE;
}

static void test_all(void) {
  LlmCfg *lc = llm_cfg();
  llm_cfg_save();
  if (*lc->base) llm_test();
  llm_router_test();
  if (lc->routerOn) llm_jev_test();
  if (lc->wsProvider != WS_NONE && *lc->wsKey) llm_search_test();
  if (llm_agent_available()) llm_agent_test();
  if (cal_count()) cal_refresh();
}

static void services_pane(double now) {
  igSetNextWindowPos((ImVec2_c){W + GAP, 0}, ImGuiCond_Always, (ImVec2_c){0, 0});
  igSetNextWindowSize((ImVec2_c){EW, EH}, ImGuiCond_Always);
  igBegin("##services", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  float full = EW - 32;
  igText("Services");
  igSameLine(0, 8);
  igTextDisabled("what the settings depend on");
  if (close_button(full - 14)) side_off();
  igSeparator();
  { // legend
    ImDrawList *dl = igGetWindowDrawList();
    const char *names[] = {"working", "needs attention", "off / not installed"};
    unsigned cols[] = {0x3fb950, 0xd29922, 0x8b949e};
    for (int i = 0; i < 3; i++) {
      ImVec2_c p = igGetCursorScreenPos();
      ImDrawList_AddCircleFilled(dl, (ImVec2_c){p.x + 5, p.y + 9}, 4.5f, U32(cols[i], 1), 12);
      igSetCursorScreenPos((ImVec2_c){p.x + 14, p.y});
      igPushFont(NULL, 14.5f); igTextDisabled("%s", names[i]); igPopFont();
      if (i < 2) igSameLine(0, 14);
    }
  }
  igDummy((ImVec2_c){0, 4});
  igBeginChild_Str("##svc", (ImVec2_c){0, 0}, 0, 0);
  LlmCfg *lc = llm_cfg();
  char m[600]; int st;

  label("CHAT");
  st = llm_test_state(m, sizeof m);
  service_row("Chat model", lc->model, *lc->base ? level_of(st) : SV_GRAY, st == TEST_RUNNING, *lc->base ? m : "No endpoint set (LLM > Model)", now);
  st = llm_router_state(m, sizeof m);
  { char first[600]; snprintf(first, sizeof first, "%s", st == TEST_NONE && !*m ? "Not tested yet" : m);
    char *nl = strchr(first, '\n'); if (nl) *nl = 0; // just the first result line
    service_row("Router model", "picks the route", !lc->actionOn ? SV_GRAY : level_of(st), st == TEST_RUNNING,
                !lc->actionOn ? "Off: cues, jev and message length route alone" : first, now); }
  st = llm_jev_state(m, sizeof m);
  { char first[600]; snprintf(first, sizeof first, "%s", m);
    char *nl = strchr(first, '\n'); if (nl) *nl = 0;
    service_row("jev", "SystemOne · router fallback", !lc->routerOn ? SV_GRAY : level_of(st), st == TEST_RUNNING, first, now); }
  st = llm_search_state(m, sizeof m);
  service_row("Web search API", lc->wsProvider == WS_BRAVE ? "Brave" : lc->wsProvider == WS_EXA ? "Exa" : lc->wsProvider == WS_TAVILY ? "Tavily" : "",
              level_of(st), st == TEST_RUNNING, m, now);
  st = llm_agent_state(m, sizeof m);
  service_row("oh-my-pi agent", "omp · research without an API", level_of(st), st == TEST_RUNNING, m, now);

  label("TOOLS");
  int curl = llm_curl_available();
  service_row("curl", "all network requests", curl ? SV_GREEN : SV_GRAY, 0, curl ? "Installed" : "Not installed: chat, routing and calendars can't connect", now);
  char fp[512]; int cjk = plat_font(1, fp, sizeof fp);
  const char *fn = strrchr(fp, '/') ? strrchr(fp, '/') + 1 : strrchr(fp, '\\') ? strrchr(fp, '\\') + 1 : fp;
  service_row("CJK font", "Chinese / Japanese / Korean text", cjk ? SV_GREEN : SV_GRAY, 0,
              cjk ? fn : "Not installed: CJK text shows as boxes (fonts-noto-cjk)", now);

  label("CALENDAR");
  int nc = cal_count(), bad = 0, off = 0, pend = 0, events = 0;
  for (int i = 0; i < nc; i++) {
    CalInfo ci; if (!cal_info(i, &ci)) break;
    if (!ci.enabled) off++; else if (!ci.fetched) pend++; else if (!ci.ok) bad++; else events += ci.count;
  }
  char cm[200];
  if (!nc) snprintf(cm, sizeof cm, "No calendars yet (Calendar > Paste link)");
  else snprintf(cm, sizeof cm, "%d calendar%s · %d events%s%s", nc, nc == 1 ? "" : "s", events, bad ? " · some can't be read" : "", off ? " · some switched off" : "");
  service_row("iCal links", "reminders", !nc ? SV_GRAY : bad ? SV_YELLOW : SV_GREEN, pend > 0, cm, now);
  igEndChild();
  igEnd();
}

/* ------------------------------------------------------------------ settings panel */

int opt_frame(Cfg *c, double dt) {
  if (!open_) return OPT_NONE;
  int out = 0;
  ui_frame_begin(&ui, dt);
  if (styled != theme()) { theme_style(igGetStyle(), 0); styled = theme(); }
  ImGuiIO *io = igGetIO_Nil();

  igSetNextWindowPos((ImVec2_c){0, 0}, ImGuiCond_Always, (ImVec2_c){0, 0});
  igSetNextWindowSize((ImVec2_c){(float)W, 0}, ImGuiCond_Always);
  igBegin("##jelly", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings);
  float full = (float)W - 16 * 2;
  // header (also the drag handle)
  ImDrawList *dl = igGetWindowDrawList();
  ImVec2_c hp = igGetCursorScreenPos();
  ImDrawList_AddCircleFilled(dl, (ImVec2_c){hp.x + 9, hp.y + 11}, 8, U32f(c->col[0], c->col[1], c->col[2]), 24);
  igSetCursorScreenPos((ImVec2_c){hp.x + 24, hp.y});
  igText("Jelly friend");
  igSameLine(0, 8);
  igTextDisabled("drag here to move");
  { // "Test all": green = everything checked works, yellow = something needs attention, pulsing = testing
    igSameLine(full - 40, 0);
    unsigned col = 0; float pulse = 0;
    int ov = services_overall();
    if (ov == TEST_RUNNING) { col = 0xd29922; pulse = 0.5f + 0.5f * sinf((float)igGetTime() * 8); }
    else if (ov == TEST_OK) col = 0x3fb950;
    else if (ov == TEST_FAIL) col = 0xd29922;
    if (icon_button("##testall", 1, col, pulse)) {
      int on = !showServices; side_off(); showServices = on;
      if (on) test_all();
    }
    if (igIsItemHovered(0))
      igSetTooltip(ov == TEST_OK ? "Test all: everything works" : ov == TEST_FAIL ? "Test all: something needs attention (click for details)"
                   : ov == TEST_RUNNING ? "Testing…" : "Test all: check every service and tool the settings depend on");
  }
  if (close_button(full - 12)) wantClose = 1;
  igSeparator();

  static double clock_;
  const char *dbgSv = getenv("JELLY_OPT_SERVICES");
  if (dbgFrames == 3 && getenv("JELLY_OPT_TABS")) side_off();
  if (dbgFrames++ == 3 && dbgSv && *dbgSv) { side_off(); showServices = 1; test_all(); }
  clock_ += dt;
  if (igBeginTabBar("##tabs", 0)) {
  if (igBeginTabItem("Jelly", NULL, tabflag("Jelly"))) {
  label("APP THEME");
  static const char *TN[] = {"White", "Dark", "Tokyo Night"};
  if (segmented("theme", TN, 3, &c->theme, full)) { theme_select(c->theme); out |= OPT_CHANGED; }

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
      if (sel) ImDrawList_AddCircle(dl, ctr, d / 2, C_ACC(1), 32, 2);
      else if (hov) ImDrawList_AddCircle(dl, ctr, d / 2, C_MUTED(0.6f), 32, 1.5f);
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
  label("OPACITY");
  igSetNextItemWidth(full);
  float pct = c->opacity * 100;
  if (igSliderFloat("##opacity", &pct, 20, 100, "%.0f%%", 0)) { c->opacity = pct / 100; out |= OPT_CHANGED; }

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
  igEndTabItem();
  }

  if (igBeginTabItem("Calendar", NULL, tabflag("Calendar"))) {
  label("CALENDARS");
  {
    int nc = cal_count();
    for (int i = 0; i < nc; i++) {
      CalInfo ci;
      if (!cal_info(i, &ci)) break;
      igPushID_Int(100 + i);
      bool on = ci.enabled;
      ImVec2_c p = igGetCursorScreenPos();
      ImDrawList_AddCircleFilled(dl, (ImVec2_c){p.x + 6, p.y + 11}, 5, U32(CAL_COLORS[i], ci.enabled ? 1 : 0.35f), 16);
      igSetCursorScreenPos((ImVec2_c){p.x + 16, p.y});
      igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding, (ImVec2_c){3, 3});
      if (igCheckbox("##on", &on)) cal_set_enabled(i, on);
      igPopStyleVar(1);
      if (igIsItemHovered(0)) igSetTooltip(on ? "Switch this calendar off" : "Switch this calendar on");
      igSameLine(0, 6);
      char name[48]; snprintf(name, sizeof name, "%.44s", ci.label);
      if (ci.enabled) igText("%s", name); else igTextDisabled("%s", name);
      igSameLine(full - 58, 0);
      if (!ci.enabled) igTextDisabled("off");
      else if (!ci.fetched) igTextDisabled("…");
      else if (!ci.ok) igTextColored(RGB(TH->danger, 1), "error");
      else igTextDisabled("%d", ci.count);
      igSameLine(full - 10, 0);
      igPushStyleVar_Float(ImGuiStyleVar_FrameBorderSize, 0);
      igPushStyleColor_Vec4(ImGuiCol_Button, RGB(0x000000, 0));
      if (igSmallButton("x")) cal_remove_url(i);
      igPopStyleColor(1); igPopStyleVar(1);
      if (igIsItemHovered(0)) igSetTooltip("Remove this calendar");
      igPopID();
    }
    float third = (full - 16) / 3;
    if (th_button("Paste link", third, 32, TH_BTN)) { request_clipboard(); snprintf(calNote, sizeof calNote, "Reading the clipboard…"); }
    if (igIsItemHovered(0))
      igSetTooltip("Copy a calendar's secret iCal link, then click here. Add as many as you like.\n"
                   "Google Calendar: Settings > your calendar >\nIntegrate calendar > Secret address in iCal format");
    igSameLine(0, 8);
    if (th_button("Events", third, 32, showEvents ? TH_BTN_ON : TH_BTN)) { int on = !showEvents; side_off(); showEvents = on; evAt = -1; }
    if (igIsItemHovered(0)) igSetTooltip("List everything in the next 3 months");
    igSameLine(0, 8);
    if (th_button("Preview", third, 32, TH_BTN)) wantPreview = 1;
    if (igIsItemHovered(0)) igSetTooltip("Show the next event beside the jelly now");
    char st[160]; cal_status(st, sizeof st);
    igPushTextWrapPos(full + 16);
    igTextDisabled("%s", *calNote ? calNote : st);
    igPopTextWrapPos();
    if (*calNote && (calNoteAt += dt) > 4) calNote[0] = 0;
  }
  igEndTabItem();
  }

  if (igBeginTabItem("Chat", NULL, tabflag("Chat"))) {
    LlmCfg *lc = llm_cfg();
    int dirty = 0;
    igPushTextWrapPos(full + 16);
    igTextDisabled("Triple-click the jelly to chat. You can ask more while it's answering: questions queue up and each "
                   "answer comes back under its question.");
    int waiting = llm_waiting();
    if (waiting) igTextColored(ACCENT, waiting == 1 ? "Answering 1 question…" : "Answering %d questions…", waiting);
    igPopTextWrapPos();
    label("MEMORY");
    igSetNextItemWidth(full - 92);
    if (igSliderInt("##mem", &lc->memoryMins, 0, 240, lc->memoryMins ? "remember %d min" : "until the box closes", 0)) dirty = 1;
    if (igIsItemHovered(0)) igSetTooltip("How long Jelly remembers the conversation after the last message.\nIt carries on after a restart too (from the saved history).");
    igSameLine(0, 8);
    if (th_button("Forget##mem", 84, 0, TH_BTN)) llm_end_session();
    if (igIsItemHovered(0)) igSetTooltip("Start a fresh conversation now");
    label("HISTORY");
    bool save = lc->saveHistory;
    if (igCheckbox("Save chat history", &save)) { lc->saveHistory = save; dirty = 1; }
    if (igIsItemHovered(0)) igSetTooltip("Off: nothing you say is written to disk");
    float half2 = (full - 8) / 2;
    if (th_button("History", half2, 32, showHistory ? TH_BTN_ON : TH_BTN)) { int on = !showHistory; side_off(); showHistory = on; histAt = -1; }
    igSameLine(0, 8);
    int armed = clock_ - deleteArmed < 3;
    if (th_button(armed ? "Sure?##del" : "Delete all##del", half2, 32, TH_BTN_DANGER)) {
      if (armed) { llm_history_delete(); histAt = -1; deleteArmed = 0; } else deleteArmed = clock_;
    }
    if (igIsItemHovered(0)) igSetTooltip("Delete all saved chat history (click twice)");
    igPushTextWrapPos(full + 16);
    igTextDisabled("Model, prompts, routing and search are in the LLM tab.");
    igPopTextWrapPos();
    if (dirty) llm_cfg_save();
    igEndTabItem();
  }

  if (igBeginTabItem("LLM", NULL, tabflag("LLM"))) {
    LlmCfg *lc = llm_cfg();
    int dirty = 0;
    if (igBeginTabBar("##llm", 0)) {
      // --- the chat model: where it is, which one, how it samples
      if (igBeginTabItem("Model", NULL, tabflag("Model"))) {
        if (igCollapsingHeader_TreeNodeFlags("Endpoint", ImGuiTreeNodeFlags_DefaultOpen)) {
          char tm[200]; int ts = llm_test_state(tm, sizeof tm);
          status_dot(ts, clock_);
          igPushTextWrapPos(full + 16);
          if (ts == TEST_FAIL) igTextColored(RGB(TH->danger, 1), "%s", tm); else igTextDisabled("%s", tm);
          igPopTextWrapPos();
          label("URL  (OPENAI-COMPATIBLE)");
          igSetNextItemWidth(full);
          dirty |= igInputTextWithHint("##base", "http://127.0.0.1:8888/v1", lc->base, sizeof lc->base, 0, NULL, NULL);
          label("MODEL");
          static char mlist[64][128]; static int nm, fetched; static double fetchAt;
          if (!fetched || clock_ - fetchAt > 2) { nm = llm_models(mlist, 64); fetchAt = clock_; }
          if (!fetched) { llm_fetch_models(); fetched = 1; } // first look at the tab: ask the server what it has
          igSetNextItemWidth(full - 72);
          if (nm) {
            if (igBeginCombo("##models", *lc->model ? lc->model : "choose a model", 0)) {
              for (int i = 0; i < nm; i++) {
                bool sel = !strcmp(mlist[i], lc->model);
                if (igSelectable_Bool(mlist[i], sel, 0, (ImVec2_c){0, 0})) { snprintf(lc->model, sizeof lc->model, "%.127s", mlist[i]); dirty = 1; }
              }
              igEndCombo();
            }
          } else dirty |= igInputTextWithHint("##model", "model name (list unavailable)", lc->model, sizeof lc->model, 0, NULL, NULL);
          igSameLine(0, 6);
          if (th_button("Reload", 66, 0, TH_BTN)) llm_fetch_models();
          if (igIsItemHovered(0)) igSetTooltip("Reload the model list from %s/models", lc->base);
          label("API KEY");
          igSetNextItemWidth(full);
          dirty |= igInputTextWithHint("##key", "none needed for local servers", lc->key, sizeof lc->key, ImGuiInputTextFlags_Password, NULL, NULL);
          igDummy((ImVec2_c){0, 2});
          if (th_button("Test", full, 30, TH_BTN)) { llm_cfg_save(); llm_test(); }
          if (igIsItemHovered(0)) igSetTooltip("Send a tiny request to check the endpoint, model and key");
        }
        if (igCollapsingHeader_TreeNodeFlags("Sampling", ImGuiTreeNodeFlags_DefaultOpen)) {
          label("TEMPERATURE");
          igSetNextItemWidth(full);
          dirty |= igSliderFloat("##temp", &lc->temperature, 0, 1.5f, "%.2f", 0);
          igPushTextWrapPos(full + 16);
          igTextDisabled("Reply length and thinking are set per route, in Prompts.");
          igPopTextWrapPos();
        }
        igEndTabItem();
      }

      // --- the three routes, each with its own prompt
      if (igBeginTabItem("Prompts", NULL, tabflag("Prompts"))) {
        static char undo[sizeof lc->route[0].system]; static int hasUndo = -1; static char optNote[200];
        char better[sizeof lc->route[0].system]; int op = llm_optimize_poll(better, sizeof better);
        static int optFor = -1;
        if (op == 1 && *better && optFor >= 0) {
          snprintf(undo, sizeof undo, "%s", lc->route[optFor].system); hasUndo = optFor;
          snprintf(lc->route[optFor].system, sizeof lc->route[optFor].system, "%s", better); dirty = 1; optNote[0] = 0;
        } else if (op == -1) snprintf(optNote, sizeof optNote, "Couldn't improve it: %.150s", better);
        static const char *RN[NROUTES] = {"Quick", "Think", "Research", "Do"};
        if (igBeginTabBar("##routes", 0)) {
          for (int r = 0; r < NROUTES; r++) {
            if (!igBeginTabItem(RN[r], NULL, tabflag(RN[r]))) continue;
            LlmRoute *rt = &lc->route[r];
            igPushID_Int(r);
            label("USED FOR");
            igSetNextItemWidth(full);
            dirty |= igInputText("##when", rt->when, sizeof rt->when, 0, NULL, NULL);
            if (r == ROUTE_RESEARCH) {
              label("HOW TO RESEARCH");
              static const char *BN[] = {"Auto", "Agent", "Model only"};
              if (segmented("rb", BN, 3, &rt->backend, full)) dirty = 1;
              if (igIsItemHovered(0)) igSetTooltip("Auto: web search + model when it's set up, otherwise the oh-my-pi agent");
            }
            bool th = rt->thinking;
            if (igCheckbox("Let the model think first (slower, deeper)", &th)) { rt->thinking = th; dirty = 1; }
            label("MAX REPLY LENGTH");
            igSetNextItemWidth(full);
            dirty |= igSliderInt("##maxtok", &rt->maxTokens, 64, 16000, "%d tokens", 0);
            label("PROMPT");
            dirty |= igInputTextMultiline("##sys", rt->system, sizeof rt->system, (ImVec2_c){full, r == ROUTE_QUICK ? 110 : 190}, ImGuiInputTextFlags_WordWrap, NULL, NULL);
            igTextDisabled("%d characters%s", (int)strlen(rt->system), r == ROUTE_QUICK ? "  ·  keep Quick short" : "");
            float third3 = (full - 16) / 3;
            igBeginDisabled(op == 2);
            if (th_button(op == 2 ? "Improving…" : "Improve", third3, 28, TH_BTN)) { llm_cfg_save(); llm_optimize(rt->system); optFor = r; optNote[0] = 0; }
            igEndDisabled();
            if (igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) igSetTooltip("Let the model rewrite this prompt to be clearer,\nkeeping every rule (you can undo)");
            igSameLine(0, 8);
            igBeginDisabled(hasUndo != r);
            if (th_button("Undo", third3, 28, TH_BTN)) { snprintf(rt->system, sizeof rt->system, "%s", undo); hasUndo = -1; dirty = 1; }
            igEndDisabled();
            igSameLine(0, 8);
            if (th_button("Default", third3, 28, TH_BTN)) { snprintf(undo, sizeof undo, "%s", rt->system); hasUndo = r; llm_reset_prompt(r); dirty = 1; }
            if (igIsItemHovered(0)) igSetTooltip("Back to this route's default prompt");
            igPopID();
            igEndTabItem();
          }
          if (igBeginTabItem("Router", NULL, tabflag("Router"))) { // the action model: picks the route
            bool on = lc->actionOn;
            if (igCheckbox("Ask the chat model which route to take", &on)) { lc->actionOn = on; dirty = 1; }
            igPushTextWrapPos(full + 16);
            igTextDisabled("Clear cues decide first (\"price\", \"weather\" → Research; \"prove\", \"code\" → Think). "
                           "Otherwise the chat model answers with one word, thinking off (~1 s). If it can't, jev and the message length decide.");
            igPopTextWrapPos();
            label("ROUTER PROMPT");
            dirty |= igInputTextMultiline("##act", lc->actionPrompt, sizeof lc->actionPrompt, (ImVec2_c){full, 170}, ImGuiInputTextFlags_WordWrap, NULL, NULL);
            float half3 = (full - 8) / 2;
            if (th_button("Test routing", half3, 28, TH_BTN)) { llm_cfg_save(); llm_router_test(); }
            if (igIsItemHovered(0)) igSetTooltip("Route five sample messages");
            igSameLine(0, 8);
            if (th_button("Default##act", half3, 28, TH_BTN)) { llm_reset_action_prompt(); dirty = 1; }
            static char tryMsg[300];
            igSetNextItemWidth(full - 74);
            int go = igInputTextWithHint("##try", "try a message…", tryMsg, sizeof tryMsg, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
            igSameLine(0, 6);
            if ((th_button("Route", 66, 0, TH_BTN) || go) && *tryMsg) { llm_cfg_save(); llm_router_try(tryMsg); }
            char rm[600]; int rs = llm_router_state(rm, sizeof rm);
            if (rs != TEST_NONE || *rm) {
              status_dot(rs, clock_);
              igPushTextWrapPos(full + 16); igTextDisabled("%s", rm); igPopTextWrapPos();
            }
            igEndTabItem();
          }
          igEndTabBar();
        }
        if (*optNote) { igPushTextWrapPos(full + 16); igTextColored(RGB(TH->danger, 1), "%s", optNote); igPopTextWrapPos(); }
        igEndTabItem();
      }

      // --- jev (SystemOne) routing: its own model and its own question set
      if (igBeginTabItem("jev", NULL, tabflag("jev"))) {
        bool on = lc->routerOn;
        if (igCheckbox("Use jev when the router model can't answer", &on)) { lc->routerOn = on; dirty = 1; }
        if (igIsItemHovered(0)) igSetTooltip("jev (SystemOne API) scores whether a message needs the web.\nIt's the fallback when the chat model can't pick the route.");
        if (igCollapsingHeader_TreeNodeFlags("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
          char jm[600]; int js = llm_jev_state(jm, sizeof jm);
          status_dot(js, clock_);
          igPushTextWrapPos(full + 16); igTextDisabled("%s", jm); igPopTextWrapPos();
          label("SYSTEMONE API");
          igSetNextItemWidth(full);
          dirty |= igInputTextWithHint("##rurl", "http://127.0.0.1:8011/v1/systemone", lc->routerUrl, sizeof lc->routerUrl, 0, NULL, NULL);
          label("RESEARCH FROM SCORE");
          igSetNextItemWidth(full);
          dirty |= igSliderFloat("##rcut", &lc->routerCut, 0.30f, 0.95f, "%.2f", 0);
          if (igIsItemHovered(0)) igSetTooltip("jev's \"needs the web\" score from which a message goes to Research.\n0.65 was best on a labelled test set.");
          if (th_button("Test jev", full, 28, TH_BTN)) { llm_cfg_save(); llm_jev_test(); }
          if (igIsItemHovered(0)) igSetTooltip("jev's own scores for five sample messages");
        }
        if (igCollapsingHeader_TreeNodeFlags("Questions (prompt)", 0)) {
          igPushTextWrapPos(full + 16);
          igTextDisabled("The SystemOne request. \"{message}\" becomes the user's message; the first yes/no answer is read as \"needs the web\".");
          igPopTextWrapPos();
          dirty |= igInputTextMultiline("##schema", lc->routerSchema, sizeof lc->routerSchema, (ImVec2_c){full, 170}, ImGuiInputTextFlags_WordWrap, NULL, NULL);
          if (th_button("Default questions", full, 28, TH_BTN)) { llm_reset_router_schema(); dirty = 1; }
        }
        igEndTabItem();
      }

      // --- web search and the oh-my-pi agent: how Research gets fresh information
      if (igBeginTabItem("Search", NULL, tabflag("Search"))) {
        if (igCollapsingHeader_TreeNodeFlags("Web search API", ImGuiTreeNodeFlags_DefaultOpen)) {
          static const char *WN[] = {"Off", "Brave", "Exa", "Tavily"};
          if (segmented("ws", WN, 4, &lc->wsProvider, full)) { dirty = 1; if (lc->wsProvider != WS_NONE && *lc->wsKey) { llm_cfg_save(); llm_search_test(); } }
          if (lc->wsProvider != WS_NONE) {
            label("API KEY");
            igSetNextItemWidth(full - 74);
            static const char *pasteTest; static int ptInit; // debugging aid: JELLY_TEST_PASTE pastes into this field
            if (!ptInit) { ptInit = 1; pasteTest = getenv("JELLY_TEST_PASTE"); }
            if (pasteTest && dbgFrames == 30) igSetKeyboardFocusHere(0);
            if (pasteTest && dbgFrames == 40) { // Ctrl is held when the clipboard arrives, as with a real Ctrl+V
              PEvent k = {0}; k.type = PE_KEY; k.win = ui.win; k.down = 1; k.mods = PM_CTRL; k.key = ImGuiKey_None; ui_event(&ui, &k);
              PEvent p = {0}; p.type = PE_PASTE; p.win = ui.win; p.paste = (char *)pasteTest; ui_event(&ui, &p);
            }
            if (pasteTest && dbgFrames == 60) fprintf(stderr, "paste test: field now '%s'\n", lc->wsKey);
            dirty |= igInputTextWithHint("##wskey", lc->wsProvider == WS_BRAVE ? "Brave Search API key" : lc->wsProvider == WS_EXA ? "Exa API key" : "Tavily API key",
                                         lc->wsKey, sizeof lc->wsKey, ImGuiInputTextFlags_Password, NULL, NULL);
            igSameLine(0, 6);
            if (th_button("Test##ws", 66, 0, TH_BTN)) { llm_cfg_save(); llm_search_test(); }
          }
          char sm[200]; int ss = llm_search_state(sm, sizeof sm);
          status_dot(ss, clock_);
          igPushTextWrapPos(full + 16);
          if (ss == TEST_FAIL) igTextColored(RGB(TH->danger, 1), "%s", sm); else igTextDisabled("%s", sm);
          igPopTextWrapPos();
        }
        if (igCollapsingHeader_TreeNodeFlags("oh-my-pi agent", ImGuiTreeNodeFlags_DefaultOpen)) {
          char am[200]; int as = llm_agent_state(am, sizeof am);
          status_dot(as, clock_);
          igPushTextWrapPos(full + 16);
          if (as == TEST_FAIL) igTextColored(RGB(TH->danger, 1), "%s", am); else igTextDisabled("%s", am);
          igPopTextWrapPos();
          if (llm_agent_available()) {
            label("AGENT MODEL");
            char autoM[256]; LlmCfg probe = *lc; probe.agentModel[0] = 0; llm_agent_model(&probe, autoM, sizeof autoM);
            char hintM[300]; snprintf(hintM, sizeof hintM, "auto: %s", *autoM ? autoM : "omp's default (not found for this endpoint)");
            igSetNextItemWidth(full - 74);
            dirty |= igInputTextWithHint("##amodel", hintM, lc->agentModel, sizeof lc->agentModel, 0, NULL, NULL);
            if (igIsItemHovered(0)) igSetTooltip("omp --model. Empty: the provider in ~/.omp/agent/models.yml\nthat serves the chat endpoint and model.");
            igSameLine(0, 6);
            if (th_button("Test##ag", 66, 0, TH_BTN)) { llm_cfg_save(); llm_agent_test(); }
            label("TIME LIMIT");
            igSetNextItemWidth(full);
            dirty |= igSliderInt("##asecs", &lc->agentSecs, 30, 600, "%d s", 0);
            label("TASKS ON THIS COMPUTER");
            bool tasks = lc->agentTasks;
            if (igCheckbox("Let jev hand tasks to oh-my-pi", &tasks)) { lc->agentTasks = tasks; dirty = 1; }
            if (igIsItemHovered(0)) igSetTooltip("When the router decides a message is a job (\"start omp and set up magpie\",\n"
                                                 "\"open firefox\"), oh-my-pi carries it out with its tools (the Do route).");
            if (lc->agentTasks) {
              static const char *AN[] = {"Read only", "Edit files", "Anything"};
              if (segmented("aa", AN, 3, &lc->agentApproval, full)) dirty = 1;
              igPushTextWrapPos(full + 16);
              igTextDisabled(lc->agentApproval == AA_READ ? "It can look, not change anything." :
                             lc->agentApproval == AA_WRITE ? "It can read and edit files, but not run commands." :
                             "It can run commands and change anything you can (omp's own default). It asks first before anything destructive.");
              igPopTextWrapPos();
            }
          }
        }
        igEndTabItem();
      }
      igEndTabBar();
    }
    if (dirty) llm_cfg_save();
    igEndTabItem();
  }
  igEndTabBar();
  }

  igDummy((ImVec2_c){0, 4});
  igSeparator();
  float half = (full - 8) / 2;
  if (th_button("Take a nap", half, 34, TH_BTN)) out |= OPT_NAP;
  igSameLine(0, 8);
  if (th_button("Quit jelly", half, 34, TH_BTN_DANGER)) out |= OPT_QUIT;

  ImVec2_c ws = igGetWindowSize();
  igEnd();

  if (showEvents) events_pane(clock_);
  if (showHistory) history_pane(clock_);
  if (showServices) services_pane(clock_);

  ui_frame_end(&ui);
  pw_want_text(ui.win, io->WantTextInput); // take the keyboard when a text field is activated

  // only the panels take clicks; the rest of the canvas is transparent and click-through
  int nh = (int)ceilf(ws.y);
  static int popupWas;
  int popup = igIsPopupOpen_Str("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  if (popup != popupWas) { popupWas = popup; shapeDirty = 1; }
  if (popup && shapeDirty) {
    pw_input_rects(ui.win, NULL, 0);
    shapeDirty = 0;
  }
  if ((nh > 50 && nh != panelH) || shapeDirty) {
    if (nh > 50) panelH = nh;
    PRect r[2] = {{0, 0, W, panelH}, {W + GAP, 0, EW, EH}};
    pw_input_rects(ui.win, r, SIDE ? 2 : 1);
    if (shapeDirty && SIDE) { // opening the events pane: keep it on screen
      int ox = ui.x, oy = ui.y;
      clamp_to_screen();
      if (ox != ui.x || oy != ui.y) ui_move(&ui, ui.x, ui.y);
    }
    shapeDirty = 0;
  }

  if (wantPreview) { wantPreview = 0; out |= OPT_PREVIEW; }
  if (wantClose) { opt_close(); out |= OPT_CLOSED; }
  return out;
}
