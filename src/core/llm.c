// Chat with an OpenAI-compatible model (local TensorFold / vLLM / Ollama, or a hosted API).
// One conversation at a time. Requests run on a background thread through the curl binary: the request body goes
// in a private temp file and the API key through curl's stdin config, so neither shows up in the process list.
// Settings live in ~/.config/jev-jelly/llm.conf and history in chat-history.jsonl, both mode 600.

#define _GNU_SOURCE
#include "jelly.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/* The three route prompts. Quick is deliberately tiny (it's sent with every small-talk message and the model should
   answer in a breath); Think asks for worked reasoning; Research is about grounding the answer in fresh results. */
#define PERSONA "You are Jelly, a small, cheerful jelly friend living on the user's desktop. Kawaii and warm, but " \
                "clarity comes first. Plain text only: no emoji, no markdown. Reply in the user's language."
#define SAFETY_SHORT "Refuse anything harmful. For health, legal or money matters give general information and " \
                     "suggest a professional. If the user seems in distress, be kind and point them to people they " \
                     "trust or local crisis services."
static const char *QUICK_PROMPT =
    PERSONA "\n\nAnswer in one to three short sentences, with no preamble and no lists. If it truly needs more, give "
    "the short answer and offer to go deeper. " SAFETY_SHORT;
static const char *THINK_PROMPT =
    PERSONA " A light touch of cuteness is fine (a \"~\" or a simple kaomoji like (^_^)).\n\n"
    "This message needs careful thought. Work it out step by step before you answer, then reply with:\n"
    "- the answer or conclusion first, in one or two sentences;\n"
    "- then the key steps, reasons or trade-offs, as short numbered points;\n"
    "- for code: complete, runnable code and one line on how to use it.\n"
    "Check math, logic and code before replying. If something can't be known, or the user seems headed the wrong "
    "way, say so gently and suggest a better path. Never invent facts.\n\n" SAFETY_SHORT;
static const char *RESEARCH_PROMPT =
    PERSONA "\n\nThis question needs live or recent information. Base the answer only on the web results given to "
    "you (or your own search):\n"
    "- lead with the answer itself: numbers with units or currency, and the date or time they refer to;\n"
    "- add at most two sentences of context;\n"
    "- name the source (site name) for each key fact;\n"
    "- if sources disagree or look out of date, say so.\n"
    "If the results don't answer the question, say that plainly. Never guess prices, scores, dates or versions. "
    SAFETY_SHORT;

static const char *DO_PROMPT =
    "You are Jelly's hands: the user asked their desktop jelly friend to do something on this computer, and it "
    "handed the job to you. Do it with your tools, then reply in one to three short sentences: what you did and the "
    "result (or what went wrong and what they can try). Reply in the user's language, plain text, no emoji.\n"
    "Never do anything destructive or hard to undo (deleting or overwriting data, uninstalling, sending messages, "
    "spending money, changing security settings) unless the user asked for exactly that; if the request is unclear "
    "or risky, don't act: say what you would do and ask.";

/* the previous defaults, so settings that still hold them move to the new ones */
static const char *OLD_BASE_PREFIX = "You are Jelly, a small squishy jelly friend who lives on the user's desktop.";

static LlmCfg cfg;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int started;

/* ------------------------------------------------------------------ small string builder */

typedef struct { char *s; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = realloc(b->s, b->cap); }
  memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bfmt(Buf *b, const char *fmt, ...) {
  char tmp[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
  bput(b, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}
static void bjson(Buf *b, const char *s) { // a JSON string literal
  bput(b, "\"", 1);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
    case '"': bput(b, "\\\"", 2); break;
    case '\\': bput(b, "\\\\", 2); break;
    case '\n': bput(b, "\\n", 2); break;
    case '\r': bput(b, "\\r", 2); break;
    case '\t': bput(b, "\\t", 2); break;
    default:
      if (*p < 0x20) bfmt(b, "\\u%04x", *p);
      else bput(b, (const char *)p, 1);
    }
  }
  bput(b, "\"", 1);
}

/* parses the JSON string starting at *p (which points at its opening quote) into a malloc'd UTF-8 string */
static char *json_string(const char **pp) {
  const char *p = *pp;
  if (*p != '"') return NULL;
  p++;
  Buf b = {0};
  bput(&b, "", 0);
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) {
      p++;
      switch (*p) {
      case 'n': bput(&b, "\n", 1); break;
      case 't': bput(&b, "\t", 1); break;
      case 'r': break;
      case 'b': case 'f': break;
      case 'u': {
        unsigned cp = 0;
        if (sscanf(p + 1, "%4x", &cp) != 1) break;
        p += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') { // surrogate pair
          unsigned lo = 0;
          if (sscanf(p + 3, "%4x", &lo) == 1 && lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
        }
        char u[4]; int n = 0;
        if (cp < 0x80) u[n++] = (char)cp;
        else if (cp < 0x800) { u[n++] = (char)(0xC0 | (cp >> 6)); u[n++] = (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { u[n++] = (char)(0xE0 | (cp >> 12)); u[n++] = (char)(0x80 | ((cp >> 6) & 63)); u[n++] = (char)(0x80 | (cp & 63)); }
        else { u[n++] = (char)(0xF0 | (cp >> 18)); u[n++] = (char)(0x80 | ((cp >> 12) & 63)); u[n++] = (char)(0x80 | ((cp >> 6) & 63)); u[n++] = (char)(0x80 | (cp & 63)); }
        bput(&b, u, (size_t)n);
        break;
      }
      default: bput(&b, p, 1);
      }
      p++;
    } else { bput(&b, p, 1); p++; }
  }
  *pp = *p ? p + 1 : p;
  return b.s;
}
/* the string value of the first "key" after `from`, or NULL */
static char *json_find_string(const char *from, const char *key) {
  char k[64]; snprintf(k, sizeof k, "\"%s\"", key);
  const char *p = strstr(from, k);
  if (!p) return NULL;
  p += strlen(k);
  while (*p && (isspace((unsigned char)*p) || *p == ':')) p++;
  return json_string(&p);
}

/* numeric value of "key" after `from` (NAN if missing) */
static double json_find_num(const char *from, const char *key) {
  char k[64]; snprintf(k, sizeof k, "\"%s\"", key);
  const char *p = from ? strstr(from, k) : NULL;
  if (!p) return NAN;
  p += strlen(k);
  while (*p && (isspace((unsigned char)*p) || *p == ':')) p++;
  char *end; double v = strtod(p, &end);
  return end == p ? NAN : v;
}

/* ------------------------------------------------------------------ routes: default prompts */

static const char *WHEN[NROUTES] = {
    "Greetings, small talk and simple questions: one to three sentences.",
    "Reasoning, math, code, planning, comparisons and in-depth explanations.",
    "Live or recent information: prices, weather, news, scores, rates, latest releases.",
    "Tasks on this computer (when allowed in Search): start, open or set up apps, settings, files, commands.",
};
/* What jev (Julia-1) is asked. Measured on a labelled set of 22 messages: this yes/no question separates "needs the
   web" from everything else at 91% (threshold 0.65, the message under "user_message"). Julia's own multi-option
   choice ignored the message, and its "needs careful thinking" judgements were worse than chance, so Think is
   decided from cues and length instead. */
static const char *DEFAULT_SCHEMA =
    "{\n"
    "  \"state\": {\"user_message\": \"{message}\"},\n"
    "  \"questions\": {\n"
    "    \"needs_web\": {\"type\": \"noul\", \"instructions\": \"Would you need to look this up on the internet today to answer correctly?\"}\n"
    "  }\n"
    "}";

/* The action model: the chat model itself, thinking off, asked for one word. On 36 messages (22 used for tuning +
   14 held out) it picked the right route every time, about 1 s each; jev alone got 8 of the 14 held out. */
static const char *ACTION_PROMPT =
    "Classify the user's message for a desktop chat assistant. Reply with exactly one word:\n"
    "search - it needs live or recent facts from the internet (prices, weather, news, scores, schedules, who holds an "
    "office now, latest versions);\n"
    "think - it needs reasoning, math, code, planning, advice with trade-offs, or a detailed explanation;\n"
    "quick - anything else: greetings, small talk, feelings, simple stable facts.";

/* ------------------------------------------------------------------ settings */

static void path_of(const char *name, char *out, size_t n) { plat_config_path(name, out, n); }

LlmCfg *llm_cfg(void) { return &cfg; }
void llm_reset_prompt(int r) {
  if (r < 0 || r >= NROUTES) return;
  snprintf(cfg.route[r].system, sizeof cfg.route[r].system, "%s", r == ROUTE_THINK ? THINK_PROMPT : r == ROUTE_RESEARCH ? RESEARCH_PROMPT
           : r == ROUTE_DO ? DO_PROMPT : QUICK_PROMPT);
}
void llm_reset_router_schema(void) { snprintf(cfg.routerSchema, sizeof cfg.routerSchema, "%s", DEFAULT_SCHEMA); }
void llm_reset_action_prompt(void) { snprintf(cfg.actionPrompt, sizeof cfg.actionPrompt, "%s", ACTION_PROMPT); }

static void cfg_defaults(void) {
  memset(&cfg, 0, sizeof cfg);
  snprintf(cfg.base, sizeof cfg.base, "http://127.0.0.1:8888/v1");
  snprintf(cfg.model, sizeof cfg.model, "Qwen3.8-Flash-Next");
  cfg.temperature = 0.7f; cfg.saveHistory = 1;
  cfg.routerOn = 1;
  snprintf(cfg.routerUrl, sizeof cfg.routerUrl, "http://127.0.0.1:8011/v1/systemone");
  llm_reset_router_schema();
  cfg.agentSecs = 180; cfg.routerCut = 0.65f;
  cfg.actionOn = 1; llm_reset_action_prompt();
  cfg.memoryMins = 30;
  cfg.agentTasks = 0; cfg.agentApproval = AA_WRITE; // tasks are off until the user turns them on
  cfg.route[ROUTE_DO].thinking = 0; cfg.route[ROUTE_DO].maxTokens = 800; cfg.route[ROUTE_DO].backend = RB_AGENT;
  for (int r = 0; r < NROUTES; r++) {
    llm_reset_prompt(r);
    snprintf(cfg.route[r].when, sizeof cfg.route[r].when, "%s", WHEN[r]);
  }
  cfg.route[ROUTE_QUICK].thinking = 0; cfg.route[ROUTE_QUICK].maxTokens = 400;
  cfg.route[ROUTE_THINK].thinking = 1; cfg.route[ROUTE_THINK].maxTokens = 8000;
  cfg.route[ROUTE_RESEARCH].thinking = 0; cfg.route[ROUTE_RESEARCH].maxTokens = 1500; cfg.route[ROUTE_RESEARCH].backend = RB_AUTO;
}

static void unescape_line(char *s) { // \n and \\ in saved multi-line values
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == '\\' && p[1] == 'n') { *o++ = '\n'; p++; }
    else if (*p == '\\' && p[1] == '\\') { *o++ = '\\'; p++; }
    else *o++ = *p;
  }
  *o = 0;
}
static void put_multi(FILE *f, const char *key, const char *v) {
  fprintf(f, "%s=", key);
  for (; *v; v++) {
    if (*v == '\n') fputs("\\n", f);
    else if (*v == '\\') fputs("\\\\", f);
    else fputc(*v, f);
  }
  fputc('\n', f);
}

static void cfg_load(void) {
  cfg_defaults();
  char p[512]; path_of("llm.conf", p, sizeof p);
  FILE *f = fopen(p, "r");
  if (!f) return;
  static char line[16384];
  int ver = 1;
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = line; char *v = eq + 1;
    int r = -1; const char *field = k;
    if (!strncmp(k, "route", 5) && k[5] >= '0' && k[5] < '0' + NROUTES && k[6] == '.') { r = k[5] - '0'; field = k + 7; }
    if (r >= 0) {
      LlmRoute *rt = &cfg.route[r];
      if (!strcmp(field, "system")) { unescape_line(v); snprintf(rt->system, sizeof rt->system, "%s", v); }
      else if (!strcmp(field, "when")) snprintf(rt->when, sizeof rt->when, "%s", v);
      else if (!strcmp(field, "thinking")) rt->thinking = atoi(v);
      else if (!strcmp(field, "max_tokens")) rt->maxTokens = atoi(v);
      else if (!strcmp(field, "backend")) rt->backend = atoi(v);
      continue;
    }
    if (!strcmp(k, "base")) snprintf(cfg.base, sizeof cfg.base, "%s", v);
    else if (!strcmp(k, "model")) snprintf(cfg.model, sizeof cfg.model, "%s", v);
    else if (!strcmp(k, "key")) snprintf(cfg.key, sizeof cfg.key, "%s", v);
    else if (!strcmp(k, "temperature")) cfg.temperature = strtof(v, NULL);
    else if (!strcmp(k, "save_history")) cfg.saveHistory = atoi(v);
    else if (!strcmp(k, "router_on")) cfg.routerOn = atoi(v);
    else if (!strcmp(k, "router_url")) snprintf(cfg.routerUrl, sizeof cfg.routerUrl, "%s", v);
    else if (!strcmp(k, "router_schema")) { unescape_line(v); snprintf(cfg.routerSchema, sizeof cfg.routerSchema, "%s", v); }
    else if (!strcmp(k, "ws_provider")) cfg.wsProvider = atoi(v);
    else if (!strcmp(k, "ws_key")) snprintf(cfg.wsKey, sizeof cfg.wsKey, "%s", v);
    else if (!strcmp(k, "agent_secs")) cfg.agentSecs = atoi(v);
    else if (!strcmp(k, "agent_model")) snprintf(cfg.agentModel, sizeof cfg.agentModel, "%s", v);
    else if (!strcmp(k, "router_cut")) cfg.routerCut = strtof(v, NULL);
    else if (!strcmp(k, "action_on")) cfg.actionOn = atoi(v);
    else if (!strcmp(k, "memory_mins")) cfg.memoryMins = atoi(v);
    else if (!strcmp(k, "agent_tasks")) cfg.agentTasks = atoi(v);
    else if (!strcmp(k, "agent_approval")) cfg.agentApproval = atoi(v);
    else if (!strcmp(k, "action_prompt")) { unescape_line(v); snprintf(cfg.actionPrompt, sizeof cfg.actionPrompt, "%s", v); }
    else if (!strcmp(k, "prompt_version")) ver = atoi(v);
    // older single-prompt settings become the Quick route
    else if (!strcmp(k, "system")) { unescape_line(v); snprintf(cfg.route[0].system, sizeof cfg.route[0].system, "%s", v); }
    else if (!strcmp(k, "max_tokens")) cfg.route[0].maxTokens = atoi(v);
  }
  fclose(f);
  if (ver < 2) { // settings from before the per-purpose templates: untouched defaults move to the new ones
    for (int r = 0; r < NROUTES; r++)
      if (!strncmp(cfg.route[r].system, OLD_BASE_PREFIX, strlen(OLD_BASE_PREFIX))) {
        llm_reset_prompt(r);
        snprintf(cfg.route[r].when, sizeof cfg.route[r].when, "%s", WHEN[r]);
        if (r == ROUTE_QUICK && cfg.route[r].maxTokens == 800) cfg.route[r].maxTokens = 400;
      }
    if (strstr(cfg.routerSchema, "\"needs_research\"")) llm_reset_router_schema();
  }
  if (ver < 3) { // the Do route arrived: its defaults (route 3 wasn't in the file before)
    llm_reset_prompt(ROUTE_DO);
    snprintf(cfg.route[ROUTE_DO].when, sizeof cfg.route[ROUTE_DO].when, "%s", WHEN[ROUTE_DO]);
    cfg.route[ROUTE_DO].maxTokens = 800; cfg.route[ROUTE_DO].backend = RB_AGENT;
  }
  for (int r = 0; r < NROUTES; r++) if (cfg.route[r].maxTokens < 32) cfg.route[r].maxTokens = 800;
  if (!(cfg.routerCut > 0.05f && cfg.routerCut < 0.99f)) cfg.routerCut = 0.65f;
  if (cfg.agentSecs < 30) cfg.agentSecs = 180;
}

void llm_cfg_save(void) {
  char p[512]; path_of("llm.conf", p, sizeof p);
  FILE *f = fopen(p, "w");
  if (!f) return;
  plat_private_file(p); // holds API keys
  fprintf(f, "base=%s\nmodel=%s\nkey=%s\ntemperature=%.2f\nsave_history=%d\nrouter_on=%d\nrouter_url=%s\n",
          cfg.base, cfg.model, cfg.key, cfg.temperature, cfg.saveHistory, cfg.routerOn, cfg.routerUrl);
  put_multi(f, "router_schema", cfg.routerSchema);
  fprintf(f, "ws_provider=%d\nws_key=%s\nagent_secs=%d\nagent_model=%s\nrouter_cut=%.2f\nprompt_version=3\n", cfg.wsProvider,
          cfg.wsKey, cfg.agentSecs, cfg.agentModel, cfg.routerCut);
  fprintf(f, "action_on=%d\nmemory_mins=%d\nagent_tasks=%d\nagent_approval=%d\n", cfg.actionOn, cfg.memoryMins,
          cfg.agentTasks, cfg.agentApproval);
  put_multi(f, "action_prompt", cfg.actionPrompt);
  for (int r = 0; r < NROUTES; r++) {
    char k[32];
    fprintf(f, "route%d.when=%s\nroute%d.thinking=%d\nroute%d.max_tokens=%d\nroute%d.backend=%d\n", r, cfg.route[r].when,
            r, cfg.route[r].thinking, r, cfg.route[r].maxTokens, r, cfg.route[r].backend);
    snprintf(k, sizeof k, "route%d.system", r);
    put_multi(f, k, cfg.route[r].system);
  }
  fclose(f);
}

/* ------------------------------------------------------------------ one request through curl */

typedef struct { char *text; char err[200]; double secs; int code; } Reply;

static void strip_thinking(char *s) { // some models inline their reasoning as <think>...</think>
  char *a;
  while ((a = strstr(s, "<think>"))) {
    char *b = strstr(a, "</think>");
    if (!b) { *a = 0; break; }
    memmove(a, b + 8, strlen(b + 8) + 1);
  }
  char *p = s; while (isspace((unsigned char)*p)) p++;
  memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s); while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void cfg_quote(Buf *b, const char *s) { // a double-quoted curl config value
  bput(b, "\"", 1);
  for (; *s; s++) { if (*s == '"' || *s == '\\') bput(b, "\\", 1); if (*s != '\n' && *s != '\r') bput(b, s, 1); }
  bput(b, "\"", 1);
}

/* Runs a program with `in` on stdin; returns its stdout (malloc'd) and exit status. The program is found on PATH
   or at `fallback`. */
static char *run_capture(const char *prog, const char *fallback, char **argv, const char *in, int *status) {
  char *out = plat_run(prog, argv, in, status);
  if (!out && fallback && strcmp(fallback, prog)) out = plat_run(fallback, argv, in, status);
  return out;
}

/* An HTTP request through curl (config on stdin, so keys never appear in the process list). GET when body is NULL.
   `headers` is a list of extra header lines. On success returns the body in *raw. */
static Reply http(const char *url, const char **headers, int nh, const char *body, int maxSecs, char **raw) {
  Reply r = {0};
  double t0 = plat_now();
  char tmp[512] = "";
  if (body) {
    FILE *tf = plat_temp_file(tmp, sizeof tmp); // private to the user
    if (!tf) { snprintf(r.err, sizeof r.err, "couldn't write a temp file"); return r; }
    fputs(body, tf);
    fclose(tf);
  }
  Buf conf = {0};
  bstr(&conf, "url = "); cfg_quote(&conf, url); bstr(&conf, "\n");
  if (body) bstr(&conf, "header = \"Content-Type: application/json\"\n");
  for (int i = 0; i < nh; i++) { bstr(&conf, "header = "); cfg_quote(&conf, headers[i]); bstr(&conf, "\n"); }
  if (body) { bstr(&conf, "data-binary = "); char at[520]; snprintf(at, sizeof at, "@%s", tmp); cfg_quote(&conf, at); bstr(&conf, "\n"); }
  bfmt(&conf, "max-time = %d\nconnect-timeout = 5\nsilent\nshow-error\ncompressed\nwrite-out = \"\\n%%{http_code}\"\n", maxSecs);
  char *argv[] = {"curl", "-K", "-", NULL};
  int st;
  char *resp = run_capture("curl", NULL, argv, conf.s, &st);
  free(conf.s);
  if (*tmp) remove(tmp);
  r.secs = plat_now() - t0;
  if (!resp) { snprintf(r.err, sizeof r.err, "curl is not installed"); return r; }
  char *nl = strrchr(resp, '\n');
  if (nl) { r.code = atoi(nl + 1); *nl = 0; }
  if (r.code == 0) snprintf(r.err, sizeof r.err, st == 28 ? "timed out" : "couldn't reach %.120s", url);
  else if (r.code >= 400) {
    char *m = json_find_string(resp, "message");
    snprintf(r.err, sizeof r.err, "HTTP %d%s%.150s", r.code, m ? ": " : "", m ? m : "");
    free(m);
  } else if (raw) { *raw = resp; resp = NULL; }
  free(resp);
  return r;
}

/* ------------------------------------------------------------------ the model (OpenAI-compatible) */

static char *make_body(const LlmCfg *c, const char *system, int thinking, int maxTok, const char **roles, char **texts, int n) {
  Buf b = {0};
  bstr(&b, "{\"model\":"); bjson(&b, c->model);
  bstr(&b, ",\"messages\":[");
  int first = 1;
  if (system && *system) { bstr(&b, "{\"role\":\"system\",\"content\":"); bjson(&b, system); bstr(&b, "}"); first = 0; }
  for (int i = 0; i < n; i++) {
    if (!first) bstr(&b, ",");
    first = 0;
    bstr(&b, "{\"role\":"); bjson(&b, roles[i]); bstr(&b, ",\"content\":"); bjson(&b, texts[i]); bstr(&b, "}");
  }
  bfmt(&b, "],\"temperature\":%.2f,\"max_tokens\":%d,\"stream\":false", c->temperature, maxTok);
  if (!thinking) bstr(&b, ",\"chat_template_kwargs\":{\"enable_thinking\":false}");
  bstr(&b, "}");
  return b.s;
}

static Reply chat_call(const LlmCfg *c, const char *system, int thinking, int maxTok, const char **roles, char **texts, int n) {
  char url[600]; snprintf(url, sizeof url, "%s", c->base);
  size_t ul = strlen(url); while (ul && url[ul - 1] == '/') url[--ul] = 0;
  strncat(url, "/chat/completions", sizeof url - strlen(url) - 1);
  char auth[400]; const char *h[1]; int nh = 0;
  if (*c->key) { snprintf(auth, sizeof auth, "Authorization: Bearer %s", c->key); h[nh++] = auth; }
  char *body = make_body(c, system, thinking, maxTok, roles, texts, n);
  char *raw = NULL;
  Reply r = http(url, h, nh, body, thinking ? 600 : 180, &raw);
  free(body);
  if (raw) {
    const char *msg = strstr(raw, "\"message\"");
    char *text = msg ? json_find_string(msg, "content") : NULL;
    if (text) { strip_thinking(text); if (!*text) { free(text); text = NULL; } }
    if (text) r.text = text;
    else snprintf(r.err, sizeof r.err, "the model sent no text (it may have run out of tokens while thinking)");
    free(raw);
  }
  return r;
}

/* ------------------------------------------------------------------ routing with jev (SystemOne) */

static int has_any(const char *msg, const char **words) {
  char low[4096]; size_t n = 0;
  for (const char *p = msg; *p && n < sizeof low - 1; p++) low[n++] = (char)tolower((unsigned char)*p);
  low[n] = 0;
  for (int i = 0; words[i]; i++) if (strstr(low, words[i])) return 1;
  return 0;
}
/* Clear cues. Words that are common in small talk ("today", "now", "current") are left out on purpose: "how are
   you today?" is not a research question. */
/* naming the agent: it looks things up (Research), or, with tasks turned on, the router decides between that and Do */
static const char *KW_AGENT[] = {"ompi", " omp ", "omp ", "oh-my-pi", "oh my pi", "use the agent", "ask the agent", NULL};
/* "how do I install htop?" is a question, not a job, even if the router says "do" */
static const char *KW_HOWTO[] = {"how do i", "how to", "how can i", "how would i", "how should i", "in python", "in c ",
    "in javascript", "in rust", "explain", "怎么", "如何", "どうやって", "어떻게", NULL};
static const char *KW_RESEARCH[] = {"web access", "online", "internet", "google it", "search", "look up", "look it up", "google", "latest", "news", "right now", "weather", "forecast",
    "price", "stock", "exchange rate", "btc", "bitcoin", "ethereum", "crypto", "release", "version of", "http://",
    "https://", "www.", "research", "sources", "who won", "score", "tonight", "tomorrow", "this week", "election",
    "搜索", "查一下", "最新", "新闻", "天气", "价格", "股价", "汇率", "比特币", "明天", "检索", "検索", "調べ", "最新の",
    "ニュース", "天気", "価格", "為替", "검색", "최신", "뉴스", "날씨", "가격", "환율", NULL};
static const char *KW_THINK[] = {"prove", "why ", "explain", "plan ", "compare", "design", "debug", "calculate", "solve",
    "step by step", "analy", "strategy", "algorithm", "write a", "write me", "code", "function", "script", "essay",
    "derive", "optimi", "trade-off", "pros and cons", "deadlock", "error:", "为什么", "证明", "分析", "解释", "计划",
    "比较", "计算", "设计", "代码", "详细", "なぜ", "証明", "説明", "分析", "計算", "設計", "コード", "왜", "증명", "설명",
    "분석", "계산", "설계", "코드", NULL};

typedef struct { int ok; double web, secs; } JevSays;

static JevSays ask_jev(const LlmCfg *c, const char *msg) {
  JevSays j = {0};
  j.web = NAN;
  Buf esc = {0}; bjson(&esc, msg);
  Buf body = {0};
  const char *tpl = c->routerSchema, *at;
  while ((at = strstr(tpl, "\"{message}\""))) { bput(&body, tpl, (size_t)(at - tpl)); bstr(&body, esc.s); tpl = at + 11; }
  bstr(&body, tpl);
  free(esc.s);
  char *raw = NULL;
  Reply r = http(c->routerUrl, NULL, 0, body.s, 4, &raw);
  free(body.s);
  j.secs = r.secs;
  if (!raw) return j;
  const char *a = strstr(raw, "\"answers\""), *q;
  if (a) {
    j.ok = 1;
    // the first yes/no answer is "does it need the web" (needs_web, or needs_research in older question sets)
    if ((q = strstr(a, "\"needs_web\"")) || (q = strstr(a, "\"needs_research\"")) || (q = strstr(a, "\"noul\"")))
      j.web = json_find_num(q, "noul");
  }
  free(raw);
  return j;
}

/* the action model's one-word pick, or -1 if it didn't answer */
static int ask_action(const LlmCfg *c, const char *msg, double *secs) {
  LlmCfg k = *c;
  k.temperature = 0;
  const char *r[1] = {"user"}; char *t[1] = {(char *)msg};
  char sys[1400];
  snprintf(sys, sizeof sys, "%s%s", c->actionPrompt, c->agentTasks ?
           "\ndo - the user tells you to carry out an action on their computer right now: start, open, install, set up "
           "or configure an app, change a setting, create or move files, run a command. A question about how to do "
           "something is not \"do\"." : "");
  Reply rep = chat_call(&k, sys, 0, 4, r, t, 1);
  *secs = rep.secs;
  if (!rep.text) return -1;
  for (char *p = rep.text; *p; p++) *p = (char)tolower((unsigned char)*p);
  int route = c->agentTasks && !strncmp(rep.text, "do", 2) ? ROUTE_DO : strstr(rep.text, "search") ? ROUTE_RESEARCH : strstr(rep.text, "think") ? ROUTE_THINK : strstr(rep.text, "quick") ? ROUTE_QUICK : -1;
  free(rep.text);
  return route;
}

/* Picks a route: clear cues first (instant); then the action model; if it can't answer, jev's "needs the web"
   score and the message length. */
static int decide(const LlmCfg *c, const char *msg, JevSays *jout, char *why, size_t n) {
  size_t len = strlen(msg);
  int kwR = has_any(msg, KW_RESEARCH), kwT = has_any(msg, KW_THINK);
  JevSays j = {0};
  j.web = NAN;
  if (jout) *jout = j;
  int kwA = has_any(msg, KW_AGENT);
  if (kwA && !c->agentTasks) { snprintf(why, n, "asks the agent to look"); return ROUTE_RESEARCH; }
  if (kwR && !kwA) { snprintf(why, n, "asks for live info"); return ROUTE_RESEARCH; }
  if (kwT && !kwA) { snprintf(why, n, "a thinking task"); return ROUTE_THINK; }
  char am[40] = "";
  if (c->actionOn) {
    double secs = 0;
    int a = ask_action(c, msg, &secs);
    if (a == ROUTE_DO && has_any(msg, KW_HOWTO)) { snprintf(why, n, "action model · a how-to question · %.1f s", secs); return ROUTE_THINK; }
    if (kwA && a != ROUTE_DO) { snprintf(why, n, "asks the agent to look · %.1f s", secs); return ROUTE_RESEARCH; }
    if (a >= 0) { snprintf(why, n, "action model · %.1f s", secs); return a; }
    snprintf(am, sizeof am, "action model offline · ");
  }
  if (kwA) { snprintf(why, n, "%sasks the agent", am); return kwR || !c->agentTasks ? ROUTE_RESEARCH : ROUTE_DO; }
  if (c->routerOn) j = ask_jev(c, msg);
  if (jout) *jout = j;
  char jv[48];
  if (!c->routerOn) snprintf(jv, sizeof jv, "jev off");
  else if (!j.ok) snprintf(jv, sizeof jv, "jev offline");
  else snprintf(jv, sizeof jv, "jev web %.2f", j.web);
  if (j.ok && !isnan(j.web) && j.web >= c->routerCut) { snprintf(why, n, "%sjev: needs the web (%.2f)", am, j.web); return ROUTE_RESEARCH; }
  if (len > 220) { snprintf(why, n, "%sa long message · %s", am, jv); return ROUTE_THINK; }
  snprintf(why, n, "%ssimple · %s", am, jv);
  return ROUTE_QUICK;
}

/* ------------------------------------------------------------------ web search */

typedef struct { char title[200], url[300], snippet[500]; } Hit;

static void urlenc(Buf *b, const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') bput(b, (const char *)p, 1);
    else bfmt(b, "%%%02X", *p);
  }
}

/* up to `max` results for a query from the configured provider; returns the count, or -1 with *err set */
static int web_search(const LlmCfg *c, const char *query, Hit *hits, int max, char *err, size_t en) {
  Buf url = {0}; char *raw = NULL; Reply r = {0};
  const char *h[2]; int nh = 0; char hdr[400];
  const char *body = NULL; Buf jb = {0};
  switch (c->wsProvider) {
  case WS_BRAVE:
    bstr(&url, "https://api.search.brave.com/res/v1/web/search?count=6&q="); urlenc(&url, query);
    snprintf(hdr, sizeof hdr, "X-Subscription-Token: %s", c->wsKey); h[nh++] = hdr; h[nh++] = "Accept: application/json";
    break;
  case WS_EXA:
    bstr(&url, "https://api.exa.ai/search");
    snprintf(hdr, sizeof hdr, "x-api-key: %s", c->wsKey); h[nh++] = hdr;
    bstr(&jb, "{\"numResults\":6,\"contents\":{\"text\":{\"maxCharacters\":600}},\"query\":"); bjson(&jb, query); bstr(&jb, "}");
    body = jb.s;
    break;
  case WS_TAVILY:
    bstr(&url, "https://api.tavily.com/search");
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", c->wsKey); h[nh++] = hdr;
    bstr(&jb, "{\"max_results\":6,\"query\":"); bjson(&jb, query); bstr(&jb, "}");
    body = jb.s;
    break;
  default:
    snprintf(err, en, "no web search configured");
    return -1;
  }
  if (!*c->wsKey) { free(url.s); free(jb.s); snprintf(err, en, "add an API key"); return -1; }
  r = http(url.s, h, nh, body, 20, &raw);
  free(url.s); free(jb.s);
  if (!raw) { snprintf(err, en, "%s", r.err); return -1; }
  // every provider returns a list of objects with a title, a url and some text
  const char *key = c->wsProvider == WS_BRAVE ? "\"web\"" : "\"results\"";
  const char *p = strstr(raw, key);
  int n = 0;
  const char *textKey = c->wsProvider == WS_BRAVE ? "description" : c->wsProvider == WS_EXA ? "text" : "content";
  while (p && n < max && (p = strstr(p, "\"url\""))) {
    char *u = json_find_string(p, "url");
    // title and text of the same result: look back to the object's start, then forward within it
    const char *objStart = p; int depth = 0;
    while (objStart > raw && !(*objStart == '{' && depth == 0)) { if (*objStart == '}') depth++; if (*objStart == '{') depth--; objStart--; }
    char *t = json_find_string(objStart, "title"), *sn = json_find_string(objStart, textKey);
    if (u && strncmp(u, "http", 4) == 0) {
      snprintf(hits[n].url, sizeof hits[n].url, "%s", u);
      snprintf(hits[n].title, sizeof hits[n].title, "%s", t ? t : u);
      snprintf(hits[n].snippet, sizeof hits[n].snippet, "%s", sn ? sn : "");
      int dup = 0; for (int i = 0; i < n; i++) if (!strcmp(hits[i].url, hits[n].url)) dup = 1;
      if (!dup) n++;
    }
    free(u); free(t); free(sn);
    p += 5;
  }
  free(raw);
  if (!n) snprintf(err, en, "no results");
  return n ? n : -1;
}

/* ------------------------------------------------------------------ the oh-my-pi agent (omp CLI) */

static char ompPath[512];
int llm_agent_available(void) {
  if (!*ompPath) {
    if (!plat_find_exe("omp", ompPath, sizeof ompPath)) snprintf(ompPath, sizeof ompPath, "-");
    else { // omp runs on bun, which sits next to it (~/.bun/bin): started from a desktop icon, PATH doesn't have it
      char dir[512]; snprintf(dir, sizeof dir, "%s", ompPath);
      char *slash = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
      if (bs > slash) slash = bs;
      if (slash) { *slash = 0; plat_path_prepend(dir); }
    }
  }
  return ompPath[0] != '-';
}

static const char *home_dir(void) {
  const char *h = getenv("HOME");
  if (!h || !*h) h = getenv("USERPROFILE"); // Windows
  return h ? h : ".";
}

static void strip_ansi(char *s) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == 0x1b && p[1] == '[') { p += 2; while (*p && !isalpha((unsigned char)*p)) p++; if (!*p) break; continue; }
    *o++ = *p;
  }
  *o = 0;
}

/* omp's own default model may be one that isn't running (it then retries until its time limit and exits with
   nothing). So it gets told which model to use: the setting if there is one, otherwise the provider in
   ~/.omp/agent/models.yml that serves Jelly's own endpoint and model, e.g. "qwen-next-local/Qwen3.8-Flash-Next". */
static void norm_url(const char *u, char *out, size_t n) {
  snprintf(out, n, "%s", u);
  size_t l = strlen(out); while (l && out[l - 1] == '/') out[--l] = 0;
  char *h = strstr(out, "localhost"); // localhost and 127.0.0.1 are the same server
  if (h) { char t[512]; snprintf(t, sizeof t, "%.*s127.0.0.1%s", (int)(h - out), out, h + 9); snprintf(out, n, "%s", t); }
}
void llm_agent_model(const LlmCfg *c, char *out, size_t n) {
  out[0] = 0;
  if (*c->agentModel) { snprintf(out, n, "%s", c->agentModel); return; }
  char p[512]; snprintf(p, sizeof p, "%s/.omp/agent/models.yml", home_dir());
  FILE *f = fopen(p, "r");
  if (!f) return;
  char line[1024], prov[128] = "", base[512] = "", want[512], any[256] = "";
  norm_url(c->base, want, sizeof want);
  while (fgets(line, sizeof line, f)) {
    char v[512], *colon = strchr(line, ':');
    // "  name:" with nothing after the colon starts a provider block (two-space indent, under "providers:")
    if (line[0] == ' ' && line[1] == ' ' && line[2] != ' ' && line[2] != '-' && colon &&
        strspn(colon + 1, " \t\r\n") == strlen(colon + 1)) {
      snprintf(prov, sizeof prov, "%.*s", (int)(colon - line - 2), line + 2); base[0] = 0;
      continue;
    }
    char *bu = strstr(line, "baseUrl:");
    if (bu && sscanf(bu + 8, "%511s", v) == 1) norm_url(v, base, sizeof base);
    char *id = strstr(line, "- id:");
    if (id && sscanf(id + 5, "%511s", v) == 1 && !strcmp(v, c->model) && *prov) {
      if (!strcmp(base, want)) { snprintf(out, n, "%.100s/%.150s", prov, v); break; }
      if (!*any) snprintf(any, sizeof any, "%.100s/%.150s", prov, v);
    }
  }
  fclose(f);
  if (!*out && *any) snprintf(out, n, "%s", any);
}

/* tools NULL: all of omp's tools, with `approval` (AA_*) deciding what runs without asking, from the home folder */
static Reply run_agent(const LlmCfg *c, const char *system, const char *prompt, int secsLimit, const char *tools, int approval) {
  Reply r = {0};
  if (!llm_agent_available()) { snprintf(r.err, sizeof r.err, "oh-my-pi (omp) is not installed"); return r; }
  char secs[32]; snprintf(secs, sizeof secs, "%ds", secsLimit);
  char model[256]; llm_agent_model(c, model, sizeof model);
  char *argv[24]; int a = 0;
  argv[a++] = "omp"; argv[a++] = "-p"; argv[a++] = "--no-session"; argv[a++] = "--mode"; argv[a++] = "text";
  argv[a++] = "--thinking"; argv[a++] = "low";
  if (tools) { argv[a++] = "--tools"; argv[a++] = (char *)tools; }
  else {
    static const char *modes[] = {"always-ask", "write", "yolo"};
    argv[a++] = "--approval-mode"; argv[a++] = (char *)modes[approval < 0 ? 0 : approval > 2 ? 2 : approval];
    const char *h = home_dir();
    static char cwd[512]; snprintf(cwd, sizeof cwd, "%s", h);
    argv[a++] = "--allow-home"; argv[a++] = "--cwd"; argv[a++] = cwd;
  }
  argv[a++] = "--max-time"; argv[a++] = secs;
  if (*model) { argv[a++] = "--model"; argv[a++] = model; }
  if (system && *system) { argv[a++] = "--append-system-prompt"; argv[a++] = (char *)system; }
  argv[a++] = (char *)prompt; argv[a] = NULL;
  double t0 = plat_now();
  int st;
  char *out = run_capture(ompPath, ompPath, argv, NULL, &st);
  r.secs = plat_now() - t0;
  if (out) {
    strip_ansi(out); strip_thinking(out);
    if (!strncmp(out, "Working...", 10)) { memmove(out, out + 10, strlen(out + 10) + 1); strip_thinking(out); } // progress line
  }
  if (out && *out && st == 0) r.text = out;
  else {
    if (r.secs >= secsLimit - 2) snprintf(r.err, sizeof r.err, "the agent found nothing within %d s", secsLimit);
    else snprintf(r.err, sizeof r.err, "the agent failed after %.0f s (exit %d%s%.100s)", r.secs, st, *model ? ", model " : ", no model found", model);
    free(out);
  }
  return r;
}

/* ------------------------------------------------------------------ the conversation + worker */

#define MAXTURNS 40
static const char *roles[MAXTURNS];
static char *texts[MAXTURNS];
static int nturns;
static long sessionId;

/* Questions can be asked while an earlier one is still being answered: they wait in `pending` and are answered in
   order; answers wait in `ready` until the chat box shows them, each with its question. */
#define QMAX 16
typedef struct { char *q, *a; char label[80]; int route, err; } LlmReply;
static char *pending[QMAX];
static char *workingQ; // the question being answered right now (owned by the worker)
static time_t lastActivity; // the latest question or answer: the memory lasts memoryMins after it
static int npending, working, gen; // gen: bumped when the conversation ends, so a late answer is dropped
static LlmReply ready[QMAX];
static int nready;

enum { JOB_CHAT = 1, JOB_TEST = 2, JOB_OPT = 4, JOB_MODELS = 8, JOB_ROUTER = 16, JOB_SEARCH = 32, JOB_AGENT = 64, JOB_JEV = 128 };
static int job; // pending jobs (bits)
static int testState = TEST_NONE, routerState = TEST_NONE, searchState = TEST_NONE, agentState = TEST_NONE;
static char testMsg[200], routerMsg[600], searchMsg[200], agentMsg[200], routerTry[400];
static int jevState = TEST_NONE;
static char jevMsg[600];
static char *optDraft, *optResult;
static int optState; // 0 none, 1 running, 2 done, 3 failed
static char optErr[200];
static char models[64][128];
static int nmodels;

static const char *OPT_PROMPT =
    "You improve system prompts. The user gives you the system prompt of Jelly, a small jelly companion that lives "
    "on their desktop and chats with them. Rewrite it so it is clearer, better organized and more effective for a "
    "chat model: keep every requirement it contains (persona, tone, behavior, language, safety rules), make "
    "instructions concrete and unambiguous, remove repetition, and keep it reasonably short. Do not add new rules "
    "that change its intent, and keep the safety guidance. Output only the improved system prompt, nothing else.";

static void history_append(const char *role, const char *text) {
  if (!cfg.saveHistory) return;
  char p[512]; path_of("chat-history.jsonl", p, sizeof p);
  FILE *f = fopen(p, "a");
  if (!f) return;
  plat_private_file(p);
  Buf b = {0};
  bfmt(&b, "{\"t\":%ld,\"s\":%ld,\"r\":", (long)time(NULL), sessionId); bjson(&b, role);
  bstr(&b, ",\"c\":"); bjson(&b, text); bstr(&b, "}\n");
  fputs(b.s, f);
  free(b.s);
  fclose(f);
}

/* Answers the latest user message: pick a route, then the model (optionally with web results) or the agent. */
static Reply answer(const LlmCfg *c, const char **r, char **t, int n, int *routeOut, char *label, size_t ln) {
  char why[200]; JevSays j = {0};
  int route = decide(c, t[n - 1], &j, why, sizeof why);
  const LlmRoute *rt = &c->route[route];
  if (getenv("JELLY_DEBUG")) fprintf(stderr, "llm: route %d (%s)\n", route, why);
  *routeOut = route;
  if (route == ROUTE_RESEARCH) {
    int searchOk = c->wsProvider != WS_NONE && *c->wsKey && searchState != TEST_FAIL;
    if (rt->backend != RB_AGENT && searchOk) { // web results + the model
      Hit hits[6]; char err[200];
      int nh = web_search(c, t[n - 1], hits, 6, err, sizeof err);
      if (nh > 0) {
        Buf b = {0};
        bstr(&b, "Web search results:\n");
        for (int i = 0; i < nh; i++) bfmt(&b, "[%d] %.180s\n    %.280s\n    %.400s\n\n", i + 1, hits[i].title, hits[i].url, hits[i].snippet);
        bstr(&b, "Question: "); bstr(&b, t[n - 1]);
        char *keep = t[n - 1]; t[n - 1] = b.s;
        Reply rep = chat_call(c, rt->system, rt->thinking, rt->maxTokens, r, t, n);
        t[n - 1] = keep; free(b.s);
        snprintf(label, ln, "searched the web");
        return rep;
      }
      if (getenv("JELLY_DEBUG")) fprintf(stderr, "llm: web search failed: %s\n", err);
    }
    // no working web search (or it found nothing): the oh-my-pi agent searches on its own
    if (rt->backend != RB_MODEL && llm_agent_available()) {
      Buf q = {0}; // the agent has its own long system prompt: restate the language and ask for honesty about sources
      bstr(&q, "Answer in the same language as this question (not any other language). Search the web if you can; if you "
               "can't get results, say so plainly instead of guessing.\n\n");
      if (n > 1) { // what was said just before, so "and tomorrow?" or "check it there" make sense
        bstr(&q, "Earlier in this conversation:\n");
        for (int i = n - 1 > 6 ? n - 7 : 0; i < n - 1; i++) bfmt(&q, "%s: %.300s\n", !strcmp(r[i], "user") ? "User" : "Jelly", t[i]);
        bstr(&q, "\n");
      }
      bstr(&q, "Question: ");
      bstr(&q, t[n - 1]);
      Reply rep = run_agent(c, rt->system, q.s, c->agentSecs, "web_search,read", 0);
      free(q.s);
      if (rep.text) { snprintf(label, ln, "asked the agent"); return rep; }
      if (getenv("JELLY_DEBUG")) fprintf(stderr, "llm: agent failed: %s\n", rep.err);
    }
    // no way to look it up: the model says so and gives only what it reliably knows
    Buf b = {0};
    bstr(&b, "(No web access right now. Say so in one short sentence, then give only what you reliably know, and "
             "where the user can check it.)\n\nQuestion: ");
    bstr(&b, t[n - 1]);
    char *keep = t[n - 1]; t[n - 1] = b.s;
    Reply rep = chat_call(c, rt->system, 0, rt->maxTokens, r, t, n);
    t[n - 1] = keep; free(b.s);
    snprintf(label, ln, "no web access: from memory");
    return rep;
  }
  if (route == ROUTE_DO) {
    if (c->agentTasks && llm_agent_available()) { // oh-my-pi does the job, with the conversation for context
      Buf q = {0};
      bstr(&q, "Reply in the same language as the request.\n\n");
      if (n > 1) {
        bstr(&q, "Earlier in this conversation:\n");
        for (int i = n - 1 > 6 ? n - 7 : 0; i < n - 1; i++) bfmt(&q, "%s: %.300s\n", !strcmp(r[i], "user") ? "User" : "Jelly", t[i]);
        bstr(&q, "\n");
      }
      bstr(&q, "Request: "); bstr(&q, t[n - 1]);
      Reply rep = run_agent(c, rt->system, q.s, c->agentSecs, NULL, c->agentApproval);
      free(q.s);
      if (rep.text) { snprintf(label, ln, "the agent did it"); return rep; }
      if (getenv("JELLY_DEBUG")) fprintf(stderr, "llm: agent task failed: %s\n", rep.err);
      snprintf(label, ln, "the agent couldn't: %.40s", rep.err);
      return rep;
    }
    // tasks are off (or omp is missing): say so instead of pretending
    Buf b = {0};
    bstr(&b, llm_agent_available() ? "(You can't act on the computer: tasks for oh-my-pi are turned off in LLM > Search. Say so "
                                     "in one short sentence and, if it helps, how the user could do it themselves.)\n\n"
                                   : "(You can't act on the computer: oh-my-pi isn't installed. Say so in one short sentence and, "
                                     "if it helps, how the user could do it themselves.)\n\n");
    bstr(&b, t[n - 1]);
    char *keep = t[n - 1]; t[n - 1] = b.s;
    const LlmRoute *qr = &c->route[ROUTE_QUICK];
    Reply rep = chat_call(c, qr->system, 0, qr->maxTokens, r, t, n);
    t[n - 1] = keep; free(b.s);
    snprintf(label, ln, "tasks are off");
    return rep;
  }
  snprintf(label, ln, "%s", route == ROUTE_THINK ? "thought it through" : "");
  return chat_call(c, rt->system, rt->thinking, rt->maxTokens, r, t, n);
}

static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mx);
  for (;;) {
    while (!job) pthread_cond_wait(&cv, &mx);
    int j = job & JOB_CHAT ? JOB_CHAT : job & JOB_OPT ? JOB_OPT : job & JOB_TEST ? JOB_TEST : job & JOB_ROUTER ? JOB_ROUTER
          : job & JOB_JEV ? JOB_JEV : job & JOB_SEARCH ? JOB_SEARCH : job & JOB_AGENT ? JOB_AGENT : JOB_MODELS;
    LlmCfg c = cfg; // a snapshot: the settings panel may keep editing
    if (j == JOB_CHAT && !npending) { job &= ~JOB_CHAT; continue; }
    if (j == JOB_CHAT) {
      char *q = pending[0]; // the oldest question joins the conversation now, so turns stay in order
      memmove(pending, pending + 1, sizeof pending[0] * (size_t)--npending);
      working = 1; workingQ = q;
      int myGen = gen;
      // memory: a conversation idle for longer than memoryMins starts afresh
      if (nturns && lastActivity && cfg.memoryMins > 0 && time(NULL) - lastActivity > (time_t)cfg.memoryMins * 60) {
        for (int i = 0; i < nturns; i++) free(texts[i]);
        nturns = 0; sessionId = 0;
      }
      lastActivity = time(NULL);
      if (!sessionId) sessionId = (long)time(NULL);
      if (nturns >= MAXTURNS) { // forget the oldest exchange
        free(texts[0]); free(texts[1]);
        memmove(roles, roles + 2, sizeof roles[0] * (MAXTURNS - 2)); memmove(texts, texts + 2, sizeof texts[0] * (MAXTURNS - 2));
        nturns -= 2;
      }
      roles[nturns] = "user"; texts[nturns++] = strdup(q);
      history_append("user", q);
      int n = nturns;
      const char *r[MAXTURNS]; char *t[MAXTURNS];
      int from = n > 24 ? n - 24 : 0; // keep the context reasonable
      if (from % 2) from++;           // start on a user turn
      for (int i = from; i < n; i++) { r[i - from] = roles[i]; t[i - from] = strdup(texts[i]); }
      pthread_mutex_unlock(&mx);
      int route = ROUTE_QUICK; char label[64] = "";
      Reply rep = answer(&c, r, t, n - from, &route, label, sizeof label);
      for (int i = 0; i < n - from; i++) free(t[i]);
      pthread_mutex_lock(&mx);
      working = 0; workingQ = NULL;
      if (getenv("JELLY_DEBUG")) fprintf(stderr, "llm: answered by route %d: %s (%.1f s)%s%s\n", route, label, rep.secs, rep.text ? "" : " error: ", rep.text ? "" : rep.err);
      if (myGen != gen) { free(rep.text); free(q); } // the conversation was closed meanwhile
      else {
        if (rep.text) {
          lastActivity = time(NULL);
          if (nturns < MAXTURNS) { roles[nturns] = "assistant"; texts[nturns++] = strdup(rep.text); }
          history_append("assistant", rep.text);
        } else if (nturns && !strcmp(roles[nturns - 1], "user")) free(texts[--nturns]); // let them try again
        if (nready == QMAX) { free(ready[0].q); free(ready[0].a); memmove(ready, ready + 1, sizeof ready[0] * (QMAX - 1)); nready--; }
        LlmReply *o = &ready[nready++];
        o->q = q; o->route = route; o->err = !rep.text;
        o->a = rep.text ? rep.text : strdup(rep.err);
        snprintf(o->label, sizeof o->label, "%s", label);
      }
      if (npending) continue; // next question (JOB_CHAT stays set)
    } else if (j == JOB_OPT) {
      char *draft = optDraft; optDraft = NULL;
      pthread_mutex_unlock(&mx);
      const char *r[1] = {"user"}; char *t[1] = {draft ? draft : ""};
      c.temperature = 0.3f;
      Reply rep = chat_call(&c, OPT_PROMPT, 0, 2000, r, t, 1);
      free(draft);
      pthread_mutex_lock(&mx);
      free(optResult); optResult = rep.text;
      if (rep.text) optState = 2; else { snprintf(optErr, sizeof optErr, "%s", rep.err); optState = 3; }
    } else if (j == JOB_TEST) {
      testState = TEST_RUNNING;
      pthread_mutex_unlock(&mx);
      const char *r[1] = {"user"}; char *t[1] = {"Reply with just the word: pong"};
      Reply rep = chat_call(&c, NULL, 0, 16, r, t, 1);
      pthread_mutex_lock(&mx);
      if (rep.text) { snprintf(testMsg, sizeof testMsg, "OK · %s · %.1f s", c.model, rep.secs); testState = TEST_OK; }
      else { snprintf(testMsg, sizeof testMsg, "%s", rep.err); testState = TEST_FAIL; }
      free(rep.text);
    } else if (j == JOB_ROUTER || j == JOB_JEV) {
      int jevOnly = j == JOB_JEV; // the jev tab's test: jev's own scores, not the whole decision
      char mine[400] = "";
      if (!jevOnly) { snprintf(mine, sizeof mine, "%s", routerTry); routerTry[0] = 0; }
      pthread_mutex_unlock(&mx);
      static const char *samples[] = {"hi jelly!", "is it going to rain in osaka", "help me split rent fairly with 3 roommates",
                                      "what is the btc price now?", "Prove that the square root of 2 is irrational."};
      static const char *names[] = {"Quick", "Think", "Research"};
      const char *one[1] = {mine};
      const char **list = *mine ? one : samples;
      int nl = *mine ? 1 : 5, ok = 1;
      Buf out = {0}; bput(&out, "", 0);
      for (int i = 0; i < nl; i++) {
        if (jevOnly) { // what jev alone says
          JevSays js = ask_jev(&c, list[i]);
          if (!js.ok) { ok = 0; bfmt(&out, "jev is not answering (%.200s)\n", c.routerUrl); break; }
          bfmt(&out, "\"%.40s\" → web %.2f%s  (%.2f s)\n", list[i], js.web, js.web >= c.routerCut ? " → Research" : "", js.secs);
        } else {
          char why[200];
          int rt = decide(&c, list[i], NULL, why, sizeof why);
          if (c.actionOn && strstr(why, "action model offline")) ok = 0;
          bfmt(&out, "\"%.40s\" → %s  (%s)\n", list[i], names[rt], why);
        }
      }
      pthread_mutex_lock(&mx);
      if (jevOnly) { jevState = !c.routerOn ? TEST_NONE : ok ? TEST_OK : TEST_FAIL; snprintf(jevMsg, sizeof jevMsg, "%.590s", out.s); }
      else { routerState = ok ? TEST_OK : TEST_FAIL; snprintf(routerMsg, sizeof routerMsg, "%.590s", out.s); }
      free(out.s);
    } else if (j == JOB_AGENT) {
      pthread_mutex_unlock(&mx);
      char model[256]; llm_agent_model(&c, model, sizeof model);
      Reply rep = run_agent(&c, NULL, "Reply with just the word: pong", 90, "read", 0);
      pthread_mutex_lock(&mx);
      if (rep.text) { agentState = TEST_OK; snprintf(agentMsg, sizeof agentMsg, "OK · %.120s · %.0f s", *model ? model : "omp default model", rep.secs); }
      else { agentState = TEST_FAIL; snprintf(agentMsg, sizeof agentMsg, "%s", rep.err); }
      free(rep.text);
    } else if (j == JOB_SEARCH) {
      searchState = TEST_RUNNING;
      pthread_mutex_unlock(&mx);
      Hit hits[6]; char err[200];
      double t0 = plat_now();
      int n = web_search(&c, "Bun JavaScript runtime", hits, 6, err, sizeof err);
      double secs = plat_now() - t0;
      pthread_mutex_lock(&mx);
      if (n > 0) { searchState = TEST_OK; snprintf(searchMsg, sizeof searchMsg, "OK · %d results · %.1f s · e.g. %.60s", n, secs, hits[0].title); }
      else { searchState = TEST_FAIL; snprintf(searchMsg, sizeof searchMsg, "%s", err); }
    } else { // model list
      pthread_mutex_unlock(&mx);
      char url[600]; snprintf(url, sizeof url, "%s", c.base);
      size_t ul = strlen(url); while (ul && url[ul - 1] == '/') url[--ul] = 0;
      strncat(url, "/models", sizeof url - strlen(url) - 1);
      char auth[400]; const char *h[1]; int nh = 0;
      if (*c.key) { snprintf(auth, sizeof auth, "Authorization: Bearer %s", c.key); h[nh++] = auth; }
      char *raw = NULL;
      http(url, h, nh, NULL, 10, &raw);
      static char got[64][128]; int n = 0;
      for (const char *p = raw; p && n < 64 && (p = strstr(p, "\"id\"")); ) {
        char *id = json_find_string(p, "id");
        if (id) { snprintf(got[n++], 128, "%s", id); free(id); }
        p += 4;
      }
      free(raw);
      pthread_mutex_lock(&mx);
      memcpy(models, got, sizeof got); nmodels = n;
    }
    job &= ~j;
  }
  return NULL;
}

void llm_start(void) {
  if (started) return;
  started = 1;
  cfg_load();
  // memory across restarts: a conversation from the last memoryMins carries on (from the saved history)
  if (cfg.memoryMins > 0 && cfg.saveHistory) {
    static LlmHist h[MAXTURNS];
    int n = llm_history(h, MAXTURNS);
    if (n && time(NULL) - h[n - 1].t < (time_t)cfg.memoryMins * 60) {
      int first = n - 1; while (first > 0 && h[first - 1].session == h[n - 1].session) first--;
      if (!h[first].user && first < n - 1) first++; // start on a question
      for (int i = first; i < n && nturns < MAXTURNS; i++) { roles[nturns] = h[i].user ? "user" : "assistant"; texts[nturns++] = strdup(h[i].text); }
      sessionId = h[n - 1].session; lastActivity = (time_t)h[n - 1].t;
    }
  }
  pthread_t th; pthread_create(&th, NULL, worker, NULL); pthread_detach(th);
  if (cfg.wsProvider != WS_NONE && *cfg.wsKey) llm_search_test(); // know early whether web search works
}

int llm_send(const char *text) {
  while (*text == ' ' || *text == '\n') text++;
  if (!*text) return 0;
  pthread_mutex_lock(&mx);
  int ok = npending < QMAX;
  if (ok) { pending[npending++] = strdup(text); job |= JOB_CHAT; pthread_cond_signal(&cv); }
  pthread_mutex_unlock(&mx);
  return ok;
}
int llm_waiting(void) { pthread_mutex_lock(&mx); int n = npending + working; pthread_mutex_unlock(&mx); return n; }
/* the questions not answered yet, the one in progress first */
int llm_pending_list(char out[][200], int max) {
  pthread_mutex_lock(&mx);
  int n = 0;
  if (workingQ && n < max) snprintf(out[n++], 200, "%s", workingQ);
  for (int i = 0; i < npending && n < max; i++) snprintf(out[n++], 200, "%s", pending[i]);
  pthread_mutex_unlock(&mx);
  return n;
}
int llm_ready(void) { pthread_mutex_lock(&mx); int n = nready; pthread_mutex_unlock(&mx); return n; }
/* the oldest unseen answer with its question: 1 = an answer, -1 = an error message in `a`, 0 = none yet */
int llm_take_reply(char *q, size_t qn, char *a, size_t an, char *label, size_t ln) {
  pthread_mutex_lock(&mx);
  int r = 0;
  if (nready) {
    LlmReply *o = &ready[0];
    snprintf(q, qn, "%s", o->q); snprintf(a, an, "%s", o->a); snprintf(label, ln, "%s", o->label);
    r = o->err ? -1 : 1;
    free(o->q); free(o->a);
    memmove(ready, ready + 1, sizeof ready[0] * (size_t)--nready);
  }
  pthread_mutex_unlock(&mx);
  return r;
}
int llm_turns(void) { pthread_mutex_lock(&mx); int n = nturns / 2; pthread_mutex_unlock(&mx); return n; }
void llm_end_session(void) {
  pthread_mutex_lock(&mx);
  for (int i = 0; i < nturns; i++) free(texts[i]);
  for (int i = 0; i < npending; i++) free(pending[i]);
  for (int i = 0; i < nready; i++) { free(ready[i].q); free(ready[i].a); }
  nturns = npending = nready = 0; sessionId = 0; gen++;
  pthread_mutex_unlock(&mx);
}
static void kick(int bits) { pthread_mutex_lock(&mx); job |= bits; pthread_cond_signal(&cv); pthread_mutex_unlock(&mx); }
void llm_test(void) {
  pthread_mutex_lock(&mx);
  testState = TEST_RUNNING; snprintf(testMsg, sizeof testMsg, "Testing…");
  pthread_mutex_unlock(&mx);
  kick(JOB_TEST | JOB_MODELS);
}
int llm_test_state(char *msg, size_t n) {
  pthread_mutex_lock(&mx);
  int s = testState;
  snprintf(msg, n, "%s", testState == TEST_NONE ? "Not tested yet" : testMsg);
  pthread_mutex_unlock(&mx);
  return s;
}
void llm_router_try(const char *msg) {
  pthread_mutex_lock(&mx); snprintf(routerTry, sizeof routerTry, "%s", msg ? msg : ""); pthread_mutex_unlock(&mx);
  llm_router_test();
}
void llm_agent_test(void) {
  pthread_mutex_lock(&mx); agentState = TEST_RUNNING; snprintf(agentMsg, sizeof agentMsg, "Asking the agent…"); pthread_mutex_unlock(&mx);
  kick(JOB_AGENT);
}
int llm_agent_state(char *msg, size_t n) {
  pthread_mutex_lock(&mx);
  int s = llm_agent_available() ? agentState : TEST_NONE;
  snprintf(msg, n, "%s", !llm_agent_available() ? "oh-my-pi (omp) is not installed" : agentState == TEST_NONE ? "Not tested yet" : agentMsg);
  pthread_mutex_unlock(&mx);
  return s;
}
int llm_curl_available(void) { char p[512]; return plat_find_exe("curl", p, sizeof p); }
void llm_jev_test(void) {
  pthread_mutex_lock(&mx); jevState = TEST_RUNNING; snprintf(jevMsg, sizeof jevMsg, "Asking jev…"); pthread_mutex_unlock(&mx);
  kick(JOB_JEV);
}
int llm_jev_state(char *msg, size_t n) {
  pthread_mutex_lock(&mx);
  int s = cfg.routerOn ? jevState : TEST_NONE;
  snprintf(msg, n, "%s", !cfg.routerOn ? "Off" : jevState == TEST_NONE ? "Not tested yet" : jevMsg);
  pthread_mutex_unlock(&mx);
  return s;
}
void llm_router_test(void) {
  pthread_mutex_lock(&mx); routerState = TEST_RUNNING; snprintf(routerMsg, sizeof routerMsg, "Asking jev…"); pthread_mutex_unlock(&mx);
  kick(JOB_ROUTER);
}
int llm_router_state(char *msg, size_t n) {
  pthread_mutex_lock(&mx); int s = routerState; snprintf(msg, n, "%s", routerMsg); pthread_mutex_unlock(&mx);
  return s;
}
void llm_search_test(void) {
  pthread_mutex_lock(&mx); searchState = TEST_RUNNING; snprintf(searchMsg, sizeof searchMsg, "Searching…"); pthread_mutex_unlock(&mx);
  kick(JOB_SEARCH);
}
int llm_search_state(char *msg, size_t n) {
  pthread_mutex_lock(&mx);
  int s = searchState;
  if (cfg.wsProvider == WS_NONE || !*cfg.wsKey) s = TEST_NONE;
  snprintf(msg, n, "%s", cfg.wsProvider == WS_NONE ? "Off: research questions go to the oh-my-pi agent" : !*cfg.wsKey ? "No API key yet: research questions go to the agent"
                         : searchState == TEST_NONE ? "Not tested yet" : searchMsg);
  pthread_mutex_unlock(&mx);
  return s;
}
void llm_optimize(const char *draft) {
  pthread_mutex_lock(&mx);
  if (optState != 1 && *draft) { free(optDraft); optDraft = strdup(draft); optState = 1; job |= JOB_OPT; pthread_cond_signal(&cv); }
  pthread_mutex_unlock(&mx);
}
int llm_optimize_poll(char *out, size_t n) {
  pthread_mutex_lock(&mx);
  int r = 0;
  if (optState == 2) { snprintf(out, n, "%s", optResult ? optResult : ""); optState = 0; r = 1; }
  else if (optState == 3) { snprintf(out, n, "%s", optErr); optState = 0; r = -1; }
  else if (optState == 1) r = 2;
  pthread_mutex_unlock(&mx);
  return r;
}
void llm_fetch_models(void) { kick(JOB_MODELS); }
int llm_models(char out[][128], int max) {
  pthread_mutex_lock(&mx);
  int n = nmodels < max ? nmodels : max;
  memcpy(out, models, (size_t)n * 128);
  pthread_mutex_unlock(&mx);
  return n;
}

/* ------------------------------------------------------------------ history */

int llm_history(LlmHist *out, int max) {
  char p[512]; path_of("chat-history.jsonl", p, sizeof p);
  FILE *f = fopen(p, "r");
  if (!f) return 0;
  // keep the newest `max` lines
  int n = 0, total = 0;
  static char line[1 << 17];
  LlmHist *ring = calloc((size_t)max, sizeof *ring);
  while (fgets(line, sizeof line, f)) {
    LlmHist h = {0};
    const char *t = strstr(line, "\"t\":"), *s = strstr(line, "\"s\":");
    if (t) h.t = strtol(t + 4, NULL, 10);
    if (s) h.session = strtol(s + 4, NULL, 10);
    char *r = json_find_string(line, "r"), *c = json_find_string(line, "c");
    if (r && c) {
      h.user = !strcmp(r, "user");
      snprintf(h.text, sizeof h.text, "%s", c);
      LlmHist *slot = &ring[total % max];
      *slot = h;
      total++;
    }
    free(r); free(c);
  }
  fclose(f);
  n = total < max ? total : max;
  for (int i = 0; i < n; i++) out[i] = ring[(total - n + i) % max];
  free(ring);
  return n;
}
void llm_history_delete(void) {
  char p[512]; path_of("chat-history.jsonl", p, sizeof p);
  remove(p);
}
