/*
 * Copie ce fichier en "secrets.h" (même dossier) et remplis tes identifiants
 * du broker privé (EMQX Cloud Serverless ou HiveMQ Cloud).
 * secrets.h est exclu de GitHub (.gitignore) : tes mots de passe ne seront
 * jamais publiés.
 * Sans secrets.h, le firmware utilise le broker public broker.hivemq.com
 * (sans mot de passe ni chiffrement) : pratique pour tester, pas pour la production.
 */
#pragma once
#define MQTT_HOST     "xxxxxxxx.ala.eu-central-1.emqxsl.com"  // adresse du broker (EMQX : "Address" / HiveMQ : "Cluster URL")
#define MQTT_USE_TLS  1                                       // chiffrement TLS (port 8883)
#define MQTT_USER     "esp32"                                 // identifiant créé sur le broker
#define MQTT_PASS     "ton-mot-de-passe"
