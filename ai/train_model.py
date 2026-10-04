"""
============================================================================
 Machine Monitor — IA embarquée (TinyML) : entraînement du modèle
============================================================================
 Un petit réseau de neurones (4 → 16 → 16 → 1, ≈ 370 poids) apprend à quoi
 ressemble une machine qui fonctionne NORMALEMENT. Il donne pour chaque mesure
 un score d'anomalie entre 0 (normal) et 1 (jamais vu en fonctionnement sain).

 Méthode (détection de nouveauté par classification) :
   - données « normales » : modèle physique de la machine
     (échauffement ∝ courant², vibration liée à la charge, facteur de crête sain)
   - données « de fond » : mesures tirées au hasard dans toute la plage possible
   - le réseau apprend à séparer les deux → il connaît la FORME de la zone normale

 L'IA détecte ainsi des pannes qu'un simple seuil ne voit pas :
   - 50 °C alors que le moteur ne consomme rien (refroidissement en panne)
   - 3 A consommés mais température ambiante (capteur de température défaillant)
   - crête de vibration très supérieure au RMS (chocs : roulement abîmé)

 Le script n'utilise que NumPy et génère directement "tinyml_model.h" (C)
 pour l'ESP32. Usage :  pip install numpy  puis  python ai/train_model.py
============================================================================
"""
from pathlib import Path
import numpy as np

rng = np.random.default_rng(7)

# ---------------------------------------------------------------------------
# 1. Caractéristiques d'entrée (les mêmes calculs sont faits dans l'ESP32)
#    vibration en log : un facteur de crête constant devient une bande droite
# ---------------------------------------------------------------------------
def features(temp, rms, peak, current):
    return np.column_stack([temp, current, np.log10(rms + 0.01), np.log10(peak + 0.01)])

LO = np.array([0.0, 0.0, -2.0, -2.0])                    # plage physique couverte
HI = np.array([100.0, 8.0, np.log10(1.51), np.log10(3.01)])
scale = lambda f: (np.clip(f, LO, HI) - LO) / (HI - LO) * 2 - 1   # → [-1, 1]

def normal_data(n):
    ambient = rng.uniform(10, 35, n)                      # °C, atelier
    current = np.abs(4.0 * rng.uniform(0, 1, n) ** 0.7 + rng.normal(0, 0.05, n))
    temp = ambient + 1.8 * current**2 + rng.normal(0, 0.8, n)   # échauffement ∝ I²
    still = rng.random(n) < 0.3                           # à l'arrêt : vibration ≈ 0
    rms = np.where(still, rng.uniform(0, 0.005, n), rng.uniform(0.01, 0.12, n) + 0.02 * current)
    peak = rms * rng.uniform(1.3, 1.9, n)                 # facteur de crête sain
    return scale(features(temp, rms, peak, current))

def background(n):
    return rng.uniform(-1, 1, (n, 4))

N = 40000
Xn, Xb = normal_data(N), background(N)
X = np.vstack([Xn, Xb])
y = np.concatenate([np.ones(N), np.zeros(N)])            # 1 = normal

# ---------------------------------------------------------------------------
# 2. Réseau 4 → 16 → 8 → 1 (tanh, sortie sigmoïde), entraîné avec Adam
# ---------------------------------------------------------------------------
sizes = [4, 16, 16, 1]
W = [rng.normal(0, np.sqrt(1 / a), (a, b)) for a, b in zip(sizes, sizes[1:])]
B = [np.zeros(b) for b in sizes[1:]]
sigmoid = lambda z: 1 / (1 + np.exp(-z))

def forward(x):
    acts = [x]
    for i, (w, b) in enumerate(zip(W, B)):
        z = acts[-1] @ w + b
        acts.append(sigmoid(z) if i == len(W) - 1 else np.tanh(z))
    return acts

params = W + B
m = [np.zeros_like(p) for p in params]
v = [np.zeros_like(p) for p in params]
lr, step = 3e-3, 0
for epoch in range(200):
    if epoch == 120: lr = 1e-3                         # affinage final
    idx = rng.permutation(len(X))
    for s in range(0, len(X), 256):
        xb, yb = X[idx[s:s + 256]], y[idx[s:s + 256]]
        acts = forward(xb)
        grad = (acts[-1][:, 0] - yb)[:, None] / len(xb)   # dérivée entropie croisée
        gW, gB = [None] * len(W), [None] * len(W)
        for i in reversed(range(len(W))):
            gW[i] = acts[i].T @ grad
            gB[i] = grad.sum(0)
            if i:
                grad = (grad @ W[i].T) * (1 - acts[i] ** 2)
        step += 1
        for k, (p, g) in enumerate(zip(params, gW + gB)):
            m[k] = 0.9 * m[k] + 0.1 * g
            v[k] = 0.999 * v[k] + 0.001 * g * g
            p -= lr * (m[k] / (1 - 0.9**step)) / (np.sqrt(v[k] / (1 - 0.999**step)) + 1e-8)
    if epoch % 20 == 0:
        p = forward(X)[-1][:, 0]
        print(f"époque {epoch:3d}  précision = {np.mean((p > 0.5) == y) * 100:.1f} %")

def score(temp, rms, peak, current):
    """Score d'anomalie 0..1 (1 - probabilité « normal »)."""
    return 1 - forward(scale(features(np.atleast_1d(temp), np.atleast_1d(rms),
                                      np.atleast_1d(peak), np.atleast_1d(current))))[-1][:, 0]

# ---------------------------------------------------------------------------
# 3. Seuil : au plus 0,5 % de fausses alertes sur de nouvelles données normales
# ---------------------------------------------------------------------------
val = normal_data(10000)
val_scores = 1 - forward(val)[-1][:, 0]
threshold = float(max(0.5, np.percentile(val_scores, 99.5)))
print(f"\nseuil = {threshold:.3f}  → fausses alertes sur données normales : "
      f"{np.mean(val_scores > threshold) * 100:.2f} %")

tests = {
    "normal 24 °C, arrêt (Wokwi)":       (24, 0.0, 0.0, 0.0),
    "normal 26 °C, 1 A":                 (26, 0.05, 0.08, 1.0),
    "normal 40 °C, 2,5 A":               (40, 0.10, 0.16, 2.5),
    "refroidissement HS : 50 °C à 0 A":  (50, 0.0, 0.0, 0.0),
    "capteur temp. HS : 15 °C à 3 A":    (15, 0.10, 0.17, 3.0),
    "surchauffe naissante : 45 °C à 0 A": (45, 0.0, 0.0, 0.0),
    "chocs roulement : crête = 5×RMS":   (35, 0.10, 0.50, 1.5),
    "panne complète (bouton)":           (59, 0.70, 1.00, 2.5),
}
print("\nScénarios (seuils classiques : 60 °C / 0,3 g / 3,5 A → tous « normaux » sauf le dernier) :")
for name, x in tests.items():
    s = score(*x)[0]
    print(f"  {name:36s} score {s:5.3f}  → {'ANOMALIE' if s > threshold else 'normal'}")

# ---------------------------------------------------------------------------
# 4. Export en C pour l'ESP32
# ---------------------------------------------------------------------------
def carr(name, a):
    a = np.asarray(a, dtype=float).ravel()
    return f"static const float {name}[{a.size}] = {{{', '.join(f'{x:.6f}f' for x in a)}}};"

lines = [
    "// Généré automatiquement par ai/train_model.py — ne pas modifier à la main.",
    "// Réseau 4-16-16-1 (tanh, sigmoïde) : probabilité que la mesure soit « normale ».",
    "#pragma once",
    f"#define TINYML_THRESHOLD {threshold:.4f}f",
    f"#define TINYML_H1 {sizes[1]}",
    f"#define TINYML_H2 {sizes[2]}",
    carr("TINYML_LO", LO), carr("TINYML_HI", HI),
]
for i, (w, b) in enumerate(zip(W, B)):
    lines += [f"// couche {i} : {w.shape[0]} -> {w.shape[1]}  (W[entrée][sortie])",
              carr(f"TINYML_W{i}", w), carr(f"TINYML_B{i}", b)]
code = "\n".join(lines) + "\n"

root = Path(__file__).resolve().parent.parent
for target in ["firmware/esp32-bridge-vscode/include", "firmware/esp32-vscode/include",
               "firmware/esp32-wokwi", "ai"]:
    (root / target / "tinyml_model.h").write_text(code, encoding="utf-8")
    if target != "ai":                                   # moteur d'inférence (inchangé)
        (root / target / "tinyml.h").write_text((root / "ai" / "tinyml.h").read_text(encoding="utf-8"), encoding="utf-8")
print(f"\n✅ tinyml_model.h généré ({sum(p.size for p in params)} poids, "
      f"≈ {sum(p.size for p in params) * 4} octets)")

# Vecteurs de test pour vérifier que le code C donne les mêmes résultats
np.savetxt(root / "ai" / "test_vectors.csv",
           [[*x, score(*x)[0]] for x in tests.values()], delimiter=",", fmt="%.6f",
           header="temp,rms,peak,current,score_python", comments="")
