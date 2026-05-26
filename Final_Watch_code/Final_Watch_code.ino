#include <Arduino.h>    // Ensure Arduino core is included
#include <Wire.h>
#include "SparkFun_BMA400_Arduino_Library.h"
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <NimBLEDevice.h>  // BLE library added

// ========================
// User-Defined Constants
// ========================
#define SCREEN_WIDTH 128   // OLED display width in pixels
#define SCREEN_HEIGHT 64   // OLED display height in pixels
#define OLED_RESET    -1   // Reset pin (or -1 if sharing Arduino reset)
#define BUTTON_PIN 3
#define HAPTIC_PIN 4
#define DEBOUNCE_DELAY 50         // 50 ms debounce
#define LONG_PRESS_THRESHOLD 2500 // 2.5 sec for long press
#define AUTO_OFF_MS 60000         // 60 seconds auto-off
#define MIN_IR_THRESHOLD 50000    // Adjust as needed for "no finger"
#define NO_FINGER_CONFIRM_MS 100  // Confirm "no finger" state after 100ms

// ========================
// Battery Monitoring Constants
// ========================
#define BATTERY_PIN A0          // Analog pin for battery voltage divider
#define R_TOP   47000.0         // 47 kΩ
#define R_BOTTOM 100000.0       // 100 kΩ
#define V_REF     3.3           // ADC reference voltage
#define ADC_MAX  4095.0         // 12-bit ADC

// ------------------------
// BLE Definitions (Do not change these)
#define SENSOR_SERVICE_UUID      "12345678-1234-5678-1234-56789abcdef0"
#define SENSOR_CHAR_UUID         "abcdef12-3456-7890-abcd-ef1234567890"

// ========================
// Global Objects & Variables
// ========================

// OLED display
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// BMA400 (Step Counter)
BMA400 accelerometer;
uint8_t i2cAddress = BMA400_I2C_ADDRESS_DEFAULT;
const int interruptPin = 2;
volatile bool interruptOccurred = false;
uint32_t bmaStepCount = 0;
uint8_t bmaActivityType = 0;
void bma400InterruptHandler() {
  interruptOccurred = true;
}

// MAX30105 (Heart Rate & SpO₂)
MAX30105 particleSensor;
bool maxPresent = true;               // ← flag for sensor presence
const byte LED_BRIGHTNESS = 0x1F;
const int SAMPLE_RATE = 300;
const int DATA_BUFFER_SIZE = 100;
const int PULSE_WIDTH = 411;
const int ADC_RANGE = 4096;
uint32_t irBuffer[DATA_BUFFER_SIZE];
uint32_t redBuffer[DATA_BUFFER_SIZE];
int bufferIndex = 0;
int32_t spo2;
int8_t validSPO2;
float bpm = 0;
unsigned long lastBeatTime = 0;
const int HR_FILTER_SIZE = 10;
float beatIntervals[HR_FILTER_SIZE] = {0};
int beatIntervalIndex = 0;

// Button and state handling
enum DeviceState {
  OFF_STATE,
  STEPS_STATE,
  HR_STATE,
  LONG_ON_STATE
};
DeviceState deviceState = OFF_STATE;
bool longPressActive = false;
bool buttonCurrentlyPressed = false;
unsigned long buttonPressTime = 0;
unsigned long lastActivityTime = 0;
static bool prevButtonState = HIGH;
static unsigned long lastChangeTime = 0;

// For "no finger" confirmation
static unsigned long noFingerStartTime = 0;
static bool noFingerConfirmed = false;

// ------------------------
// BLE Global Variables
// ------------------------
NimBLEServer* pBLEServer = nullptr;
NimBLEService* pBLEService = nullptr;
NimBLECharacteristic* pSensorCharacteristic = nullptr;

// ------------------------
// BLE Setup Function
// ------------------------
void setupBLE() {
  NimBLEDevice::init("SensorDevice");
  pBLEServer = NimBLEDevice::createServer();
  pBLEService = pBLEServer->createService(SENSOR_SERVICE_UUID);
  pSensorCharacteristic = pBLEService->createCharacteristic(
                            SENSOR_CHAR_UUID,
                            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
                          );
  pSensorCharacteristic->setValue("No Data");
  pBLEService->start();
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName("SensorDevice");
  pAdvertising->addServiceUUID(SENSOR_SERVICE_UUID);
  pAdvertising->start();
  Serial.println("BLE Advertising Started");
}

// ------------------------
// BLE Update State Machine
// ------------------------
void updateBLE() {
  if (!pBLEServer || pBLEServer->getConnectedCount() == 0) return;

  static uint8_t bleState = 0;
  static unsigned long lastBLETime = 0;
  const unsigned long gapTime = 1000;

  if (millis() - lastBLETime < gapTime) return;
  lastBLETime = millis();

  String dataToSend;
  switch (bleState) {
    case 0:
      dataToSend = "HR:" + String(bpm, 1);
      break;
    case 1:
      dataToSend = "SpO2:" + String(spo2);
      break;
    case 2:
      dataToSend = "Steps:" + String(bmaStepCount);
      break;
    case 3: {
      String activity;
      switch (bmaActivityType) {
        case BMA400_RUN_ACT:  activity = "Running"; break;
        case BMA400_WALK_ACT: activity = "Walking"; break;
        case BMA400_STILL_ACT:activity = "Standing"; break;
        default:              activity = "Unknown"; break;
      }
      dataToSend = "Activity:" + activity;
      break;
    }
  }

  pSensorCharacteristic->setValue(dataToSend.c_str());
  pSensorCharacteristic->notify();
  bleState = (bleState + 1) % 4;
}

// ========================
// Haptic Functions
// ========================
void setupHaptic() {
  pinMode(HAPTIC_PIN, OUTPUT);
  digitalWrite(HAPTIC_PIN, LOW);
}
void buzzHaptic(unsigned long durationMs) {
  digitalWrite(HAPTIC_PIN, HIGH);
  delay(durationMs);
  digitalWrite(HAPTIC_PIN, LOW);
}

// ========================
// Battery Percentage (Smoothed)
// ========================
int getBatteryPercentage() {
  static int lastPct = -1;
  float raw = analogRead(BATTERY_PIN);
  float v_adc = (raw / ADC_MAX) * V_REF;
  float v_bat = v_adc * (R_TOP + R_BOTTOM) / R_BOTTOM;
  v_bat = constrain(v_bat, 3.0, 4.2); // LiPo voltage range
  int pct = map(v_bat * 100, 300, 420, 0, 100);
  if (lastPct < 0 || abs(pct - lastPct) >= 3) {
    lastPct = pct;
  }
  return lastPct;
}

// Draw battery icon + percentage at top-right
void drawBatteryIcon() {
  int pct = getBatteryPercentage();
  int x = SCREEN_WIDTH - 28;
  int y = 0;
  int w = 20;
  int h = 8;
  int level = map(pct, 0, 100, 0, w - 4);

  // Outline
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  display.drawRect(x + w, y + 2, 2, 4, SSD1306_WHITE);

  // Fill level
  display.fillRect(x + 2, y + 2, level, h - 4, SSD1306_WHITE);

  // Percentage text
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x - 18, y);
  display.print(pct);
  display.print("%");
}

// ========================
// Setup Function
// ========================
void setup() {
  Serial.begin(115200);
  Wire.begin(5, 6);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setupHaptic();

  // OLED initialization
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED allocation failed");
    while (1);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Combined Sensor Init");
  display.display();
  delay(1000);

  // BMA400 initialization
  while (accelerometer.beginI2C(i2cAddress) != BMA400_OK) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("BMA400 not connected!");
    display.println("Check wiring/I2C addr.");
    display.display();
    delay(1000);
  }
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("BMA400 connected!");
  display.display();
  delay(1000);

  bma400_step_int_conf config = { .int_chan = BMA400_INT_CHANNEL_1 };
  accelerometer.setStepCounterInterrupt(&config);
  accelerometer.setInterruptPinMode(BMA400_INT_CHANNEL_1, BMA400_INT_PUSH_PULL_ACTIVE_1);
  accelerometer.enableInterrupt(BMA400_STEP_COUNTER_INT_EN, true);
  attachInterrupt(digitalPinToInterrupt(interruptPin), bma400InterruptHandler, RISING);

  // MAX30105 initialization (modified)
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    maxPresent = false;
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("MAX30105 not");
    display.println("connected!");
    display.display();
    delay(2000);
    display.clearDisplay();
    display.display();
  } else {
    particleSensor.setup(LED_BRIGHTNESS, 4, 2, SAMPLE_RATE, PULSE_WIDTH, ADC_RANGE);
    particleSensor.enableDIETEMPRDY();
    Serial.println("Place finger on sensor...");
    delay(3000);
  }

  // BLE setup
  setupBLE();
}

// ========================
// Main Loop Function
// ========================
void loop() {
  handleButtonPress();

  if (!longPressActive && (deviceState == STEPS_STATE || deviceState == HR_STATE)) {
    if (millis() - lastActivityTime >= AUTO_OFF_MS) {
      deviceState = OFF_STATE;
    }
  }

  if (deviceState == OFF_STATE) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    return;
  } else {
    display.ssd1306_command(SSD1306_DISPLAYON);
  }

  // BMA400 step counter
  if (interruptOccurred) {
    interruptOccurred = false;
    uint16_t interruptStatus = 0;
    accelerometer.getInterruptStatus(&interruptStatus);
    if (interruptStatus & BMA400_ASSERTED_STEP_INT) {
      accelerometer.getStepCount(&bmaStepCount, &bmaActivityType);
    }
  }

  // MAX30105 HR & SpO₂ (guarded by maxPresent)
  if (maxPresent && (deviceState == HR_STATE || deviceState == LONG_ON_STATE)) {
    static unsigned long lastSample = millis();
    if (millis() - lastSample >= 10) {
      lastSample = millis();
      float irValue  = particleSensor.getIR();
      float redValue = particleSensor.getRed();
      irBuffer[bufferIndex]  = irValue;
      redBuffer[bufferIndex] = redValue;
      detectHeartBeat(irValue);
      bufferIndex++;
      if (bufferIndex >= DATA_BUFFER_SIZE) {
        processSpO2();
        bufferIndex = 0;
      }
    }
  }

  // Update OLED display
  updateDisplay();

  // BLE notifications
  updateBLE();
}

// ========================
// Handle Button (Original)
// ========================
void handleButtonPress() {
  bool rawState = digitalRead(BUTTON_PIN);
  if (rawState != prevButtonState) lastChangeTime = millis();
  bool stableState = (millis() - lastChangeTime < DEBOUNCE_DELAY) ? prevButtonState : rawState;
  prevButtonState = rawState;

  if (!buttonCurrentlyPressed && stableState == LOW) {
    buttonPressTime = millis();
    buttonCurrentlyPressed = true;
  }

  if (buttonCurrentlyPressed && stableState == LOW && !longPressActive &&
      millis() - buttonPressTime >= LONG_PRESS_THRESHOLD) {
    longPressActive = true;
    buzzHaptic(250);
    deviceState = LONG_ON_STATE;
    lastActivityTime = millis();
  }

  if (buttonCurrentlyPressed && stableState == HIGH) {
    if (millis() - buttonPressTime < LONG_PRESS_THRESHOLD) {
      buzzHaptic(150);
      if (deviceState == OFF_STATE) deviceState = STEPS_STATE;
      else if (deviceState == STEPS_STATE) deviceState = HR_STATE;
      else if (deviceState == HR_STATE) deviceState = OFF_STATE;
      else if (deviceState == LONG_ON_STATE) {
        deviceState = OFF_STATE;
        longPressActive = false;
      }
      lastActivityTime = millis();
    }
    buttonCurrentlyPressed = false;
  }
}

// ========================
// Heart Rate Detection (Original)
// ========================
void detectHeartBeat(float irValue) {
  if (irValue < MIN_IR_THRESHOLD) return;
  static float threshold = 0;
  static bool pulseDetected = false;
  static float irHistory[DATA_BUFFER_SIZE] = {0};
  static int historyIndex = 0;

  irHistory[historyIndex] = irValue;
  historyIndex = (historyIndex + 1) % DATA_BUFFER_SIZE;

  float mean = 0, sumSq = 0;
  for (int i = 0; i < DATA_BUFFER_SIZE; i++) mean += irHistory[i];
  mean /= DATA_BUFFER_SIZE;
  for (int i = 0; i < DATA_BUFFER_SIZE; i++) sumSq += pow(irHistory[i] - mean, 2);
  threshold = mean + sqrt(sumSq / DATA_BUFFER_SIZE);

  if (!pulseDetected && irValue > threshold) {
    pulseDetected = true;
    unsigned long currentTime = millis();
    unsigned long beatDuration = currentTime - lastBeatTime;
    if (beatDuration > 300 && beatDuration < 2000) {
      beatIntervals[beatIntervalIndex] = 60000.0 / beatDuration;
      beatIntervalIndex = (beatIntervalIndex + 1) % HR_FILTER_SIZE;
      float total = 0; int count = 0;
      for (int i = 0; i < HR_FILTER_SIZE; i++) {
        if (beatIntervals[i] > 40 && beatIntervals[i] < 180) {
          total += beatIntervals[i]; count++;
        }
      }
      bpm = (count > 0) ? (total / count) : 0;
    }
    lastBeatTime = currentTime;
  }
  if (pulseDetected && irValue < threshold) pulseDetected = false;
}

void processSpO2() {
  if (irBuffer[0] < MIN_IR_THRESHOLD) return;
  int32_t heartRate; int8_t validHeartRate;
  maxim_heart_rate_and_oxygen_saturation(
    irBuffer, DATA_BUFFER_SIZE, redBuffer,
    &spo2, &validSPO2, &heartRate, &validHeartRate
  );
}

// ========================
// OLED Update Function
// ========================
void updateDisplay() {
  display.clearDisplay();

  // small indicator if HR sensor missing
  if (!maxPresent) {
    display.setCursor(0, 0);
    display.print("HR sensor X");
  }

  uint32_t currentIR = (bufferIndex > 0) ? irBuffer[bufferIndex - 1] : 0;
  display.setCursor(0, 8);
  if (currentIR < MIN_IR_THRESHOLD) {
    if (noFingerStartTime == 0) {
      noFingerStartTime = millis();
      noFingerConfirmed = false;
    } else if (millis() - noFingerStartTime >= NO_FINGER_CONFIRM_MS) {
      noFingerConfirmed = true;
    }
  } else {
    noFingerStartTime = 0;
    noFingerConfirmed = false;
  }

  if (noFingerConfirmed) {
    display.print("No finger");
  } else {
    display.print("SpO2: ");
    if (validSPO2) display.print(spo2);
    else          display.print("---");
    display.print("%");
    display.setCursor(0, 16);
    display.print("BPM: ");
    if (bpm > 40 && bpm < 180) display.print(bpm);
    else                        display.print("---");
  }

  display.setCursor(0, 32);
  display.print("Steps: ");
  display.print(bmaStepCount);

  display.setCursor(0, 42);
  display.print("Activity: ");
  switch (bmaActivityType) {
    case BMA400_RUN_ACT:
      display.print("Running");
      display.setCursor(0, 52);
      break;
    case BMA400_WALK_ACT:
      display.print("Walking");
      display.setCursor(0, 52);
      break;
    case BMA400_STILL_ACT:
      display.print("Standing");
      display.setCursor(0, 52);
      display.print("still");
      break;
    default:
      display.print("Unknown");
      display.setCursor(0, 52);
      break;
  }

  // Draw battery icon + percentage
  drawBatteryIcon();

  display.display();
}
