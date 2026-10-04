/* Vérifie que l'inférence C donne les mêmes scores que Python (ai/test_vectors.csv).
 * gcc -O2 -Wall -Wextra -I ai ai/test_tinyml.c -lm -o t && ./t */
#include <stdio.h>
#include "tinyml.h"

int main(void) {
  FILE *f = fopen("ai/test_vectors.csv", "r");
  if (!f) { puts("lance depuis la racine du projet"); return 1; }
  char line[256]; int n = 0, fail = 0;
  if (!fgets(line, sizeof line, f)) return 1;                 // en-tête
  float t, r, p, c, ref;
  while (fscanf(f, "%f,%f,%f,%f,%f", &t, &r, &p, &c, &ref) == 5) {
    float s = tinyml_score(t, r, p, c);
    int ok = fabsf(s - ref) < 1e-3f;
    printf("%5.1f °C  rms %.2f  crête %.2f  %.1f A  → C %.4f  Python %.4f  %s  %s\n",
           t, r, p, c, s, ref, s > TINYML_THRESHOLD ? "ANOMALIE" : "normal  ",
           s > TINYML_THRESHOLD ? TINYML_CAUSE_STR[tinyml_explain(t, r, p, c)] : "");
    n++; fail += !ok;
  }
  /* Anti-rebond : 2 mesures anormales isolées ne déclenchent rien, 3 oui */
  tinyml_state_t st = {0};
  tinyml_update(&st, 50, 0, 0, 0); tinyml_update(&st, 50, 0, 0, 0);
  int early = st.anomaly;
  tinyml_update(&st, 50, 0, 0, 0);
  int confirmed = st.anomaly;
  printf("\nanti-rebond : après 2 mesures %d, après 3 mesures %d (cause %s)\n",
         early, confirmed, TINYML_CAUSE_STR[st.cause]);
  fail += early != 0 || confirmed != 1;
  printf("%s : %d vecteurs, %d échec(s)\n", fail ? "ÉCHEC" : "OK", n, fail);
  return fail != 0;
}
