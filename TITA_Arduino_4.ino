// ============================================================================
// TITA — Arduino Uno
// Receives commands from ESP32 via SoftwareSerial (Pin 2 RX, Pin 3 TX)
// Controls P10 LED panel via DMD library + TimerOne (pins UNCHANGED)
// ============================================================================
//
// ── PIN SUMMARY (UNCHANGED from your original code) ─────────────────────────
//  P10 Panel        CLK / SCK     Pin 13  (SPI)
//  P10 Panel        R / MOSI      Pin 11  (SPI)
//  P10 Panel        SCLK / LAT    Pin 8   (DMD default)
//  P10 Panel        A             Pin 6   (DMD default)
//  P10 Panel        B             Pin 7   (DMD default)
//  P10 Panel        OE            DMD + TimerOne handle automatically
//  ESP32 GPIO 17    TX  →         Pin 2   (SoftwareSerial RX)
//  Arduino                        Pin 3   (SoftwareSerial TX — unused)
//  Common GND       GND ↔ GND
// ============================================================================
//
// ── PROTOCOL RECEIVED FROM ESP32 ────────────────────────────────────────────
//  "SUB:<subject>"      Show subject on display, turn display ON
//  "TTS:<text>"         Scroll TTS message 4x then restore subject
//  "DISPLAY:ON"         Enable display, restore subject text
//  "DISPLAY:OFF"        Clear and disable display
//  "ACK:BELL"           Flash display 3x, show "Class Ending Soon!"
//  "ACK:BELL_DONE"      Bell done — restore subject
//  "ACK:TTS_START"      TTS audio started
//  "ACK:TTS_DONE"       TTS done — restore subject
//  "ACK:TTS_FAIL"       TTS failed — restore subject
// ============================================================================

#include <SPI.h>
#include <DMD.h>
#include <TimerOne.h>
#include "SystemFont5x7.h"
#include "Arial_black_16.h"
#include <SoftwareSerial.h>

#define ROW    1
#define COLUMN 1
#define FONT   Arial_Black_16

DMD            led_module(ROW, COLUMN);
SoftwareSerial espSerial(2, 3);   // RX = Pin 2 (ESP32 TX), TX = Pin 3 (unused)

// ── STATE ─────────────────────────────────────────────────────────────────
String currentSubject = "WAITING";  // last subject name received
String displayText    = "ADVANCED CLASSROOM TIMETABLE MANAGEMENT AND ANNOUNCEMENT SYSTEM";  // text currently scrolling (subject/bell)
String pendingTtsText = "";         // TTS text waiting to scroll 4x
bool   displayEnabled = true;       // whether panel is active
bool   ttsScrolling   = false;      // true only when TTS: command received

// ── DMD SCAN ISR ─────────────────────────────────────────────────────────
void scan_module() {
  led_module.scanDisplayBySPI();
}

// ============================================================================
// DISPLAY HELPERS
// ============================================================================

void enableDisplay() {
  displayEnabled = true;
  Serial.println("[Display] ON");
}

void disableDisplay() {
  displayEnabled = false;
  led_module.clearScreen(true);
  Serial.println("[Display] OFF");
}

// Read and clean one line from espSerial — strips garbage bytes
String readCleanLine() {
  String line = espSerial.readStringUntil('\n');
  line.trim();
  String clean = "";
  for (int i = 0; i < (int)line.length(); i++) {
    char c = line.charAt(i);
    if (c >= 32 && c <= 126) clean += c;
  }
  clean.trim();
  return clean;
}

// ── SCROLL ONE FULL PASS ──────────────────────────────────────────────────
// Used for normal subject scrolling. Breaks early if serial arrives.
void scrollOnce(const String& text) {
  if (!displayEnabled || text.length() == 0) return;

  led_module.selectFont(FONT);
  led_module.drawMarquee(text.c_str(), text.length(), (32 * COLUMN), 0);

  long timming = millis();
  bool done    = false;
  while (!done) {
    if ((timming + 20) < millis()) {
      done    = led_module.stepMarquee(-1, 0);
      timming = millis();
    }
    if (espSerial.available()) break;  // yield to incoming command
  }
}

// ── SCROLL ONE FULL PASS — NO INTERRUPT ───────────────────────────────────
// Used inside TTS 4x scroll. Never breaks early — completes the full pass
// regardless of serial activity. Serial is read AFTER all 4 passes finish.
void scrollOnceNoBreak(const String& text) {
  if (!displayEnabled || text.length() == 0) return;

  led_module.selectFont(FONT);
  led_module.drawMarquee(text.c_str(), text.length(), (32 * COLUMN), 0);

  long timming = millis();
  bool done    = false;
  while (!done) {
    if ((timming + 20) < millis()) {
      done    = led_module.stepMarquee(-1, 0);
      timming = millis();
    }
    // NO espSerial.available() check here — TTS must complete each pass
  }
}

// ── SCROLL TTS TEXT EXACTLY 4 FULL PASSES ────────────────────────────────
// Completes all 4 passes without any serial interruption.
// Any bytes that arrived during scrolling are drained and processed after.
void scrollTts4x(const String& text) {
  Serial.println("[Display] TTS x4 start: " + text);

  for (int p = 0; p < 4; p++) {
    if (!displayEnabled) break;
    scrollOnceNoBreak(text);
    Serial.println("[Display] TTS pass " + String(p + 1) + "/4 done");
  }

  Serial.println("[Display] TTS x4 done — back to subject: " + currentSubject);

  // Drain any bytes that arrived during the 4x scroll
  while (espSerial.available()) {
    String line = readCleanLine();
    if (line.length() > 0) {
      Serial.println("[UART-drain] " + line);
      // Only handle subject/display updates from drained commands
      // Ignore ACK:TTS_DONE here — we restore subject ourselves below
    }
  }
}

// Flash the panel N times — visual bell alert
void flashDisplay(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    led_module.clearScreen(true);
    delay(delayMs);
    if (displayEnabled && displayText.length() > 0) {
      led_module.selectFont(FONT);
      led_module.drawMarquee(displayText.c_str(), displayText.length(), (32 * COLUMN), 0);
    }
    delay(delayMs);
  }
}

// ============================================================================
// COMMAND PROCESSOR
// ============================================================================

void processCommand(const String& line) {

  Serial.println("[UART] " + line);

  // ── Subject ───────────────────────────────────────────────────────────────
  if (line.startsWith("SUB:")) {
    currentSubject = line.substring(4);
    currentSubject.trim();
    Serial.println("[TITA] Subject: " + currentSubject);
    enableDisplay();
    // Only switch displayText if TTS is NOT currently scrolling
    if (!ttsScrolling) {
      displayText = currentSubject;
    }
  }

  // ── TTS: scroll message 4x, then restore subject ──────────────────────────
  // Subject is NEVER skipped unless ESP32 explicitly sends TTS:
  else if (line.startsWith("TTS:")) {
    pendingTtsText = line.substring(4);
    pendingTtsText.trim();
    ttsScrolling   = true;
    Serial.println("[TITA] TTS: " + pendingTtsText);
    // Actual 4x scroll handled in loop()
  }

  // ── Display ON / OFF ─────────────────────────────────────────────────────
  else if (line == "DISPLAY:ON") {
    enableDisplay();
    if (!ttsScrolling) displayText = currentSubject;
  }
  else if (line == "DISPLAY:OFF") {
    disableDisplay();
  }

  // ── ACK: Bell warning ────────────────────────────────────────────────────
  else if (line == "ACK:BELL") {
    Serial.println("[ACK] Bell — flashing panel");
    flashDisplay(3, 300);
    displayText = "Class Ending Soon!";
  }
  else if (line == "ACK:BELL_DONE") {
    Serial.println("[ACK] Bell done — restoring subject");
    displayText = currentSubject;
  }

  // ── ACK: TTS ─────────────────────────────────────────────────────────────
  else if (line == "ACK:TTS_START") {
    Serial.println("[ACK] TTS started");
  }
  else if (line == "ACK:TTS_DONE") {
    Serial.println("[ACK] TTS done — restoring subject");
    ttsScrolling   = false;
    pendingTtsText = "";
    displayText    = currentSubject;
  }
  else if (line == "ACK:TTS_FAIL") {
    Serial.println("[ACK] TTS failed — restoring subject");
    ttsScrolling   = false;
    pendingTtsText = "";
    displayText    = currentSubject;
  }

  else {
    Serial.println("[UART] Unknown — ignored");
  }
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  Serial.begin(9600);
  espSerial.begin(9600);

  Timer1.initialize(2000);
  Timer1.attachInterrupt(scan_module);

  led_module.clearScreen(true);

  Serial.println("[TITA-UNO] Ready");
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {

  // ── Check for incoming command from ESP32 ─────────────────────────────────
  if (espSerial.available()) {
    String clean = readCleanLine();
    if (clean.length() > 0) {
      processCommand(clean);
    }
  }

  // ── TTS: scroll 4 full uninterrupted passes, then restore subject ──────────
  // ONLY triggered when ESP32 sends TTS: — subject scrolls normally otherwise.
  if (ttsScrolling && pendingTtsText.length() > 0) {
    String ttsText = pendingTtsText;  // copy before clearing
    scrollTts4x(ttsText);
    // Always restore subject after TTS finishes
    ttsScrolling   = false;
    pendingTtsText = "";
    displayText    = currentSubject;
    return;  // go back to top of loop() to read any pending serial
  }

  // ── Default: scroll subject name (or bell message) continuously ───────────
  scrollOnce(displayText);
}
