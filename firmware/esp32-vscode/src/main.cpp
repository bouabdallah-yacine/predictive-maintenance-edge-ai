#include <Arduino.h>
/*
 * ============================================================================
 *  Machine Monitor — Nœud ESP32 (simulable sur Wokwi)
 * ============================================================================
 *  Surveillance d'une machine industrielle : température, vibration, courant.
 *  Architecture temps réel FreeRTOS :
 *
 *    taskVibration (100 Hz)  ─┐
 *    taskEnvironment (5 Hz)  ─┼─► g_sensors (mutex) ─► taskAnalysis (1 Hz)
 *                             │                         │  machine à états
 *    ISR bouton ─► sémaphore ─► taskButton              ├─► LEDs / buzzer
 *                                                       └─► xTelemetryQueue
 *                                                              │
 *    taskDisplay (2 Hz) ◄─ g_last (mutex)       taskMqtt ◄────┘  Wi-Fi + MQTT
 *
 *  Le bus I2C est partagé entre le MPU6050 et l'OLED → protégé par un mutex.
 *  La file de télémétrie sert aussi de tampon quand le Wi-Fi est coupé.
 *
 *  Câblage (voir diagram.json) :
 *    DHT22 -> GPIO15 | MPU6050 + SSD1306 -> I2C (SDA 21, SCL 22)
 *    Potentiomètre (simule ACS712) -> GPIO34 (ADC1_CH6)
 *    LED verte 25, LED jaune 26, LED rouge 27, buzzer 14, bouton 13
 * ============================================================================
 */
#include <WiFi.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ----------------------------------------------------------------------------
//  Configuration (à adapter)
// ----------------------------------------------------------------------------
#define WIFI_SSID      "Wokwi-GUEST"   // réseau Wi-Fi simulé de Wokwi
#define WIFI_PASS      ""
#define MQTT_HOST      "broker.hivemq.com"
#define MQTT_PORT      1883
#define DEVICE_ID      "machine01"
// ⚠️ Broker public : choisis un préfixe unique (le même dans backend/.env)
#define TOPIC_PREFIX   "pfe-monitor-7f3a"

// Brochage
#define PIN_DHT        15
#define PIN_CURRENT    34
#define PIN_LED_OK     25
#define PIN_LED_WARN   26
#define PIN_LED_ALARM  27
#define PIN_BUZZER     14
#define PIN_BUTTON     13

// Seuils (warning / critique) + hystérésis
#define TEMP_WARN      60.0f   // °C
#define TEMP_CRIT      75.0f
#define TEMP_HYST       2.0f
#define VIB_WARN        0.30f  // g RMS
#define VIB_CRIT        0.60f
#define VIB_HYST        0.05f
#define CURR_WARN       3.5f   // A
#define CURR_CRIT       4.5f
#define CURR_HYST       0.2f

// ACS712-05B : 185 mV/A, alimenté en 5 V. Sur ESP32 (3,3 V) on passe par un
// pont diviseur 3,3/5 → zéro à 1,65 V et sensibilité 185*0,66 = 122 mV/A.
#define ACS_ZERO_V      1.65f
#define ACS_SENS_V_A    0.1221f

// Échantillonnage vibration
#define VIB_FS_HZ       100
#define VIB_WINDOW      100    // 1 s de données par fenêtre RMS

// ----------------------------------------------------------------------------
//  Types partagés
// ----------------------------------------------------------------------------
enum Level : uint8_t { LVL_NORMAL = 0, LVL_WARNING = 1, LVL_CRITICAL = 2 };
static const char *LEVEL_STR[] = {"NORMAL", "WARNING", "CRITICAL"};

struct SensorData {
  float temperature;   // °C
  float humidity;      // %
  float vibRms;        // g (composante dynamique)
  float vibPeak;       // g
  float current;       // A
  bool  dhtOk;
  bool  mpuOk;
};

struct Telemetry {
  uint32_t seq;
  uint32_t uptimeMs;
  SensorData s;
  Level tempLvl, vibLvl, currLvl, global;
  bool faultInjected;
};

// ----------------------------------------------------------------------------
//  Objets globaux / primitives RTOS
// ----------------------------------------------------------------------------
DHTesp dht;
Adafruit_MPU6050 mpu;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

SemaphoreHandle_t i2cMutex;      // bus I2C partagé MPU/OLED
SemaphoreHandle_t dataMutex;     // protège g_sensors et g_last
SemaphoreHandle_t buttonSem;     // donné par l'ISR du bouton
QueueHandle_t     telemetryQueue;

SensorData g_sensors = {};
Telemetry  g_last    = {};
volatile bool g_faultMode  = false;  // injection de panne (bouton ou MQTT)
volatile bool g_buzzerMute = false;
volatile bool g_mqttUp     = false;

char topicTelemetry[64], topicCmd[64], topicStatus[64];

// ----------------------------------------------------------------------------
//  Utilitaires
// ----------------------------------------------------------------------------
static Level evalLevel(float v, float warn, float crit, float hyst, Level prev) {
  // Hystérésis : on ne redescend d'un niveau que si la valeur passe sous
  // (seuil - hyst). Évite le clignotement des alarmes autour du seuil.
  if (v >= crit) return LVL_CRITICAL;
  if (prev == LVL_CRITICAL && v >= crit - hyst) return LVL_CRITICAL;
  if (v >= warn) return LVL_WARNING;
  if (prev >= LVL_WARNING && v >= warn - hyst) return LVL_WARNING;
  return LVL_NORMAL;
}

static void IRAM_ATTR onButtonISR() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(buttonSem, &woken);
  portYIELD_FROM_ISR(woken);
}

// ----------------------------------------------------------------------------
//  Tâche : acquisition vibration (MPU6050, 100 Hz, RMS glissant sur 1 s)
// ----------------------------------------------------------------------------
void taskVibration(void *) {
  static float window[VIB_WINDOW];
  uint16_t idx = 0;
  uint32_t n = 0;
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / VIB_FS_HZ);

  for (;;) {
    vTaskDelayUntil(&last, period);   // période stricte, sans dérive
    sensors_event_t a, g, t;
    bool ok = false;
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      ok = mpu.getEvent(&a, &g, &t);
      xSemaphoreGive(i2cMutex);
    }
    if (!ok) continue;

    // Norme de l'accélération en g, moins la gravité → partie dynamique
    float mag = sqrtf(a.acceleration.x * a.acceleration.x +
                      a.acceleration.y * a.acceleration.y +
                      a.acceleration.z * a.acceleration.z) / 9.80665f;
    float dyn = mag - 1.0f;

    if (g_faultMode) {
      // Panne simulée : balourd à 25 Hz + bruit
      dyn += 0.9f * sinf(2.0f * PI * 25.0f * n / VIB_FS_HZ) +
             0.1f * ((float)random(-100, 100) / 100.0f);
    }
    window[idx] = dyn;
    idx = (idx + 1) % VIB_WINDOW;
    n++;

    if (idx == 0) {                    // fenêtre pleine → RMS + crête
      // RMS de la composante alternative : on retire la moyenne de la
      // fenêtre (gravité, inclinaison, offset du capteur) pour ne garder
      // que la vibration réelle.
      float mean = 0, sum = 0, peak = 0;
      for (int i = 0; i < VIB_WINDOW; i++) mean += window[i];
      mean /= VIB_WINDOW;
      for (int i = 0; i < VIB_WINDOW; i++) {
        float d = window[i] - mean;
        sum += d * d;
        peak = fmaxf(peak, fabsf(d));
      }
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      g_sensors.vibRms  = sqrtf(sum / VIB_WINDOW);
      g_sensors.vibPeak = peak;
      g_sensors.mpuOk   = true;
      xSemaphoreGive(dataMutex);
    }
  }
}

// ----------------------------------------------------------------------------
//  Tâche : température/humidité (DHT22) + courant (ADC)
// ----------------------------------------------------------------------------
void taskEnvironment(void *) {
  uint8_t tick = 0;
  for (;;) {
    // Courant : moyenne de 32 échantillons ADC (filtre le bruit)
    uint32_t mv = 0;
    for (int i = 0; i < 32; i++) mv += analogReadMilliVolts(PIN_CURRENT);
    float volts   = (mv / 32.0f) / 1000.0f;
    float current = fabsf((volts - ACS_ZERO_V) / ACS_SENS_V_A);
    if (g_faultMode) current += 2.5f;   // surintensité simulée

    // DHT22 : pas plus d'une lecture toutes les 2 s
    TempAndHumidity th = {NAN, NAN};
    bool readDht = (tick % 10 == 0);
    if (readDht) th = dht.getTempAndHumidity();

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    g_sensors.current = current;
    if (readDht) {
      g_sensors.dhtOk = (dht.getStatus() == DHTesp::ERROR_NONE);
      if (g_sensors.dhtOk) {
        g_sensors.temperature = th.temperature + (g_faultMode ? 35.0f : 0.0f);
        g_sensors.humidity    = th.humidity;
      }
    }
    xSemaphoreGive(dataMutex);

    tick++;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ----------------------------------------------------------------------------
//  Tâche : analyse (1 Hz) → niveaux d'alerte, actionneurs, file de télémétrie
// ----------------------------------------------------------------------------
void taskAnalysis(void *) {
  uint32_t seq = 0;
  Level tL = LVL_NORMAL, vL = LVL_NORMAL, cL = LVL_NORMAL;
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));

    Telemetry t = {};
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    t.s = g_sensors;
    xSemaphoreGive(dataMutex);

    tL = evalLevel(t.s.temperature, TEMP_WARN, TEMP_CRIT, TEMP_HYST, tL);
    vL = evalLevel(t.s.vibRms,      VIB_WARN,  VIB_CRIT,  VIB_HYST,  vL);
    cL = evalLevel(t.s.current,     CURR_WARN, CURR_CRIT, CURR_HYST, cL);

    t.seq = seq++;
    t.uptimeMs = millis();
    t.tempLvl = tL; t.vibLvl = vL; t.currLvl = cL;
    t.global = (Level)max((int)tL, max((int)vL, (int)cL));
    t.faultInjected = g_faultMode;

    // Actionneurs locaux : l'alarme fonctionne même sans réseau
    digitalWrite(PIN_LED_OK,    t.global == LVL_NORMAL);
    digitalWrite(PIN_LED_WARN,  t.global == LVL_WARNING);
    digitalWrite(PIN_LED_ALARM, t.global == LVL_CRITICAL);
    if (t.global == LVL_CRITICAL && !g_buzzerMute) tone(PIN_BUZZER, 2000, 300);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    g_last = t;
    xSemaphoreGive(dataMutex);

    // File pleine (réseau coupé longtemps) → on jette la plus ancienne mesure
    if (xQueueSend(telemetryQueue, &t, 0) != pdTRUE) {
      Telemetry dropped;
      xQueueReceive(telemetryQueue, &dropped, 0);
      xQueueSend(telemetryQueue, &t, 0);
    }
  }
}

// ----------------------------------------------------------------------------
//  Tâche : bouton "injection de panne" (anti-rebond logiciel)
// ----------------------------------------------------------------------------
void taskButton(void *) {
  for (;;) {
    if (xSemaphoreTake(buttonSem, portMAX_DELAY) == pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (digitalRead(PIN_BUTTON) == LOW) {
        g_faultMode = !g_faultMode;
        Serial.printf("[BTN] Injection de panne : %s\n", g_faultMode ? "ON" : "OFF");
      }
      while (xSemaphoreTake(buttonSem, 0) == pdTRUE) {}  // purge des rebonds
    }
  }
}

// ----------------------------------------------------------------------------
//  Tâche : affichage OLED (2 Hz)
// ----------------------------------------------------------------------------
void taskDisplay(void *) {
  for (;;) {
    Telemetry t;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    t = g_last;
    xSemaphoreGive(dataMutex);

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      oled.clearDisplay();
      oled.setTextSize(1);
      oled.setTextColor(SSD1306_WHITE);
      oled.setCursor(0, 0);
      oled.printf("%s  %s %s\n", DEVICE_ID, g_mqttUp ? "MQTT" : "----",
                  t.faultInjected ? "SIM!" : "");
      oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
      oled.setCursor(0, 13);
      oled.printf("Temp : %5.1f C  %c\n", t.s.temperature, "OWC"[t.tempLvl]);
      oled.printf("Hum  : %5.1f %%\n", t.s.humidity);
      oled.printf("Vib  : %5.2f g   %c\n", t.s.vibRms, "OWC"[t.vibLvl]);
      oled.printf("Cour.: %5.2f A   %c\n", t.s.current, "OWC"[t.currLvl]);
      oled.setTextSize(1);
      oled.setCursor(0, 54);
      oled.printf("Etat: %s", LEVEL_STR[t.global]);
      oled.display();
      xSemaphoreGive(i2cMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ----------------------------------------------------------------------------
//  MQTT : commandes reçues (topic .../cmd)
//    "fault_on" | "fault_off" | "mute" | "unmute"
// ----------------------------------------------------------------------------
void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  String cmd;
  for (unsigned i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.printf("[MQTT] Commande reçue : %s\n", cmd.c_str());
  if (cmd == "fault_on")  g_faultMode = true;
  else if (cmd == "fault_off") g_faultMode = false;
  else if (cmd == "mute")   g_buzzerMute = true;
  else if (cmd == "unmute") g_buzzerMute = false;
}

static bool publishTelemetry(const Telemetry &t) {
  JsonDocument doc;
  doc["deviceId"]    = DEVICE_ID;
  doc["seq"]         = t.seq;
  doc["uptimeMs"]    = t.uptimeMs;
  doc["temperature"] = roundf(t.s.temperature * 10) / 10;
  doc["humidity"]    = roundf(t.s.humidity * 10) / 10;
  doc["vibRms"]      = roundf(t.s.vibRms * 1000) / 1000;
  doc["vibPeak"]     = roundf(t.s.vibPeak * 1000) / 1000;
  doc["current"]     = roundf(t.s.current * 100) / 100;
  doc["state"]       = LEVEL_STR[t.global];
  doc["faultInjected"] = t.faultInjected;
  JsonObject lv = doc["levels"].to<JsonObject>();
  lv["temperature"] = LEVEL_STR[t.tempLvl];
  lv["vibration"]   = LEVEL_STR[t.vibLvl];
  lv["current"]     = LEVEL_STR[t.currLvl];
  JsonObject h = doc["health"].to<JsonObject>();
  h["dht"] = t.s.dhtOk;
  h["mpu"] = t.s.mpuOk;

  char buf[400];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  return mqtt.publish(topicTelemetry, (const uint8_t *)buf, n, false);
}

// ----------------------------------------------------------------------------
//  Tâche : connectivité Wi-Fi + MQTT, publication, reconnexion automatique
// ----------------------------------------------------------------------------
void taskMqtt(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);   // canal 6 = démarrage rapide sur Wokwi
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);

  uint32_t backoffMs = 1000;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      g_mqttUp = false;
      Serial.println("[WiFi] connexion...");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!mqtt.connected()) {
      g_mqttUp = false;
      String cid = String("esp32-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      // Last Will : le broker publie "offline" si l'ESP32 disparaît
      if (mqtt.connect(cid.c_str(), nullptr, nullptr, topicStatus, 1, true, "offline")) {
        mqtt.publish(topicStatus, "online", true);
        mqtt.subscribe(topicCmd);
        g_mqttUp = true;
        backoffMs = 1000;
        Serial.printf("[MQTT] connecté à %s — topic %s\n", MQTT_HOST, topicTelemetry);
      } else {
        Serial.printf("[MQTT] échec rc=%d, nouvel essai dans %lu ms\n", mqtt.state(), backoffMs);
        vTaskDelay(pdMS_TO_TICKS(backoffMs));
        backoffMs = min<uint32_t>(backoffMs * 2, 30000);   // backoff exponentiel
        continue;
      }
    }
    mqtt.loop();

    // Vide la file : publie aussi les mesures accumulées pendant une coupure
    Telemetry t;
    while (mqtt.connected() && xQueuePeek(telemetryQueue, &t, 0) == pdTRUE) {
      if (!publishTelemetry(t)) break;          // on garde la mesure pour plus tard
      xQueueReceive(telemetryQueue, &t, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ----------------------------------------------------------------------------
//  Initialisation
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Machine Monitor — ESP32 / FreeRTOS ===");

  snprintf(topicTelemetry, sizeof(topicTelemetry), "%s/%s/telemetry", TOPIC_PREFIX, DEVICE_ID);
  snprintf(topicCmd,       sizeof(topicCmd),       "%s/%s/cmd",       TOPIC_PREFIX, DEVICE_ID);
  snprintf(topicStatus,    sizeof(topicStatus),    "%s/%s/status",    TOPIC_PREFIX, DEVICE_ID);

  pinMode(PIN_LED_OK, OUTPUT);
  pinMode(PIN_LED_WARN, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_CURRENT, ADC_11db);   // plage 0–3,3 V

  i2cMutex       = xSemaphoreCreateMutex();
  dataMutex      = xSemaphoreCreateMutex();
  buttonSem      = xSemaphoreCreateBinary();
  telemetryQueue = xQueueCreate(30, sizeof(Telemetry));   // ≈30 s de tampon

  Wire.begin(21, 22);
  Wire.setClock(400000);
  dht.setup(PIN_DHT, DHTesp::DHT22);

  if (!mpu.begin()) Serial.println("[ERR] MPU6050 introuvable");
  else {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);
  }
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("[ERR] OLED introuvable");
  else { oled.clearDisplay(); oled.display(); }

  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButtonISR, FALLING);

  //            fonction          nom       pile  param prio  handle  cœur
  xTaskCreatePinnedToCore(taskVibration,   "vib",     4096, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(taskEnvironment, "env",     4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskAnalysis,    "analysis",4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskButton,      "button",  2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskDisplay,     "display", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(taskMqtt,        "mqtt",    8192, NULL, 2, NULL, 0);
}

void loop() {
  // Tout est géré par les tâches FreeRTOS ; loop() sert juste de moniteur.
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 5000) {
    lastLog = millis();
    Serial.printf("[SYS] heap libre=%u  file=%u  faute=%d\n", ESP.getFreeHeap(),
                  uxQueueMessagesWaiting(telemetryQueue), g_faultMode);
  }
  vTaskDelay(pdMS_TO_TICKS(100));
}
