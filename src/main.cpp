#define LED_PIN 18

#include <ArduinoOTA.h>
#include <Arduino_MQTT_Client.h>
#include <ThingsBoard.h>
#include <WiFi.h>

#include "../project_config.h"
#include "DHT20.h"
#include "Wire.h"
#include "scheduler.h"

constexpr uint32_t MAX_MESSAGE_SIZE = 1024U;
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;
constexpr uint32_t MAX_WIFI_CONNECTION_ATTEMPT = 20;

constexpr char LED_STATE_ATTR[] = "ledState";

constexpr uint32_t TELEMETRY_SEND_INTERVAL = 10000U;
constexpr uint32_t READ_DHT20_INTERVAL = 5000U;

constexpr uint32_t SERVER_CHECK_INTERVAL = 1000U;
constexpr uint32_t WIFI_RECONNECT_INTERVAL = 1000U;
constexpr uint32_t WIFI_CONNECTION_ATTEMPT_INTERVAL = 250U;

constexpr uint32_t DEFAULT_TASK_DELAY = 1000U;
constexpr uint32_t MUTEX_WAIT_TICKS = 100U;

volatile bool attributesChanged = false;
volatile bool ledState = false;

constexpr std::array<const char *, 1U> SHARED_ATTRIBUTES_LIST = {LED_STATE_ATTR};

volatile uint16_t blinkingInterval = 1000U;
uint32_t previousStateChange;
uint32_t previousDataSend;

SemaphoreHandle_t sensorDataMutex;
bool serverConnected = false;
volatile bool attributeChanged = false;

struct {
    bool connected = false;
    bool connecting = false;
    uint8_t connectionAttempts = 0;
    uint32_t lastAttemptTime = 0;
} wifiState;

struct {
    double temperature = NAN;
    double humidity = NAN;
} sensor;

WiFiClient wifiClient;
Arduino_MQTT_Client mqttClient(wifiClient);
ThingsBoard tb(mqttClient, MAX_MESSAGE_SIZE);

DHT20 DHT;

TaskHandle_t TaskReadDHT20Handle = NULL;
TaskHandle_t TaskConnectWiFiHandle = NULL;
TaskHandle_t TaskConnectServerHandle = NULL;
TaskHandle_t TaskSendDHT20DataHandle = NULL;
TaskHandle_t TaskSendAttributesHandle = NULL;

void initTask();
void initWiFi();
void initDHT20();

void TaskReadDHT20(void *pvParameters);
void TaskConnectWiFi(void *pvParameters);
void TaskConnectServer(void *pvParameters);
void TaskSendDHT20Data(void *pvParameters);
void TaskSendAttributes(void *pvParameters);

RPC_Response setLedSwitchState(const RPC_Data &data) {
    Serial.println("Switch state received ");
    bool newState = data;
    Serial.print("Switch state changed to ");
    Serial.println(newState);
    digitalWrite(LED_PIN, newState);
    attributesChanged = true;
    return RPC_Response("setLedSwitchValue", newState);
}

const std::array<RPC_Callback, 1U> callbacks = {
    RPC_Callback{"setLedSwitchValue", setLedSwitchState}};

void processSharedAttributes(const Shared_Attribute_Data &data) {
    for (auto it = data.begin(); it != data.end(); ++it) {
        if (strcmp(it->key().c_str(), LED_STATE_ATTR) == 0) {
            // Update LED indicator
            ledState = it->value().as<bool>();
            digitalWrite(LED_PIN, ledState);
            Serial.print("DHT20 sensor: ");
            Serial.println(ledState);
            // Create & Delete sensor task
            if (ledState == true && (TaskReadDHT20Handle == NULL &&
                                     TaskSendDHT20DataHandle == NULL)) {
                xTaskCreate(TaskReadDHT20, "readDHT20Sensor", 4096U, NULL, 1,
                            &TaskReadDHT20Handle);
                xTaskCreate(TaskSendDHT20Data, "sendDHT20Sensor", 4096U, NULL,
                            1, &TaskSendDHT20DataHandle);

            } else if (ledState == false && (TaskReadDHT20Handle != NULL &&
                                             TaskSendDHT20DataHandle != NULL)) {
                vTaskDelete(TaskReadDHT20Handle);
                vTaskDelete(TaskSendDHT20DataHandle);
                TaskReadDHT20Handle = NULL;
                TaskSendDHT20DataHandle = NULL;
            }
        }
        attributesChanged = true;
    }
}
const Shared_Attribute_Callback attributes_callback(
    &processSharedAttributes, SHARED_ATTRIBUTES_LIST.cbegin(),
    SHARED_ATTRIBUTES_LIST.cend());
const Attribute_Request_Callback attribute_shared_request_callback(
    &processSharedAttributes, SHARED_ATTRIBUTES_LIST.cbegin(),
    SHARED_ATTRIBUTES_LIST.cend());

void TaskReadDHT20(void *pvParameters) {
    while (1) {
        if (millis() - DHT.lastRead() >= READ_DHT20_INTERVAL) {
            int status = DHT.read();

            if (status == DHT20_OK) {
                if (xSemaphoreTake(sensorDataMutex,
                                   pdMS_TO_TICKS(MUTEX_WAIT_TICKS)) == pdTRUE) {
                    sensor.temperature = DHT.getTemperature();
                    sensor.humidity = DHT.getHumidity();
                    Serial.print("Data Sensor: ");
                    Serial.print(sensor.temperature, 1);
                    Serial.print("°C | ");
                    Serial.print(sensor.humidity, 1);
                    Serial.println("%");
                    xSemaphoreGive(sensorDataMutex);
                }
            } 
            else 
                Serial.print("DHT20 sensor Error");
        }
        vTaskDelay(pdMS_TO_TICKS(READ_DHT20_INTERVAL));
    }
}

void TaskConnectWiFi(void *pvParameters) {
    while (1) {
        // WiFi is connected
        if (WiFi.status() == WL_CONNECTED) {
            // Connection was established
            if (!wifiState.connected) {
                Serial.println("WiFi connected.");
                wifiState.connected = true;
            }
            // Maintain connection
            wifiState.connecting = false;
            wifiState.connectionAttempts = 0;
            vTaskDelay(pdMS_TO_TICKS(DEFAULT_TASK_DELAY));
            continue;
        }

        // WiFi is not connected
        
        uint32_t currentTime = millis();
        if (!wifiState.connecting && (currentTime - wifiState.lastAttemptTime >=
                                      WIFI_RECONNECT_INTERVAL)) {
            Serial.println("Wifi connecting ...");
            WiFi.disconnect();  // Ensure clean connection attempt
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

            wifiState.connecting = true;
            wifiState.connectionAttempts = 0;
            wifiState.lastAttemptTime = currentTime;
        }

        // Connection attempt in progress
        if (wifiState.connecting) {
            Serial.print(".");

            // Increase the attempt counters and check for max attempts
            wifiState.connectionAttempts++;
            if (wifiState.connectionAttempts >= MAX_WIFI_CONNECTION_ATTEMPT) {
                Serial.println(
                    "\n WiFi failed to connect. Will retry later.");
                WiFi.disconnect();
                wifiState.connecting = false;
            }
            vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECTION_ATTEMPT_INTERVAL));
        } else {
            vTaskDelay(pdMS_TO_TICKS(DEFAULT_TASK_DELAY));
        }
    }
}

void TaskConnectServer(void *pvParameters) {
    while (1) {
        // Validate WiFi connection
        if (!wifiState.connected) {
            serverConnected = false;
            vTaskDelay(pdMS_TO_TICKS(DEFAULT_TASK_DELAY));
            continue;
        }

        if (!tb.connected()) {
            Serial.print("Connecting to ThingsBoard server: ");
            Serial.println(THINGSBOARD_SERVER);

            if (tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT)) {
                // Connect server succeed
                Serial.println(
                    "Connected to ThingsBoard server.");
                serverConnected = true;                                                                                                                                                                                                          

                // Implement shared attribute
                if (!tb.Shared_Attributes_Request(
                        attribute_shared_request_callback)) {
                    Serial.println(
                        "Failed to SharedAttribute Request.");
                    continue;
                }
                if (!tb.Shared_Attributes_Subscribe(attributes_callback)) {
                    Serial.println(
                        "Failed to SharedAttribute Subscribe.");
                    continue;
                }
                Serial.println(
                    "SharedAttribute Subscribe done.");
            } else {
                // Connect server failed
                Serial.println(
                    "Failed to connect to ThingsBoard Server");
                serverConnected = false;
            }
        } else {
            serverConnected = true;
        }
        vTaskDelay(pdMS_TO_TICKS(SERVER_CHECK_INTERVAL));
    }
}

void TaskSendDHT20Data(void *pvParameters) {
    uint32_t previousDataSendTime = 0;

    while (1) {
        if (serverConnected && wifiState.connected) {
            uint32_t currentTime = millis();

            if (currentTime - previousDataSendTime >= TELEMETRY_SEND_INTERVAL) {
                previousDataSendTime = currentTime;
                if (xSemaphoreTake(sensorDataMutex,
                                   pdMS_TO_TICKS(MUTEX_WAIT_TICKS) == pdTRUE)) {
                    if (isnan(sensor.temperature) || isnan(sensor.humidity)) {
                        Serial.println("DHT20 sensor: Failed to read.");
                    } else {
                        Serial.print("Data sent: ");
                        Serial.print(sensor.temperature);
                        Serial.print("°C | ");
                        Serial.print(sensor.humidity);
                        Serial.println("%");

                        tb.sendTelemetryData("temperature", sensor.temperature);
                        tb.sendTelemetryData("humidity", sensor.humidity);
                    }
                    xSemaphoreGive(sensorDataMutex);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_SEND_INTERVAL));
    }
}

void TaskSendAttributes(void *pvParameters) {
    while (true) {
        if (attributesChanged) {
            attributesChanged = false;
        }

        tb.sendAttributeData("rssi", WiFi.RSSI());
        tb.sendAttributeData("channel", WiFi.channel());
        tb.sendAttributeData("bssid", WiFi.BSSIDstr().c_str());
        tb.sendAttributeData("localIp", WiFi.localIP().toString().c_str());
        tb.sendAttributeData("ssid", WiFi.SSID().c_str());

        vTaskDelay(1000 / portTICK_PERIOD_MS);  // 1s delay
    }
}

void initWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    Serial.println("WiFi initialized.");
}

void initDHT20() {
    // Wire initialize
    if (Wire.begin()) {
        Serial.println("Wire initialized.");
    } else {
        Serial.println("Wire failed to initialize.");
    }
    // DHT initialize
    if (DHT.begin()) {
        Serial.println("DHT20 sensor initialized.");
    } else {
        Serial.println("DHT20 sensor failed to initialize.");
    }
    // Indicator LED initialize
    pinMode(LED_PIN, OUTPUT);
}

void initTask() {
    xTaskCreate(TaskConnectWiFi, "connectWiFi", 4096U, NULL, 1,
                &TaskConnectWiFiHandle);
    xTaskCreate(TaskConnectServer, "connectServer", 4096U, NULL, 1,
                &TaskConnectServerHandle);
    xTaskCreate(TaskSendAttributes, "sendAttributes", 4096U, NULL, 1,
                &TaskSendAttributesHandle);
    Serial.println("RTOS Task created.");
}

void setup() {
    Serial.begin(SERIAL_DEBUG_BAUD);
    sensorDataMutex = xSemaphoreCreateMutex();
    if (sensorDataMutex == NULL) {
        Serial.println("System failed to create mutex.");
        while (1); 
    }

    initWiFi();
    initDHT20();

    initTask();
}
void loop() {
    tb.loop();
    vTaskDelay(pdMS_TO_TICKS(10U));
}