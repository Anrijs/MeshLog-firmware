#pragma once

#include <Mesh.h>
#include <esp_wifi.h>

struct NodePrefs {  // persisted to file
  float airtime_factor;
  char node_name[32];
  double node_lat, node_lon;
  float freq;
    uint8_t tx_power_dbm;
  uint8_t path_hash_mode;
  uint8_t unused[2];
};

struct TelemetryRule {
  uint8_t pubkey[PUB_KEY_SIZE]; // pubkey
  uint8_t key_len;
  uint8_t path[MAX_PATH_SIZE];
  int8_t path_len;
    char password[16];  // login password
    uint32_t start; // second of day
    uint32_t interval; // interval in seconds
    uint32_t next = 0; // next run
    bool loggedin = false;
};

struct TelemetryRules {
    uint16_t version;
    uint8_t retries;
    uint8_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
    std::vector<TelemetryRule*> rules;
};

struct WiFiPrefs {
    uint8_t version = 1;
    char ssid[33];
    char password[64];
    int8_t txpower = WIFI_POWER_8_5dBm;
};

struct LogPrefs {
    uint16_t version;
    uint32_t selfreport;
    char auth[32];
    char url[256];
    uint8_t doraw;
    uint8_t dofwd;
    uint8_t web;
    uint8_t usbraw;
    uint8_t reserved[7];
};
