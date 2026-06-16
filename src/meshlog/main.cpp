#include <Arduino.h>
#include <esp_task_wdt.h>

#ifndef ESP32
#error "Platform not supported."
#endif

#ifdef WEBSERVER_ENABLE
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#ifdef WEBSERVER_OTA_ENABLE
#include <AsyncElegantOTA.h>
#endif
#include "html.h"
#else
#undef WEBSERVER_OTA_ENABLE
#endif

#include <HTTPClient.h>
#include "mesh/MyMesh.h"

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 60 * 60 * 3;
const int   daylightOffset_sec = 3600;
const unsigned long ntpSyncInterval = 5 * 60 * 1000;
unsigned long ntpNext = 0;

// RTOS, wifi thread
TaskHandle_t WiFiTask;
TaskHandle_t MeshTask;
void WiFiTaskCode(void* pvParameters);
void MeshTaskCode(void* pvParameters);

// Web
#ifdef WEBSERVER_ENABLE
void setupWebserver();
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
#endif


void logSystem(String tag);

void task_sleep(uint32_t ms) {
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

unsigned long getTimestamp() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return(0);
    }
    time(&now);
    return now;
}

StdRNG fast_rng;
LoggerMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, *new VolatileRTCClock(), tables); // TODO: test with 'rtc_clock' in target.cpp

void halt() {
    while (1) ;
}

void logSystem(String tag="SYS") {
    long now = millis();
    char sender[(PUB_KEY_SIZE * 2) + 1];
    mesh::Utils::toHex(sender, the_mesh.getPubKey(), PUB_KEY_SIZE);
    
    JsonDocument doc;
    doc["version"] = 1;
    doc["type"] = tag; //"SYS";
    doc["reporter"] = sender;
    doc["time"]["local"] = getTimestamp();
    // sysinfo
    doc["sys"]["type"] = "status";
    doc["sys"]["heap_total"] = ESP.getHeapSize();
    doc["sys"]["heap_free"] = ESP.getFreeHeap();
    doc["sys"]["rssi"] = WiFi.RSSI();
    doc["sys"]["uptime"] = now;
    doc["sys"]["version"]["logger"] = LOGGER_VER_TEXT;
    doc["sys"]["version"]["meshcore"] = FIRMWARE_VER_TEXT;
    doc["sys"]["version"]["date"] = BUILD_DATE;
    doc["sys"]["version"]["board"] = board.getManufacturerName();
    // stats
    doc["sys"]["stats"]["tx"]["packets"] = the_mesh.stats.tx.packets;
    doc["sys"]["stats"]["tx"]["packets_total"] = the_mesh.stats.tx.packets_total;
    doc["sys"]["stats"]["tx"]["air_time"] = the_mesh.stats.tx.air_time;
    doc["sys"]["stats"]["tx"]["air_time_total"] = the_mesh.stats.tx.air_time_total;
    doc["sys"]["stats"]["tx"]["air_time_duty"] = the_mesh.stats.getDuty(now, the_mesh.stats.tx.air_time);
    doc["sys"]["stats"]["rx"]["packets"] = the_mesh.stats.rx.packets;
    doc["sys"]["stats"]["rx"]["packets_total"] = the_mesh.stats.rx.packets_total;
    doc["sys"]["stats"]["rx"]["air_time"] = the_mesh.stats.rx.air_time;
    doc["sys"]["stats"]["rx"]["air_time_total"] = the_mesh.stats.rx.air_time_total;
    doc["sys"]["stats"]["rx"]["air_time_duty"] = the_mesh.stats.getDuty(now, the_mesh.stats.rx.air_time);
    // contact
    doc["contact"]["new"] = false;
    doc["contact"]["type"] = ADV_TYPE_CHAT;
    doc["contact"]["flags"] = 0;
    doc["contact"]["name"] = the_mesh.getNodePrefs()->node_name;
    doc["contact"]["pubkey"] = sender;
    doc["contact"]["lat"] = the_mesh.getNodePrefs()->node_lat;
    doc["contact"]["lon"] = the_mesh.getNodePrefs()->node_lon;
    messageQueue.push(doc);
}

void WiFiTaskCode(void * pvParameters) {
    static bool connected = false;
    static bool sendsys   = false;
    static bool webserver = false;
    static bool reported = false;
    static unsigned sendFailures = 0;
    static unsigned long lastConencted = 0;
    static unsigned long nextReport = 30000;
    
    Serial.print("WiFiTask running on core ");
    Serial.println(xPortGetCoreID());
    
    WiFiClientSecure* client = new WiFiClientSecure;
    client->setInsecure();
    
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            sendsys = false;
            lastConencted = millis();
            
            if (lastConencted >= ntpNext) {
                the_mesh.setNtpSynced(false);
            }
            
            if (!the_mesh.isNtpSynced()) {
                configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
                unsigned time = getTimestamp();
                the_mesh.setClock(time, true);
                ntpNext = lastConencted + ntpSyncInterval;
            }
            
            long now = millis();
            if (!reported || (the_mesh.getLogPrefs()->selfreport > 0 && now > nextReport)) {
                logSystem();
                if (the_mesh.getLogPrefs()->selfreport != -1) {
                    nextReport = now + (the_mesh.getLogPrefs()->selfreport * 1000);
                }
                
                // reset stats
                the_mesh.stats.reset();
            }
            
            String msg;
            size_t queued = 0;
            if (messageQueue.peek(msg, &queued)) {
                String auth = "Bearer ";
                auth += the_mesh.getLogPrefs()->auth;

                if (the_mesh.debugPrint()) {
                    if (the_mesh.dbg) Serial.printf("Queue peek %s\n", msg.c_str());
                }

                if (memcmp(the_mesh.getLogPrefs()->url, "http", 4) != 0) {
                    if (the_mesh.dbg) Serial.println("Url not set.");
                    messageQueue.pop();
                } else {
                    // WiFi send
                    HTTPClient https;
                    bool sent = false;

                    if (the_mesh.dbg) Serial.printf("[HTTP] Send packet: %u bytes (%u rem.)\n", msg.length(), queued);
                    if (https.begin(*client, the_mesh.getLogPrefs()->url)) { // HTTPS connection
                        https.addHeader("Content-Type", "application/json");

                        if (auth.length() > 7) {
                            https.addHeader("Authorization", auth);
                        }

                        if (the_mesh.debugPrint()) {
                            if (the_mesh.dbg) Serial.println("[HTTP] Post data");
                        }
                        int httpResponseCode = https.POST(msg);

                        if (httpResponseCode > 0) {
                            String response = https.getString();
                            if (the_mesh.dbg) Serial.printf("[HTTP] POST: %d | %s\n", httpResponseCode, response.c_str());
                            sent = true;
                        } else {
                            if (the_mesh.dbg) Serial.printf("[HTTP] ERROR: %d\n", httpResponseCode);
                            ++sendFailures;
                        }

                        messageQueue.pop();
                        https.end();
                    } else {
                        ++sendFailures;
                        if (the_mesh.dbg) Serial.println("[HTTP] Unable to connect");
                    }

                    if (sent) {
                        unsigned bef = ESP.getFreeHeap();
                        sendFailures = 0;
                        reported = true;
                        unsigned aft = ESP.getFreeHeap();
                        if (the_mesh.debugPrint()) {
                            if (the_mesh.dbg) Serial.printf("free mem: %u -> %u >> %d\n",bef,aft,bef-aft);
                        }
                    }
                }
            }
            
            #ifdef WEBSERVER_ENABLE
            if (webserver) {
                ws.cleanupClients(5); 
            } else if (!webserver && the_mesh.getLogPrefs()->web) {
                if (the_mesh.dbg) Serial.println("Start webserver");
                setupWebserver();
                webserver = true;
            }
            #endif
        } else if (connected && (millis() > (lastConencted + 5000) || sendFailures > 5)) {
            WiFi.disconnect();
            connected = false;
            sendFailures = 0;
        }
        
        if (!connected && millis() > (lastConencted + 10000)) {
            char sender[(PUB_KEY_SIZE * 2) + 1];
            mesh::Utils::toHex(sender, the_mesh.getPubKey(), PUB_KEY_SIZE);
            
            if (the_mesh.dbg) Serial.println("Reconenct wifi...");
            
            // if WiFi is down, try reconnecting
            if (!sendsys) {
                JsonDocument doc;
                doc["version"] = 1;
                doc["type"] = "SYS";
                doc["reporter"] = sender;
                doc["time"]["local"] = getTimestamp(); //()->getCurrentTime();
                doc["sys"]["type"] = "wifi";
                doc["sys"]["message"] = "Reconnecting to WiFi...";
                doc["sys"]["heap_total"] = ESP.getHeapSize();
                doc["sys"]["heap_free"] = ESP.getFreeHeap();
                doc["sys"]["uptime"] = millis();
                messageQueue.push(doc);
                sendsys = true;
            }
            
            WiFi.reconnect();
            lastConencted = millis();
        }
        
        task_sleep(25);
    }
}

void MeshTaskCode(void * pvParameters) {
    Serial.print("MeshTask running on core ");
    Serial.println(xPortGetCoreID());
    
    for (;;) {
        the_mesh.loop();
        task_sleep(10);
    }
}

void startWifiTask(int core) {
    xTaskCreatePinnedToCore(
        WiFiTaskCode,   /* Task function. */
        "WiFiTask",     /* name of task. */
        20000,          /* Stack size of task */
        NULL,           /* parameter of the task */
        1,              /* priority of the task */
        &WiFiTask,      /* Task handle to keep track of created task */
        core            /* pin task to core */
    );
}

void startMeshTask(int core) {
    xTaskCreatePinnedToCore(
        MeshTaskCode,   /* Task function. */
        "MeshTask",     /* name of task. */
        10000,          /* Stack size of task */
        NULL,           /* parameter of the task */
        1,              /* priority of the task */
        &MeshTask,      /* Task handle to keep track of created task */
        core            /* pin task to core */
    );
}

void setupWebserver() {
    #ifdef WEBSERVER_ENABLE
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", htmlChat);
    });
    
    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", htmlSettings);
    });
    
    server.on("/chat.json", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["name"] = the_mesh.getNodePrefs()->node_name;
        JsonArray arr = doc["msg"].to<JsonArray>();
        
        int size = the_mesh.getHistorySize();
        for (int i=0;i<size;i++) {
            JsonDocument doc2;
            DeserializationError error = deserializeJson(doc2, the_mesh.getHistory(i));
            if (error) continue;
            arr.add(doc2);
        }
        
        String postData;
        serializeJson(doc, postData);
        
        request->send(200, "application/json", postData);
    });
    
    server.on("/settings.json", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["node_prefs"]["node_name"] = the_mesh.getNodePrefs()->node_name;
        doc["node_prefs"]["node_lat"] = the_mesh.getNodePrefs()->node_lat;
        doc["node_prefs"]["node_lon"] = the_mesh.getNodePrefs()->node_lon;
        doc["node_prefs"]["freq"] = the_mesh.getNodePrefs()->freq;
        doc["node_prefs"]["tx_power_dbm"] = the_mesh.getNodePrefs()->tx_power_dbm;
        doc["node_prefs"]["hash_mode"] = the_mesh.getNodePrefs()->path_hash_mode;
        
        doc["wifi_prefs"]["ssid"] = the_mesh.getWiFiPrefs()->ssid;
        doc["wifi_prefs"]["password"] = the_mesh.getWiFiPrefs()->password;
        doc["wifi_prefs"]["txpower"] = the_mesh.getWiFiPrefs()->txpower / 4.0;
        
        doc["logger_prefs"]["url"] = the_mesh.getLogPrefs()->url;
        doc["logger_prefs"]["auth"] = the_mesh.getLogPrefs()->auth;
        doc["logger_prefs"]["selfreport"] = the_mesh.getLogPrefs()->selfreport;
        
        String postData;
        serializeJson(doc, postData);
        
        request->send(200, "application/json", postData);
    });
    
    server.on("/contacts.json", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        JsonArray arr = doc["contacts"].to<JsonArray>();
        
        ContactsIterator iter;
        ContactInfo c;
        int i = 0;
        
        while (iter.hasNext(&the_mesh, c)) {
            JsonDocument obj;
            obj["id"] = i++;
            
            String tmp = "";
            for (int j=0;j<PUB_KEY_SIZE;j++) {
                if (j > 0) tmp += ':';
                if (c.id.pub_key[j] < 0x10) tmp += '0';
                tmp += String(c.id.pub_key[j], HEX);
            }
            obj["pk"] = tmp;
            obj["n"] = c.name;
            obj["t"] = c.type;
            obj["m"] = c.lastmod;
            obj["s"] = c.sync_since;
            obj["a"] = c.last_advert_timestamp;
            
            arr.add(obj);
        }
        
        String postData;
        serializeJson(doc, postData);
        
        request->send(200, "application/json", postData);
    });
    
    server.on("/telemetry.json", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        JsonArray arr = doc["telemetry"].to<JsonArray>();
        
        const TelemetryRules* tel = the_mesh.getTelemetryRules();
        
        for (int i=0; i<tel->rules.size(); i++) {
            JsonDocument obj;
            obj["id"] = i;
            
            TelemetryRule* rule = tel->rules[i];
            ContactInfo* c = the_mesh.lookupContactByPubKey(rule->pubkey, rule->key_len);
            
            String tmp = "";
            for (int j=0;j<rule->key_len && j < PUB_KEY_SIZE;j++) {
                if (j > 0) tmp += ':';
                if (rule->pubkey[j] < 0x10) tmp += '0';
                tmp += String(rule->pubkey[j], HEX);
            }
            obj["pk"] = tmp;
            
            tmp = "";
            if (rule->path_len == -1) {
                tmp = "Flood";
            } else {
                for (int j=0;j<rule->path_len && j<MAX_PATH_SIZE;j++) {
                    if (j > 0) tmp += ',';
                    if (rule->path[j] < 0x10) tmp += '0';
                    tmp += String(rule->path[j], HEX);
                }
            }
            obj["path"] = tmp;
            obj["password"] = rule->password;
            obj["start"] = rule->start;
            obj["interval"] = rule->interval;
            obj["next"] = rule->next;
            obj["loggedin"] = rule->loggedin;
            
            if (c) {
                obj["name"] = c->name;
            } else {
                obj["name"] = "unknown";
            }
            arr.add(obj);
        }
        
        String postData;
        serializeJson(doc, postData);
        
        request->send(200, "application/json", postData);
    });
    
    server.on("/exec", HTTP_POST,
        [](AsyncWebServerRequest *request) { },
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, data);
            
            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            
            JsonArray commands = doc["commands"];
            
            for (const char* cmd : commands) {
                Serial.println(cmd);
                the_mesh.handleCommand(cmd);
            }
            
            request->send(200, "application/json", "{}");
        }
    );
    
    ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
        (void)len;
        
        if (type == WS_EVT_CONNECT) {
            if (the_mesh.dbg) Serial.println("ws connect");
            client->setCloseClientOnQueueFull(false);
            client->ping();
        } else if (type == WS_EVT_DISCONNECT) {
            if (the_mesh.dbg) Serial.println("ws disconnect");
        } else if (type == WS_EVT_ERROR) {
            if (the_mesh.dbg) Serial.println("ws error");
        } else if (type == WS_EVT_PONG) {
            if (the_mesh.dbg) Serial.println("ws pong");
        } else if (type == WS_EVT_DATA) {
            if (the_mesh.dbg) Serial.println("ws data");
        }
    });
    
    // shows how to prevent a third WS client to connect
    server.addHandler(&ws);
    
    server.begin();
    #endif
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    board.begin();
    
    if (!radio_init()) { halt(); }
    
    fast_rng.begin(radio_get_rng_seed());
    
    SPIFFS.begin(true);
    the_mesh.begin(SPIFFS);
    
    radio_set_params(the_mesh.getFreqPref(), LORA_BW, LORA_SF, LORA_CR);
    radio_set_tx_power(the_mesh.getTxPowerPref());
    
    the_mesh.showWelcome();
    
    int core = xPortGetCoreID() == 1 ? 0 : 1;
    startMeshTask(0);
    startWifiTask(1);
    
    constexpr uint32_t watchdog_timeout_s = 15;
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t watchdog_config = {};
    watchdog_config.timeout_ms = watchdog_timeout_s * 1000;
    watchdog_config.idle_core_mask = (1U << portNUM_PROCESSORS) - 1;
    watchdog_config.trigger_panic = true;
    esp_task_wdt_reconfigure(&watchdog_config);
#else
    if (esp_task_wdt_init(watchdog_timeout_s, true) == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_deinit();
        esp_task_wdt_init(watchdog_timeout_s, true);
    }
#endif
}

void loop() {
    vTaskDelay(1);
}
