#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <dht.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

HTTPClient http;


WiFiMulti WiFiMulti;

WebSocketsClient webSocket;

TaskHandle_t httpRequestHandle;
SemaphoreHandle_t MainSemaphore = xSemaphoreCreateMutex();

namespace JSON {
    class serializer {
    public:
        static String serializeRequestData(const char* key1, int val1, const char* key2, int val2, const char* eventName, const char* eventValue);
    };
    class deserializer {
    public:
        static DynamicJsonDocument deserializeData(const char* input);
    };
}

DynamicJsonDocument JSON::deserializer::deserializeData(const char* input) {
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, input);
    if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
    }
    return doc;
};

String JSON::serializer::serializeRequestData(const char* key1, int val1, const char* key2, int val2, const char* eventName, const char* eventValue) {
    DynamicJsonDocument doc(256);
    String JSONData;
    if(key1 && val1) {
        doc[key1] = val1;
    }

    if(key2 && val2) {
        doc[key2] = val2;
    }
    doc[eventName] = eventValue;
    serializeJson(doc, JSONData);
    return JSONData;
}

using namespace JSON;

DHT dht(26, DHT11);

struct readings {
    int temperature;
    int humidity;
};

int refreshRate = 1000;

readings values;

[[noreturn]] void sendHttp(void *pvParameters) {
    while (true) {
        if (xSemaphoreTake(MainSemaphore, portMAX_DELAY) == pdTRUE) {
            if (webSocket.isConnected()) {
                String payload = serializer::serializeRequestData("temperature", values.temperature, "humidity", values.humidity, "type", "SEND_DATA");
                webSocket.sendTXT(payload);
                Serial.printf("[CORE [%u]]: Sent data to server\n", xPortGetCoreID());
                vTaskDelay(pdMS_TO_TICKS(1000));
                xSemaphoreGive(MainSemaphore);
            } else {
                Serial.printf("[CORE [%u] CRIT]: Websocket has been disconnected\n", xPortGetCoreID());
                xSemaphoreGive(MainSemaphore);
                vTaskDelete(httpRequestHandle);
            }
        }
    }
}



void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[WS] Disconnected!\n");
            break;
        case WStype_CONNECTED:
            Serial.printf("[WS] Connected to url: %s\n", payload);
            xTaskCreatePinnedToCore(sendHttp, "httpRequest", 10000, nullptr, 1, &httpRequestHandle, 0);
            break;
        case WStype_TEXT: {
            const char *Payload = (char *) payload;
            DynamicJsonDocument payloadData = deserializer::deserializeData(Payload);
            if (payloadData["type"]) {
                if (strcmp(payloadData["type"], "PONG") == 0) {
                    Serial.printf("[WS]: Ping/Pong Frame Event: %s\n", payloadData["type"].as<const char *>());
                } else if (strcmp(payloadData["type"], "CHANGE_RATE") == 0) {
                    Serial.printf("[WS]: Frequency change requested to %d\n", payloadData["data"].as<int>());
                    refreshRate = payloadData["data"].as<int>();
                }
            }
        }
        break;
        case WStype_ERROR:
            break;
        case WStype_BIN:
            break;
        case WStype_FRAGMENT_TEXT_START:
            break;
        case WStype_FRAGMENT_BIN_START:
            break;
        case WStype_FRAGMENT:
            break;
        case WStype_FRAGMENT_FIN:
            break;
        case WStype_PING:
            break;
        case WStype_PONG:
            break;
    }

}


void setup() {
    Serial.begin(9600);
    dht.begin();
    pinMode(2, OUTPUT);
    WiFiMulti.addAP("ssid", "pass");
    while(WiFiMulti.run() != WL_CONNECTED) {
        Serial.println("Attemping connect");
        delay(500);
    }
    Serial.printf("[WS]: Connected to %s\n", WiFi.SSID().c_str());
    webSocket.begin("api.meow.lol", 8080, "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(2000);
}

static unsigned long lastRetrieve = 0;

void loop() {
    webSocket.loop();
    if (millis() - lastRetrieve >= refreshRate) {
        if (dht.readHumidity() > 95) {
            Serial.println("[SENSOR]: Failed to read humidity from DHT sensor");
            values.humidity = 0;
        }
        if (dht.readTemperature() > 50) {
            Serial.println("[SENSOR]: Failed to read temperature from DHT sensor");
            values.temperature = 0;
        }
        values.temperature = int(dht.readTemperature());
        values.humidity = int(dht.readHumidity());
        Serial.printf("[SENSOR]: READ %d percent and %d with RR %d\n", values.humidity, values.temperature, refreshRate);
        lastRetrieve = millis();
    }
}
