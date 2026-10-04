/*
 * ============================================================================
 *  ESP32 — Passerelle UART ↔ Wi-Fi/MQTT pour le nœud STM32
 * ============================================================================
 *  Architecture complète :
 *
 *    STM32 (capteurs, FreeRTOS) ──UART──► ESP32 (cette carte) ──Wi-Fi/MQTT──► serveur
 *                               ◄──UART── commandes "F1"/"F0" ◄── dashboard
 *
 *  Matériel réel : STM32 USART TX → ESP32 GPIO16 (RX2), STM32 RX ← GPIO17 (TX2), GND commun.
 *  Simulation Wokwi (WOKWI_SIM) : le STM32 et l'ESP32 tournent dans deux
 *  simulations séparées, reliées par un « câble virtuel » sur le PC
 *  (backend/tools/virtual-cable.js) ; la liaison passe alors par l'UART0.
 *
 *  protocol.c / protocol.h sont les MÊMES fichiers que côté STM32 :
 *  un seul code de protocole, testé une fois, utilisé des deux côtés.
 *
 *  Tâches FreeRTOS :
 *   - taskUart : lit l'UART, décode les trames, les pousse dans une file
 *   - taskMqtt : Wi-Fi + MQTT, publie le JSON, relaie les commandes au STM32
 * ============================================================================
 */
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "protocol.h"

#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASS     ""
// --- Broker MQTT : identifiants dans secrets.h (non publié sur GitHub) ------
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef MQTT_HOST
  #define MQTT_HOST    "broker.hivemq.com"   // repli : broker public (démo)
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
#define TOPIC_PREFIX  "pfe-monitor-7f3a"   // identique au backend
#define DEVICE_ID     "stm32-01"           // la machine surveillée par le STM32

#define PIN_LED_RX    2     // clignote à chaque trame reçue du STM32
#define PIN_LED_NET   4     // allumée quand MQTT est connecté

#ifdef WOKWI_SIM
  #define LINK Serial         // simulation : UART0, relié au câble virtuel
#else
  #define LINK Serial2        // matériel : UART2 (GPIO16 RX / GPIO17 TX)
  #define UART_RX_PIN 16
  #define UART_TX_PIN 17
#endif

static const char *LEVELS[] = {"NORMAL", "WARNING", "CRITICAL"};

#if MQTT_USE_TLS
WiFiClientSecure net;   // TLS : connexion chiffrée + certificat vérifié
#else
WiFiClient net;
#endif
PubSubClient mqtt(net);
QueueHandle_t frameQueue;
proto_parser_t parser;
char topicTelemetry[64], topicStatus[64], topicCmd[64];

// Les messages de debug partent sur la même liaison que vers le STM32 en
// simulation : on les préfixe par '#' (ignorés par le STM32) et on évite F1/F0.
static void logLine(const char *s) {
#ifdef WOKWI_SIM
  LINK.print("# [ESP32] ");
  LINK.println(s);
#else
  Serial.println(s);
#endif
}

// ---------------------------------------------------------------------------
//  Réception UART : décodage octet par octet (parseur à états, avec checksum)
// ---------------------------------------------------------------------------
void taskUart(void *) {
  proto_frame_t f;
  for (;;) {
    while (LINK.available()) {
      if (proto_parser_feed(&parser, (char)LINK.read(), &f)) {
        digitalWrite(PIN_LED_RX, !digitalRead(PIN_LED_RX));
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

// ---------------------------------------------------------------------------
//  Commandes du dashboard → STM32
// ---------------------------------------------------------------------------
void onMqttMessage(char *, byte *payload, unsigned int len) {
  String cmd;
  for (unsigned i = 0; i < len; i++) cmd += (char)payload[i];
  if (cmd == "fault_on")  LINK.print("F1\n");
  if (cmd == "fault_off") LINK.print("F0\n");
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
  doc["via"]           = "esp32-bridge";
  JsonObject h = doc["health"].to<JsonObject>();
  h["temp"] = (bool)(f.flags & PROTO_FLAG_DHT_OK);
  h["mpu"]  = (bool)(f.flags & PROTO_FLAG_MPU_OK);
  char buf[320];
  size_t n = serializeJson(doc, buf, sizeof buf);
  return mqtt.publish(topicTelemetry, (const uint8_t *)buf, n, false);
}

// ---------------------------------------------------------------------------
//  Wi-Fi + MQTT avec reconnexion automatique
// ---------------------------------------------------------------------------
void taskMqtt(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
#if MQTT_USE_TLS
  net.setCACert(ISRG_ROOT_X1);
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  bool wasConnected = false;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      digitalWrite(PIN_LED_NET, LOW);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!mqtt.connected()) {
      digitalWrite(PIN_LED_NET, LOW);
      if (wasConnected) { logLine("MQTT perdu, reconnexion"); wasConnected = false; }
      String cid = String("bridge-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqtt.connect(cid.c_str(), MQTT_USER[0] ? MQTT_USER : nullptr, MQTT_PASS[0] ? MQTT_PASS : nullptr,
                       topicStatus, 1, true, "offline")) {
        mqtt.publish(topicStatus, "online", true);
        mqtt.subscribe(topicCmd);
        digitalWrite(PIN_LED_NET, HIGH);
        logLine("MQTT connecte, en attente des trames du STM32");
        wasConnected = true;
      } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }
    mqtt.loop();
    proto_frame_t f;
    while (mqtt.connected() && xQueuePeek(frameQueue, &f, 0) == pdTRUE) {
      if (!publishFrame(f)) break;          // on garde la trame pour plus tard
      xQueueReceive(frameQueue, &f, 0);
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
  frameQueue = xQueueCreate(30, sizeof(proto_frame_t));
  logLine("Passerelle STM32 -> MQTT demarree");
  xTaskCreatePinnedToCore(taskUart, "uart", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskMqtt, "mqtt", 8192, NULL, 2, NULL, 0);
}

void loop() {
  static uint32_t t = 0;
  static uint32_t lastOk = 0;
  if (millis() - t > 15000) {
    t = millis();
    char msg[96];
    snprintf(msg, sizeof msg, "trames OK=%lu (+%lu) err.checksum=%lu err.format=%lu",
             (unsigned long)parser.ok_count, (unsigned long)(parser.ok_count - lastOk),
             (unsigned long)parser.err_checksum, (unsigned long)parser.err_format);
    lastOk = parser.ok_count;
    logLine(msg);
  }
  vTaskDelay(pdMS_TO_TICKS(100));
}
