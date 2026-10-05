/*
 * Copy this file to "secrets.h" (same folder) and fill in your private
 * broker credentials (EMQX Cloud Serverless or HiveMQ Cloud).
 * secrets.h is excluded from GitHub (.gitignore): your passwords will
 * never be published.
 * Without secrets.h, the firmware uses the public broker broker.hivemq.com
 * (no password, no encryption): handy for testing, not for production.
 */
#pragma once
#define MQTT_HOST     "xxxxxxxx.ala.eu-central-1.emqxsl.com"  // broker address (EMQX: "Address" / HiveMQ: "Cluster URL")
#define MQTT_USE_TLS  1                                       // TLS encryption (port 8883)
#define MQTT_USER     "esp32"                                 // username created on the broker
#define MQTT_PASS     "your-password"
