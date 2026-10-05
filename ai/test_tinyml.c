/* Checks that the C inference gives the same scores as Python (ai/test_vectors.csv).
 * gcc -O2 -Wall -Wextra -I ai ai/test_tinyml.c -lm -o t && ./t */
#include <stdio.h>
#include "tinyml.h"

int main(void) {
  FILE *f = fopen("ai/test_vectors.csv", "r");
  if (!f) { puts("run from the project root"); return 1; }
  char line[256]; int n = 0, fail = 0;
  if (!fgets(line, sizeof line, f)) return 1;                 // header
  float t, r, p, c, ref;
  while (fscanf(f, "%f,%f,%f,%f,%f", &t, &r, &p, &c, &ref) == 5) {
    float s = tinyml_score(t, r, p, c);
    int ok = fabsf(s - ref) < 1e-3f;
    printf("%5.1f °C  rms %.2f  peak %.2f  %.1f A  → C %.4f  Python %.4f  %s  %s\n",
           t, r, p, c, s, ref, s > TINYML_THRESHOLD ? "ANOMALY " : "normal  ",
           s > TINYML_THRESHOLD ? TINYML_CAUSE_STR[tinyml_explain(t, r, p, c)] : "");
    n++; fail += !ok;
  }
  /* Debounce: 2 isolated abnormal measurements trigger nothing, 3 do */
  tinyml_state_t st = {0};
  tinyml_update(&st, 50, 0, 0, 0); tinyml_update(&st, 50, 0, 0, 0);
  int early = st.anomaly;
  tinyml_update(&st, 50, 0, 0, 0);
  int confirmed = st.anomaly;
  printf("\ndebounce: after 2 measurements %d, after 3 measurements %d (cause %s)\n",
         early, confirmed, TINYML_CAUSE_STR[st.cause]);
  fail += early != 0 || confirmed != 1;
  printf("%s: %d vectors, %d failure(s)\n", fail ? "FAIL" : "OK", n, fail);
  return fail != 0;
}
