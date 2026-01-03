#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <BH1750.h>
#include <ArduinoJson.h> // Recommended for creating JSON payloads

// =============================================================
// CONFIGURATION (Based on Section 2.2 & 4.0)
// =============================================================
#define DHTPIN 4
#define DHTTYPE DHT22
#define SOIL_PIN 34
#define BATTERY_PIN 35
#define AWS_IOT_PUBLISH_TOPIC   "willow/sensors/data"
#define AWS_IOT_SUBSCRIBE_TOPIC "willow/sensors/downlink"

// Thresholds for Delta Compression (Section 2.1, Page 32)
const float TEMP_THRESHOLD = 0.5;   // 0.5°C change required
const float MOIST_THRESHOLD = 2.0;  // 2% change required

// Wi-Fi & AWS Credentials (Placeholders)
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* AWS_IOT_ENDPOINT = "your-endpoint.iot.region.amazonaws.com";

// X.509 Certificates (Section 4.1, Page 44)
static const char AWS_CERT_CA[] PROGMEM = R"EOF( ... INSERT AWS CA CERT ... )EOF";
static const char AWS_CERT_CRT[] PROGMEM = R"EOF( ... INSERT DEVICE CERT ... )EOF";
static const char AWS_CERT_PRIVATE[] PROGMEM = R"EOF( ... INSERT PRIVATE KEY ... )EOF";

// Global Objects
DHT dht(DHTPIN, DHTTYPE);
BH1750 lightMeter;
WiFiClientSecure net = WiFiClientSecure();
PubSubClient client(net);

// RTC Memory to store data during Deep Sleep (Section 2.1, Page 32)
// "Data is cached and saved in RTC memory but not transmitted"
RTC_DATA_ATTR float last_uploaded_temp = 0.0;
RTC_DATA_ATTR float last_uploaded_moist = 0.0;

// =============================================================
// 1. SIGNAL CONDITIONING (Section 2.1, Page 32)
// =============================================================
// Implements "Windowed Moving Average" - Reads 5 samples over 10 seconds
struct SensorData {
    float temperature;
    float humidity;
    float moisture;
    float light;
    float battery;
};

SensorData acquire_and_filter_data() {
    float temp_sum = 0, humid_sum = 0, moist_sum = 0, light_sum = 0;
    int samples = 5; 
    
    // PDF: "Reads 5 samples over a 10 second interval and calculates the mean"
    for (int i = 0; i < samples; i++) {
        temp_sum += dht.readTemperature();
        humid_sum += dht.readHumidity();
        moist_sum += analogRead(SOIL_PIN); 
        light_sum += lightMeter.readLightLevel();
        
        delay(2000); // 2 seconds * 5 samples = 10 seconds total
    }

    SensorData data;
    data.temperature = temp_sum / samples;
    data.humidity = humid_sum / samples;
    
    // Normalize Soil Moisture (0-4095 to Percentage)
    data.moisture = map(moist_sum / samples, 4095, 0, 0, 100); 
    data.light = light_sum / samples;

    // Battery Logic (Section 1.4, Page 29)
    int adc_value = analogRead(BATTERY_PIN);
    data.battery = (adc_value / 4095.0) * 4.2 * 2; 

    return data;
}

// =============================================================
// 2. AWS CONNECTIVITY (Section 3.1 & 4.1)
// =============================================================
void connectToAWS() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    // Secure Connection (TLS 1.2 + Mutal Auth)
    net.setCACert(AWS_CERT_CA);
    net.setCertificate(AWS_CERT_CRT);
    net.setPrivateKey(AWS_CERT_PRIVATE);

    client.setServer(AWS_IOT_ENDPOINT, 8883);

    while (!client.connected()) {
        Serial.print("Connecting to AWS IoT...");
        if (client.connect("ESP32_Willow_Device")) {
            Serial.println("Connected!");
        } else {
            delay(100);
        }
    }
}

// =============================================================
// 3. MAIN LOGIC LOOP
// =============================================================
void setup() {
    Serial.begin(115200);
    dht.begin();
    Wire.begin();
    lightMeter.begin();

    // 1. Acquire Clean Data
    Serial.println("Acquiring 5-sample average...");
    SensorData currentData = acquire_and_filter_data();

    // 2. Bandwidth Optimization (Delta Compression) - Page 32
    // Compare current cleaned data with previous transmitted value (from RTC)
    float tempDiff = abs(currentData.temperature - last_uploaded_temp);
    float moistDiff = abs(currentData.moisture - last_uploaded_moist);

    bool significantChange = (tempDiff > TEMP_THRESHOLD) || (moistDiff > MOIST_THRESHOLD);

    // Logic: If significant change OR battery is high (force update), upload.
    // Otherwise, cache in RTC and sleep.
    if (significantChange) {
        Serial.println("Significant change detected. Uploading to AWS...");
        
        connectToAWS();

        // Create JSON Payload
        StaticJsonDocument<200> doc;
        doc["temperature"] = currentData.temperature;
        doc["humidity"] = currentData.humidity;
        doc["moisture"] = currentData.moisture;
        doc["light"] = currentData.light;
        doc["battery"] = currentData.battery;
        
        char jsonBuffer[512];
        serializeJson(doc, jsonBuffer);

        // MQTT Publish (Section 4.0, Page 21)
        client.publish(AWS_IOT_PUBLISH_TOPIC, jsonBuffer);

        // Update RTC Memory
        last_uploaded_temp = currentData.temperature;
        last_uploaded_moist = currentData.moisture;
    
    } else {
        Serial.println("Delta small. Skipping upload to save power (Section 2.1).");
    }

    // 3. Deep Sleep (Section 1.4, Page 30)
    // "Wake up every 5 minutes" (High Energy Mode) or "60 mins" (Low Energy)
    uint64_t sleep_duration;
    if (currentData.battery > 4.0) {
        sleep_duration = 5 * 60 * 1000000; // 5 Minutes
    } else {
        sleep_duration = 60 * 60 * 1000000; // 60 Minutes
    }

    esp_sleep_enable_timer_wakeup(sleep_duration);
    esp_deep_sleep_start();
}

void loop() {
    // Empty because we use Deep Sleep in setup() as per "Stateless" design (Page 29)
}




