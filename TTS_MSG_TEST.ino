// ============================================================================
// TITA — ESP32
// Timetable fetch via HTTP + RTC subject identification
// MQTT only for TTS audio messages
// Subject sent to Arduino Uno via Serial2 TX (GPIO 17)
// ============================================================================
//
// ── PIN SUMMARY ─────────────────────────────────────────────────────────────
//  RTC DS3231       SDA           21
//  RTC DS3231       SCL           22
//  I2S Amplifier    BCLK          26
//  I2S Amplifier    LRC/WS        25
//  I2S Amplifier    DOUT/DIN      23
//  Arduino Uno      RX (Pin 0)    GPIO 17 (Serial2 TX)
//  Arduino Uno      GND           GND (common ground)
// ============================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <RTClib.h>
#include <SPIFFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <AudioFileSourceSPIFFS.h>

// ── CONFIG ──────────────────────────────────────────────
const char* WIFI_SSID   = "Bee..";
const char* WIFI_PASS   = "12341234";
const char* DEVICE_ID   = "ECE_A222";
const char* SERVER      = "https://tita-backend.onrender.com";
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const int   POLL_EVERY  = 60000;   // fetch timetable every 60 seconds

// Audio file in SPIFFS to play as "5-minute warning" bell
// Upload your bell/chime file to SPIFFS as /bell.mp3
#define BELL_FILE "/bell.mp3"
#define TTS_FILE  "/tts.mp3"

#define I2S_DOUT  23
#define I2S_BCLK  26
#define I2S_LRC   25

// ── GLOBAL OBJECTS ──────────────────────────────────────
RTC_DS3231 rtc;

AudioGeneratorMP3*     mp3    = nullptr;
AudioOutputI2S*        i2sOut = nullptr;
AudioFileSourceSPIFFS* spiffs = nullptr;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ── TIMETABLE ───────────────────────────────────────────
struct Period {
  String startTime;
  String endTime;
  String subject;
};

#define MAX_PERIODS 12
Period todayPeriods[MAX_PERIODS];
int  periodCount    = 0;
bool timetableReady = false;

unsigned long lastPoll      = 0;
unsigned long lastCheck     = 0;
unsigned long lastMqttRetry = 0;
String        lastSent      = "";

// ── TTS ─────────────────────────────────────────────────
bool   isPlaying  = false;
String pendingUrl = "";
String pendingId  = "";
String currentId  = "";

// ── BELL (5-min warning) ─────────────────────────────────
// Tracks which period index has already had its bell played this session
bool bellPlayed[MAX_PERIODS];
unsigned long lastBellCheck = 0;

// ── AUDIO QUEUE TYPE ─────────────────────────────────────
enum PlayType { PLAY_NONE, PLAY_TTS, PLAY_BELL };
PlayType currentPlayType = PLAY_NONE;
PlayType pendingPlayType = PLAY_NONE;

// ============================================================================
// HELPERS
// ============================================================================

int timeToMinutes(String t) {
  t.trim();
  int colon = t.indexOf(':');
  if (colon < 0) return -1;
  return t.substring(0, colon).toInt() * 60 + t.substring(colon + 1).toInt();
}

String getDayName(int dow) {
  const char* days[] = {
    "Sunday","Monday","Tuesday","Wednesday",
    "Thursday","Friday","Saturday"
  };
  return String(days[dow]);
}

// Pad single-digit numbers for display
String fmt2(int v) {
  return (v < 10 ? "0" : "") + String(v);
}

// ============================================================================
// WIFI
// ============================================================================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("[WiFi] Connecting");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println(WiFi.isConnected()
    ? "\n[WiFi] Connected — IP: " + WiFi.localIP().toString()
    : "\n[WiFi] Failed");
}

// ============================================================================
// SEND TO ARDUINO  (subject / TTS text / commands)
// ============================================================================

// Send subject name — Arduino shows it on P10 display
void sendSubjectToArduino(const String& subject) {
  Serial2.println("SUB:" + subject);
  Serial.println("[UART] Subject → Arduino: " + subject);
}

// Send TTS text so Arduino can display it alongside audio
void sendTtsToArduino(const String& text) {
  Serial2.println("TTS:" + text);
  Serial.println("[UART] TTS text → Arduino: " + text);
}

// Tell Arduino to turn the P10 display ON or OFF via OE pin
void sendDisplayCmd(bool on) {
  Serial2.println(on ? "DISPLAY:ON" : "DISPLAY:OFF");
  Serial.println("[UART] Display → " + String(on ? "ON" : "OFF"));
}

// Acknowledge Arduino that TTS is starting/done
void sendAck(const String& msg) {
  Serial2.println("ACK:" + msg);
  Serial.println("[UART] ACK → Arduino: " + msg);
}

// ============================================================================
// TIMETABLE FETCH
// ============================================================================

void pollTimetable() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Timetable] Skipped — no WiFi");
    return;
  }

  HTTPClient http;
  String url = String(SERVER) + "/api/timetable/" + DEVICE_ID;
  http.begin(url);
  http.setTimeout(10000);
  int code = http.GET();

  if (code != 200) {
    Serial.println("[Timetable] HTTP " + String(code) + " — fetch failed");
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, body)) {
    Serial.println("[Timetable] JSON parse error");
    return;
  }

  DateTime now  = rtc.now();
  String today  = getDayName(now.dayOfTheWeek());

  // ── Minimal date/day print ────────────────────────────
  Serial.println("[RTC] " + fmt2(now.day()) + "/" + fmt2(now.month()) + "/" + String(now.year())
    + "  " + fmt2(now.hour()) + ":" + fmt2(now.minute()) + "  " + today);

  // Resolve JSON array (supports formats A / B / wrapped)
  JsonArray rows;
  if      (doc.is<JsonArray>())           rows = doc.as<JsonArray>();
  else if (doc.containsKey("rows"))       rows = doc["rows"].as<JsonArray>();
  else if (doc.containsKey("timetable")) rows = doc["timetable"].as<JsonArray>();
  else if (doc.containsKey("data"))       rows = doc["data"].as<JsonArray>();

  if (rows.isNull()) {
    Serial.println("[Timetable] No array found in response");
    return;
  }

  periodCount = 0;
  memset(bellPlayed, false, sizeof(bellPlayed));   // reset bell flags on fresh fetch

  for (JsonObject row : rows) {
    String rowDay = row.containsKey("day") ? row["day"].as<String>() :
                    row.containsKey("Day") ? row["Day"].as<String>() : "";
    rowDay.trim();
    if (!rowDay.equalsIgnoreCase(today)) continue;

    String startTime = row.containsKey("startTime") ? row["startTime"].as<String>() :
                       row.containsKey("start")     ? row["start"].as<String>()     :
                       row.containsKey("from")      ? row["from"].as<String>()      : "";
    startTime.trim();

    String endTime   = row.containsKey("endTime")   ? row["endTime"].as<String>()   :
                       row.containsKey("end")       ? row["end"].as<String>()       :
                       row.containsKey("to")        ? row["to"].as<String>()        : "";
    endTime.trim();

    String subject   = row.containsKey("subject")   ? row["subject"].as<String>()   :
                       row.containsKey("Subject")   ? row["Subject"].as<String>()   :
                       row.containsKey("name")      ? row["name"].as<String>()      :
                       row.containsKey("course")    ? row["course"].as<String>()    : "";
    subject.trim();
    if (subject.isEmpty() || subject == "null") subject = "Free";

    if (periodCount < MAX_PERIODS) {
      todayPeriods[periodCount++] = { startTime, endTime, subject };
    }
  }

  timetableReady = (periodCount > 0);
  lastSent       = "";   // force re-send after fresh fetch

  // ── Print all periods for today ───────────────────────
  Serial.println("[Timetable] " + today + " — " + String(periodCount) + " period(s) loaded:");
  for (int i = 0; i < periodCount; i++) {
    Serial.println("  [" + String(i + 1) + "] "
      + todayPeriods[i].startTime + " - "
      + todayPeriods[i].endTime   + "  →  "
      + todayPeriods[i].subject);
  }
}

// ============================================================================
// CHECK AND SEND CURRENT SUBJECT
// ============================================================================

void checkAndSendSubject() {
  if (!timetableReady || periodCount == 0) return;

  DateTime now    = rtc.now();
  int      nowMin = now.hour() * 60 + now.minute();
  int      matchIdx = -1;

  for (int i = 0; i < periodCount; i++) {
    int s = timeToMinutes(todayPeriods[i].startTime);
    int e = timeToMinutes(todayPeriods[i].endTime);
    if (s < 0 || e < 0) continue;
    if (nowMin >= s && nowMin < e) { matchIdx = i; break; }
  }

  String subject = (matchIdx >= 0) ? todayPeriods[matchIdx].subject : "FREE";

  // Print current period only when it changes
  if (subject != lastSent) {
    Serial.println("[Period] Current: " + subject);
    lastSent = subject;

    if (!subject.equalsIgnoreCase("FREE")) {
      sendSubjectToArduino(subject);
      sendDisplayCmd(true);         // turn display ON for active class
    } 
  }
}

// ============================================================================
// 5-MINUTE WARNING BELL
// ============================================================================

void checkBellWarning() {
  if (!timetableReady || periodCount == 0) return;

  DateTime now    = rtc.now();
  int      nowMin = now.hour() * 60 + now.minute();

  for (int i = 0; i < periodCount; i++) {
    if (bellPlayed[i]) continue;

    int e = timeToMinutes(todayPeriods[i].endTime);
    if (e < 0) continue;

    // Fire bell exactly at (endTime - 5) minutes
    int bellAt = e - 5;
    if (nowMin == bellAt) {
      Serial.println("[Bell] 5-min warning for: " + todayPeriods[i].subject
        + "  (ends " + todayPeriods[i].endTime + ")");
      bellPlayed[i] = true;

      if (isPlaying) {
        // Queue the bell to play after current audio
        pendingUrl      = "";       // no URL — it's a local file
        pendingId       = "";
        pendingPlayType = PLAY_BELL;
      } else {
        playBell();
      }
      break;   // only one bell at a time
    }
  }
}

// ============================================================================
// MQTT — only used for TTS
// ============================================================================

void tryConnectMQTT() {
  if (mqttClient.connected()) return;
  if (millis() - lastMqttRetry < 10000) return;
  lastMqttRetry = millis();

  String clientId = "TITA-" + String(DEVICE_ID) + "-" + String(random(0xFFFF), HEX);
  Serial.print("[MQTT] Connecting...");
  if (mqttClient.connect(clientId.c_str())) {
    String topic = "tita/" + String(DEVICE_ID) + "/tts";
    mqttClient.subscribe(topic.c_str());
    Serial.println(" OK");
  } else {
    Serial.println(" Failed (rc=" + String(mqttClient.state()) + ")");
  }
}

// ============================================================================
// TTS
// ============================================================================

void markTtsPlayed(String id) {
  if (id.isEmpty()) return;
  HTTPClient http;
  http.begin(String(SERVER) + "/api/tts/" + id + "/played");
  http.addHeader("Content-Type", "application/json");
  http.PUT("{}");
  http.end();
}

bool downloadMp3(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  int fileSize = http.getSize();
  if (SPIFFS.exists(TTS_FILE)) SPIFFS.remove(TTS_FILE);
  File f = SPIFFS.open(TTS_FILE, FILE_WRITE);
  if (!f) { http.end(); return false; }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[512];
  int written = 0, timeout = 0;
  while (http.connected() && (fileSize < 0 || written < fileSize)) {
    int avail = stream->available();
    if (avail > 0) {
      int r = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
      f.write(buf, r);
      written += r;
      timeout  = 0;
    } else {
      delay(1);
      if (++timeout > 5000) break;
    }
  }
  f.close();
  http.end();
  return written > 0;
}

void stopAudio() {
  if (mp3)    { mp3->stop();    delete mp3;    mp3    = nullptr; }
  if (spiffs) {                 delete spiffs; spiffs = nullptr; }
  isPlaying       = false;
  currentPlayType = PLAY_NONE;
}

void playFromSpiffs(const char* file, PlayType type, const String& ttsId = "") {
  stopAudio();
  currentId       = ttsId;
  currentPlayType = type;
  spiffs = new AudioFileSourceSPIFFS(file);
  mp3    = new AudioGeneratorMP3();
  if (!mp3->begin(spiffs, i2sOut)) {
    stopAudio();
    if (type == PLAY_TTS) markTtsPlayed(ttsId);
    return;
  }
  isPlaying = true;
}

void playBell() {
  Serial.println("[Bell] Playing warning chime");
  sendAck("BELL");                          // tell Arduino bell is ringing
  playFromSpiffs(BELL_FILE, PLAY_BELL);
}

void downloadAndPlay(const String& url, const String& ttsId, const String& ttsText) {
  Serial.println("[TTS] Playing: " + ttsText);
  sendTtsToArduino(ttsText);                // send text to Arduino display
  sendAck("TTS_START");                     // acknowledge start
  if (downloadMp3(url)) {
    playFromSpiffs(TTS_FILE, PLAY_TTS, ttsId);
  } else {
    markTtsPlayed(ttsId);
    sendAck("TTS_FAIL");
  }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, msg)) return;

  String audioUrl = doc["audio_url"].as<String>();
  String ttsId    = doc["id"].as<String>();
  String text     = doc["text"].as<String>();

  Serial.println("[TTS] Received: " + text);

  if (isPlaying) {
    pendingUrl      = audioUrl;
    pendingId       = ttsId;
    pendingPlayType = PLAY_TTS;
    // Store pending TTS text for later use
    // (simple approach: re-send text now so it shows on display immediately)
    sendTtsToArduino(text);
  } else {
    downloadAndPlay(audioUrl, ttsId, text);
  }
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[TITA] Booting...");

  // Serial2 TX only → Arduino Uno Pin 0
  Serial2.begin(9600, SERIAL_8N1, -1, 17);

  // RTC
  Wire.begin(21, 22);
  if (!rtc.begin()) {
    Serial.println("[RTC] ERROR — not found");
  } else {
    DateTime now = rtc.now();
    Serial.println("[RTC] OK — "
      + fmt2(now.day()) + "/" + fmt2(now.month()) + "/" + String(now.year())
      + " " + fmt2(now.hour()) + ":" + fmt2(now.minute()));
    // Uncomment to set time once, then comment again:
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // SPIFFS
  if (!SPIFFS.begin(true)) Serial.println("[SPIFFS] Failed");
  else                     Serial.println("[SPIFFS] OK");

  // I2S audio
  i2sOut = new AudioOutputI2S();
  i2sOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  i2sOut->SetGain(1.7);

  // WiFi
  connectWiFi();

  // MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);

  // Init bell flags
  memset(bellPlayed, false, sizeof(bellPlayed));

  // Fetch timetable + check immediately
  pollTimetable();
  lastPoll = millis();
  checkAndSendSubject();
  lastCheck = millis();

  Serial.println("[TITA] Ready");
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {

  // MQTT — non-blocking reconnect + process (TTS only)
  tryConnectMQTT();
  if (mqttClient.connected()) mqttClient.loop();

  // Timetable refresh every 60 seconds
  if (millis() - lastPoll >= POLL_EVERY) {
    pollTimetable();
    lastPoll = millis();
  }

  // Subject check every 30 seconds
  if (millis() - lastCheck >= 30000) {
    checkAndSendSubject();
    lastCheck = millis();
  }

  // 5-minute bell check every 30 seconds
  if (millis() - lastBellCheck >= 30000) {
    checkBellWarning();
    lastBellCheck = millis();
  }

  // Audio playback loop
  if (isPlaying && mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      PlayType donetype = currentPlayType;
      String   doneId   = currentId;
      stopAudio();

      if (donetype == PLAY_TTS) {
        markTtsPlayed(doneId);
        sendAck("TTS_DONE");
      } else if (donetype == PLAY_BELL) {
        sendAck("BELL_DONE");
      }

      // Play next queued item
      if (pendingPlayType == PLAY_TTS && !pendingUrl.isEmpty()) {
        String url = pendingUrl, id = pendingId;
        pendingUrl = ""; pendingId = "";
        pendingPlayType = PLAY_NONE;
        downloadAndPlay(url, id, "");   // text already sent to Arduino earlier
      } else if (pendingPlayType == PLAY_BELL) {
        pendingPlayType = PLAY_NONE;
        playBell();
      }
    }
  }

  delay(10);
}
