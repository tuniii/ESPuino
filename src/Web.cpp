#include <Arduino.h>
#include <WiFi.h>
#include <Update.h>
#include <nvs.h>
#include <esp_task_wdt.h>
#include "freertos/ringbuf.h"
#include "ESPAsyncWebServer.h"
#include "ArduinoJson.h"
#include "settings.h"
#include "AudioPlayer.h"
#include "Battery.h"
#include "Cmd.h"
#include "Common.h"
#include "Ftp.h"
#include "Led.h"
#include "Log.h"
#include "MemX.h"
#include "Mqtt.h"
#include "Rfid.h"
#include "SdCard.h"
#include "System.h"
#include "Web.h"
#include "Wlan.h"

#if (LANGUAGE == 1)
    #include "HTMLaccesspoint_DE.h"
    #include "HTMLmanagement_DE.h"
#endif

#if (LANGUAGE == 2)
    #include "HTMLaccesspoint_EN.h"
    #include "HTMLmanagement_EN.h"
#endif

extern const uint8_t index_html_start[] asm("_binary_webui_dist_index_html_gz_start");
extern const size_t index_html_size asm("_binary_webui_dist_index_html_gz_size");

typedef struct {
    char nvsKey[13];
    char nvsEntry[275];
} nvs_t;

const char mqttTab[] PROGMEM = "<a class=\"nav-item nav-link\" id=\"nav-mqtt-tab\" data-toggle=\"tab\" href=\"#nav-mqtt\" role=\"tab\" aria-controls=\"nav-mqtt\" aria-selected=\"false\"><i class=\"fas fa-network-wired\"></i> MQTT</a>";
const char ftpTab[] PROGMEM = "<a class=\"nav-item nav-link\" id=\"nav-ftp-tab\" data-toggle=\"tab\" href=\"#nav-ftp\" role=\"tab\" aria-controls=\"nav-ftp\" aria-selected=\"false\"><i class=\"fas fa-folder\"></i> FTP</a>";

AsyncWebServer wServer(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

static bool webserverStarted = false;

static RingbufHandle_t explorerFileUploadRingBuffer;
static QueueHandle_t explorerFileUploadStatusQueue;
static TaskHandle_t fileStorageTaskHandle;

static void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
static void explorerHandleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
static void explorerHandleFileStorageTask(void *parameter);
static void explorerHandleListRequest(AsyncWebServerRequest *request);
static void explorerHandleDeleteRequest(AsyncWebServerRequest *request);
static void explorerHandleCreateRequest(AsyncWebServerRequest *request);
static void explorerHandleRenameRequest(AsyncWebServerRequest *request);
static void explorerHandleAudioRequest(AsyncWebServerRequest *request);

static bool Web_DumpNvsToSd(const char *_namespace, const char *_destFile);

static void onWebsocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
static String templateProcessor(const String &templ);
static void webserverStart(void);

void Web_Init(void) {
    wServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", accesspoint_HTML);
    });

    wServer.on("/init", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("ssid", true) && request->hasParam("pwd", true) && request->hasParam("hostname", true)) {
            Serial.println(request->getParam("ssid", true)->value());
            Serial.println(request->getParam("pwd", true)->value());
            Serial.println(request->getParam("hostname", true)->value());
            gPrefsSettings.putString("SSID", request->getParam("ssid", true)->value());
            gPrefsSettings.putString("Password", request->getParam("pwd", true)->value());
            gPrefsSettings.putString("Hostname", request->getParam("hostname", true)->value());
        }
        request->send_P(200, "text/html", accesspoint_HTML);
    });

    wServer.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
    #if (LANGUAGE == 1)
        request->send(200, "text/html", "ESPuino wird neu gestartet...");
    #else
        request->send(200, "text/html", "ESPuino is being restarted...");
    #endif
        Serial.flush();
        ESP.restart();
    });

    wServer.on("/shutdown", HTTP_GET, [](AsyncWebServerRequest *request) {
    #if (LANGUAGE == 1)
        request->send(200, "text/html", "ESPuino wird ausgeschaltet...");
    #else
        request->send(200, "text/html", "ESPuino is being shutdown...");
        #endif
        System_RequestSleep();
    });

    // allow cors for local debug
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    wServer.begin();
    Log_Println((char *) FPSTR(httpReady), LOGLEVEL_NOTICE);
}

void Web_Cyclic(void) {
    webserverStart();
    ws.cleanupClients();
}

void notFound(AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
}

void webserverStart(void) {
    if (Wlan_IsConnected() && !webserverStarted) {
        // attach AsyncWebSocket for Mgmt-Interface
        ws.onEvent(onWebsocketEvent);
        wServer.addHandler(&ws);

        // attach AsyncEventSource
        wServer.addHandler(&events);

        // Default
        wServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->send_P(200, "text/html", management_HTML, templateProcessor);
        });

        // to avoid multiple requests to ESP
        wServer.on("/vue", [](AsyncWebServerRequest *request) {
            AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html_start, (size_t)&index_html_size);
            response->addHeader("Content-Disposition", "inline; filename=\"index.html\"");
            response->addHeader("Content-Encoding", "gzip");
            request->send(response);
        });

        // Log
        wServer.on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->send(200, "text/plain", Log_GetRingBuffer());
        });

        // Info
        wServer.on(
            "/info", HTTP_GET, [](AsyncWebServerRequest *request) {
                String info = "Free heap: " + String(ESP.getFreeHeap());
                //heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
                info += "\nLargest free heap block: " + String((uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                info += "\nFree PSRAM: " + String(ESP.getFreePsram());
                request->send_P(200, "text/plain", info.c_str());
            });

        // NVS-backup-upload
        wServer.on(
            "/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
                request->send_P(200, "text/html", backupRecoveryWebsite);
            },
            handleUpload);

        // NVS-backup-upload
        wServer.on(
            "/update", HTTP_POST, [](AsyncWebServerRequest *request) {
                request->send(200, "text/html", restartWebsite); },
            [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
                if (!index)
                {
                    Update.begin();
                    Serial.println("Start firmware update.");
                }

                Update.write(data, len);
                Serial.print(".");

                if (final)
                {
                    Update.end(true);
                    Serial.println("Firmware update finished.");
                }
            });

        // ESP-restart
        wServer.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->send_P(200, "text/html", restartWebsite);
            Serial.flush();
            ESP.restart();
        });

        // ESP-shutdown
        wServer.on("/shutdown", HTTP_GET, [](AsyncWebServerRequest *request) {
            request->send_P(200, "text/html", shutdownWebsite);
            System_RequestSleep();
        });

        // Fileexplorer (realtime)
        wServer.on("/explorer", HTTP_GET, explorerHandleListRequest);

        wServer.on(
            "/explorer", HTTP_POST, [](AsyncWebServerRequest *request) {
                request->send(200);
            },
            explorerHandleFileUpload);

        wServer.on("/explorer", HTTP_DELETE, explorerHandleDeleteRequest);

        wServer.on("/explorer", HTTP_PUT, explorerHandleCreateRequest);

        wServer.on("/explorer", HTTP_PATCH, explorerHandleRenameRequest);

        wServer.on("/exploreraudio", HTTP_POST, explorerHandleAudioRequest);

        wServer.onNotFound(notFound);

        // allow cors for local debug
        DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
        wServer.begin();
        webserverStarted = true;
    }
}

// Used for substitution of some variables/templates of html-files. Is called by webserver's template-engine
String templateProcessor(const String &templ)
{
    if (templ == "FTP_USER") {
        return gPrefsSettings.getString("ftpuser", "-1");
    } else if (templ == "FTP_PWD") {
        return gPrefsSettings.getString("ftppassword", "-1");
    } else if (templ == "FTP_USER_LENGTH") {
        return String(ftpUserLength - 1);
    } else if (templ == "FTP_PWD_LENGTH") {
        return String(ftpPasswordLength - 1);
    } else if (templ == "SHOW_FTP_TAB") { // Only show FTP-tab if FTP-support was compiled
    #ifdef FTP_ENABLE
        return (String) FPSTR(ftpTab);
    #else
        return String();
    #endif
    } else if (templ == "INIT_LED_BRIGHTNESS") {
        return String(gPrefsSettings.getUChar("iLedBrightness", 0));
    } else if (templ == "NIGHT_LED_BRIGHTNESS") {
        return String(gPrefsSettings.getUChar("nLedBrightness", 0));
    } else if (templ == "MAX_INACTIVITY") {
        return String(gPrefsSettings.getUInt("mInactiviyT", 0));
    } else if (templ == "INIT_VOLUME") {
        return String(gPrefsSettings.getUInt("initVolume", 0));
    } else if (templ == "CURRENT_VOLUME") {
        return String(AudioPlayer_GetCurrentVolume());
    } else if (templ == "MAX_VOLUME_SPEAKER") {
        return String(gPrefsSettings.getUInt("maxVolumeSp", 0));
    } else if (templ == "MAX_VOLUME_HEADPHONE") {
        return String(gPrefsSettings.getUInt("maxVolumeHp", 0));
    } else if (templ == "WARNING_LOW_VOLTAGE") {
        return String(gPrefsSettings.getFloat("wLowVoltage", warningLowVoltage));
    } else if (templ == "VOLTAGE_INDICATOR_LOW") {
        return String(gPrefsSettings.getFloat("vIndicatorLow", voltageIndicatorLow));
    } else if (templ == "VOLTAGE_INDICATOR_HIGH") {
        return String(gPrefsSettings.getFloat("vIndicatorHigh", voltageIndicatorHigh));
    } else if (templ == "VOLTAGE_CHECK_INTERVAL") {
        return String(gPrefsSettings.getUInt("vCheckIntv", voltageCheckInterval));
    } else if (templ == "MQTT_SERVER") {
        return gPrefsSettings.getString("mqttServer", "-1");
    } else if (templ == "SHOW_MQTT_TAB") { // Only show MQTT-tab if MQTT-support was compiled
    #ifdef MQTT_ENABLE
        return (String) FPSTR(mqttTab);
    #else
        return String();
        #endif
    } else if (templ == "MQTT_ENABLE") {
        if (Mqtt_IsEnabled()) {
            return String("checked=\"checked\"");
        } else {
            return String();
        }
    } else if (templ == "MQTT_USER") {
        return gPrefsSettings.getString("mqttUser", "-1");
    } else if (templ == "MQTT_PWD") {
        return gPrefsSettings.getString("mqttPassword", "-1");
    } else if (templ == "MQTT_USER_LENGTH") {
        return String(mqttUserLength - 1);
    } else if (templ == "MQTT_PWD_LENGTH") {
        return String(mqttPasswordLength - 1);
    } else if (templ == "MQTT_SERVER_LENGTH") {
        return String(mqttServerLength - 1);
    } else if (templ == "MQTT_PORT") {
        return String(gMqttPort);
    } else if (templ == "IPv4") {
        IPAddress myIP = WiFi.localIP();
        snprintf(Log_Buffer, Log_BufferLength, "%d.%d.%d.%d", myIP[0], myIP[1], myIP[2], myIP[3]);
        return String(Log_Buffer);
    } else if (templ == "RFID_TAG_ID") {
        return String(gCurrentRfidTagId);
    } else if (templ == "HOSTNAME") {
        return gPrefsSettings.getString("Hostname", "-1");
    }

    return String();
}

// Takes inputs from webgui, parses JSON and saves values in NVS
// If operation was successful (NVS-write is verified) true is returned
bool processJsonRequest(char *_serialJson) {
    StaticJsonDocument<1000> doc;
    DeserializationError error = deserializeJson(doc, _serialJson);
    JsonObject object = doc.as<JsonObject>();

    if (error) {
        #if (LANGUAGE == 1)
            Serial.print(F("deserializeJson() fehlgeschlagen: "));
        #else
            Serial.print(F("deserializeJson() failed: "));
        #endif
        Serial.println(error.c_str());
        return false;
    }

    if (doc.containsKey("general")) {
        uint8_t iVol = doc["general"]["iVol"].as<uint8_t>();
        uint8_t mVolSpeaker = doc["general"]["mVolSpeaker"].as<uint8_t>();
        uint8_t mVolHeadphone = doc["general"]["mVolHeadphone"].as<uint8_t>();
        uint8_t iBright = doc["general"]["iBright"].as<uint8_t>();
        uint8_t nBright = doc["general"]["nBright"].as<uint8_t>();
        uint8_t iTime = doc["general"]["iTime"].as<uint8_t>();
        float vWarning = doc["general"]["vWarning"].as<float>();
        float vIndLow = doc["general"]["vIndLow"].as<float>();
        float vIndHi = doc["general"]["vIndHi"].as<float>();
        uint8_t vInt = doc["general"]["vInt"].as<uint8_t>();

        gPrefsSettings.putUInt("initVolume", iVol);
        gPrefsSettings.putUInt("maxVolumeSp", mVolSpeaker);
        gPrefsSettings.putUInt("maxVolumeHp", mVolHeadphone);
        gPrefsSettings.putUChar("iLedBrightness", iBright);
        gPrefsSettings.putUChar("nLedBrightness", nBright);
        gPrefsSettings.putUInt("mInactiviyT", iTime);
        gPrefsSettings.putFloat("wLowVoltage", vWarning);
        gPrefsSettings.putFloat("vIndicatorLow", vIndLow);
        gPrefsSettings.putFloat("vIndicatorHigh", vIndHi);
        gPrefsSettings.putUInt("vCheckIntv", vInt);

        // Check if settings were written successfully
        if (gPrefsSettings.getUInt("initVolume", 0) != iVol ||
            gPrefsSettings.getUInt("maxVolumeSp", 0) != mVolSpeaker ||
            gPrefsSettings.getUInt("maxVolumeHp", 0) != mVolHeadphone ||
            gPrefsSettings.getUChar("iLedBrightness", 0) != iBright ||
            gPrefsSettings.getUChar("nLedBrightness", 0) != nBright ||
            gPrefsSettings.getUInt("mInactiviyT", 0) != iTime ||
            gPrefsSettings.getFloat("wLowVoltage", 999.99) != vWarning ||
            gPrefsSettings.getFloat("vIndicatorLow", 999.99) != vIndLow ||
            gPrefsSettings.getFloat("vIndicatorHigh", 999.99) != vIndHi ||
            gPrefsSettings.getUInt("vCheckIntv", 17777) != vInt) {
            return false;
        }
    } else if (doc.containsKey("ftp")) {
        const char *_ftpUser = doc["ftp"]["ftpUser"];
        const char *_ftpPwd = doc["ftp"]["ftpPwd"];

        gPrefsSettings.putString("ftpuser", (String)_ftpUser);
        gPrefsSettings.putString("ftppassword", (String)_ftpPwd);

        if (!(String(_ftpUser).equals(gPrefsSettings.getString("ftpuser", "-1")) ||
              String(_ftpPwd).equals(gPrefsSettings.getString("ftppassword", "-1")))) {
            return false;
        }
    } else if (doc.containsKey("mqtt")) {
        uint8_t _mqttEnable = doc["mqtt"]["mqttEnable"].as<uint8_t>();
        const char *_mqttServer = object["mqtt"]["mqttServer"];
        gPrefsSettings.putUChar("enableMQTT", _mqttEnable);
        gPrefsSettings.putString("mqttServer", (String)_mqttServer);
        const char *_mqttUser = doc["mqtt"]["mqttUser"];
        const char *_mqttPwd = doc["mqtt"]["mqttPwd"];
        uint16_t _mqttPort = doc["mqtt"]["mqttPort"].as<uint16_t>();

        gPrefsSettings.putUChar("enableMQTT", _mqttEnable);
        gPrefsSettings.putString("mqttServer", (String)_mqttServer);
        gPrefsSettings.putString("mqttServer", (String)_mqttServer);
        gPrefsSettings.putString("mqttUser", (String)_mqttUser);
        gPrefsSettings.putString("mqttPassword", (String)_mqttPwd);
        gPrefsSettings.putUInt("mqttPort", _mqttPort);

        if ((gPrefsSettings.getUChar("enableMQTT", 99) != _mqttEnable) ||
            (!String(_mqttServer).equals(gPrefsSettings.getString("mqttServer", "-1")))) {
            return false;
        }
    } else if (doc.containsKey("rfidMod")) {
        const char *_rfidIdModId = object["rfidMod"]["rfidIdMod"];
        uint8_t _modId = object["rfidMod"]["modId"];
        char rfidString[12];
        if (_modId <= 0) {
            gPrefsRfid.remove(_rfidIdModId);
        } else {
            snprintf(rfidString, sizeof(rfidString) / sizeof(rfidString[0]), "%s0%s0%s%u%s0", stringDelimiter, stringDelimiter, stringDelimiter, _modId, stringDelimiter);
            gPrefsRfid.putString(_rfidIdModId, rfidString);

            String s = gPrefsRfid.getString(_rfidIdModId, "-1");
            if (s.compareTo(rfidString)) {
                return false;
            }
        }
        Web_DumpNvsToSd("rfidTags", (const char*) FPSTR(backupFile)); // Store backup-file every time when a new rfid-tag is programmed
    } else if (doc.containsKey("rfidAssign")) {
        const char *_rfidIdAssinId = object["rfidAssign"]["rfidIdMusic"];
        char _fileOrUrlAscii[MAX_FILEPATH_LENTGH];
        convertUtf8ToAscii(object["rfidAssign"]["fileOrUrl"], _fileOrUrlAscii);
        uint8_t _playMode = object["rfidAssign"]["playMode"];
        char rfidString[275];
        snprintf(rfidString, sizeof(rfidString) / sizeof(rfidString[0]), "%s%s%s0%s%u%s0", stringDelimiter, _fileOrUrlAscii, stringDelimiter, stringDelimiter, _playMode, stringDelimiter);
        gPrefsRfid.putString(_rfidIdAssinId, rfidString);
        Serial.println(_rfidIdAssinId);
        Serial.println(rfidString);

        String s = gPrefsRfid.getString(_rfidIdAssinId, "-1");
        if (s.compareTo(rfidString)) {
            return false;
        }
        Web_DumpNvsToSd("rfidTags", (const char*) FPSTR(backupFile)); // Store backup-file every time when a new rfid-tag is programmed
    } else if (doc.containsKey("wifiConfig")) {
        const char *_ssid = object["wifiConfig"]["ssid"];
        const char *_pwd = object["wifiConfig"]["pwd"];
        const char *_hostname = object["wifiConfig"]["hostname"];

        gPrefsSettings.putString("SSID", _ssid);
        gPrefsSettings.putString("Password", _pwd);
        gPrefsSettings.putString("Hostname", (String)_hostname);

        String sSsid = gPrefsSettings.getString("SSID", "-1");
        String sPwd = gPrefsSettings.getString("Password", "-1");
        String sHostname = gPrefsSettings.getString("Hostname", "-1");

        if (sSsid.compareTo(_ssid) || sPwd.compareTo(_pwd)) {
            return false;
        }
    }
    else if (doc.containsKey("ping")) {
        Web_SendWebsocketData(0, 20);
        return false;
    } else if (doc.containsKey("controls")) {
        if (object["controls"].containsKey("set_volume")) {
            uint8_t new_vol = doc["controls"]["set_volume"].as<uint8_t>();
            AudioPlayer_VolumeToQueueSender(new_vol, true);
        } if (object["controls"].containsKey("action")) {
            uint8_t cmd = doc["controls"]["action"].as<uint8_t>();
            Cmd_Action(cmd);
        }
    }

    return true;
}

// Sends JSON-answers via websocket
void Web_SendWebsocketData(uint32_t client, uint8_t code) {
    char *jBuf;
    jBuf = (char *)x_calloc(255, sizeof(char));

    const size_t CAPACITY = JSON_OBJECT_SIZE(1) + 20;
    StaticJsonDocument<CAPACITY> doc;
    JsonObject object = doc.to<JsonObject>();

    if (code == 1) {
        object["status"] = "ok";
    } else if (code == 2) {
        object["status"] = "error";
    } else if (code == 10) {
        object["rfidId"] = gCurrentRfidTagId;
    } else if (code == 20) {
        object["pong"] = "pong";
    }

    serializeJson(doc, jBuf, 255);

    if (client == 0) {
        ws.printfAll(jBuf);
    } else {
        ws.printf(client, jBuf);
    }
    free(jBuf);
}

// Processes websocket-requests
void onWebsocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        //client connected
        Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
        //client->printf("Hello Client %u :)", client->id());
        client->ping();
    } else if (type == WS_EVT_DISCONNECT) {
        //client disconnected
        Serial.printf("ws[%s][%u] disconnect\n", server->url(), client->id());
    } else if (type == WS_EVT_ERROR) {
        //error was received from the other end
        Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
    } else if (type == WS_EVT_PONG) {
        //pong message was received (in response to a ping request maybe)
        Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
    } else if (type == WS_EVT_DATA) {
        //data packet
        AwsFrameInfo *info = (AwsFrameInfo *)arg;
        if (info->final && info->index == 0 && info->len == len) {
            //the whole message is in a single frame and we got all of it's data
            Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);

            if (processJsonRequest((char *)data)) {
                Web_SendWebsocketData(client->id(), 1);
            }

            if (info->opcode == WS_TEXT) {
                data[len] = 0;
                Serial.printf("%s\n", (char *)data);
            } else {
                for (size_t i = 0; i < info->len; i++) {
                    Serial.printf("%02x ", data[i]);
                }
                Serial.printf("\n");
            }
        }
    }
}

// Handles file upload request from the explorer
// requires a GET parameter path, as directory path to the file
void explorerHandleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {

    System_UpdateActivityTimer();

    // New File
    if (!index) {
        String utf8FilePath;
        static char filePath[MAX_FILEPATH_LENTGH];
        if (request->hasParam("path")) {
            AsyncWebParameter *param = request->getParam("path");
            utf8FilePath = param->value() + "/" + filename;
        } else {
            utf8FilePath = "/" + filename;
        }

        convertUtf8ToAscii(utf8FilePath, filePath);

        snprintf(Log_Buffer, Log_BufferLength, "%s: %s", (char *)FPSTR (writingFile), utf8FilePath.c_str());
        Log_Println(Log_Buffer, LOGLEVEL_INFO);

        // Create Ringbuffer for upload
        if (explorerFileUploadRingBuffer == NULL) {
            explorerFileUploadRingBuffer = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF);
        }

        // Create Queue for receiving a signal from the store task as synchronisation
        if (explorerFileUploadStatusQueue == NULL) {
            explorerFileUploadStatusQueue = xQueueCreate(1, sizeof(uint8_t));
        }

        // Create Task for handling the storage of the data
        xTaskCreate(
            explorerHandleFileStorageTask, /* Function to implement the task */
            "fileStorageTask",             /* Name of the task */
            4000,                          /* Stack size in words */
            filePath,                      /* Task input parameter */
            2 | portPRIVILEGE_BIT,         /* Priority of the task */
            &fileStorageTaskHandle         /* Task handle. */
        );
    }

    if (len) {
        // stream the incoming chunk to the ringbuffer
        xRingbufferSend(explorerFileUploadRingBuffer, data, len, portTICK_PERIOD_MS * 1000);
    }

    if (final) {
        // notify storage task that last data was stored on the ring buffer
        xTaskNotify(fileStorageTaskHandle, 1u, eNoAction);
        // watit until the storage task is sending the signal to finish
        uint8_t signal;
        xQueueReceive(explorerFileUploadStatusQueue, &signal, portMAX_DELAY);

        // delete task
        vTaskDelete(fileStorageTaskHandle);
    }
}

void explorerHandleFileStorageTask(void *parameter) {

    File uploadFile;
    size_t item_size;
    uint8_t *item;
    uint8_t value = 0;

    BaseType_t uploadFileNotification;
    uint32_t uploadFileNotificationValue;

    uploadFile = gFSystem.open((char *)parameter, "w");

    for (;;) {
        esp_task_wdt_reset();

        item = (uint8_t *)xRingbufferReceive(explorerFileUploadRingBuffer, &item_size, portTICK_PERIOD_MS * 100);
        if (item != NULL) {
            uploadFile.write(item, item_size);
            vRingbufferReturnItem(explorerFileUploadRingBuffer, (void *)item);
        } else {
            // No data in the buffer, check if all data arrived for the file
            uploadFileNotification = xTaskNotifyWait(0, 0, &uploadFileNotificationValue, 0);
            if (uploadFileNotification == pdPASS) {
                uploadFile.close();
                // done exit loop to terminate
                break;
            }
            vTaskDelay(portTICK_PERIOD_MS * 100);
        }
    }
    // send signal to upload function to terminate
    xQueueSend(explorerFileUploadStatusQueue, &value, 0);
    vTaskDelete(NULL);
}

// Sends a list of the content of a directory as JSON file
// requires a GET parameter path for the directory
void explorerHandleListRequest(AsyncWebServerRequest *request) {
    DynamicJsonDocument jsonBuffer(16384);
    //StaticJsonDocument<4096> jsonBuffer;
    String serializedJsonString;
    AsyncWebParameter *param;
    char filePath[MAX_FILEPATH_LENTGH];
    JsonArray obj = jsonBuffer.createNestedArray();
    File root;
    if (request->hasParam("path")) {
        param = request->getParam("path");
        convertUtf8ToAscii(param->value(), filePath);
        root = gFSystem.open(filePath);
    } else {
        root = gFSystem.open("/");
    }

    if (!root) {
        snprintf(Log_Buffer, Log_BufferLength, (char *) FPSTR(failedToOpenDirectory));
        Log_Println(Log_Buffer, LOGLEVEL_DEBUG);
        return;
    }

    if (!root.isDirectory()) {
        snprintf(Log_Buffer, Log_BufferLength, (char *) FPSTR(notADirectory));
        Log_Println(Log_Buffer, LOGLEVEL_DEBUG);
        return;
    }

    File file = root.openNextFile();

    while (file) {
        // ignore hidden folders, e.g. MacOS spotlight files
        if (!startsWith(file.name(), (char *)"/.")) {
            JsonObject entry = obj.createNestedObject();
            convertAsciiToUtf8(file.name(), filePath);
            std::string path = filePath;
            std::string fileName = path.substr(path.find_last_of("/") + 1);

            entry["name"] = fileName;
            entry["dir"].set(file.isDirectory());
        }
        file = root.openNextFile();

        esp_task_wdt_reset();
    }

    serializeJson(obj, serializedJsonString);
    request->send(200, "application/json; charset=utf-8", serializedJsonString);
}

bool explorerDeleteDirectory(File dir) {

    File file = dir.openNextFile();
    while (file) {

        if (file.isDirectory()) {
            explorerDeleteDirectory(file);
        } else {
            gFSystem.remove(file.name());
        }

        file = dir.openNextFile();

        esp_task_wdt_reset();
    }

    return gFSystem.rmdir(dir.name());
}

// Handles delete request of a file or directory
// requires a GET parameter path to the file or directory
void explorerHandleDeleteRequest(AsyncWebServerRequest *request) {
    File file;
    AsyncWebParameter *param;
    char filePath[MAX_FILEPATH_LENTGH];
    if (request->hasParam("path")) {
        param = request->getParam("path");
        convertUtf8ToAscii(param->value(), filePath);
        if (gFSystem.exists(filePath)) {
            file = gFSystem.open(filePath);
            if (file.isDirectory()) {
                if (explorerDeleteDirectory(file)) {
                    snprintf(Log_Buffer, Log_BufferLength, "DELETE:  %s deleted", param->value().c_str());
                    Log_Println(Log_Buffer, LOGLEVEL_INFO);
                } else {
                    snprintf(Log_Buffer, Log_BufferLength, "DELETE:  Cannot delete %s", param->value().c_str());
                    Log_Println(Log_Buffer, LOGLEVEL_ERROR);
                }
            } else {
                if (gFSystem.remove(filePath)) {
                    snprintf(Log_Buffer, Log_BufferLength, "DELETE:  %s deleted", param->value().c_str());
                    Log_Println(Log_Buffer, LOGLEVEL_INFO);
                } else {
                    snprintf(Log_Buffer, Log_BufferLength, "DELETE:  Cannot delete %s", param->value().c_str());
                    Log_Println(Log_Buffer, LOGLEVEL_ERROR);
                }
            }
        } else {
            snprintf(Log_Buffer, Log_BufferLength, "DELETE: Path %s does not exist", param->value().c_str());
            Log_Println(Log_Buffer, LOGLEVEL_ERROR);
        }
    } else {
        Log_Println("DELETE: No path variable set", LOGLEVEL_ERROR);
    }
    request->send(200);
    esp_task_wdt_reset();
}

// Handles create request of a directory
// requires a GET parameter path to the new directory
void explorerHandleCreateRequest(AsyncWebServerRequest *request) {
    AsyncWebParameter *param;
    char filePath[MAX_FILEPATH_LENTGH];
    if (request->hasParam("path")) {
        param = request->getParam("path");
        convertUtf8ToAscii(param->value(), filePath);
        if (gFSystem.mkdir(filePath)) {
            snprintf(Log_Buffer, Log_BufferLength, "CREATE:  %s created", param->value().c_str());
            Log_Println(Log_Buffer, LOGLEVEL_INFO);
        } else {
            snprintf(Log_Buffer, Log_BufferLength, "CREATE:  Cannot create %s", param->value().c_str());
            Log_Println(Log_Buffer, LOGLEVEL_ERROR);
        }
    } else {
        Log_Println("CREATE: No path variable set", LOGLEVEL_ERROR);
    }
    request->send(200);
}

// Handles rename request of a file or directory
// requires a GET parameter srcpath to the old file or directory name
// requires a GET parameter dstpath to the new file or directory name
void explorerHandleRenameRequest(AsyncWebServerRequest *request) {
    AsyncWebParameter *srcPath;
    AsyncWebParameter *dstPath;
    char srcFullFilePath[MAX_FILEPATH_LENTGH];
    char dstFullFilePath[MAX_FILEPATH_LENTGH];
    if (request->hasParam("srcpath") && request->hasParam("dstpath")) {
        srcPath = request->getParam("srcpath");
        dstPath = request->getParam("dstpath");
        convertUtf8ToAscii(srcPath->value(), srcFullFilePath);
        convertUtf8ToAscii(dstPath->value(), dstFullFilePath);
        if (gFSystem.exists(srcFullFilePath)) {
            if (gFSystem.rename(srcFullFilePath, dstFullFilePath)) {
                snprintf(Log_Buffer, Log_BufferLength, "RENAME:  %s renamed to %s", srcPath->value().c_str(), dstPath->value().c_str());
                Log_Println(Log_Buffer, LOGLEVEL_INFO);
            } else {
                snprintf(Log_Buffer, Log_BufferLength, "RENAME:  Cannot rename %s", srcPath->value().c_str());
                Log_Println(Log_Buffer, LOGLEVEL_ERROR);
            }
        } else {
            snprintf(Log_Buffer, Log_BufferLength, "RENAME: Path %s does not exist", srcPath->value().c_str());
            Log_Println(Log_Buffer, LOGLEVEL_ERROR);
        }
    } else {
        Log_Println("RENAME: No path variable set", LOGLEVEL_ERROR);
    }

    request->send(200);
}

// Handles audio play requests
// requires a GET parameter path to the audio file or directory
// requires a GET parameter playmode
void explorerHandleAudioRequest(AsyncWebServerRequest *request) {
    AsyncWebParameter *param;
    String playModeString;
    uint32_t playMode;
    char filePath[MAX_FILEPATH_LENTGH];
    if (request->hasParam("path") && request->hasParam("playmode")) {
        param = request->getParam("path");
        convertUtf8ToAscii(param->value(), filePath);
        param = request->getParam("playmode");
        playModeString = param->value();

        playMode = atoi(playModeString.c_str());
        AudioPlayer_TrackQueueDispatcher(filePath, 0, playMode, 0);
    } else {
        Log_Println("AUDIO: No path variable set", LOGLEVEL_ERROR);
    }

    request->send(200);
}

// Handles uploaded backup-file and writes valid entries into NVS
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    Led_SetPause(true); // Workaround to prevent exceptions due to Neopixel-signalisation while NVS-write
    char ebuf[290];
    uint16_t j = 0;
    char *token;
    uint8_t count = 0;
    nvs_t nvsEntry[1];

    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n') {
            ebuf[j++] = data[i];
        } else {
            ebuf[j] = '\0';
            j = 0;
            token = strtok(ebuf, stringOuterDelimiter);
            while (token != NULL) {
                if (!count) {
                    count++;
                    memcpy(nvsEntry[0].nvsKey, token, strlen(token));
                    nvsEntry[0].nvsKey[strlen(token)] = '\0';
                } else if (count == 1) {
                    count = 0;
                    memcpy(nvsEntry[0].nvsEntry, token, strlen(token));
                    nvsEntry[0].nvsEntry[strlen(token)] = '\0';
                }
                token = strtok(NULL, stringOuterDelimiter);
            }
            if (isNumber(nvsEntry[0].nvsKey) && nvsEntry[0].nvsEntry[0] == '#') {
                snprintf(Log_Buffer, Log_BufferLength, "%s: %s => %s", (char *) FPSTR(writeEntryToNvs), nvsEntry[0].nvsKey, nvsEntry[0].nvsEntry);
                Log_Println(Log_Buffer, LOGLEVEL_NOTICE);
                gPrefsRfid.putString(nvsEntry[0].nvsKey, nvsEntry[0].nvsEntry);
            }
        }
    }
    Led_SetPause(false);
}

// Dumps all RFID-entries from NVS into a file on SD-card
bool Web_DumpNvsToSd(const char *_namespace, const char *_destFile) {
    Led_SetPause(true);          // Workaround to prevent exceptions due to Neopixel-signalisation while NVS-write

    File backupFile = gFSystem.open(_destFile, FILE_WRITE);
    if (!backupFile) {
        return false;
    }

    // Example of listing all the key-value pairs of any type under specified partition and namespace
    nvs_iterator_t it = nvs_entry_find("nvs", _namespace, NVS_TYPE_STR);
    while (it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        String s = gPrefsRfid.getString((const char *)info.key);
        backupFile.printf("%s%s%s%s\n", stringOuterDelimiter, info.key, stringOuterDelimiter, s.c_str());
        it = nvs_entry_next(it);
    };

    backupFile.close();
    Led_SetPause(false);

    return true;
}
