// The model client's pure parts: JSON reading, reasoning stripping, and routing without any server.
#include "../src/core/llm.c"
#include "check.h"

void test_llm(void) {
  // JSON strings: escapes, \u sequences and surrogate pairs become UTF-8
  const char *js = "{\"message\":{\"content\":\"hi \\\"there\\\"\\n\\u00e9\\u4f60\\ud83d\\ude00\"}}";
  char *s = json_find_string(js, "content");
  CHECK(s && !strcmp(s, "hi \"there\"\n\xc3\xa9\xe4\xbd\xa0\xf0\x9f\x98\x80"), "got '%s'", s ? s : "(null)");
  free(s);
  CHECK(json_find_num("{\"a\": 0.25}", "a") == 0.25, "number");
  CHECK(isnan(json_find_num("{\"a\": 1}", "b")), "missing number is NaN");

  // a JSON string written by bjson reads back the same
  Buf b = {0};
  bjson(&b, "tab\there \"q\" \\ \x01");
  const char *p = b.s;
  char *back = json_string(&p);
  CHECK(back && !strcmp(back, "tab\there \"q\" \\ \x01"), "round trip '%s'", back ? back : "(null)");
  free(back); free(b.s);

  // reasoning some models inline is removed from replies
  char t[] = "<think>let me see</think>\n  The answer is 4. ";
  strip_thinking(t);
  CHECK(!strcmp(t, "The answer is 4."), "stripped '%s'", t);

  // routing with no router model and no jev: cues and length decide
  LlmCfg c; memset(&c, 0, sizeof c);
  c.routerOn = 0; c.actionOn = 0; c.routerCut = 0.65f;
  char why[200];
  CHECK(decide(&c, "btc price", NULL, why, sizeof why) == ROUTE_RESEARCH, "btc price -> research (%s)", why);
  CHECK(decide(&c, "weather in tokyo tomorrow", NULL, why, sizeof why) == ROUTE_RESEARCH, "weather (%s)", why);
  CHECK(decide(&c, "比特币现在多少钱", NULL, why, sizeof why) == ROUTE_RESEARCH, "Chinese price question (%s)", why);
  CHECK(decide(&c, "Prove that sqrt 2 is irrational.", NULL, why, sizeof why) == ROUTE_THINK, "prove -> think (%s)", why);
  CHECK(decide(&c, "hi jelly!", NULL, why, sizeof why) == ROUTE_QUICK, "hi -> quick (%s)", why);
  CHECK(decide(&c, "how are you today?", NULL, why, sizeof why) == ROUTE_QUICK, "'today' in small talk stays quick (%s)", why);
  CHECK(decide(&c, "use ompi to check it", NULL, why, sizeof why) == ROUTE_RESEARCH, "naming omp -> research, so the agent can look (%s)", why);
  CHECK(decide(&c, "use omp to check tampines", NULL, why, sizeof why) == ROUTE_RESEARCH, "omp (%s)", why);
  CHECK(decide(&c, "compare postgres and sqlite", NULL, why, sizeof why) == ROUTE_THINK, "'compare' isn't 'omp' (%s)", why);
  char longMsg[400]; memset(longMsg, 'a', 300); longMsg[300] = 0;
  CHECK(decide(&c, longMsg, NULL, why, sizeof why) == ROUTE_THINK, "a long message -> think (%s)", why);

  // curl config quoting keeps a Windows path and quotes intact
  Buf q = {0};
  cfg_quote(&q, "C:\\Users\\me\\a \"b\".json");
  CHECK(!strcmp(q.s, "\"C:\\\\Users\\\\me\\\\a \\\"b\\\".json\""), "quoted %s", q.s);
  free(q.s);
}
