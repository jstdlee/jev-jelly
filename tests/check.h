// A tiny test harness: CHECK(cond, ...) prints the failure and counts it.
#pragma once
#include <stdio.h>
extern int tests_run, tests_failed;
#define CHECK(cond, ...)                                                                  \
  do {                                                                                    \
    tests_run++;                                                                          \
    if (!(cond)) {                                                                        \
      tests_failed++;                                                                     \
      fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                     \
      fprintf(stderr, __VA_ARGS__);                                                       \
      fputc('\n', stderr);                                                                \
    }                                                                                     \
  } while (0)
void test_llm(void);
void test_calendar(void);
void test_platform(void);
