// The core's shared declarations: the jelly (jelly.c), its panels (options.c, chat.c, bubble.c), the calendar,
// the model client and the themes. Nothing here depends on the OS: that's platform/plat.h.
#pragma once
#include "../platform/plat.h"

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
  float opacity; // 0.2 .. 1: how see-through the whole jelly is
  int theme;     // THEME_* for the panels
} Cfg;

enum { FACE_TINY = 0, FACE_CLASSIC = 2 }; // 1 was the retired brows-only face

enum { OPT_NONE = 0, OPT_CHANGED = 1, OPT_NAP = 2, OPT_QUIT = 4, OPT_CLOSED = 8 };

void opt_open(int x, int y); // screen position of the panel's top-left
void opt_close(void);
void opt_show_events(int on);
int opt_is_open(void);
int opt_event(const PEvent *e); // 1 if it was the panel's
int opt_frame(Cfg *cfg, double dt); // draws the panel, edits cfg live, returns OPT_* flags

/* ---- calendar reminders (calendar.c) ---- */
#include <stddef.h>
#include <time.h>
#define CAL_MAX_URLS 10
#define CAL_DESC_MAX 600
#define CAL_URL_MAX 1024
typedef struct {
  char title[128], where[96], uid[160], desc[CAL_DESC_MAX];
  time_t start, end;
  int allday, cal; // cal: index of the calendar it came from
} CalEvent;
typedef struct {
  char label[96]; // the calendar's own name, or a short form of its link
  int enabled, ok, count, fetched;
} CalInfo;
int cal_count(void);
int cal_info(int i, CalInfo *out);
void cal_set_enabled(int i, int on);
void cal_start(void);
int cal_add_url(const char *url); // accepts https:// and webcal:// links; returns 0 if invalid or duplicate
void cal_remove_url(int i);
void cal_refresh(void);
void cal_status(char *out, size_t n);
int cal_events(CalEvent *out, int max); // everything in the next three months, soonest first
int cal_pick(time_t now, CalEvent *out); // an event worth mentioning right now, if any (call when someone's at the desk)
void cal_when(const CalEvent *e, time_t now, char *out, size_t n);
void cal_localtime(time_t t, struct tm *out);

/* ---- reminder bubble (bubble.c) ---- */
enum { BUB_NONE = 0, BUB_POPPED = 1, BUB_EXPIRED = 2 };
void bub_show(const CalEvent *e, float tint[3], double lifetime); // seconds on screen
int bub_visible(void);
int bub_event(const PEvent *e);
int bub_frame(double dt, float anchorX, float anchorY, float jellyR); // anchor: top of the jelly's head, screen px
void bub_center(float *x, float *y);

/* options panel additions */
enum { OPT_PREVIEW = 16 };

/* ---- chat with an OpenAI-compatible model (llm.c) ---- */
#define NROUTES 3
enum { ROUTE_QUICK, ROUTE_THINK, ROUTE_RESEARCH };
enum { RB_AUTO, RB_AGENT, RB_MODEL };           // research route: search+model / oh-my-pi agent / model only
enum { WS_NONE, WS_BRAVE, WS_EXA, WS_TAVILY };     // web search providers (API key each)
typedef struct {
  char system[4096]; // this route's personality / instructions
  char when[200];    // what it's for (shown in settings)
  int thinking, maxTokens, backend;
} LlmRoute;
typedef struct {
  char base[256], model[128], key[256]; // OpenAI-compatible endpoint
  float temperature;
  int saveHistory;
  int routerOn;                         // route each message with jev (SystemOne API)
  char routerUrl[256];
  char routerSchema[2048];              // SystemOne request; "{message}" is replaced by the message
  int wsProvider;
  char wsKey[256];
  int agentSecs;                        // oh-my-pi time limit
  char agentModel[128];                 // omp --model; empty: found in ~/.omp/agent/models.yml from base + model
  float routerCut;                      // jev "needs the web" score from which a message goes to Research
  int actionOn;                         // ask the chat model for the route (one word) when no cue decides
  char actionPrompt[1024];
  LlmRoute route[NROUTES];
} LlmCfg;
enum { LLM_IDLE, LLM_BUSY, LLM_DONE, LLM_ERROR };
enum { TEST_NONE, TEST_RUNNING, TEST_OK, TEST_FAIL };
typedef struct { long t, session; int user; char text[1200]; } LlmHist;
void llm_start(void);
LlmCfg *llm_cfg(void);
void llm_cfg_save(void);
void llm_reset_prompt(int route);                // back to the default prompt of a route
void llm_reset_router_schema(void);
void llm_reset_action_prompt(void);
void llm_router_test(void);                      // route a few sample messages
void llm_router_try(const char *msg);            // route one message of your own
int llm_router_state(char *msg, size_t n);        // TEST_* + result lines
void llm_jev_test(void);                         // jev's own "needs the web" scores for a few samples
int llm_jev_state(char *msg, size_t n);           // TEST_* + result lines
void llm_search_test(void);
int llm_search_state(char *msg, size_t n);        // TEST_*
int llm_agent_available(void);                   // oh-my-pi (omp) installed?
void llm_agent_model(const LlmCfg *c, char *out, size_t n); // the model omp will be told to use
void llm_agent_test(void);
int llm_agent_state(char *msg, size_t n);         // TEST_*
int llm_curl_available(void);
int llm_send(const char *text);                  // queues a question (answered in order); 0 if the queue is full
int llm_waiting(void);                           // questions not answered yet
int llm_ready(void);                             // answers not shown yet
int llm_pending_list(char out[][200], int max);  // unanswered questions, the one being answered first
int llm_take_reply(char *q, size_t qn, char *a, size_t an, char *label, size_t ln); // 1 answer, -1 error, 0 none
int llm_turns(void);
void llm_end_session(void);
void llm_test(void);
int llm_test_state(char *msg, size_t n);
void llm_optimize(const char *prompt);          // rewrite the system prompt to be clearer, in the background
int llm_optimize_poll(char *out, size_t n);     // 0 none, 2 running, 1 done (out = rewritten), -1 failed (out = why)
void llm_fetch_models(void);                    // GET {base}/models in the background
int llm_models(char out[][128], int max);       // the last list fetched
int llm_history(LlmHist *out, int max);
void llm_history_delete(void);

/* ---- chat box (chat.c) ---- */
enum { CH_HIDDEN, CH_INPUT, CH_WAIT, CH_REPLY };
enum { CHAT_NONE = 0, CHAT_SENT = 1, CHAT_REPLIED = 2, CHAT_ERROR = 4, CHAT_CLOSED = 8 };
void chat_open(float headX, float headY, float jellyR);
int chat_event(const PEvent *e);
int chat_frame(double dt, float headX, float headY, float jellyR, const float tint[3]);
int chat_mode(void);
int chat_busy(void);                             // questions are being answered (the jelly thinks)
void chat_debug_send(const char *text);

/* ---- themes (theme.c) ---- */
enum { THEME_WHITE = 0, THEME_DARK = 1, THEME_TOKYO = 2 };
typedef struct {
  unsigned bg; float bgAlpha;   // panel background
  unsigned frame; float frameAlpha; // controls: a tint of this over the background
  unsigned text, muted, accent, accent2, danger, warn;
  int light;
} Theme;
const Theme *theme(void);
void theme_select(int id);
void theme_style(void *imguiStyle, float bgAlpha); // bgAlpha <= 0: the theme's own
unsigned th_u32(unsigned hex, float alpha);        // 0xRRGGBB + alpha -> ImGui's packed color
enum { TH_BTN = 0, TH_BTN_ON = 1, TH_BTN_DANGER = 2, TH_BTN_QUIET = 3 };
int th_button(const char *label, float w, float h, int kind); // gummy jelly button; w/h <= 0: fit the label
void th_jelly(void *drawList, float x0, float y0, float x1, float y1, int kind, float hover, float squish);
void th_jelly_item(void *drawList, float x0, float y0, float x1, float y1, int kind); // animated by the last item
