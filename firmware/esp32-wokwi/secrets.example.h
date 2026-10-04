/*
 * Copie ce fichier en "secrets.h" (même dossier) et remplis tes identifiants
 * HiveMQ Cloud. secrets.h est exclu de GitHub (.gitignore) : tes mots de passe
 * ne seront jamais publiés.
 * Sans secrets.h, le firmware utilise le broker public broker.hivemq.com
 * (sans mot de passe ni chiffrement) : pratique pour tester, pas pour la production.
 */
#pragma once
#define MQTT_HOST     "xxxxxxxxxxxx.s1.eu.hivemq.cloud"   // "Cluster URL" HiveMQ Cloud
#define MQTT_USE_TLS  1                                    // chiffrement TLS (port 8883)
#define MQTT_USER     "esp32"                              // identifiant créé dans "Access Management"
#define MQTT_PASS     "ton-mot-de-passe"
