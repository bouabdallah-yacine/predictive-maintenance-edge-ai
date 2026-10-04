# 🤖 IA embarquée (TinyML) : détection d'anomalies sur l'ESP32

Un petit **réseau de neurones (4 → 16 → 16 → 1, 369 poids, ≈ 1,5 Ko)** tourne directement
dans l'ESP32 et donne, pour chaque mesure, un **score d'anomalie** entre 0 et 1.

## Pourquoi, en plus des seuils ?

Les seuils regardent chaque grandeur **séparément**. L'IA regarde la **combinaison** :

| Situation | Seuils (60 °C / 0,3 g / 3,5 A) | IA embarquée |
|---|---|---|
| 45 °C alors que le moteur ne consomme rien (refroidissement HS) | ✅ normal | 🚨 anomalie (température) |
| 15 °C alors que le moteur consomme 3 A (capteur défaillant) | ✅ normal | 🚨 anomalie (température) |
| Crête de vibration = 5 × RMS (chocs : roulement abîmé) | ✅ normal | 🚨 anomalie (vibration) |
| Fonctionnement normal (24 °C à l'arrêt, 40 °C à 2,5 A…) | ✅ normal | ✅ normal |

## Comment ça marche

1. **Données** : un modèle physique de la machine génère des mesures de fonctionnement
   normal (échauffement ∝ courant², vibration liée à la charge, facteur de crête sain).
2. **Entraînement** (`train_model.py`, NumPy seulement) : le réseau apprend à distinguer ces
   mesures normales de mesures tirées au hasard : il apprend la *forme* de la zone normale.
   Seuil : 0,5 → **0,04 % de fausses alertes** sur des données normales jamais vues.
3. **Export** : les poids sont écrits en C dans `tinyml_model.h`.
4. **Inférence** (`tinyml.h`, C pur, sans bibliothèque) : ≈ 600 multiplications par mesure.
   Anti-rebond (3 mesures consécutives) et **explication** : la grandeur qui, ramenée à une
   valeur normale, fait le plus baisser le score est donnée comme « cause probable ».
5. Le verdict part en MQTT (`"ai": {"score", "anomaly", "cause"}`) → alerte dans le
   dashboard et sur Telegram.

## Commandes

```bash
pip install numpy
python ai/train_model.py                                    # entraîne et copie le modèle dans les firmwares
gcc -O2 -Wall -I ai ai/test_tinyml.c -lm -o t && ./t        # vérifie : C == Python
```
