/*
 * ============================================================================
 *  ESP32 — UART ↔ Wi-Fi/MQTT gateway for the STM32 node
 * ============================================================================
 *  Full architecture:
 *
 *    STM32 (sensors, FreeRTOS) ──UART──► ESP32 (this board) ──Wi-Fi/MQTT──► server
 *                              ◄──UART── "F1"/"F0" commands ◄── dashboard
 *
 *  Real hardware: STM32 USART TX → ESP32 GPIO16 (RX2), STM32 RX ← GPIO17 (TX2), common GND.
 *  Wokwi simulation (WOKWI_SIM): the STM32 and the ESP32 run in two separate
 *  simulations, connected by a "virtual cable" on the PC
 *  (backend/tools/virtual-cable.js); the link then goes through UART0.
 *
 *  protocol.c / protocol.h are the SAME files as on the STM32 side:
 *  a single protocol implementation, tested once, used on both ends.
 *
 *  FreeRTOS tasks:
 *   - taskUart: reads the UART, decodes the frames, runs the on-device AI
 *               (TinyML), pushes the result into a queue
 *   - taskMqtt: Wi-Fi + MQTT, publishes the JSON, relays commands to the STM32
 *
 *  On-device AI ("edge AI"): a neural network (ai/train_model.py)
 *  computes an anomaly score for every frame, directly on the ESP32.
 * ============================================================================
 */
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "protocol.h"
#include "tinyml.h"         // on-device AI: anomaly detection

#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASS     ""
// --- MQTT broker: credentials in secrets.h (not published on GitHub) -------
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef MQTT_HOST
  #define MQTT_HOST    "broker.hivemq.com"   // fallback: public broker (demo)
#endif
#ifndef MQTT_USE_TLS
  #define MQTT_USE_TLS 0
#endif
#ifndef MQTT_PORT
  #define MQTT_PORT    (MQTT_USE_TLS ? 8883 : 1883)
#endif
#ifndef MQTT_USER
  #define MQTT_USER    ""
#endif
#ifndef MQTT_PASS
  #define MQTT_PASS    ""
#endif
#if MQTT_USE_TLS
  #include <WiFiClientSecure.h>
  #include "ca_cert.h"
#endif
#define TOPIC_PREFIX  "pfe-monitor-7f3a"   // same as the backend
#define DEVICE_ID     "stm32-01"           // the machine monitored by the STM32

#define PIN_LED_RX    2     // toggles on every frame received from the STM32
#define PIN_LED_NET   4     // on while MQTT is connected

#ifdef WOKWI_SIM
  #define LINK Serial         // simulation: UART0, connected to the virtual cable
#else
  #define LINK Serial2        // hardware: UART2 (GPIO16 RX / GPIO17 TX)
  #define UART_RX_PIN 16
  #define UART_TX_PIN 17
#endif

static const char *LEVELS[] = {"NORMAL", "WARNING", "CRITICAL"};

#if MQTT_USE_TLS
WiFiClientSecure net;   // TLS: encrypted connection + verified certificate
#else
WiFiClient net;
#endif
PubSubClient mqtt(net);
// One STM32 frame + the on-device AI verdict
struct Sample {
  proto_frame_t f;
  float   aiScore;
  uint8_t aiAnomaly;
  uint8_t aiCause;
};

QueueHandle_t frameQueue;
tinyml_state_t ai = {};
proto_parser_t parser;
char topicTelemetry[64], topicStatus[64], topicCmd[64];

// In simulation, debug messages go out on the same link as the STM32 traffic:
// they are prefixed with '#' (ignored by the STM32) and must avoid F1/F0.
static void logLine(const char *s) {
#ifdef WOKWI_SIM
  LINK.print("# [ESP32] ");
  LINK.println(s);
#else
  Serial.println(s);
#endif
}

// ---------------------------------------------------------------------------
//  UART reception: byte-by-byte decoding (state-machine parser, with checksum)
// ---------------------------------------------------------------------------
void taskUart(void *) {
  Sample smp;
  for (;;) {
    while (LINK.available()) {
      if (proto_parser_feed(&parser, (char)LINK.read(), &smp.f)) {
        const proto_frame_t &f = smp.f;
        digitalWrite(PIN_LED_RX, !digitalRead(PIN_LED_RX));
        // --- On-device AI: anomaly score of this measurement --------------
        if (f.flags & PROTO_FLAG_DHT_OK) {
          if (tinyml_update(&ai, f.temp_d / 10.0f, f.vib_mg / 1000.0f,
                            f.peak_mg / 1000.0f, f.curr_ma / 1000.0f)) {
            char msg[80];
            if (ai.anomaly)
              snprintf(msg, sizeof msg, "AI: anomaly detected (probable cause: %s, score %.2f)",
                       TINYML_CAUSE_STR[ai.cause], ai.score);
            else
              snprintf(msg, sizeof msg, "AI: back to normal operation");
            logLine(msg);
          }
        }
        smp.aiScore = ai.score;
        smp.aiAnomaly = ai.anomaly;
        smp.aiCause = ai.cause;
        if (xQueueSend(frameQueue, &smp, 0) != pdTRUE) {    // queue full
          Sample old;
          xQueueReceive(frameQueue, &old, 0);
          xQueueSend(frameQueue, &smp, 0);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------------------------------------------------------------------
//  Dashboard commands → STM32
// ---------------------------------------------------------------------------
void onMqttMessage(char *, byte *payload, unsigned int len) {
  String cmd;
  for (unsigned i = 0; i < len; i++) cmd += (char)payload[i];
  if (cmd == "fault_on")  LINK.print("F1\n");
  if (cmd == "fault_off") LINK.print("F0\n");
}

bool publishFrame(const Sample &smp) {
  const proto_frame_t &f = smp.f;
  JsonDocument doc;
  doc["deviceId"]    = DEVICE_ID;
  doc["seq"]         = f.seq;
  doc["uptimeMs"]    = millis();
  doc["temperature"] = f.temp_d / 10.0;
  doc["humidity"]    = f.hum_d / 10.0;
  doc["vibRms"]      = f.vib_mg / 1000.0;
  doc["vibPeak"]     = f.peak_mg / 1000.0;
  doc["current"]     = f.curr_ma / 1000.0;
  uint8_t lvl = (f.flags & PROTO_LEVEL_MASK) >> PROTO_LEVEL_SHIFT;
  doc["state"]         = LEVELS[lvl > 2 ? 2 : lvl];
  doc["faultInjected"] = (bool)(f.flags & PROTO_FLAG_FAULT);
  doc["via"]           = "esp32-bridge";
  JsonObject h = doc["health"].to<JsonObject>();
  h["temp"] = (bool)(f.flags & PROTO_FLAG_DHT_OK);
  h["mpu"]  = (bool)(f.flags & PROTO_FLAG_MPU_OK);
  JsonObject a = doc["ai"].to<JsonObject>();      // on-device AI verdict
  a["score"]   = roundf(smp.aiScore * 1000) / 1000;
  a["anomaly"] = (bool)smp.aiAnomaly;
  a["cause"]   = TINYML_CAUSE_STR[smp.aiCause];
  char buf[400];
  size_t n = serializeJson(doc, buf, sizeof buf);
  return mqtt.publish(topicTelemetry, (const uint8_t *)buf, n, false);
}

// ---------------------------------------------------------------------------
//  Wi-Fi + MQTT with automatic reconnection
// ---------------------------------------------------------------------------
void taskMqtt(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
#if MQTT_USE_TLS
  net.setCACert(ISRG_ROOT_X1);
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(768);
  bool wasConnected = false;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      digitalWrite(PIN_LED_NET, LOW);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!mqtt.connected()) {
      digitalWrite(PIN_LED_NET, LOW);
      if (wasConnected) { logLine("MQTT lost, reconnecting"); wasConnected = false; }
      String cid = String("bridge-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqtt.connect(cid.c_str(), MQTT_USER[0] ? MQTT_USER : nullptr, MQTT_PASS[0] ? MQTT_PASS : nullptr,
                       topicStatus, 1, true, "offline")) {
        mqtt.publish(topicStatus, "online", true);
        mqtt.subscribe(topicCmd);
        digitalWrite(PIN_LED_NET, HIGH);
        logLine("MQTT connected, waiting for STM32 frames");
        wasConnected = true;
      } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }
    mqtt.loop();
    Sample smp;
    while (mqtt.connected() && xQueuePeek(frameQueue, &smp, 0) == pdTRUE) {
      if (!publishFrame(smp)) break;        // keep the frame for later
      xQueueReceive(frameQueue, &smp, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
#ifdef WOKWI_SIM
  LINK.begin(115200);
#else
  Serial.begin(115200);
  LINK.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
#endif
  pinMode(PIN_LED_RX, OUTPUT);
  pinMode(PIN_LED_NET, OUTPUT);
  snprintf(topicTelemetry, sizeof topicTelemetry, "%s/%s/telemetry", TOPIC_PREFIX, DEVICE_ID);
  snprintf(topicStatus,    sizeof topicStatus,    "%s/%s/status",    TOPIC_PREFIX, DEVICE_ID);
  snprintf(topicCmd,       sizeof topicCmd,       "%s/%s/cmd",       TOPIC_PREFIX, DEVICE_ID);
  proto_parser_init(&parser);
  frameQueue = xQueueCreate(30, sizeof(Sample));
  logLine("STM32 -> MQTT gateway started (TinyML on-device AI enabled)");
  xTaskCreatePinnedToCore(taskUart, "uart", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskMqtt, "mqtt", 8192, NULL, 2, NULL, 0);
}

void loop() {
  static uint32_t t = 0;
  static uint32_t lastOk = 0;
  if (millis() - t > 15000) {
    t = millis();
    char msg[96];
    snprintf(msg, sizeof msg, "frames OK=%lu (+%lu) err.checksum=%lu err.format=%lu",
             (unsigned long)parser.ok_count, (unsigned long)(parser.ok_count - lastOk),
             (unsigned long)parser.err_checksum, (unsigned long)parser.err_format);
    lastOk = parser.ok_count;
    logLine(msg);
  }
  vTaskDelay(pdMS_TO_TICKS(100));
}
