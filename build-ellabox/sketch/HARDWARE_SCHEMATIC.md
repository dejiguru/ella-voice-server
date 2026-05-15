#line 1 "/home/dejiguru/Arduino/ellabox/HARDWARE_SCHEMATIC.md"
# EllaBox AI Robot - Hardware Schematic

## Main Controller
**ESP32-S3-DevKitC-1**
- Flash: 8MB
- PSRAM: 8MB OPI PSRAM
- USB: Hardware CDC (USB Serial)
- Operating Voltage: 3.3V

---

## Pin Assignments

### Audio System

#### Microphone (INMP441 I2S MEMS)
| INMP441 Pin | ESP32-S3 Pin | Description |
|-------------|--------------|-------------|
| SCK (BCLK)  | GPIO 41      | Bit Clock (Reverted from 15) |
| WS (LRCLK)  | GPIO 42      | Word Select / Left-Right Clock (Reverted from 7) |
| SD (DOUT)   | GPIO 4       | Serial Data Out |
| L/R         | GND          | Left channel (GND) or Right (3.3V) |
| VDD         | 3.3V         | Power |
| GND         | GND          | Ground |

**Sample Rate:** 16kHz, 16-bit, Mono

#### Speaker (I2S DAC - MAX98357A)
| MAX98357A Pin | ESP32-S3 Pin | Description |
|---------------|--------------|-------------|
| BCLK          | GPIO 48      | Bit Clock |
| LRC (LRCLK)   | GPIO 21      | Left-Right Clock |
| DIN (DOUT)    | GPIO 18      | Data Input |
| GAIN          | GND          | Gain setting (9dB) |
| SD            | 3.3V         | Shutdown (HIGH = enabled) |
| VIN           | 5V           | Power (Recommended 5V if module has regulator) |
| GND           | GND          | Ground |

**Sample Rate:** 24kHz, 16-bit, Mono

---

### Motor Control

#### Motor Driver (L298N or similar H-Bridge)
| Function      | ESP32-S3 Pin | Description |
|---------------|--------------|-------------|
| MOTOR1_IN1    | GPIO 16      | Left Motor Forward |
| MOTOR1_IN2    | GPIO 17      | Left Motor Backward |
| MOTOR2_IN1    | GPIO 5       | Right Motor Forward |
| MOTOR2_IN2    | GPIO 6       | Right Motor Backward |

**PWM Frequency:** 1000 Hz  
**Default Speed:** 180 (0-255 range)

#### Servo (Neck Movement)
| Function   | ESP32-S3 Pin | Description |
|------------|--------------|-------------|
| SERVO_PIN  | GPIO 1       | PWM Signal for neck servo |
| BUZZER_PIN | GPIO 40      | Piezo buzzer for alerts |

**Range:** 50° (down) to 130° (up), Center: 90°

---

### Sensors (I2C via TCA9548A Multiplexer)

#### I2C Multiplexer (TCA9548A)
| TCA9548A Pin | ESP32-S3 Pin | Description |
|--------------|--------------|-------------|
| SDA          | GPIO 8       | I2C Data |
| SCL          | GPIO 9       | I2C Clock |
| A0-A2        | GND          | Address: 0x70 |
| VCC          | 3.3V         | Power |
| GND          | GND          | Ground |

**I2C Address:** 0x70  
**Clock Speed:** 400kHz

#### Channel Assignments
| Channel | Device | Description |
|---------|--------|-------------|
| 0       | SSD1306 | Left Eye OLED (128x64) |
| 1       | SSD1306 | Right Eye OLED (128x64) |
| 2       | AHT20 + ENS160 | Shared environmental sensor branch |
| 4       | MAX30102 | Heart Rate & SpO2 Sensor |
| 5       | VL53L0X / MPU6050 | ToF sensor active; IMU planned on same branch |

#### Additional I2C Sensor Addresses
| Sensor | I2C Address | Description |
|--------|-------------|-------------|
| AHT20 | 0x38 | Temperature & Humidity Sensor |
| ENS160 | 0x53 | Air Quality Sensor (TVOC, eCO2, AQI) |
| MAX30102 | 0x57 | Heart Rate & SpO2 Sensor |
| MPU6050 | 0x68 | IMU (planned/stubbed in firmware, not currently wired) |
| VL53L0X | 0x29 | Time-of-Flight Distance Sensor (Head-mounted, rotates with neck servo) |

---

### Distance Sensors

#### HC-SR04 Ultrasonic Sensor (Chest-mounted, Fixed Forward)
| HC-SR04 Pin | ESP32-S3 Pin | Description |
|-------------|--------------|-------------|
| VCC         | 5V           | Power |
| TRIG        | GPIO 15      | Trigger Pin (Moved from strapping pin 3) |
| ECHO        | GPIO 38      | Echo Pin (Moved from JTAG pin 41) |
| GND         | GND          | Ground |

**Range:** 2cm - 400cm  
**Accuracy:** ±3mm  
**Measurement Angle:** 15°  
**Purpose:** Room mapping, obstacle detection, person detection in front  
**Mounting:** Fixed on chest, always faces forward regardless of neck position

**Note:** Future VL53L0X ToF sensor will be head-mounted and rotate with the neck servo for scanning. The ultrasonic sensor provides fixed forward detection for navigation and safety.
**Level Shifting:** HC-SR04 `ECHO` is a 5V output. Use a resistor divider or logic-level shifter before the ESP32-S3 input pin.

---

### Display (TFT)

#### ILI9341 TFT Display (320x240, SPI)
| ILI9341 Pin | ESP32-S3 Pin | Description |
|-------------|--------------|-------------|
| MOSI        | GPIO 11      | SPI Data Out |
| MISO        | GPIO 13      | SPI Data In |
| SCK         | GPIO 12      | SPI Clock |
| CS          | GPIO 10      | Chip Select |
| DC          | GPIO 2       | Data/Command |
| RST         | -1           | Reset (not used) |
| LED         | 3.3V         | Backlight (always on) |
| VCC         | 5V/3.3V      | Power (Use 5V if module has AMS1117 regulator) |
| GND         | GND          | Ground |

**SPI Frequency:** 20 MHz (reduced from 40MHz to prevent artifacts)

#### Touchscreen (XPT2046, SPI)
| XPT2046 Pin | ESP32-S3 Pin | Description |
|-------------|--------------|-------------|
| T_CLK       | GPIO 12      | SPI Clock (shared) |
| T_CS        | GPIO 47      | Touch Chip Select |
| T_DIN       | GPIO 11      | SPI MOSI (shared) |
| T_DO        | GPIO 13      | SPI MISO (shared) |
| T_IRQ       | GPIO 14      | Touch interrupt |

---

## Power Supply

### Power Distribution
| Component | Voltage | Current (Typical) | Notes |
|-----------|---------|-------------------|-------|
| ESP32-S3  | 3.3V    | ~200mA (WiFi on)  | Via USB or 5V regulator |
| Motors    | 5-12V   | 500mA-2A each     | Via motor driver |
| Servo     | 5V      | 100-500mA         | Separate 5V rail recommended |
| Speaker   | 5V      | 500mA peak        | MAX98357A amplifier |
| Sensors   | 3.3V    | ~50mA total       | Via ESP32 3.3V rail |
| TFT Display | 3.3V  | ~100mA            | Backlight included |

**Recommended Battery:** 7.4V 2S LiPo (2000-3000mAh)  
**Voltage Regulators:**
- 5V Buck Converter (3A+) for motors, servo, speaker
- 3.3V LDO (500mA+) for ESP32 and sensors (if not USB powered)

---

## Network Configuration

### WiFi
- **SSID:** Configured in `secrets.h`
- **Mode:** Station (STA)
- **Protocols:** 802.11 b/g/n
- **Frequency:** 2.4 GHz

### Cloud Services
- **Node.js Proxy Server:** `ella-voice-server.onrender.com` (Port 10000)
- **STT Provider:** Deepgram Nova-3
- **AI Provider:** Groq (openai/gpt-oss-20b)
- **TTS Provider:** Google TTS (via Node server)
- **MQTT Broker:** Configured in `secrets.h`
- **Firebase:** Real-time database for web control

---

## Firmware Configuration

### Audio Settings
```cpp
#define SAMPLE_RATE 16000           // Microphone sample rate
#define OPUS_SAMPLE_RATE 16000      // OPUS encoder rate
#define OPUS_FRAME_SIZE 960         // 60ms frames
#define NODE_STT_PCM_CHUNK_SIZE 640 // 40ms audio chunks
```

### Distance Sensor Settings
```cpp
#define ULTRASONIC_TRIG_PIN 15      // HC-SR04 trigger
#define ULTRASONIC_ECHO_PIN 38      // HC-SR04 echo
// HC-SR04 valid range: 2-400cm
// Timeout: 30ms (~5m max range)
```

### Motor Settings
```cpp
#define MOTOR_PWM_FREQ 1000         // PWM frequency (Hz)
#define MOTOR_PWM_RESOLUTION 8      // 8-bit (0-255)
int motorSpeed = 180;               // Default speed
```

### I2C Settings
```cpp
#define I2C_SDA 8
#define I2C_SCL 9
#define I2C_FREQ 400000             // 400kHz
#define TCA_ADDR 0x70               // Multiplexer address
```

### SPI Settings
```cpp
#define TFT_MOSI 11
#define TFT_MISO 13
#define TFT_SCLK 12
#define TFT_CS 10
#define TFT_DC 2
#define TFT_RST -1
#define TOUCH_CS 47
#define TOUCH_IRQ 14
```

---

## Assembly Notes

### Critical Connections
1. **I2S Microphone:** Ensure proper grounding and short wire lengths (<10cm) for clean audio
2. **Motor Driver:** Use separate power supply for motors to avoid ESP32 brownouts
3. **I2C Pull-ups:** 4.7kΩ resistors on SDA/SCL if not built into modules; especially important at 400kHz
4. **Speaker Amplifier:** Keep away from microphone to prevent feedback
5. **IMU Orientation:** Mount MPU6050 flat with chip facing up for proper heading calculation

### Power Considerations
- **Decoupling Capacitors:** 
  - 100µF electrolytic near motor driver
  - 10µF ceramic near ESP32 VIN
  - 0.1µF ceramic near each sensor VCC
- **Ground Plane:** Connect all grounds to a common point (star ground)
- **Motor Noise:** Add 0.1µF capacitors across motor terminals to reduce EMI

### Mechanical
- **Servo Range:** Limit neck movement to 50°-130° to prevent mechanical binding
- **Motor Mounting:** Ensure motors are securely mounted to prevent vibration affecting IMU
- **Sensor Placement:**
  - **HC-SR04 Ultrasonic:** Chest-mounted, fixed forward-facing for navigation and obstacle detection
  - **VL53L0X ToF (future):** Head-mounted, rotates with neck servo for scanning environment
  - **MPU6050 IMU:** Center of robot, away from motors
  - **Microphone:** Top-mounted, away from speaker

---

## Troubleshooting

### Audio Issues
- **No microphone input:** Check I2S pins, verify L/R pin is grounded
- **Distorted audio:** Reduce motor PWM frequency or add filtering
- **Echo/feedback:** Increase physical distance between mic and speaker

### Motor Issues
- **Motors not moving:** Check power supply voltage and current capacity
- **Erratic movement:** Verify PWM pins, check for loose connections
- **IMU drift:** Calibrate IMU using `[IMURESET]` command

### Sensor Issues
- **I2C device not found:** Check multiplexer channel, verify pull-up resistors
- **Display not working:** Verify SPI connections, check CS/DC/RST pins
- **Inaccurate readings:** Allow sensors to warm up (30s for ENS160)
- **Ultrasonic no reading:** Check 5V power supply, verify TRIG/ECHO pins not swapped, and confirm the `ECHO` level shifter/divider is installed
- **Ultrasonic erratic readings:** Ensure sensor is mounted firmly, avoid soft surfaces that absorb sound waves

---

## Safety Warnings

⚠️ **IMPORTANT:**
- Never connect motors directly to ESP32 pins - always use motor driver
- Ensure proper voltage regulation - ESP32 is 3.3V logic only
- Add reverse polarity protection on battery input
- Include fuse on main power line (2-3A recommended)
- Keep LiPo batteries away from heat sources
- Never short circuit motor driver outputs

---

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-05-03 | Initial schematic documentation |
| 1.1 | 2026-05-07 | Moved HC-SR04 echo to GPIO 41 and corrected mux/channel documentation |
| 1.2 | 2026-05-11 | Reverted Mic to 41/42, moved Ultrasonic to 15/38, fixed Touch IRQ noise |

---

## Additional Resources

- **ESP32-S3 Datasheet:** https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf
- **INMP441 Datasheet:** https://invensense.tdk.com/products/digital/inmp441/
- **MAX98357A Datasheet:** https://www.analog.com/media/en/technical-documentation/data-sheets/MAX98357A-MAX98357B.pdf
- **TCA9548A Datasheet:** https://www.ti.com/lit/ds/symlink/tca9548a.pdf
- **HC-SR04 Datasheet:** https://cdn.sparkfun.com/datasheets/Sensors/Proximity/HCSR04.pdf

---

**Document Created:** 2026-05-03  
**Hardware Version:** EllaBox v1.0  
**Firmware Version:** See `ellabox.ino` header
**Last Updated:** 2026-05-11 - Reverted Mic to 41/42, moved Ultrasonic to 15/38, fixed Touch IRQ noise
