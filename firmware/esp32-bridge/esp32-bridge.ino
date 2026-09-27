/*
 * ============================================================================
 *  ESP32 — Passerelle UART → Wi-Fi/MQTT (version "architecture complète")
 * ============================================================================
 *  À utiliser quand le STM32 fait l'acquisition :
 *     STM32 (USART1 TX PA9) ──► ESP32 GPIO16 (RX2)     + GND commun !
 *     STM32 (USART1 RX PA10) ◄── ESP32 GPIO17 (TX2)
 *
 *  protocol.c / protocol.h sont les MÊMES fichiers que côté STM32 :
 *  un seul code de protocole, testé une fois, utilisé des deux côtés.
 *
 *  Deux tâches FreeRTOS :
 *   - taskUart : lit Serial2, décode les trames, les pousse dans une queue
 *   - taskMqtt : Wi-Fi + MQTT, publie le JSON (même format que sketch.ino)
 * ============================================================================
 */
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "protocol.h"

#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASS     ""
#define MQTT_HOST     "broker.hivemq.com"
#define MQTT_PORT     1883
#define TOPIC_PREFIX  "pfe-monitor-7f3a"
#define DEVICE_ID     "stm32-01"
#define UART_RX_PIN   16
#define UART_TX_PIN   17

static const char *LEVELS[] = {"NORMAL", "WARNING", "CRITICAL"};

WiFiClient net;
PubSubClient mqtt(net);
QueueHandle_t frameQueue;
proto_parser_t parser;
char topicTelemetry[64], topicStatus[64];

void taskUart(void *) {
  proto_frame_t f;
  for (;;) {
    while (Serial2.available()) {
      if (proto_parser_feed(&parser, (char)Serial2.read(), &f)) {
        if (xQueueSend(frameQueue, &f, 0) != pdTRUE) {      // file pleine
          proto_frame_t old;
          xQueueReceive(frameQueue, &old, 0);
          xQueueSend(frameQueue, &f, 0);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool publishFrame(const proto_frame_t &f) {
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
  JsonObject h = doc["health"].to<JsonObject>();
  h["dht"] = (bool)(f.flags & PROTO_FLAG_DHT_OK);
  h["mpu"] = (bool)(f.flags & PROTO_FLAG_MPU_OK);
  char buf[320];
  size_t n = serializeJson(doc, buf, sizeof buf);
  return mqtt.publish(topicTelemetry, (const uint8_t *)buf, n, false);
}

void taskMqtt(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if (!mqtt.connected()) {
      String cid = String("bridge-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqtt.connect(cid.c_str(), nullptr, nullptr, topicStatus, 1, true, "offline")) {
        mqtt.publish(topicStatus, "online", true);
        Serial.println("[MQTT] connecté");
      } else { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    }
    mqtt.loop();
    proto_frame_t f;
    while (mqtt.connected() && xQueuePeek(frameQueue, &f, 0) == pdTRUE) {
      if (!publishFrame(f)) break;
      xQueueReceive(frameQueue, &f, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  snprintf(topicTelemetry, sizeof topicTelemetry, "%s/%s/telemetry", TOPIC_PREFIX, DEVICE_ID);
  snprintf(topicStatus, sizeof topicStatus, "%s/%s/status", TOPIC_PREFIX, DEVICE_ID);
  proto_parser_init(&parser);
  frameQueue = xQueueCreate(30, sizeof(proto_frame_t));
  xTaskCreatePinnedToCore(taskUart, "uart", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskMqtt, "mqtt", 8192, NULL, 2, NULL, 0);
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t > 10000) {
    t = millis();
    Serial.printf("[UART] trames OK=%lu  err.checksum=%lu  err.format=%lu\n",
                  (unsigned long)parser.ok_count, (unsigned long)parser.err_checksum,
                  (unsigned long)parser.err_format);
  }
  vTaskDelay(pdMS_TO_TICKS(100));
}
