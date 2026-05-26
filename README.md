# ⌚ Wearable Health Monitoring Band / Smartwatch

> A compact, low-power wearable health band built around the Seeed Studio XIAO ESP32S3. Features heart rate & SpO₂ monitoring, hardware-offloaded step tracking, NimBLE wireless data transmission, and an active power-saving state machine.

### 📸 Physical Build & Interface Gallery

| Wearable smartwatch face | Mobile App BLE telemetry | CAD PETG Casing Render |
| :---: | :---: | :---: |
| ![Smartwatch Face](images/watch_face.jpg) | ![App Interface](images/app_interface.jpg) | ![Casing CAD](images/watch_photo6.jpg) |

| Smartwatch hardware Assembly | Smartwatch on Wrist | Smartwatch Internal Build |
| :---: | :---: | :---: |
| ![Watch Assembly 1](images/watch_photo1.jpg) | ![Watch Assembly 2](images/watch_photo2.jpg) | ![Watch Assembly 3](images/watch_photo3.jpg) |

---

## ⚡ Key Technical Metrics

| Metric | Value | Details |
|---|---|---|
| **Main Processor** | Seeed Studio XIAO ESP32S3 | Compact, dual-core Tensilica Xtensa LX7 |
| **Biometric Sensor** | MAX30102 | Heart Rate + SpO₂ via I2C |
| **Activity Tracker** | BMA400 | Ultra-low power 3-axis accelerometer |
| **Display** | SSD1306 128×64 | I2C split-color OLED (yellow/blue) |
| **BLE Protocol** | NimBLE-Arduino | Memory-optimized Bluetooth Low Energy |
| **Battery Check** | 12-bit ADC (A0) | Smoothed 47kΩ/100kΩ resistor divider |
| **Power Supply** | 300mAh Li-Po | Compact battery cell integrated in case |
| **Haptic Pin** | 5mm Button Motor | Tactile alert output on GPIO 4 |

---

## 🏗️ Hardware Architecture

```mermaid
graph TD
    Battery["Li-Po Battery cell (300mAh)"] --> ESP32S3["Seeed Studio XIAO ESP32S3"]
    
    subgraph Inputs["Inputs"]
        Button["Push Button (GPIO 3)<br>(Wake / State Controller)"]
        BMA400["BMA400 Accelerometer (I2C)<br>(Hardware Step Interrupt on GPIO 2)"]
        MAX30102["MAX30102 Optical Biometric Sensor (I2C)<br>(Heart Rate + SpO2)"]
    end
    
    subgraph Outputs["Outputs"]
        SSD1306["SSD1306 OLED 128x64 (I2C)<br>(Split-Color Yellow/Blue UI)"]
        Haptic["Vibration Haptic Motor<br>(GPIO 4, PWM alert)"]
    end
    
    Inputs --> ESP32S3
    ESP32S3 --> Outputs
    
    subgraph Firmware["Low-Power Smartwatch OS (ESP32S3 Core)"]
        States["Low-Power State Machine<br>(OFF -> STEPS -> HR -> LONG_ON)"]
        Optical["Optical Finger Detection<br>(100ms Debounce Noise Filter)"]
        Pedometer["Hardware Pedometer Offload<br>(Interrupt Handler)"]
    end
    
    ESP32S3 --> Firmware
    ESP32S3 -->|NimBLE BLE Server| BLE["SensorDevice Telemetry Bridge<br>(String Broadcast every 1s)"]
```

### Complete Pin Configuration

| Peripheral | ESP32S3 Pin | Mode / Description |
|-----------|---|---|
| **I2C SDA** | **GPIO 5** | Data bus for MAX30102, BMA400, and SSD1306 |
| **I2C SCL** | **GPIO 6** | Clock bus for I2C peripherals |
| **Push Button** | **GPIO 3** | Input with internal pull-up (Wake/State Trigger) |
| **Haptic Motor**| **GPIO 4** | Vibration alert output |
| **BMA400 INT** | **GPIO 2** | Hardware step counter interrupt (Rising edge) |
| **Battery Sense**| **A0 (GPIO 1)**| Analog input for voltage divider check |

---

## 🧠 Core Software Logic & State Machine

### 1. Power State Machine
To maximize wearable battery life, the smartwatch operates in four distinct button-controlled hardware states:

```mermaid
stateDiagram-v2
    [*] --> OFF_STATE : Default
    OFF_STATE --> STEPS_STATE : Short Button Press (OLED wakes, BMA400 active)
    STEPS_STATE --> HR_STATE : Short Button Press (MAX30102 active, reading biometrics)
    HR_STATE --> OFF_STATE : Short Button Press / 60s Auto-Sleep Timeout
    
    OFF_STATE --> LONG_ON_STATE : 2.5s Long Press (Auto-sleep disabled)
    STEPS_STATE --> LONG_ON_STATE : 2.5s Long Press
    HR_STATE --> LONG_ON_STATE : 2.5s Long Press
    
    LONG_ON_STATE --> OFF_STATE : Short Button Press
```

* **`OFF_STATE`:** Display turned OFF (`SSD1306_DISPLAYOFF`), MAX30102 in sleep mode.
* **`STEPS_STATE`:** OLED wakes, showing BMA400 step count and classified activity. MAX30102 remains in standby.
* **`HR_STATE`:** Power-up of the MAX30102 for active heart rate and SpO₂ readings.
* **`LONG_ON_STATE`:** Triggered by a **2.5-second long press** (confirmed by a 250ms haptic buzz). Overrides the auto-sleep, locking the device continuously ON.
* **Auto-Sleep:** If running in standard states (`STEPS_STATE` or `HR_STATE`), the system monitors idle time and shuts down completely after **60 seconds** of inactivity (`AUTO_OFF_MS = 60000`).

### 2. Optical Finger Detection
To prevent false measurements when the watch is off the wrist:
1. The MAX30102 reads raw Infrared (IR) levels.
2. If the IR reading falls below `MIN_IR_THRESHOLD` (**50000**):
   * A debounce timer starts to confirm the finger has been removed.
   * If IR remains low for **100ms** (`NO_FINGER_CONFIRM_MS`), the state machine confirms finger removal (`noFingerConfirmed = true`).
   * The UI displays `"No Finger"`, and the BPM/SpO₂ buffers are safely cleared.

### 3. Hardware-Offloaded Step Counting
Step counting is offloaded entirely to the BMA400's internal hardware ASIC:
* The ESP32S3 configures step counter interrupts on channel 1 (`BMA400_INT_CHANNEL_1`) as active push-pull.
* When motion is detected, the BMA400 fires a rising-edge interrupt to **GPIO 2** of the ESP32S3.
* The ESP32S3 wakes briefly, updates the global `bmaStepCount` and `bmaActivityType` (`Walking`, `Running`, `Standing still`), and returns to standby, leaving the main CPU completely unburdened.

---

## 📁 Repository Structure

📂 **`Final_Watch_code/`** — Core ESP32S3 smartwatch Arduino firmware  
  * `Final_Watch_code.ino` — Core smartwatch OS, BLE stack, and state machine  
📂 **`3D print final/`** — 3D printing casing files and CAD source files  
📂 **`images/`** — High-resolution photos, pin configurations, and app screenshots  
  * `watch_face.jpg` — OLED yellow/blue screen showing biometrics and battery status  
  * `app_interface.jpg` — Mobile app showing BLE health telemetry received  
  * `watch_photo1.jpg` to `watch_photo5.jpg` — Physical board assemblies and wrist fitting  
  * `watch_photo6.jpg` — CAD casing render of the comfortable PETG casing  
📄 **`Smartwatch_Final_Report.pdf`** — Complete technical project documentation report  
📄 **`README.md`** — Core project documentation  

---

## 🚀 Getting Started

### Prerequisites
- **Arduino IDE** with ESP32 board package
- **Required Libraries:**
  * `NimBLE-Arduino` (Memory-optimized BLE stack)
  * `Adafruit_SSD1306` & `Adafruit_GFX`
  * `SparkFun_BMA400_Arduino_Library`
  * `MAX30105` (Adafruit/SparkFun variant including `spo2_algorithm.h`)

### Flashing Firmware
1. Open `Final_Watch_code/Final_Watch_code.ino` in the Arduino IDE.
2. Select **XIAO ESP32S3** as the target board.
3. Click **Upload** to compile and flash.

---

## 🔬 Lessons Learned & Engineering Pivots

1. **NimBLE Stack Selection:** The standard ESP32 BLE library consumes nearly **1.1MB of flash memory**, leaving almost zero room for UI graphics or games. Transitioning to **NimBLE** reduced the memory footprint by over **60%**, preserving room for dynamic easter-egg animations.
2. **Voltage Divider Impedance:** The internal input impedance of the ESP32S3's analog pins can cause voltage sag under high-value resistor dividers. Tuning to a **47kΩ + 100kΩ divider** provided the ideal balance between minimal battery parasitic drain and accurate 12-bit battery state-of-charge measurements on **A0**.
3. **Optomechanical Isolation:** Switching cell casing from Nylon Carbon Fiber to **PETG** provided superior skin comfort. Added a compliance-fit **TPU inner ring** around the MAX30102 sensor lens. This soft gasket blocks ambient light leakage while keeping the skin-contact force gentle and stable.

---

## 👤 Author

**Balaji Rayudu S**  
B.Tech Electronics & Computers Engineering, Semester IV  
Amrita Vishwa Vidyapeetham, Bengaluru  
