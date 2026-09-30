// Calendar reminders: reads calendars from secret iCal (ICS) links (Google Calendar "Secret address in iCal
// format", Outlook, iCloud, Fastmail...), expands the next three months of events, recurring ones included, for the
// events list, and decides when the jelly should mention one from the next 48 hours. No OAuth: the link itself is the credential, so it's stored 0600.
//
// Fetching runs on a background thread through the curl binary (posix_spawn: forking a GL process is unsafe on
// some drivers). Times are converted with the system tz database by switching TZ under a lock.

#define _GNU_SOURCE
#include "jelly.h"
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MAXEV 800
#define WINDOW_BACK (60 * 60)       // keep events that started up to an hour ago (still "now")
#define WINDOW_AHEAD (92 * 24 * 60 * 60) // and everything in the next three months (reminders use 48 hours)

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;   // urls, events, status
static pthread_mutex_t tzmx = PTHREAD_MUTEX_INITIALIZER; // anything that reads or switches TZ
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
typedef struct {
  char url[CAL_URL_MAX], name[96];
  int enabled, ok, count, fetched;
} Src;
static Src src[CAL_MAX_URLS];
static int nsrc, wake, started;
static CalEvent evs[MAXEV];
static int nev;
static char status[160] = "No calendars yet";

/* ------------------------------------------------------------------ urls file */

static void urls_path(char *out, size_t n) {
  const char *h = getenv("HOME");
  snprintf(out, n, "%s/.config/jev-jelly/calendars", h ? h : ".");
}
static void urls_save(void) { // caller holds mx
  char p[512]; urls_path(p, sizeof p);
  FILE *f = fopen(p, "w");
  if (!f) return;
  chmod(p, 0600); // a secret iCal link is a password to your calendar
  for (int i = 0; i < nsrc; i++) fprintf(f, "%s%s\n", src[i].enabled ? "" : "!", src[i].url);
  fclose(f);
}
static void urls_load(void) {
  char p[512]; urls_path(p, sizeof p);
  FILE *f = fopen(p, "r");
  if (!f) return;
  char line[CAL_URL_MAX + 2];
  while (nsrc < CAL_MAX_URLS && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    int on = line[0] != '!';
    const char *u = on ? line : line + 1;
    if (strncmp(u, "http", 4)) continue;
    memset(&src[nsrc], 0, sizeof src[nsrc]);
    snprintf(src[nsrc].url, CAL_URL_MAX, "%.1023s", u);
    src[nsrc++].enabled = on;
  }
  fclose(f);
}

/* short, non-secret label for a link: host + last path segment */
static void link_label(const char *u, char *out, size_t n) {
  const char *h = strstr(u, "://"); h = h ? h + 3 : u;
  const char *slash = strchr(h, '/');
  const char *last = strrchr(u, '/');
  int hl = slash ? (int)(slash - h) : (int)strlen(h);
  if (last && last != slash && strlen(last) < 40) snprintf(out, n, "%.*s/…%s", hl, h, last);
  else snprintf(out, n, "%.*s/…", hl, h);
}

int cal_count(void) { pthread_mutex_lock(&mx); int n = nsrc; pthread_mutex_unlock(&mx); return n; }
int cal_info(int i, CalInfo *out) {
  pthread_mutex_lock(&mx);
  int ok = i >= 0 && i < nsrc;
  if (ok) {
    memset(out, 0, sizeof *out);
    if (*src[i].name) snprintf(out->label, sizeof out->label, "%s", src[i].name);
    else link_label(src[i].url, out->label, sizeof out->label);
    out->enabled = src[i].enabled; out->ok = src[i].ok; out->count = src[i].count; out->fetched = src[i].fetched;
  }
  pthread_mutex_unlock(&mx);
  return ok;
}
void cal_set_enabled(int i, int on) {
  pthread_mutex_lock(&mx);
  if (i >= 0 && i < nsrc && src[i].enabled != on) { src[i].enabled = on; urls_save(); wake = 1; pthread_cond_signal(&cv); }
  pthread_mutex_unlock(&mx);
}
int cal_add_url(const char *in) {
  char u[CAL_URL_MAX];
  while (isspace((unsigned char)*in)) in++;
  if (!strncmp(in, "webcal://", 9)) snprintf(u, sizeof u, "https://%s", in + 9);
  else snprintf(u, sizeof u, "%s", in);
  u[strcspn(u, " \t\r\n")] = 0;
  if (strncmp(u, "https://", 8) && strncmp(u, "http://", 7)) return 0;
  pthread_mutex_lock(&mx);
  int ok = nsrc < CAL_MAX_URLS;
  for (int i = 0; i < nsrc; i++) if (!strcmp(src[i].url, u)) ok = 0;
  if (ok) {
    memset(&src[nsrc], 0, sizeof src[nsrc]);
    snprintf(src[nsrc].url, CAL_URL_MAX, "%s", u);
    src[nsrc++].enabled = 1;
    urls_save(); wake = 1; pthread_cond_signal(&cv);
  }
  pthread_mutex_unlock(&mx);
  return ok;
}
void cal_remove_url(int i) {
  pthread_mutex_lock(&mx);
  if (i >= 0 && i < nsrc) {
    memmove(&src[i], &src[i + 1], (size_t)(nsrc - i - 1) * sizeof src[0]);
    nsrc--; urls_save(); wake = 1; pthread_cond_signal(&cv);
  }
  pthread_mutex_unlock(&mx);
}
void cal_refresh(void) { pthread_mutex_lock(&mx); wake = 1; pthread_cond_signal(&cv); pthread_mutex_unlock(&mx); }
void cal_status(char *out, size_t n) { pthread_mutex_lock(&mx); snprintf(out, n, "%s", status); pthread_mutex_unlock(&mx); }
int cal_events(CalEvent *out, int max) {
  pthread_mutex_lock(&mx);
  int n = nev < max ? nev : max;
  memcpy(out, evs, (size_t)n * sizeof *out);
  pthread_mutex_unlock(&mx);
  return n;
}

/* ------------------------------------------------------------------ time zones */

static const char *WIN_TZ[][2] = { // Outlook writes Windows zone names
    {"Pacific Standard Time", "America/Los_Angeles"}, {"Mountain Standard Time", "America/Denver"},
    {"Central Standard Time", "America/Chicago"},     {"Eastern Standard Time", "America/New_York"},
    {"GMT Standard Time", "Europe/London"},           {"W. Europe Standard Time", "Europe/Berlin"},
    {"Romance Standard Time", "Europe/Paris"},        {"China Standard Time", "Asia/Shanghai"},
    {"Singapore Standard Time", "Asia/Singapore"},    {"Tokyo Standard Time", "Asia/Tokyo"},
    {"Korea Standard Time", "Asia/Seoul"},            {"India Standard Time", "Asia/Kolkata"},
    {"AUS Eastern Standard Time", "Australia/Sydney"}, {"UTC", "UTC"}};

/* the IANA zone to use for a TZID, or NULL for the local zone */
static const char *zone_for(const char *tzid) {
  if (!tzid || !*tzid) return NULL;
  for (size_t i = 0; i < sizeof WIN_TZ / sizeof WIN_TZ[0]; i++)
    if (!strcmp(tzid, WIN_TZ[i][0])) return WIN_TZ[i][1];
  if (strstr(tzid, "..")) return NULL;
  char p[256]; snprintf(p, sizeof p, "/usr/share/zoneinfo/%s", tzid);
  return access(p, R_OK) == 0 ? tzid : NULL;
}

/* Everything that runs mktime for a zone happens inside one of these scopes, holding tzmx. */
static char savedTZ[256]; static int hadTZ, switched;
static void tz_enter(const char *zone) {
  pthread_mutex_lock(&tzmx);
  switched = 0;
  if (!zone) return;
  const char *o = getenv("TZ");
  hadTZ = o != NULL;
  if (hadTZ) snprintf(savedTZ, sizeof savedTZ, "%s", o);
  char z[300]; snprintf(z, sizeof z, ":%s", zone);
  setenv("TZ", z, 1); tzset(); switched = 1;
}
static void tz_leave(void) {
  if (switched) { if (hadTZ) setenv("TZ", savedTZ, 1); else unsetenv("TZ"); tzset(); }
  pthread_mutex_unlock(&tzmx);
}
void cal_localtime(time_t t, struct tm *out) {
  pthread_mutex_lock(&tzmx); localtime_r(&t, out); pthread_mutex_unlock(&tzmx);
}

/* ------------------------------------------------------------------ ICS parsing */

typedef struct {
  struct tm tm;
  char tz[64];
  int utc, date, ok;
} Dt;

typedef struct {
  char uid[160], title[128], where[96], rrule[256], desc[CAL_DESC_MAX];
  Dt ds, de;
  long dur; int hasDur;
  time_t ex[32]; int nex;
  Dt rid; int hasRid, cancelled;
} Raw;

static void unescape_nl(char *s, int keepLines) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      *o++ = (*p == 'n' || *p == 'N') ? (keepLines ? '\n' : ' ') : *p;
    } else *o++ = *p;
  }
  *o = 0;
}
static void unescape(char *s) { unescape_nl(s, 0); }

static Dt parse_dt(const char *params, const char *v) {
  Dt d; memset(&d, 0, sizeof d);
  const char *tz = params ? strstr(params, "TZID=") : NULL;
  if (tz) {
    tz += 5;
    if (*tz == '"') tz++;
    size_t n = strcspn(tz, "\";:");
    if (n >= sizeof d.tz) n = sizeof d.tz - 1;
    memcpy(d.tz, tz, n); d.tz[n] = 0;
  }
  int Y, M, D, h = 0, m = 0, s = 0;
  if (sscanf(v, "%4d%2d%2d", &Y, &M, &D) != 3) return d;
  if (v[8] == 'T' && sscanf(v + 9, "%2d%2d%2d", &h, &m, &s) >= 2) d.utc = strchr(v + 9, 'Z') != NULL;
  else d.date = 1;
  d.tm.tm_year = Y - 1900; d.tm.tm_mon = M - 1; d.tm.tm_mday = D;
  d.tm.tm_hour = h; d.tm.tm_min = m; d.tm.tm_sec = s; d.tm.tm_isdst = -1;
  d.ok = 1;
  return d;
}
static const char *dt_zone(const Dt *d) { return d->utc ? "UTC" : d->date ? NULL : zone_for(d->tz); }

/* inside a tz scope for dt_zone(d) */
static time_t dt_conv(struct tm tm, const Dt *d) {
  tm.tm_isdst = -1;
  return d->utc ? timegm(&tm) : mktime(&tm);
}
static time_t dt_time(const Dt *d) {
  tz_enter(dt_zone(d)); time_t t = dt_conv(d->tm, d); tz_leave(); return t;
}

static long parse_duration(const char *v) { // P1D, PT1H30M, P1W...
  long sign = 1, total = 0, n = 0; int inTime = 0;
  if (*v == '-') { sign = -1; v++; }
  else if (*v == '+') v++;
  for (; *v; v++) {
    if (isdigit((unsigned char)*v)) n = n * 10 + (*v - '0');
    else {
      switch (*v) {
      case 'T': inTime = 1; break;
      case 'W': total += n * 604800; break;
      case 'D': total += n * 86400; break;
      case 'H': total += n * 3600; break;
      case 'M': if (inTime) total += n * 60; break;
      case 'S': total += n; break;
      }
      n = 0;
    }
  }
  return sign * total;
}

/* override instances (RECURRENCE-ID) replace or cancel one occurrence of a series */
typedef struct { char uid[160]; time_t when; } Ovr;
static Ovr *ovr; static int novr, capovr;

static CalEvent *out_; static int nout_, capout_;
static int curCal;        // which calendar is being parsed
static char curName[96];  // its X-WR-CALNAME
static time_t w0_, w1_;

static void emit(const Raw *r, time_t start, long dur) {
  time_t end = start + (dur > 0 ? dur : r->ds.date ? 86400 : 0);
  if (end <= w0_ && end != start) return;
  if (end == start && start < w0_) return;
  if (start >= w1_) return;
  if (nout_ == capout_) { capout_ = capout_ ? capout_ * 2 : 64; out_ = realloc(out_, (size_t)capout_ * sizeof *out_); }
  CalEvent *e = &out_[nout_++];
  memset(e, 0, sizeof *e);
  snprintf(e->title, sizeof e->title, "%s", *r->title ? r->title : "(busy)");
  snprintf(e->where, sizeof e->where, "%s", r->where);
  snprintf(e->desc, sizeof e->desc, "%s", r->desc);
  e->cal = curCal;
  snprintf(e->uid, sizeof e->uid, "%s", r->uid);
  e->start = start; e->end = end; e->allday = r->ds.date;
}

static int overridden(const char *uid, time_t t) {
  for (int i = 0; i < novr; i++) if (ovr[i].when == t && !strcmp(ovr[i].uid, uid)) return 1;
  return 0;
}

static int wd_index(const char *s) {
  static const char *W[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  for (int i = 0; i < 7; i++) if (!strncmp(s, W[i], 2)) return i;
  return -1;
}

/* Expands a recurring event into the window. Supports FREQ DAILY/WEEKLY/MONTHLY/YEARLY with INTERVAL, COUNT,
   UNTIL, BYDAY (weekly sets and monthly "2TU" / "-1FR"), EXDATE and RECURRENCE-ID overrides. */
static void expand(const Raw *r, long dur) {
  char freq[16] = "", byday[128] = "";
  int interval = 1, count = 0;
  time_t until = 0;
  char rr[256]; snprintf(rr, sizeof rr, "%s", r->rrule);
  for (char *save, *tok = strtok_r(rr, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
    if (!strncmp(tok, "FREQ=", 5)) snprintf(freq, sizeof freq, "%s", tok + 5);
    else if (!strncmp(tok, "INTERVAL=", 9)) interval = atoi(tok + 9) > 0 ? atoi(tok + 9) : 1;
    else if (!strncmp(tok, "COUNT=", 6)) count = atoi(tok + 6);
    else if (!strncmp(tok, "UNTIL=", 6)) { Dt u = parse_dt(NULL, tok + 6); if (u.ok) { if (u.date) u.tm.tm_hour = 23, u.tm.tm_min = 59; until = dt_time(&u); } }
    else if (!strncmp(tok, "BYDAY=", 6)) snprintf(byday, sizeof byday, "%s", tok + 6);
  }
  int days[7] = {0}, anyDay = 0, ord = 0, ordDay = -1;
  for (char *save, *tok = strtok_r(byday, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    char *p = tok; int o = 0, sgn = 1;
    if (*p == '-') { sgn = -1; p++; } else if (*p == '+') p++;
    while (isdigit((unsigned char)*p)) o = o * 10 + (*p++ - '0');
    int w = wd_index(p);
    if (w < 0) continue;
    days[w] = 1; anyDay = 1;
    if (o && ordDay < 0) { ord = o * sgn; ordDay = w; }
  }

  const Dt *d = &r->ds;
  tz_enter(dt_zone(d));
  struct tm base = d->tm;
  time_t t0 = dt_conv(base, d);
  { struct tm tmp = base; tmp.tm_isdst = -1; mktime(&tmp); base.tm_wday = tmp.tm_wday; }
  int n = 0, guard = 0;
#define TRY(TM)                                                                           \
  do {                                                                                    \
    time_t t_ = dt_conv(TM, d);                                                           \
    if (t_ < t0) break;                                                                   \
    if ((count && n >= count) || (until && t_ > until) || t_ >= w1_) goto done;          \
    n++;                                                                                  \
    int skip_ = 0;                                                                        \
    for (int e_ = 0; e_ < r->nex; e_++) if (labs((long)(r->ex[e_] - t_)) < 60) skip_ = 1; \
    if (!skip_ && !overridden(r->uid, t_)) emit(r, t_, dur);                              \
  } while (0)

  if (!strcmp(freq, "DAILY")) {
    for (int k = 0; guard++ < 40000; k++) { struct tm tm = base; tm.tm_mday += k * interval; TRY(tm); }
  } else if (!strcmp(freq, "WEEKLY")) {
    if (!anyDay) days[base.tm_wday] = 1;
    int back = (base.tm_wday + 6) % 7; // weeks start on Monday
    for (int w = 0; guard++ < 20000; w++)
      for (int k = 0; k < 7; k++) {
        int wd = (k + 1) % 7; // MO..SU
        if (!days[wd]) continue;
        struct tm tm = base; tm.tm_mday += -back + w * 7 * interval + k;
        TRY(tm);
      }
  } else if (!strcmp(freq, "MONTHLY")) {
    for (int m = 0; guard++ < 5000; m++) {
      struct tm tm = base; tm.tm_mon += m * interval;
      if (ordDay >= 0) { // nth weekday of the month
        struct tm first = tm; first.tm_mday = 1; first.tm_isdst = -1; mktime(&first);
        if (ord > 0) tm.tm_mday = 1 + (ordDay - first.tm_wday + 7) % 7 + (ord - 1) * 7;
        else {
          struct tm last = tm; last.tm_mon += 1; last.tm_mday = 0; last.tm_isdst = -1; mktime(&last);
          tm.tm_mday = last.tm_mday - (last.tm_wday - ordDay + 7) % 7 + (ord + 1) * 7;
        }
        struct tm chk = tm; chk.tm_isdst = -1; mktime(&chk);
        if (chk.tm_mon != ((base.tm_mon + m * interval) % 12 + 12) % 12) continue;
      } else {
        struct tm chk = tm; chk.tm_isdst = -1; mktime(&chk);
        if (chk.tm_mday != base.tm_mday) continue; // no Feb 31st
      }
      TRY(tm);
    }
  } else if (!strcmp(freq, "YEARLY")) {
    for (int y = 0; guard++ < 400; y++) {
      struct tm tm = base; tm.tm_year += y * interval;
      struct tm chk = tm; chk.tm_isdst = -1; mktime(&chk);
      if (chk.tm_mday != base.tm_mday) continue;
      TRY(tm);
    }
  } else {
    emit(r, t0, dur);
  }
done:
#undef TRY
  tz_leave();
}

static void unfold(char *s) { // RFC 5545: a line starting with a space or tab continues the previous one
  char *o = s;
  for (char *p = s; *p;) {
    if (p[0] == '\r' && p[1] == '\n' && (p[2] == ' ' || p[2] == '\t')) { p += 3; continue; }
    if (p[0] == '\n' && (p[1] == ' ' || p[1] == '\t')) { p += 2; continue; }
    *o++ = *p++;
  }
  *o = 0;
}

static void parse_ics(char *text) {
  unfold(text);
  Raw *masters = NULL; int nm = 0, capm = 0;
  Raw cur; int in = 0;
  for (char *save, *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
    if (!strcmp(line, "BEGIN:VEVENT")) { memset(&cur, 0, sizeof cur); in = 1; continue; }
    if (!in && !strncmp(line, "X-WR-CALNAME:", 13)) { snprintf(curName, sizeof curName, "%s", line + 13); unescape(curName); continue; }
    if (!in) continue;
    if (!strcmp(line, "END:VEVENT")) {
      in = 0;
      if (!cur.ds.ok) continue;
      long dur = cur.hasDur ? cur.dur : cur.de.ok ? (long)(dt_time(&cur.de) - dt_time(&cur.ds)) : 0;
      if (cur.hasRid) { // this instance replaces one occurrence of its series
        if (novr == capovr) { capovr = capovr ? capovr * 2 : 32; ovr = realloc(ovr, (size_t)capovr * sizeof *ovr); }
        snprintf(ovr[novr].uid, sizeof ovr[novr].uid, "%s", cur.uid);
        ovr[novr++].when = dt_time(&cur.rid);
        if (!cur.cancelled) emit(&cur, dt_time(&cur.ds), dur);
      } else if (cur.cancelled) {
        continue;
      } else if (*cur.rrule) {
        cur.dur = dur;
        if (nm == capm) { capm = capm ? capm * 2 : 32; masters = realloc(masters, (size_t)capm * sizeof *masters); }
        masters[nm++] = cur;
      } else {
        emit(&cur, dt_time(&cur.ds), dur);
      }
      continue;
    }
    char *colon = strchr(line, ':');
    if (!colon) continue;
    *colon = 0;
    char *name = line, *val = colon + 1, *params = strchr(name, ';');
    if (params) *params++ = 0;
    if (!strcmp(name, "UID")) snprintf(cur.uid, sizeof cur.uid, "%s", val);
    else if (!strcmp(name, "SUMMARY")) { snprintf(cur.title, sizeof cur.title, "%s", val); unescape(cur.title); }
    else if (!strcmp(name, "LOCATION")) { snprintf(cur.where, sizeof cur.where, "%s", val); unescape(cur.where); }
    else if (!strcmp(name, "DESCRIPTION")) { snprintf(cur.desc, sizeof cur.desc, "%s", val); unescape_nl(cur.desc, 1); }
    else if (!strcmp(name, "DTSTART")) cur.ds = parse_dt(params, val);
    else if (!strcmp(name, "DTEND")) cur.de = parse_dt(params, val);
    else if (!strcmp(name, "DURATION")) { cur.dur = parse_duration(val); cur.hasDur = 1; }
    else if (!strcmp(name, "RRULE")) snprintf(cur.rrule, sizeof cur.rrule, "%s", val);
    else if (!strcmp(name, "RECURRENCE-ID")) { cur.rid = parse_dt(params, val); cur.hasRid = cur.rid.ok; }
    else if (!strcmp(name, "STATUS")) cur.cancelled = !strcmp(val, "CANCELLED");
    else if (!strcmp(name, "EXDATE"))
      for (char *s2, *tok = strtok_r(val, ",", &s2); tok && cur.nex < 32; tok = strtok_r(NULL, ",", &s2)) {
        Dt x = parse_dt(params, tok);
        if (x.ok) cur.ex[cur.nex++] = dt_time(&x);
      }
  }
  for (int i = 0; i < nm; i++) expand(&masters[i], masters[i].dur);
  free(masters);
}

/* ------------------------------------------------------------------ fetching */

static char *fetch(const char *url, int *err) {
  int fd[2];
  if (pipe(fd)) { *err = 1; return NULL; }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
  posix_spawn_file_actions_addclose(&fa, fd[0]);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  char *argv[] = {"curl", "-fsSL", "--compressed", "--max-time", "30", "--max-filesize", "33554432", "--", (char *)url, NULL};
  pid_t pid;
  int rc = posix_spawnp(&pid, "curl", &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fd[1]);
  if (rc) { close(fd[0]); *err = 2; return NULL; }
  size_t cap = 1 << 16, len = 0;
  char *buf = malloc(cap);
  ssize_t k;
  while ((k = read(fd[0], buf + len, cap - len - 1)) > 0) {
    len += (size_t)k;
    if (cap - len < 4096) { cap *= 2; buf = realloc(buf, cap); }
  }
  close(fd[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  buf[len] = 0;
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || !strstr(buf, "BEGIN:VCALENDAR")) { free(buf); *err = 3; return NULL; }
  *err = 0;
  return buf;
}

static int cmp_ev(const void *a, const void *b) {
  const CalEvent *x = a, *y = b;
  return x->start < y->start ? -1 : x->start > y->start;
}

static void *worker(void *arg) {
  (void)arg;
  static Src snap[CAL_MAX_URLS];
  static char names[CAL_MAX_URLS][96];
  static int oks[CAL_MAX_URLS], counts[CAL_MAX_URLS];
  for (;;) {
    pthread_mutex_lock(&mx);
    int n = nsrc;
    memcpy(snap, src, sizeof src);
    pthread_mutex_unlock(&mx);
    time_t now = time(NULL);
    w0_ = now - WINDOW_BACK; w1_ = now + WINDOW_AHEAD;
    nout_ = 0; novr = 0;
    int okc = 0, bad = 0, on = 0;
    for (int i = 0; i < n; i++) {
      oks[i] = 0; counts[i] = 0; names[i][0] = 0;
      if (!snap[i].enabled) continue;
      on++;
      int err;
      char *text = fetch(snap[i].url, &err);
      if (!text) { bad++; continue; }
      curCal = i; curName[0] = 0;
      parse_ics(text);
      free(text);
      snprintf(names[i], sizeof names[i], "%s", curName);
      oks[i] = 1; okc++;
    }
    qsort(out_, (size_t)nout_, sizeof *out_, cmp_ev);
    int k = 0; // drop duplicates (the same event shared into two calendars)
    for (int i = 0; i < nout_; i++) {
      if (k && out_[k - 1].start == out_[i].start && !strcmp(out_[k - 1].title, out_[i].title)) continue;
      out_[k++] = out_[i];
    }
    if (k > MAXEV) k = MAXEV;
    for (int i = 0; i < k; i++) counts[out_[i].cal]++;
    pthread_mutex_lock(&mx);
    for (int i = 0; i < n && i < nsrc; i++) // write back by link: the list may have changed while fetching
      for (int j = 0; j < nsrc; j++)
        if (!strcmp(src[j].url, snap[i].url)) {
          src[j].ok = oks[i]; src[j].count = counts[i]; src[j].fetched = snap[i].enabled;
          if (*names[i]) snprintf(src[j].name, sizeof src[j].name, "%.95s", names[i]);
          for (int e = 0; e < k; e++) if (out_[e].cal == i) out_[e].cal = j; // (indices settle on the next pass)
        }
    if (on == 0) { nev = 0; snprintf(status, sizeof status, n ? "All calendars are switched off" : "No calendars yet"); }
    else if (okc == 0) snprintf(status, sizeof status, "Couldn't read %s (check the link)", on > 1 ? "the calendars" : "the calendar");
    else {
      nev = k;
      memcpy(evs, out_, (size_t)nev * sizeof *evs);
      struct tm lt; cal_localtime(now, &lt);
      char hm[16]; strftime(hm, sizeof hm, "%H:%M", &lt);
      snprintf(status, sizeof status, "%d event%s in the next 3 months · updated %s%s", nev, nev == 1 ? "" : "s", hm,
               bad ? " · a link failed" : "");
    }
    if (getenv("JELLY_DEBUG")) {
      fprintf(stderr, "calendar: %s\n", status);
      for (int i = 0; i < nev && i < 20; i++) {
        struct tm lt; cal_localtime(evs[i].start, &lt);
        char b[32]; strftime(b, sizeof b, "%a %d %b %H:%M", &lt);
        fprintf(stderr, "  [%d] %s  %4ld min%s  %s%s\n", evs[i].cal, b, (long)(evs[i].end - evs[i].start) / 60,
                evs[i].allday ? " allday" : "", evs[i].title, *evs[i].desc ? "  (+desc)" : "");
      }
    }
    // wait ten minutes, or until the list of links changes
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 600;
    while (!wake) if (pthread_cond_timedwait(&cv, &mx, &until)) break;
    wake = 0;
    pthread_mutex_unlock(&mx);
  }
  return NULL;
}

void cal_start(void) {
  if (started) return;
  started = 1;
  urls_load();
  pthread_t th;
  pthread_create(&th, NULL, worker, NULL);
  pthread_detach(th);
}

/* ------------------------------------------------------------------ when to mention an event */

/* Reminders repeat, but never more than 3 times a day per event, and only while someone is at the desk:
     now   (15 min before until 5 min after the start): once, straight away;
     soon  (15-70 min before): once, at least 8 minutes after the previous bubble;
     heads-up (anything later in the next 48 h): again and again, at least 3 hours apart for the same event and
       20-40 minutes apart overall, up to 3 a day. On the event's own day only one heads-up, so that with "soon"
       and "now" it still adds up to 3.
   Dismissing a bubble doesn't stop it: it may come back later that day within those limits. */
typedef struct { char key[200]; int day, dayCount, stages; time_t lastAt; } Seen;
static Seen seen[160];
static int nseen;
static time_t lastShown, bootAt;
static int gapSec = 25 * 60;

static int day_of(time_t t) { struct tm lt; cal_localtime(t, &lt); return lt.tm_year * 1000 + lt.tm_yday; }

static Seen *seen_for(const CalEvent *e, int today) {
  char key[200]; snprintf(key, sizeof key, "%.150s@%ld", e->uid, (long)e->start);
  Seen *s = NULL;
  for (int i = 0; i < nseen && !s; i++) if (!strcmp(seen[i].key, key)) s = &seen[i];
  if (!s) {
    if (nseen == 160) { memmove(seen, seen + 40, sizeof(Seen) * 120); nseen = 120; }
    s = &seen[nseen++];
    memset(s, 0, sizeof *s);
    snprintf(s->key, sizeof s->key, "%s", key);
  }
  if (s->day != today) { s->day = today; s->dayCount = 0; } // a new day, a fresh allowance
  return s;
}

int cal_pick(time_t now, CalEvent *out) {
  if (!bootAt) bootAt = now;
  static CalEvent e[MAXEV];
  int n = cal_events(e, MAXEV);
  int today = day_of(now), best = -1, stage = 0;
  // 1) about to start: straight away
  for (int i = 0; i < n && best < 0; i++) {
    long dt = (long)(e[i].start - now);
    Seen *s = seen_for(&e[i], today);
    if (!e[i].allday && dt <= 15 * 60 && dt >= -5 * 60 && !(s->stages & 4) && s->dayCount < 3) best = i, stage = 4;
  }
  // 2) within the hour
  if (best < 0 && now - lastShown >= 8 * 60)
    for (int i = 0; i < n && best < 0; i++) {
      long dt = (long)(e[i].start - now);
      Seen *s = seen_for(&e[i], today);
      if (!e[i].allday && dt > 15 * 60 && dt <= 70 * 60 && !(s->stages & 2) && s->dayCount < 3) best = i, stage = 2;
    }
  // 3) heads-up for something later, now and then
  if (best < 0 && now - lastShown >= gapSec && now - bootAt >= 180) {
    int cand[3], nc = 0;
    for (int i = 0; i < n && nc < 3 && e[i].start - now <= 48 * 3600; i++) {
      long dt = (long)(e[i].start - now);
      int eligible = e[i].allday ? (e[i].end > now && dt <= 36 * 3600) : dt > 70 * 60;
      if (!eligible) continue;
      Seen *s = seen_for(&e[i], today);
      int onItsDay = day_of(e[i].start) == today && !e[i].allday;
      int allowance = onItsDay ? 1 : 3;
      if (s->dayCount < allowance && now - s->lastAt >= 3 * 3600) cand[nc++] = i;
    }
    if (nc) best = cand[rand() % nc], stage = 1;
  }
  if (best < 0) return 0;
  Seen *s = seen_for(&e[best], today);
  s->dayCount++; s->lastAt = now;
  if (stage > 1) s->stages |= stage;
  lastShown = now;
  gapSec = 20 * 60 + rand() % (20 * 60);
  *out = e[best];
  return 1;
}

/* "in 25 min", "now", "tomorrow 09:30", "today · all day" */
void cal_when(const CalEvent *e, time_t now, char *out, size_t n) {
  long dt = (long)(e->start - now);
  struct tm s, t; cal_localtime(e->start, &s); cal_localtime(now, &t);
  struct tm tomorrow = t; tomorrow.tm_mday += 1;
  pthread_mutex_lock(&tzmx); mktime(&tomorrow); pthread_mutex_unlock(&tzmx);
  int today = s.tm_yday == t.tm_yday && s.tm_year == t.tm_year;
  int tmrw = s.tm_yday == tomorrow.tm_yday && s.tm_year == tomorrow.tm_year;
  char hm[16]; strftime(hm, sizeof hm, "%H:%M", &s);
  if (e->allday) snprintf(out, n, "%s · all day", today || e->start <= now ? "today" : tmrw ? "tomorrow" : "soon");
  else if (dt <= 0 && now < e->end) snprintf(out, n, dt > -60 ? "starting now" : "started %ld min ago", -dt / 60);
  else if (dt < 60) snprintf(out, n, "in a moment");
  else if (dt < 3600) snprintf(out, n, "in %ld min", (dt + 59) / 60);
  else if (dt < 6 * 3600) snprintf(out, n, "in %ld h %02ld min · %s", dt / 3600, (dt % 3600) / 60, hm);
  else if (today) snprintf(out, n, "today %s", hm);
  else if (tmrw) snprintf(out, n, "tomorrow %s", hm);
  else { char d[32]; strftime(d, sizeof d, "%a %H:%M", &s); snprintf(out, n, "%s", d); }
}
