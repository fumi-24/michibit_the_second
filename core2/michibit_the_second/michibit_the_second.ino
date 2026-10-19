/*
 * michibit the second
 * A BLE-controlled Mars rover on M5Stack Core2 + 4EncoderMotor,
 * with a Stackchan-style animated avatar face.
 *
 * Author: fumi24
 * Submitted to M5Stack Global Innovation Contest 2026
 *
 * License: MIT (this file)
 *
 * Third-party libraries used:
 *   M5Unified               - MIT           - https://github.com/m5stack/M5Unified
 *   M5Stack_Avatar           - MIT           - https://github.com/meganetaaan/m5stack-avatar
 *   M5Module-4EncoderMotor   - MIT           - https://github.com/m5stack/M5Module-4EncoderMotor
 *   ArduinoJson              - MIT           - https://github.com/bblanchon/ArduinoJson
 *   ESP32 BLE Library        - Apache-2.0    - (part of arduino-esp32)
 *   Adafruit NeoPixel        - LGPL-3.0      - https://github.com/adafruit/Adafruit_NeoPixel
 *   arduino-esp32 (core)     - LGPL-2.1      - https://github.com/espressif/arduino-esp32
 *
 * This project's own code is MIT licensed; each dependency remains under its
 * own original license.
 */

#include <M5Unified.h>
#include <Avatar.h>
#include <M5Module4EncoderMotor.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <esp_task_wdt.h>

using namespace m5avatar;

#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define DRIVE_CHAR_UUID     "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define ARM_CHAR_UUID       "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define TELEMETRY_CHAR_UUID "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
#define DEVICE_NAME "mars-core2"

// Max simultaneous BLE connections (currently: 2 phones + 1 PC for telemetry monitoring)
#define MAX_BLE_CLIENTS 3
// Telemetry notify interval (ms)
#define TELEMETRY_INTERVAL 300
// Upper bound (seconds) for how long a single loop() iteration may take.
// Normally loop() completes in tens of milliseconds, so exceeding this is only
// expected if I2C communication with the motor module has locked up.
// Triggers an automatic reboot when exceeded.
#define WDT_TIMEOUT_SEC 4
// Motor output limit (%). 100 = normal, 50 = half of max speed. Range 0~100.
int speedLimitTirePercent = 100;  // Drivetrain (left/right wheels)
int speedLimitArm1Percent = 100;  // Arm 1 (left)
int speedLimitArm2Percent = 100;  // Arm 2 (right)

// Channel assignment
#define CH_L  0  // Left wheel
#define CH_R  1  // Right wheel
#define CH_AL 2  // Left arm
#define CH_AR 3  // Right arm

// Detail screen layout
#define DETAIL_TOP 22
#define DETAIL_MID_Y ((DETAIL_TOP + 240) / 2)
#define DETAIL_MID_X 160
#define DETAIL_CELL_H ((240 - DETAIL_TOP) / 2)

// M5GO Bottom2 LED bar (GPIO25, SK6812 x10, 5 on each side)
#define LED_PIN 25
#define LED_COUNT 10
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

Avatar avatar;
M5Module4EncoderMotor driver;
BLEServer *pServer = nullptr;
BLECharacteristic *pTelemetryChar = nullptr;
bool driverReady = false;
bool needReAdv = false;
unsigned long reAdvTime = 0;
int driverReconnectCount = 0;
unsigned long lastDriverCheck = 0;
unsigned long lastTelemetrySend = 0;

// BLE input
// driveBuf/armBuf are accessed from both the BLE stack task and loop(), so they
// are protected with a fixed-size buffer + spinlock instead of String
// (String involves heap reallocation, which can corrupt data if another task
// reads it mid-assignment).
volatile bool driveFlag = false;
volatile bool armFlag = false;
volatile bool estopRequestFlag = false;
portMUX_TYPE bleBufMux = portMUX_INITIALIZER_UNLOCKED;
char driveBuf[64] = "";
char armBuf[64] = "";
int driveL = 0, driveR = 0;
int armL = 0, armR = 0;
unsigned long lastDriveTime = 0, lastArmTime = 0;

// Face position
float curX = 0, curY = 0, curTilt = 0, curMouth = 0;
float tgtX = 0, tgtY = 0, tgtTilt = 0;
float ease = 0.10;

// State
int state = 0;
int wakePhase = 0;
unsigned long wakePhaseStart = 0;

// Idle look-around
unsigned long nextLookTime = 0;
int lookChain = 0;
float doubleTakeX = 0;
bool doubleTaking = false;
int doubleTakePhase = 0;
float lastLookDir = 0;

// Emergency stop
bool eStop = false;

// LED
float ledHuePhase = 0;      // for rainbow animation
float ledBreathPhase = 0;   // for breathing animation
unsigned long lastLedUpdate = 0;

// Screen mode
int screenMode = 0;
unsigned long lastScreenTouch = 0;
uint8_t prevActiveButtons = 0;

// Forward declarations
void stopAll();
void reinitDriver();
void toggleEStop();
void forceAvatarRedraw();
void drawCellContent(int idx);
void drawDetailHeader();
bool isActive();
bool isAnyButtonPressed();
void updateLeds();
void updateSideLed(int side, int value);
float readMotorVoltage();
float readMotorCurrent();
void buildTelemetryJson(char *buf, size_t bufSize);
void sendTelemetry();

// Sound
void beepOn()  { M5.Speaker.tone(1200, UINT32_MAX); }
void beepOff() { M5.Speaker.stop(); }

// === LED (M5GO Bottom2, 5 per side) ===
// Set a pixel from HSV (0-255,0-255,0-255), converting to RGB internally
void setLedHSV(int i, uint8_t h, uint8_t s, uint8_t v) {
  leds.setPixelColor(i, leds.ColorHSV((uint16_t)h * 257, s, v));
}
void setAllLed(uint32_t color) {
  for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, color);
}

// Light up one side (5 pixels) according to drive state.
// side: 0=left (index 0-4), 1=right (index 5-9)
// value: -180~180 (raw BLE value; absolute value below 10 is treated as stopped)
void updateSideLed(int side, int value) {
  bool isLeftSide = (side == 0);

  if (abs(value) < 10) {
    // Stopped: dim white on this side only
    for (int i = 0; i < 5; i++) {
      int idx = isLeftSide ? i : (9 - i);
      setLedHSV(idx, 0, 0, 15);
    }
    return;
  }

  float speed = abs(value) / 180.0;

  if (value > 0) {
    // Forward: green chase animation (from center outward)
    for (int i = 0; i < 5; i++) {
      int idx = isLeftSide ? i : (9 - i);
      int raw = (int)(ledHuePhase * (0.5 + speed) - i * 40);
      int b = ((raw % 200) + 200) % 200;
      uint8_t v = (b < 60) ? 220 : 20;
      setLedHSV(idx, 85, 255, v);
    }
  } else {
    // Reverse: blinking orange
    bool on = (millis() / 200) % 2 == 0;
    uint32_t c = on ? leds.Color(255, 100, 0) : leds.Color(30, 15, 0);
    for (int i = 0; i < 5; i++) {
      int idx = isLeftSide ? i : (9 - i);
      leds.setPixelColor(idx, c);
    }
  }
}

void updateLeds() {
  // eStop: solid red everywhere (highest priority)
  if (eStop) {
    setAllLed(leds.Color(255, 0, 0));
    leds.show();
    return;
  }

  int connCount = pServer ? pServer->getConnectedCount() : 0;

  if (connCount == 0) {
    // Not connected: slow blue breathing
    ledBreathPhase += 0.03;
    if (ledBreathPhase > 6.283) ledBreathPhase -= 6.283;
    uint8_t v = (uint8_t)((sin(ledBreathPhase) * 0.5 + 0.5) * 60 + 10);
    setAllLed(leds.Color(0, 0, v));

  } else if (isActive()) {
    bool armAct = (abs(armL) >= 10 || abs(armR) >= 10);
    bool tireAct = (abs(driveL) >= 10 || abs(driveR) >= 10);

    if (armAct && !tireAct) {
      // Arm-only operation: purple/cyan rainbow (fun, same on both sides)
      ledHuePhase += 8;
      if (ledHuePhase > 255) ledHuePhase -= 255;
      for (int i = 0; i < LED_COUNT; i++) {
        setLedHSV(i, (uint8_t)((int)ledHuePhase + i * 15) % 255, 255, 180);
      }
    } else {
      // Driving: light each side independently
      // (correctly shows opposite directions on each side too)
      ledHuePhase += 4;
      if (ledHuePhase > 1000) ledHuePhase -= 1000;
      updateSideLed(0, driveL);  // left wheel
      updateSideLed(1, driveR);  // right wheel
    }

  } else {
    // Connected but idle: slow flowing rainbow (fun even while waiting)
    ledHuePhase += 0.8;
    if (ledHuePhase > 255) ledHuePhase -= 255;
    for (int i = 0; i < LED_COUNT; i++) {
      setLedHSV(i, (uint8_t)((int)ledHuePhase + i * 25) % 255, 200, 60);
    }
  }

  leds.show();
}

// === Motor control ===
// BLE value (-180~180) -> PWM (-127~127), absolute value below 10 is a dead zone
int toPwm(int raw) {
  if (abs(raw) < 10) return 0;
  return constrain(map(raw, -180, 180, -127, 127), -127, 127);
}

void setMotor(uint8_t ch, int raw) {
  if (!driverReady) return;
  int pwm = toPwm(raw);
  int limit = (ch == CH_L || ch == CH_R) ? speedLimitTirePercent :
              (ch == CH_AL) ? speedLimitArm1Percent : speedLimitArm2Percent;
  pwm = pwm * limit / 100;  // apply output limit
  if (ch == CH_L || ch == CH_R) pwm = -pwm;  // invert drivetrain rotation direction
  driver.setMotorSpeed(ch, pwm);
}

void stopAll() {
  if (!driverReady) return;
  for (int i = 0; i < 4; i++) driver.setMotorSpeed(i, 0);
  driveL = driveR = armL = armR = 0;
}

void reinitDriver() {
  if (driver.begin(&Wire1, MODULE_4ENCODERMOTOR_ADDR, 21, 22)) {
    driverReady = true;
    for (int i = 0; i < 4; i++) driver.setMode(i, NORMAL_MODE);
    stopAll();
    driverReconnectCount++;
    Serial.println("[4ENC] Recovered");
  } else {
    driverReady = false;
  }
}

bool checkDriver() {
  Wire1.beginTransmission(MODULE_4ENCODERMOTOR_ADDR);
  return (Wire1.endTransmission() == 0);
}

// === Telemetry (for PC monitoring, via BLE Notify) ===
// Reads the overall (all 4 channels combined) supply voltage/current of the
// 4EncoderMotor module. Per-motor values cannot be read from the hardware.
float readMotorVoltage() {
  if (!driverReady) return 0;
  return driver.getAnalogInput(_8bit) / 255.0 * 3.3 / 0.16;
}

float readMotorCurrent() {
  if (!driverReady) return 0;
  return driver.getMotorCurrent();
}

// Build the current state into a JSON string.
// JsonDocument/String involve heap allocation, so this function (called every
// 300ms) uses a fixed buffer + snprintf instead to reduce overhead.
void buildTelemetryJson(char *buf, size_t bufSize) {
  snprintf(buf, bufSize,
    "{\"v\":%.2f,\"a\":%.2f,\"estop\":%d,\"dl\":%d,\"dr\":%d,\"al\":%d,\"ar\":%d}",
    readMotorVoltage(), readMotorCurrent(), eStop ? 1 : 0, driveL, driveR, armL, armR);
}

// Send the telemetry JSON via Notify if a PC (or other client) is connected
void sendTelemetry() {
  if (!pTelemetryChar || !pServer || pServer->getConnectedCount() == 0) return;
  char buf[96];
  buildTelemetryJson(buf, sizeof(buf));
  pTelemetryChar->setValue((uint8_t*)buf, strlen(buf));
  pTelemetryChar->notify();
}

// eStop
void drawDetailHeader() {
  M5.Lcd.fillRect(0, 0, 320, DETAIL_TOP + 1, BLACK);
  int connCount = pServer ? pServer->getConnectedCount() : 0;
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setCursor(4, 4);
  M5.Lcd.printf("BLE:%d/%d  4ENC:%s  Re:%d",
    connCount, MAX_BLE_CLIENTS, driverReady ? "OK" : "NG", driverReconnectCount);
  if (eStop) {
    M5.Lcd.fillRect(200, 0, 120, 18, RED);
    M5.Lcd.setTextColor(WHITE, RED);
    M5.Lcd.setCursor(220, 4);
    M5.Lcd.print("E-STOP");
  }
  // redraw the divider line under the header
  M5.Lcd.drawLine(0, DETAIL_TOP, 320, DETAIL_TOP, 0x4208);
}

// Force the avatar display to redraw (setExpression/setSpeechText alone
// don't get reflected on screen)
void forceAvatarRedraw() {
  avatar.suspend();
  delay(40);
  M5.Lcd.fillScreen(BLACK);
  avatar.resume();
  delay(30);
  if (eStop) {
    avatar.setExpression(Expression::Angry);
    avatar.setSpeechText("STOP!");
  } else {
    avatar.setExpression(Expression::Neutral);
    avatar.setSpeechText("");
  }
}

void toggleEStop() {
  eStop = !eStop;
  updateLeds();       // immediately reflect eStop state on the LEDs
  lastLedUpdate = millis();
  if (eStop) {
    stopAll();
    if (screenMode == 0) forceAvatarRedraw();
    else drawDetailHeader();
    Serial.println("[ESTOP] ON");
    beepOn();
  } else {
    if (screenMode == 0) forceAvatarRedraw();
    else drawDetailHeader();
    Serial.println("[ESTOP] OFF");
    beepOff();
  }
}

// Checks whether the received string contains "estop":1 (whitespace after
// the colon is tolerated). Called directly from the onWrite callback so that
// the estop:1 command isn't lost if a subsequent BLE write overwrites the
// driveJson/armJson buffer before loop() gets a chance to read it.
bool jsonHasEstopOne(const String &v) {
  int idx = v.indexOf("estop");
  if (idx < 0) return false;
  int colon = v.indexOf(':', idx);
  if (colon < 0) return false;
  int p = colon + 1;
  while (p < (int)v.length() && v[p] == ' ') p++;
  return p < (int)v.length() && v[p] == '1';
}

// BLE callbacks
class DriveCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    String v = pChar->getValue();
    if (v.length() > 0) {
      if (jsonHasEstopOne(v)) estopRequestFlag = true;
      portENTER_CRITICAL(&bleBufMux);
      v.toCharArray(driveBuf, sizeof(driveBuf));
      portEXIT_CRITICAL(&bleBufMux);
      driveFlag = true; lastDriveTime = millis();
    }
  }
};
class ArmCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    String v = pChar->getValue();
    if (v.length() > 0) {
      if (jsonHasEstopOne(v)) estopRequestFlag = true;
      portENTER_CRITICAL(&bleBufMux);
      v.toCharArray(armBuf, sizeof(armBuf));
      portEXIT_CRITICAL(&bleBufMux);
      armFlag = true; lastArmTime = millis();
    }
  }
};
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    if (s->getConnectedCount() < MAX_BLE_CLIENTS) { needReAdv = true; reAdvTime = millis() + 500; }
  }
  void onDisconnect(BLEServer *s) override {
    needReAdv = true; reAdvTime = millis() + 500;
    stopAll();
  }
};

// Helpers
bool isActive() { return abs(driveL) >= 10 || abs(driveR) >= 10 || abs(armL) >= 10 || abs(armR) >= 10; }
float idleSeconds() {
  unsigned long last = max(lastDriveTime, lastArmTime);
  if (last == 0) return 9999;
  return (millis() - last) / 1000.0;
}
float randomDir() {
  float dir = (random(2) == 0 ? 1.0 : -1.0);
  if (dir == lastLookDir && random(10) < 7) dir = -dir;
  lastLookDir = dir;
  return dir;
}

// ============================================================
//  Detail screen
// ============================================================
void drawCellContent(int idx) {
  bool isUpper = (idx < 4);
  bool isLeft = (idx % 4 < 2);
  bool isForward = (idx % 2 == 0);
  int cellTopY = isUpper ? DETAIL_TOP : DETAIL_MID_Y;
  int cellX = isLeft ? 0 : DETAIL_MID_X;
  int subY = isForward ? cellTopY + 1 : cellTopY + DETAIL_CELL_H / 2 + 1;
  const char* names[] = {"L-TIRE","R-TIRE","L-ARM","R-ARM"};
  int nameIdx = (isUpper ? 0 : 2) + (isLeft ? 0 : 1);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setCursor(cellX + 30, cellTopY + 8);
  M5.Lcd.print(names[nameIdx]);
  M5.Lcd.setTextSize(1);
  int subMidY = subY + DETAIL_CELL_H / 4 - 4;
  M5.Lcd.setTextColor(isForward ? 0x07E0 : 0xFDA0, BLACK);
  M5.Lcd.setCursor(cellX + 60, subMidY);
  M5.Lcd.print(isForward ? "FWD ^" : "REV v");
}

void drawDetailScreen() {
  M5.Lcd.fillScreen(BLACK);
  drawDetailHeader();
  M5.Lcd.drawLine(0, DETAIL_TOP, 320, DETAIL_TOP, 0x4208);
  M5.Lcd.drawLine(DETAIL_MID_X, DETAIL_TOP, DETAIL_MID_X, 240, 0x4208);
  M5.Lcd.drawLine(0, DETAIL_MID_Y, 320, DETAIL_MID_Y, 0x4208);
  for (int i = 0; i < 8; i++) drawCellContent(i);
}

uint8_t touchToButton(int tx, int ty) {
  if (ty < DETAIL_TOP || ty >= 240) return 0;
  bool isLeft = (tx < DETAIL_MID_X);
  bool isUpper = (ty < DETAIL_MID_Y);
  bool isForward;
  if (isUpper) isForward = (ty < DETAIL_TOP + DETAIL_CELL_H / 2);
  else         isForward = (ty < DETAIL_MID_Y + DETAIL_CELL_H / 2);
  if (isUpper && isLeft)  return isForward ? (1 << 0) : (1 << 1);
  if (isUpper && !isLeft) return isForward ? (1 << 2) : (1 << 3);
  if (!isUpper && isLeft) return isForward ? (1 << 4) : (1 << 5);
  return isForward ? (1 << 6) : (1 << 7);
}

void applyButtons(uint8_t b) {
  if (eStop || !driverReady) return;
  // Each of channels 0~3 has one forward bit / reverse bit pair (0x01,0x02 / 0x04,0x08 / ...)
  for (int ch = 0; ch < 4; ch++) {
    uint8_t fwdBit = 1 << (ch * 2);
    uint8_t revBit = fwdBit << 1;
    int v = (b & fwdBit) ? 100 : (b & revBit) ? -100 : 0;
    setMotor(ch, v);
    // Also reflect the state in driveL/driveR/armL/armR since LED display and
    // isActive() read from these, even though the source here is touch input
    if (ch == CH_L) driveL = v;
    else if (ch == CH_R) driveR = v;
    else if (ch == CH_AL) armL = v;
    else if (ch == CH_AR) armR = v;
  }
}

void drawHighlight(uint8_t bit, bool on) {
  int idx = 0;
  for (int i = 0; i < 8; i++) if (bit & (1 << i)) { idx = i; break; }
  bool isUpper = (idx < 4);
  bool isLeft = (idx % 4 < 2);
  bool isForward = (idx % 2 == 0);
  int hlX = isLeft ? 1 : DETAIL_MID_X + 1;
  int hlW = DETAIL_MID_X - 2;
  int hlH = DETAIL_CELL_H / 2 - 2;
  int hlY;
  if (isUpper) hlY = isForward ? DETAIL_TOP + 1 : DETAIL_TOP + DETAIL_CELL_H / 2 + 1;
  else         hlY = isForward ? DETAIL_MID_Y + 1 : DETAIL_MID_Y + DETAIL_CELL_H / 2 + 1;
  if (on) {
    uint16_t color = isForward ? 0x03E0 : 0xF800;
    M5.Lcd.fillRect(hlX, hlY, hlW, hlH, color);
    const char* names[] = {"L-TIRE FWD","L-TIRE REV","R-TIRE FWD","R-TIRE REV",
                           "L-ARM FWD","L-ARM REV","R-ARM FWD","R-ARM REV"};
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(WHITE, color);
    M5.Lcd.setCursor(hlX + 10, hlY + hlH / 2 - 8);
    M5.Lcd.print(names[idx]);
  } else {
    int cellTopY = isUpper ? DETAIL_TOP + 1 : DETAIL_MID_Y + 1;
    int cellX = isLeft ? 0 : DETAIL_MID_X + 1;
    M5.Lcd.fillRect(cellX, cellTopY, DETAIL_MID_X - 1, DETAIL_CELL_H - 1, BLACK);
    int cellIdx = (isUpper ? 0 : 2) + (isLeft ? 0 : 1);
    drawCellContent(cellIdx * 2);
    drawCellContent(cellIdx * 2 + 1);
  }
}

// Checks the touch hardware directly, every call, for whether a button in the
// detail screen's button area is currently being pressed. This deliberately
// avoids relying on a stored state variable, so there's no risk of it getting
// stuck in a "still pressed" state if an update is ever missed.
bool isAnyButtonPressed() {
  if (screenMode != 1) return false;  // buttons only exist on the detail screen
  int cnt = M5.Touch.getCount();
  for (int i = 0; i < cnt; i++) {
    auto t = M5.Touch.getDetail(i);
    if (t.isPressed() && t.y >= DETAIL_TOP) return true;
  }
  return false;
}

void handleDetailTouch() {
  uint8_t activeButtons = 0;
  int cnt = M5.Touch.getCount();
  for (int i = 0; i < cnt; i++) {
    auto t = M5.Touch.getDetail(i);
    if (t.isPressed() && t.y >= DETAIL_TOP) {
      activeButtons |= touchToButton(t.x, t.y);
      lastScreenTouch = millis();
    }
  }
  if (activeButtons != prevActiveButtons) {
    for (int i = 0; i < 8; i++) {
      uint8_t bit = 1 << i;
      if ((prevActiveButtons & bit) && !(activeButtons & bit)) drawHighlight(bit, false);
      if (!(prevActiveButtons & bit) && (activeButtons & bit)) drawHighlight(bit, true);
    }
    applyButtons(activeButtons);
    prevActiveButtons = activeButtons;
  }
}

void switchToDetail() {
  screenMode = 1;
  lastScreenTouch = millis();
  prevActiveButtons = 0;
  avatar.suspend();
  delay(150);
  drawDetailScreen();
  M5.update();
  Serial.println("[SCREEN] Detail");
}

void switchToAvatar() {
  screenMode = 0;
  stopAll();
  M5.Lcd.fillScreen(BLACK);
  delay(30);
  avatar.resume();
  delay(80);
  if (eStop) {
    avatar.setExpression(Expression::Angry);
    avatar.setSpeechText("STOP!");
  } else {
    avatar.setExpression(Expression::Neutral);
    avatar.setSpeechText("");
  }
  Serial.println("[SCREEN] Avatar");
}

// ============================================================
//  setup
// ============================================================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Speaker.begin();
  M5.Speaker.setVolume(30);

  // Dedicated watchdog for loop(): if I2C communication locks up and loop()
  // stops progressing, this triggers an automatic reboot.
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  // The m5stack core already initializes the TWDT at boot with its own
  // default settings, so we tear it down first and recreate it with our
  // intended 4-second timeout.
  esp_task_wdt_deinit();
  esp_err_t wdtInitErr = esp_task_wdt_init(&wdtConfig);
  esp_err_t wdtAddErr = esp_task_wdt_add(NULL);
  if (wdtInitErr != ESP_OK || (wdtAddErr != ESP_OK && wdtAddErr != ESP_ERR_INVALID_ARG)) {
    Serial.printf("[WDT] FAILED init=%d add=%d\n", wdtInitErr, wdtAddErr);
  } else {
    Serial.println("[WDT] OK");
  }

  // Initialize the M5GO Bottom2 LED bar
  leds.begin();
  leds.setBrightness(80);
  leds.clear();
  leds.show();

  avatar.init(8);
  avatar.setExpression(Expression::Sleepy);

  // 4EncoderMotor Module (stacked below the main unit -> I2C SDA=21, SCL=22, addr=0x24)
  if (driver.begin(&Wire1, MODULE_4ENCODERMOTOR_ADDR, 21, 22)) {
    driverReady = true;
    for (int i = 0; i < 4; i++) driver.setMode(i, NORMAL_MODE);
    stopAll();
    Serial.println("[4ENC] OK");
  } else {
    Serial.println("[4ENC] FAILED");
  }

  BLEDevice::init(DEVICE_NAME);
  // The default ATT MTU (23 bytes, ~20 bytes usable) would cause the telemetry
  // JSON to arrive truncated, so request a larger MTU during negotiation.
  BLEDevice::setMTU(247);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService *svc = pServer->createService(BLEUUID(SERVICE_UUID), 25);
  BLECharacteristic *drv = svc->createCharacteristic(DRIVE_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  drv->setCallbacks(new DriveCallback());
  BLECharacteristic *arm = svc->createCharacteristic(ARM_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  arm->setCallbacks(new ArmCallback());
  pTelemetryChar = svc->createCharacteristic(TELEMETRY_CHAR_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  pTelemetryChar->addDescriptor(new BLE2902());
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setMinPreferred(0x06); adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  nextLookTime = millis() + 1000;
  Serial.println("[READY]");
}

// ============================================================
//  loop
// ============================================================
void loop() {
  M5.update();

  if (needReAdv && millis() >= reAdvTime) {
    needReAdv = false; BLEDevice::startAdvertising();
  }

  // Emergency stop request (already detected instantly in the onWrite
  // callback; here we just debounce by time to avoid toggling repeatedly)
  if (estopRequestFlag) {
    estopRequestFlag = false;
    static unsigned long lastEstopTrigger = 0;
    if (millis() - lastEstopTrigger > 250) {
      lastEstopTrigger = millis();
      toggleEStop();
    }
  }

  // Health check (every 5 seconds)
  if (millis() - lastDriverCheck > 5000) {
    lastDriverCheck = millis();
    if (driverReady && !checkDriver()) {
      Serial.println("[4ENC] Lost");
      driverReady = false;
    }
    if (!driverReady) reinitDriver();
  }

  // Send telemetry (Notify at a fixed interval, if a client such as a PC is connected)
  if (millis() - lastTelemetrySend > TELEMETRY_INTERVAL) {
    lastTelemetrySend = millis();
    sendTelemetry();
  }

  // BLE receive
  // Ignore BLE input while a button is currently being pressed, so it
  // doesn't fight with touch input (checked once here and reused for both
  // drive and arm below)
  bool buttonPressed = isAnyButtonPressed();
  if (driveFlag) {
    driveFlag = false;
    char localBuf[sizeof(driveBuf)];
    portENTER_CRITICAL(&bleBufMux);
    memcpy(localBuf, driveBuf, sizeof(driveBuf));
    portEXIT_CRITICAL(&bleBufMux);
    JsonDocument doc;
    if (!deserializeJson(doc, localBuf)) {
      if (!eStop && !buttonPressed) {
        if (doc.containsKey("left"))  { driveL = doc["left"].as<int>(); setMotor(CH_L, driveL); }
        if (doc.containsKey("right")) { driveR = doc["right"].as<int>(); setMotor(CH_R, driveR); }
      }
    }
  }
  if (armFlag) {
    armFlag = false;
    char localBuf[sizeof(armBuf)];
    portENTER_CRITICAL(&bleBufMux);
    memcpy(localBuf, armBuf, sizeof(armBuf));
    portEXIT_CRITICAL(&bleBufMux);
    JsonDocument doc;
    if (!deserializeJson(doc, localBuf)) {
      if (!eStop && !buttonPressed) {
        if (doc.containsKey("left"))  { armL = doc["left"].as<int>(); setMotor(CH_AL, armL); }
        if (doc.containsKey("right")) { armR = doc["right"].as<int>(); setMotor(CH_AR, armR); }
      }
    }
  }

  // Timeout stop (for BLE control; excluded while driving via touch buttons,
  // since lastDriveTime isn't updated by touch input)
  if (!buttonPressed && (driveL != 0 || driveR != 0) && millis() - lastDriveTime > 1000) {
    stopAll();
    updateLeds();  // immediately reflect the state change on the LEDs
    lastLedUpdate = millis();
  }

  // BtnC: toggle emergency stop
  if (M5.BtnC.wasPressed()) toggleEStop();

  // LED update (runs regardless of screen mode, every 30ms)
  if (millis() - lastLedUpdate > 30) {
    lastLedUpdate = millis();
    updateLeds();
  }

  int connCount = pServer->getConnectedCount();

  // ============================================================
  //  Avatar (Stackchan-style) screen
  // ============================================================
  if (screenMode == 0) {

    // State transitions
    if (state == 0) {
      if (connCount > 0 && (isActive() || idleSeconds() < 1)) {
        state = 1; wakePhase = 0; wakePhaseStart = millis();
      }
    } else if (state == 2) {
      if (connCount == 0 || idleSeconds() > 30) {
        state = 0;
        if (!eStop) avatar.setExpression(Expression::Sleepy);
      }
    }

    // Sleeping
    if (state == 0) {
      tgtX = 0; tgtY = 0.3; tgtTilt = 0; ease = 0.03;
      curMouth += (0.0 - curMouth) * 0.05;
    }
    // Wake-up sequence
    else if (state == 1) {
      unsigned long elapsed = millis() - wakePhaseStart;
      if (wakePhase == 0) {
        if (!eStop) avatar.setExpression(Expression::Neutral);
        tgtX = 0; tgtY = 0; tgtTilt = 0; ease = 0.06;
        if (elapsed > 500) { wakePhase = 1; wakePhaseStart = millis(); }
      } else if (wakePhase == 1) {
        tgtX = -0.8; tgtTilt = -0.08; ease = 0.30;
        if (elapsed > 400) { wakePhase = 2; wakePhaseStart = millis(); }
      } else if (wakePhase == 2) {
        tgtX = 0.8; tgtTilt = 0.08; ease = 0.35;
        if (elapsed > 400) { wakePhase = 3; wakePhaseStart = millis(); }
      } else if (wakePhase == 3) {
        tgtX = 0; tgtY = 0; tgtTilt = 0; ease = 0.08;
        if (elapsed > 400) { state = 2; nextLookTime = millis() + 500 + random(500); }
      }
    }
    // Awake
    else {
      if (isActive()) {
        ease = 0.15;
        float turnRate = (float)(driveL - driveR) / 360.0;
        float speed = (abs(driveL) + abs(driveR)) / 360.0;
        tgtX = constrain(turnRate * 2.0, -1.0, 1.0);
        tgtY = constrain(-speed * 0.2, -0.25, 0.1);
        tgtTilt = constrain(-turnRate * 0.10, -0.12, 0.12);
        if (abs(driveL) < 10 && abs(driveR) < 10 && (abs(armL) >= 10 || abs(armR) >= 10)) {
          float armDir = (float)(armR - armL) / 360.0;
          tgtX = constrain(armDir * 0.6, -0.4, 0.4);
          tgtY = 0.15;
          tgtTilt = armDir * 0.05;
        }
        curMouth += (speed * 0.3 - curMouth) * 0.08;
      } else {
        curMouth += (0.0 - curMouth) * 0.05;
        // Idle look-around
        if (millis() > nextLookTime) {
          if (doubleTaking) {
            doubleTakePhase++;
            if (doubleTakePhase == 1) {
              tgtX = 0; tgtY = 0; tgtTilt = 0;
              nextLookTime = millis() + 150 + random(150);
              ease = 0.40;
            } else {
              tgtX = doubleTakeX * 0.9;
              tgtTilt = doubleTakeX * 0.06;
              nextLookTime = millis() + 500 + random(500);
              doubleTaking = false;
              ease = 0.35;
            }
          } else if (lookChain > 0) {
            lookChain--;
            tgtX = (random(100) - 50) / 60.0;
            tgtY = (random(40) - 20) / 80.0;
            tgtTilt = tgtX * 0.05;
            nextLookTime = millis() + 150 + random(200);
            ease = 0.35;
          } else {
            int pat = random(100);
            if (pat < 10) {
              float dir = randomDir();
              tgtX = dir * (0.6 + random(40) / 100.0);
              tgtY = (random(20) - 10) / 100.0;
              tgtTilt = dir * 0.05;
              nextLookTime = millis() + 800 + random(1500);
              ease = 0.25;
            } else if (pat < 20) {
              float dir = randomDir();
              tgtX = dir * (0.3 + random(60) / 100.0);
              tgtY = (random(20) - 10) / 100.0;
              tgtTilt = dir * 0.03;
              nextLookTime = millis() + 2500 + random(4000);
              ease = 0.08;
            } else if (pat < 28) {
              doubleTakeX = randomDir() * (0.6 + random(30) / 100.0);
              tgtX = doubleTakeX;
              tgtTilt = doubleTakeX * 0.07;
              doubleTaking = true;
              doubleTakePhase = 0;
              nextLookTime = millis() + 400 + random(300);
              ease = 0.25;
            } else if (pat < 45) {
              float dir = randomDir();
              tgtX = dir * (0.3 + random(60) / 100.0);
              tgtY = (random(25) - 12) / 100.0;
              tgtTilt = tgtX * 0.03;
              nextLookTime = millis() + 1500 + random(2500);
              ease = 0.04;
            } else {
              tgtX = (random(10) - 5) / 100.0;
              tgtY = (random(6) - 3) / 100.0;
              tgtTilt = 0;
              nextLookTime = millis() + 2000 + random(4000);
              ease = 0.05;
            }
          }
        }
      }
    }

    // lerp
    curX    += (tgtX    - curX)    * ease;
    curY    += (tgtY    - curY)    * ease;
    curTilt += (tgtTilt - curTilt) * ease;

    // Apply to the avatar
    avatar.setPosition((int)(-curY * 15), (int)(curX * 40));
    avatar.setRotation(curTilt);
    avatar.setMouthOpenRatio(constrain(curMouth, 0, 0.5));
    float pupilX = constrain(curX * 0.5, -0.5, 0.5);
    float pupilY = constrain(curY * 0.3, -0.3, 0.3);
    avatar.setLeftGaze(pupilY, pupilX);
    avatar.setRightGaze(pupilY, pupilX);

    // Switch screen
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) switchToDetail();
  }

  // ============================================================
  //  Detail screen
  // ============================================================
  else {
    handleDetailTouch();
    if (M5.BtnB.wasPressed()) switchToAvatar();
    if (millis() - lastScreenTouch > 10000) switchToAvatar();
  }

  esp_task_wdt_reset();  // notify that loop() completed one iteration successfully
  delay(25);
}
