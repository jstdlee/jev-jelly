// The calendar's ICS parsing and recurrence expansion, across time zones.
#include "../src/core/calendar.c"
#include "check.h"

static int parse(const char *ics, time_t from, time_t to) {
  free(out_); out_ = NULL; nout_ = capout_ = 0; novr = 0;
  w0_ = from; w1_ = to;
  char *buf = strdup(ics);
  parse_ics(buf);
  free(buf);
  qsort(out_, (size_t)nout_, sizeof *out_, cmp_ev);
  return nout_;
}
static time_t utc(int y, int mo, int d, int h, int mi) {
  struct tm t; memset(&t, 0, sizeof t);
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi;
  return plat_timegm(&t);
}

void test_calendar(void) {
  time_t from = utc(2026, 1, 1, 0, 0), to = utc(2026, 12, 31, 0, 0);

  // a single UTC event with an escaped title and a location
  int n = parse("BEGIN:VCALENDAR\r\nX-WR-CALNAME:Home\r\nBEGIN:VEVENT\r\nUID:a\r\nSUMMARY:Lunch\\, then a walk\r\n"
                "LOCATION:Park\r\nDTSTART:20260310T120000Z\r\nDTEND:20260310T130000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", from, to);
  CHECK(n == 1, "one event, got %d", n);
  if (n == 1) {
    CHECK(out_[0].start == utc(2026, 3, 10, 12, 0), "start");
    CHECK(out_[0].end - out_[0].start == 3600, "an hour long");
    CHECK(!strcmp(out_[0].title, "Lunch, then a walk"), "title '%s'", out_[0].title);
    CHECK(!strcmp(curName, "Home"), "calendar name '%s'", curName);
  }

  // a TZID time: 09:00 in New York in July (EDT, UTC-4) is 13:00 UTC; in January (EST) 14:00
  n = parse("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:b\r\nSUMMARY:Standup\r\nDTSTART;TZID=America/New_York:20260701T090000\r\n"
            "DURATION:PT15M\r\nEND:VEVENT\r\nBEGIN:VEVENT\r\nUID:c\r\nSUMMARY:Winter\r\n"
            "DTSTART;TZID=America/New_York:20260115T090000\r\nDTEND;TZID=America/New_York:20260115T100000\r\nEND:VEVENT\r\n"
            "END:VCALENDAR\r\n", from, to);
  CHECK(n == 2, "two events, got %d", n);
  if (n == 2) {
    CHECK(out_[0].start == utc(2026, 1, 15, 14, 0), "EST start off by %ld s", (long)(out_[0].start - utc(2026, 1, 15, 14, 0)));
    CHECK(out_[1].start == utc(2026, 7, 1, 13, 0), "EDT start off by %ld s", (long)(out_[1].start - utc(2026, 7, 1, 13, 0)));
    CHECK(out_[1].end - out_[1].start == 900, "15 minutes");
  }

  // Outlook writes Windows zone names
  n = parse("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:d\r\nSUMMARY:Tokyo\r\nDTSTART;TZID=\"Tokyo Standard Time\":20260601T090000\r\n"
            "DURATION:PT1H\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", from, to);
  CHECK(n == 1 && out_[0].start == utc(2026, 6, 1, 0, 0), "Tokyo 09:00 is 00:00 UTC");

  // weekly on Tuesday and Thursday, 5 times, one of them removed with EXDATE
  n = parse("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:e\r\nSUMMARY:Gym\r\nDTSTART:20260303T180000Z\r\nDURATION:PT1H\r\n"
            "RRULE:FREQ=WEEKLY;BYDAY=TU,TH;COUNT=5\r\nEXDATE:20260305T180000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", from, to);
  CHECK(n == 4, "5 occurrences minus 1 exdate, got %d", n);
  if (n == 4) {
    CHECK(out_[0].start == utc(2026, 3, 3, 18, 0), "first Tue");
    CHECK(out_[1].start == utc(2026, 3, 10, 18, 0), "the Thu is skipped, then Tue");
    CHECK(out_[3].start == utc(2026, 3, 17, 18, 0), "last is Tue 17th");
  }

  // monthly on the last Friday, 3 times
  n = parse("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:f\r\nSUMMARY:Review\r\nDTSTART:20260130T150000Z\r\nDURATION:PT1H\r\n"
            "RRULE:FREQ=MONTHLY;BYDAY=-1FR;COUNT=3\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", from, to);
  CHECK(n == 3, "three last Fridays, got %d", n);
  if (n == 3) {
    CHECK(out_[1].start == utc(2026, 2, 27, 15, 0), "Feb 27");
    CHECK(out_[2].start == utc(2026, 3, 27, 15, 0), "Mar 27");
  }

  // an all-day event, and a cancelled one that must not show
  n = parse("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:g\r\nSUMMARY:Holiday\r\nDTSTART;VALUE=DATE:20260501\r\nEND:VEVENT\r\n"
            "BEGIN:VEVENT\r\nUID:h\r\nSUMMARY:Off\r\nSTATUS:CANCELLED\r\nDTSTART:20260502T100000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", from, to);
  CHECK(n == 1 && out_[0].allday && out_[0].end - out_[0].start == 86400, "one all-day event");
}

void test_platform(void) {
  CHECK(plat_zone_known("Europe/Berlin"), "Europe/Berlin is a known zone");
  CHECK(!plat_zone_known("Nowhere/Atlantis"), "a made-up zone is not");
  struct tm t; memset(&t, 0, sizeof t);
  t.tm_year = 2026 - 1900; t.tm_mon = 6; t.tm_mday = 1; t.tm_hour = 12; t.tm_isdst = -1;
  time_t berlin = plat_mktime_in(&t, "Europe/Berlin");
  CHECK(berlin == utc(2026, 7, 1, 10, 0), "12:00 in Berlin in July is 10:00 UTC (off by %ld s)", (long)(berlin - utc(2026, 7, 1, 10, 0)));
  char p[512]; plat_config_path("x.conf", p, sizeof p);
  CHECK(strstr(p, "jev-jelly") != NULL, "config path %s", p);
  char f[512]; FILE *tf = plat_temp_file(f, sizeof f);
  CHECK(tf != NULL, "temp file");
  if (tf) { fputs("x", tf); fclose(tf); remove(f); }
  double t0 = plat_now(); plat_sleep(0.02);
  CHECK(plat_now() - t0 >= 0.015, "sleep and the clock");
}
