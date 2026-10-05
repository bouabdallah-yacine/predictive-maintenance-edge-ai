"""
============================================================================
 Machine Monitor — on-device AI (TinyML): model training
============================================================================
 A small neural network (4 → 16 → 16 → 1, ≈ 370 weights) learns what a
 NORMALLY running machine looks like. For each measurement it outputs an
 anomaly score between 0 (normal) and 1 (never seen during healthy operation).

 Method (novelty detection through classification):
   - "normal" data: physical model of the machine
     (heating ∝ current², load-dependent vibration, healthy crest factor)
   - "background" data: measurements drawn at random across the whole range
   - the network learns to separate the two → it knows the SHAPE of the normal zone

 The model therefore detects faults that a simple threshold misses:
   - 50 °C while the motor draws no current (cooling failure)
   - 3 A drawn but ambient temperature (faulty temperature sensor)
   - vibration peak far above the RMS (shocks: damaged bearing)

 The script only uses NumPy and directly generates "tinyml_model.h" (C)
 for the ESP32. Usage:  pip install numpy  then  python ai/train_model.py
============================================================================
"""
from pathlib import Path
import numpy as np

rng = np.random.default_rng(7)

# ---------------------------------------------------------------------------
# 1. Input features (the same computations are done on the ESP32)
#    log-scale vibration: a constant crest factor becomes a straight band
# ---------------------------------------------------------------------------
def features(temp, rms, peak, current):
    return np.column_stack([temp, current, np.log10(rms + 0.01), np.log10(peak + 0.01)])

LO = np.array([0.0, 0.0, -2.0, -2.0])                    # physical range covered
HI = np.array([100.0, 8.0, np.log10(1.51), np.log10(3.01)])
scale = lambda f: (np.clip(f, LO, HI) - LO) / (HI - LO) * 2 - 1   # → [-1, 1]

def normal_data(n):
    ambient = rng.uniform(10, 38, n)                      # °C, workshop
    current = np.abs(4.0 * rng.uniform(0, 1, n) ** 0.7 + rng.normal(0, 0.05, n))
    temp = ambient + 1.8 * current**2 + rng.normal(0, 0.8, n)   # heating ∝ I²
    still = rng.random(n) < 0.3                           # stopped: vibration ≈ 0
    rms = np.where(still, rng.uniform(0, 0.01, n), rng.uniform(0.01, 0.12, n) + 0.02 * current)
    crest = np.where(still, rng.uniform(1.0, 4.0, n),     # sensor noise while stopped
                     rng.uniform(1.3, 1.9, n))            # healthy crest factor while running
    peak = rms * crest
    return scale(features(temp, rms, peak, current))

def background(n):
    return rng.uniform(-1, 1, (n, 4))

N = 40000
Xn, Xb = normal_data(N), background(N)
X = np.vstack([Xn, Xb])
y = np.concatenate([np.ones(N), np.zeros(N)])            # 1 = normal

# ---------------------------------------------------------------------------
# 2. 4 → 16 → 16 → 1 network (tanh, sigmoid output), trained with Adam
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
    if epoch == 120: lr = 1e-3                         # final fine-tuning
    idx = rng.permutation(len(X))
    for s in range(0, len(X), 256):
        xb, yb = X[idx[s:s + 256]], y[idx[s:s + 256]]
        acts = forward(xb)
        grad = (acts[-1][:, 0] - yb)[:, None] / len(xb)   # cross-entropy derivative
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
        print(f"epoch {epoch:3d}  accuracy = {np.mean((p > 0.5) == y) * 100:.1f} %")

def score(temp, rms, peak, current):
    """Anomaly score 0..1 (1 - probability of "normal")."""
    return 1 - forward(scale(features(np.atleast_1d(temp), np.atleast_1d(rms),
                                      np.atleast_1d(peak), np.atleast_1d(current))))[-1][:, 0]

# ---------------------------------------------------------------------------
# 3. Threshold: at most 0.5 % false alarms on new normal data
# ---------------------------------------------------------------------------
val = normal_data(10000)
val_scores = 1 - forward(val)[-1][:, 0]
threshold = float(max(0.5, np.percentile(val_scores, 99.5)))
print(f"\nthreshold = {threshold:.3f}  → false alarms on normal data: "
      f"{np.mean(val_scores > threshold) * 100:.2f} %")

tests = {
    "normal 24 °C, stopped (Wokwi)":     (24, 0.0, 0.0, 0.0),
    "normal 26 °C, 1 A":                 (26, 0.05, 0.08, 1.0),
    "normal 40 °C, 2.5 A":               (40, 0.10, 0.16, 2.5),
    "cooling failed: 50 °C at 0 A":      (50, 0.0, 0.0, 0.0),
    "temp. sensor failed: 15 °C at 3 A": (15, 0.10, 0.17, 3.0),
    "early overheating: 45 °C at 0 A":   (45, 0.0, 0.0, 0.0),
    "bearing shocks: peak = 5×RMS":      (35, 0.10, 0.50, 1.5),
    "full fault (button)":               (59, 0.70, 1.00, 2.5),
}
print("\nScenarios (classic thresholds: 60 °C / 0.3 g / 3.5 A → all \"normal\" except the last one):")
for name, x in tests.items():
    s = score(*x)[0]
    print(f"  {name:36s} score {s:5.3f}  → {'ANOMALY' if s > threshold else 'normal'}")

# ---------------------------------------------------------------------------
# 4. C export for the ESP32
# ---------------------------------------------------------------------------
def carr(name, a):
    a = np.asarray(a, dtype=float).ravel()
    return f"static const float {name}[{a.size}] = {{{', '.join(f'{x:.6f}f' for x in a)}}};"

lines = [
    "// Auto-generated by ai/train_model.py — do not edit by hand.",
    "// 4-16-16-1 network (tanh, sigmoid): probability that the measurement is \"normal\".",
    "#pragma once",
    f"#define TINYML_THRESHOLD {threshold:.4f}f",
    f"#define TINYML_H1 {sizes[1]}",
    f"#define TINYML_H2 {sizes[2]}",
    carr("TINYML_LO", LO), carr("TINYML_HI", HI),
]
for i, (w, b) in enumerate(zip(W, B)):
    lines += [f"// layer {i}: {w.shape[0]} -> {w.shape[1]}  (W[input][output])",
              carr(f"TINYML_W{i}", w), carr(f"TINYML_B{i}", b)]
code = "\n".join(lines) + "\n"

root = Path(__file__).resolve().parent.parent
for target in ["firmware/esp32-bridge-vscode/include", "firmware/esp32-vscode/include",
               "firmware/esp32-wokwi", "ai"]:
    (root / target / "tinyml_model.h").write_text(code, encoding="utf-8")
    if target != "ai":                                   # inference engine (unchanged)
        (root / target / "tinyml.h").write_text((root / "ai" / "tinyml.h").read_text(encoding="utf-8"), encoding="utf-8")
print(f"\n✅ tinyml_model.h generated ({sum(p.size for p in params)} weights, "
      f"≈ {sum(p.size for p in params) * 4} bytes)")

# Test vectors to check that the C code gives the same results
np.savetxt(root / "ai" / "test_vectors.csv",
           [[*x, score(*x)[0]] for x in tests.values()], delimiter=",", fmt="%.6f",
           header="temp,rms,peak,current,score_python", comments="")
