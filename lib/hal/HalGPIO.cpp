#include <HalGPIO.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_sleep.h>

// X3 I2C fingerprint addresses (shared with CrossInk's cphw namespace)
#define I2C_ADDR_BQ27220 0x55  // Battery fuel gauge
#define I2C_ADDR_DS3231  0x68  // RTC
#define I2C_ADDR_QMI8658 0x6B  // IMU
#define I2C_ADDR_QMI8658_ALT 0x6A  // IMU alternate address
#define BQ27220_CUR_REG  0x0C  // Current register
#define BQ27220_SOC_REG  0x08  // State of charge register
#define BQ27220_VOLT_REG 0x09  // Voltage register
#define DS3231_SEC_REG   0x00  // Seconds register
#define QMI8658_WHO_AM_I_REG 0x00  // Who am I register
#define QMI8658_WHO_AM_I_VALUE 0x15

// X3 I2C pins (SDA=GPIO20, SCL=GPIO0)
#define X3_I2C_SDA 20
#define X3_I2C_SCL  0
#define X3_I2C_FREQ 100000

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  Wire.requestFrom(addr, (uint8_t)1);
  if (Wire.available()) { *out = Wire.read(); return true; }
  return false;
}

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  Wire.requestFrom(addr, (uint8_t)2);
  if (Wire.available() >= 2) {
    int16_t raw = Wire.read() | (Wire.read() << 8);
    *out = static_cast<uint16_t>(raw);
    return true;
  }
  return false;
}

bool probeBQ27220Signature() {
  uint16_t raw;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) return false;
  int16_t current = static_cast<int16_t>(raw);
  if (current < -500 || current > 500) return false;  // Sanity: within ±500mA
  uint16_t voltageMv;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_VOLT_REG, &voltageMv)) return false;
  if (voltageMv < 3200 || voltageMv > 4200) return false;  // Sanity: 3.2-4.2V
  return true;
}

bool probeDS3231Signature() {
  uint8_t sec;
  if (!readI2CReg8(I2C_ADDR_DS3231, DS3231_SEC_REG, &sec)) return false;
  if (sec > 59) return false;  // BCD seconds: 0-59 valid
  return true;
}

bool probeQMI8658Signature() {
  uint8_t whoami;
  if (readI2CReg8(I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  if (readI2CReg8(I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  return false;
}

struct X3ProbeResult {
  bool bq27220 = false;
  bool ds3231 = false;
  bool qmi8658 = false;
  uint8_t score() const { return (bq27220 ? 1 : 0) + (ds3231 ? 1 : 0) + (qmi8658 ? 1 : 0); }
};

X3ProbeResult runX3ProbePass() {
  X3ProbeResult result;
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);

  result.bq27220 = probeBQ27220Signature();
  result.ds3231 = probeDS3231Signature();
  result.qmi8658 = probeQMI8658Signature();

  Wire.end();
  pinMode(20, INPUT);
  pinMode(0, INPUT);
  return result;
}

uint8_t readNvsDeviceValue(const char* key, uint8_t defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) return defaultValue;
  uint8_t raw = prefs.getUChar(key, defaultValue);
  prefs.end();
  if (raw > 2) return defaultValue;
  return raw;
}

void writeNvsDeviceValue(const char* key, uint8_t value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) return;
  prefs.putUChar(key, value);
  prefs.end();
}

}  // namespace

void HalGPIO::begin() {
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  // BAT_GPIO0 is configured for ADC via adc1_config_channel_atten in InputManager::begin()
  // — do NOT call pinMode() here as it reconfigures the pin as digital input in dual framework
  // On X4, GPIO20 (UART0_RXD) is used for USB detection.
  // On X3, GPIO20 is I2C SDA for the BQ27220 gauge — do NOT configure it as digital input.
  if (_deviceType != DeviceType::X3) {
    pinMode(UART0_RXD, INPUT);
  }
}

void HalGPIO::update() { inputMgr.update(); }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

void HalGPIO::startDeepSleep() {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  // Arm the wakeup trigger *after* the button is released
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

// ============================================================================
// BQ27220 gauge battery reading (X3 only)
// ============================================================================

int HalGPIO::getBQ27220BatteryPercentage() const {
  static int cachedPct = -1;
  static unsigned long lastReadMs = 0;

  // Load from NVS on first call
  if (cachedPct < 0) {
    Preferences prefs;
    prefs.begin("battery", true);
    cachedPct = prefs.getInt("pct", -1);
    prefs.end();
  }

  unsigned long now = millis();

  // Battery changes slowly — poll every 30 seconds
  if (cachedPct < 0 || (now - lastReadMs) >= 30000) {
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    uint16_t soc;
    bool ok = readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_SOC_REG, &soc);
    Wire.end();
    pinMode(20, INPUT);
    pinMode(0, INPUT);

    if (ok) {
      // BQ27220 SOC is in 0.1% units (0-1000 = 0%-100%)
      int newPct = static_cast<int>(soc) / 10;
      if (newPct > 100) newPct = 100;
      if (newPct < 0) newPct = 0;

      // Rate-limit drops: max 2% per read cycle
      if (cachedPct >= 0 && newPct < cachedPct - 2) {
        newPct = cachedPct - 2;
      }

      if (newPct != cachedPct) {
        Preferences prefs;
        prefs.begin("battery", false);
        prefs.putInt("pct", newPct);
        prefs.end();
      }
      cachedPct = newPct;
    }
    lastReadMs = now;
  }

  return cachedPct;
}

int HalGPIO::getBatteryPercentage() const {
  // On X3, read from BQ22720 fuel gauge via I2C
  if (_deviceType == DeviceType::X3) {
    return getBQ27220BatteryPercentage();
  }

  // X4: ADC-based battery reading
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);
  static int cachedPct = -1;
  static float smoothedMv = -1.0f;
  static unsigned long lastReadMs = 0;
  static bool adcSettled = false;

  // On first call, load the last persisted reading so we don't show a stale default
  if (cachedPct < 0) {
    Preferences prefs;
    prefs.begin("battery", true);  // read-only
    cachedPct = prefs.getInt("pct", -1);
    prefs.end();
  }

  unsigned long now = millis();

  const bool usbCharging = isUsbConnected();

  // ADC reads high for ~2 minutes after boot/wake — trust NVS cache during settling.
  // Skip this window when charging: voltage is actively changing and we want real readings.
  if (!adcSettled) {
    if (!usbCharging && now < 120000) {
      // Still settling — return NVS cached value if we have one
      if (cachedPct >= 0) return cachedPct;
      // No NVS value at all — fall through to read ADC (better than showing nothing)
    }
    adcSettled = true;
  }

  // Battery voltage changes on a timescale of minutes — no need to read every frame
  if (cachedPct < 0 || (now - lastReadMs) >= 30000) {
    float rawMv = static_cast<float>(battery.readMillivolts());

    // EMA smoothing on millivolts (before the nonlinear polynomial).
    // Alpha=0.3 means ~70% weight on history — takes ~5 reads (~2.5 min) to converge,
    // which rejects brief voltage spikes from charging cycles / SPI / BLE noise.
    // When charging, use alpha=1.0 (no smoothing) so voltage tracks in real time.
    if (smoothedMv < 0) {
      smoothedMv = rawMv;  // seed with first real reading
    } else if (usbCharging) {
      smoothedMv = rawMv;  // no smoothing while charging
    } else {
      smoothedMv = 0.3f * rawMv + 0.7f * smoothedMv;
    }

    int newPct = BatteryMonitor::percentageFromMillivolts(static_cast<uint16_t>(smoothedMv));

    // Rate-limit drops only: max 2% decrease per read cycle (every 30s) on battery.
    // Rising is uncapped so the display catches up quickly after charging.
    // When charging via USB, drops are also uncapped (voltage actively rising anyway).
    if (cachedPct >= 0) {
      const int maxDrop = usbCharging ? 20 : 2;
      if (newPct < cachedPct - maxDrop) newPct = cachedPct - maxDrop;
    }

    if (newPct != cachedPct) {
      Preferences prefs;
      prefs.begin("battery", false);
      prefs.putInt("pct", newPct);
      prefs.end();
    }
    cachedPct = newPct;
    lastReadMs = now;
  }
  return cachedPct;
}

bool HalGPIO::isUsbConnected() const {
  // On X3, GPIO20 is I2C SDA (gauge) — infer USB/charging from gauge current
  if (_deviceType == DeviceType::X3) {
    uint16_t raw;
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    bool connected = readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw);
    Wire.end();
    pinMode(20, INPUT);
    pinMode(0, INPUT);
    if (connected) {
      int16_t currentMa = static_cast<int16_t>(raw);
      return currentMa > 50;  // Positive current = charging
    }
    return false;
  }

  // X4: U0RXD/GPIO20 reads HIGH when USB is connected
  return digitalRead(UART0_RXD) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const bool usbConnected = isUsbConnected();
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}

HalGPIO::DeviceType HalGPIO::detectDevice() {
  // Check NVS override first (0=auto, 1=X4, 2=X3)
  uint8_t override = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, 0);
  if (override == 2) {
    if (Serial) Serial.println("[HW] Device override active: X3");
    _deviceType = DeviceType::X3;
    return _deviceType;
  }
  if (override == 1) {
    if (Serial) Serial.println("[HW] Device override active: X4");
    _deviceType = DeviceType::X4;
    return _deviceType;
  }

  // Check NVS cache
  uint8_t cached = readNvsDeviceValue(NVS_KEY_DEV_CACHED, 0);
  if (cached == 2) {
    if (Serial) Serial.println("[HW] Using cached device type: X3");
    _deviceType = DeviceType::X3;
    return _deviceType;
  }
  if (cached == 1) {
    if (Serial) Serial.println("[HW] Using cached device type: X4");
    _deviceType = DeviceType::X4;
    return _deviceType;
  }

  // No cache: run active X3 fingerprint probe (two passes, need >=2 of 3 each)
  if (Serial) Serial.println("[HW] Running X3 fingerprint probe...");
  X3ProbeResult pass1 = runX3ProbePass();
  delay(2);
  X3ProbeResult pass2 = runX3ProbePass();

  uint8_t score1 = pass1.score();
  uint8_t score2 = pass2.score();

  if (Serial) Serial.printf("[HW] X3 probe scores: pass1=%u(bq=%d rtc=%d imu=%d) pass2=%u(bq=%d rtc=%d imu=%d)\n",
                            score1, pass1.bq27220, pass1.ds3231, pass1.qmi8658,
                            score2, pass2.bq27220, pass2.ds3231, pass2.qmi8658);

  bool x3Confirmed = (score1 >= 2) && (score2 >= 2);
  bool x4Confirmed = (score1 == 0) && (score2 == 0);

  if (x3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, 2);
    _deviceType = DeviceType::X3;
    if (Serial) Serial.println("[HW] Hardware detect: X3");
  } else if (x4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, 1);
    _deviceType = DeviceType::X4;
    if (Serial) Serial.println("[HW] Hardware detect: X4");
  } else {
    // Conservative fallback
    _deviceType = DeviceType::X4;
    if (Serial) Serial.println("[HW] Inconclusive probe, defaulting to X4");
  }

  return _deviceType;
}
