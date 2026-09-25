/*
  ECG Monitoring System - ESP32 + AD8232
  ----------------------------------------------------
  - Reads analog ECG waveform from AD8232 sensor
  - Detects R-peaks in real time to compute heart rate (BPM)
  - Flags tachycardia (>100 BPM) / bradycardia (<60 BPM)
  - Detects "leads off" (electrode disconnected) condition
  - Streams raw ECG, BPM, and alert status to Ubidots over HTTP

  Wiring (AD8232 -> ESP32):
    OUTPUT -> GPIO34 (ADC1_CH6, analog in)
    LO+    -> GPIO32
    LO-    -> GPIO33
    SDN    -> 3V3 (or a GPIO held HIGH to keep it enabled)
    3.3V   -> 3V3
    GND    -> GND

  Ubidots setup:
    1. Create a device (any label, e.g. "esp32-ecg").
    2. Create three variables under it: ecg, bpm, alert
    3. Copy your Ubidots TOKEN into UBIDOTS_TOKEN below.
    4. Use the STEM/Educational or Industrial account as applicable;
       this sketch uses the STEM/Ubidots.com endpoint by default.
*/

#include <WiFi.h>
#include <HTTPClient.h>

// ---------- WiFi ----------
const char* WIFI_SSID     = "Rahul Ranjan Singh's iphone ";
const char* WIFI_PASSWORD = "rahulranjan2";

// ---------- Ubidots ----------
const char* UBIDOTS_TOKEN  = "BBUS-yPhKRIykkFcHaWJK3hvrOlJqtVvioY";
const char* DEVICE_LABEL   = "esp32-ecg";
const char* UBIDOTS_HOST   = "https://industrial.api.ubidots.com"; // use http://things.ubidots.com for STEM/education accounts

// ---------- Pins ----------
const int ECG_PIN   = 34;   // AD8232 OUTPUT
const int LO_PLUS    = 32;  // AD8232 LO+
const int LO_MINUS   = 33;  // AD8232 LO-

const int   SAMPLE_INTERVAL_MS = 4;     
const int   PEAK_THRESHOLD     = 2200; 
const unsigned long REFRACTORY_MS = 300;

unsigned long lastSampleTime = 0;
unsigned long lastPeakTime   = 0;
unsigned long lastBeatInterval = 0;
bool aboveThreshold = false;

int  bpm = 0;
const int BPM_WINDOW = 5;    
int  bpmHistory[BPM_WINDOW];
int  bpmIndex = 0;
int  bpmCount = 0;

const int TACHY_BPM = 100;
const int BRADY_BPM = 60;

const unsigned long UPLOAD_INTERVAL_MS = 2000;
unsigned long lastUploadTime = 0;

const int ALARM_PIN = 25;

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected, IP: " + WiFi.localIP().toString());
}

void setup() {
  Serial.begin(115200);

  pinMode(LO_PLUS, INPUT);
  pinMode(LO_MINUS, INPUT);
  pinMode(ALARM_PIN, OUTPUT);
  digitalWrite(ALARM_PIN, LOW);

  analogReadResolution(12); // 0-4095

  connectWiFi();
}

// Sends a variable set to Ubidots via HTTP POST
void sendToUbidots(int ecgValue, int bpmValue, const char* alertStatus) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  HTTPClient http;
  String url = String(UBIDOTS_HOST) + "/api/v1.6/devices/" + DEVICE_LABEL;

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Auth-Token", UBIDOTS_TOKEN);

  String payload = "{";
  payload += "\"ecg\": " + String(ecgValue) + ",";
  payload += "\"bpm\": " + String(bpmValue) + ",";
  payload += "\"alert\": {\"value\": 1, \"context\": {\"status\": \"" + String(alertStatus) + "\"}}";
  payload += "}";

  int httpCode = http.POST(payload);
  if (httpCode <= 0) {
    Serial.print("Ubidots POST failed: ");
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}

const char* classifyBpm(int value) {
  if (value == 0) return "no_signal";
  if (value > TACHY_BPM) return "tachycardia";
  if (value < BRADY_BPM) return "bradycardia";
  return "normal";
}

void loop() {
  unsigned long now = millis();


  bool leadsOff = (digitalRead(LO_PLUS) == HIGH) || (digitalRead(LO_MINUS) == HIGH);

  if (leadsOff) {
    Serial.println("LEADS OFF - check electrode contact");
    digitalWrite(ALARM_PIN, HIGH); // continuous alarm on disconnect
    delay(SAMPLE_INTERVAL_MS);
    return;
  }

  if (now - lastSampleTime >= SAMPLE_INTERVAL_MS) {
    lastSampleTime = now;
    int ecgValue = analogRead(ECG_PIN);

    if (ecgValue > PEAK_THRESHOLD && !aboveThreshold &&
        (now - lastPeakTime) > REFRACTORY_MS) {
      aboveThreshold = true;

      if (lastPeakTime != 0) {
        lastBeatInterval = now - lastPeakTime;
        int instantBpm = 60000 / lastBeatInterval;

        bpmHistory[bpmIndex] = instantBpm;
        bpmIndex = (bpmIndex + 1) % BPM_WINDOW;
        if (bpmCount < BPM_WINDOW) bpmCount++;

        long sum = 0;
        for (int i = 0; i < bpmCount; i++) sum += bpmHistory[i];
        bpm = sum / bpmCount;
      }
      lastPeakTime = now;
    } else if (ecgValue < PEAK_THRESHOLD) {
      aboveThreshold = false;
    }

    Serial.println(ecgValue);

    const char* status = classifyBpm(bpm);
    digitalWrite(ALARM_PIN, (strcmp(status, "tachycardia") == 0 ||
                              strcmp(status, "bradycardia") == 0) ? HIGH : LOW);

    if (now - lastUploadTime >= UPLOAD_INTERVAL_MS) {
      lastUploadTime = now;
      sendToUbidots(ecgValue, bpm, status);
      Serial.print("Uploaded -> BPM: ");
      Serial.print(bpm);
      Serial.print(" | Status: ");
      Serial.println(status);
    }
  }
}
