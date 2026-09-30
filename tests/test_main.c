// Core unit tests: no display, no network. `make test` (Linux) or run build/windows/test_core.exe (Windows).
#include "check.h"
#include <stdlib.h>
int tests_run, tests_failed;
int main(void) {
  test_platform();
  test_llm();
  test_calendar();
  printf("%d checks, %d failed\n", tests_run, tests_failed);
  return tests_failed ? 1 : 0;
}
