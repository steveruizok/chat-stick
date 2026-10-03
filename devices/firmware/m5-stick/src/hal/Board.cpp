#include "Board.h"

#include "../Config.h"
#include <M5PM1.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <time.h>

namespace {
M5PM1 pm1;
bool pm1Ready = false;
unsigned long lastPm1PollMs = 0;
bool externalSpeaker = false;
uint8_t currentBrightness = DEFAULT_BRIGHTNESS;
m5pm1_pwr_src_t currentPowerSource = M5PM1_PWR_SRC_UNKNOWN;

const DeviceCapabilities kCapabilities = {.externalSpeakerSwitch = true,
                                          .externalSpeakerGain = true,
                                          .lightSleep = false,
                                          .batteryLevel = true,
                                          .batteryVoltage = true,
                                          .usbPowerStatus = true,
                                          .endpointPreference = false,
                                          .bootDisplay = false,
                                          .debugDisplay = false};

const char *sourceLabel(m5pm1_pwr_src_t source) {
  switch (source) {
  case M5PM1_PWR_SRC_5VIN:
    return "USB";
  case M5PM1_PWR_SRC_5VINOUT:
    return "5Vout";
  case M5PM1_PWR_SRC_BAT:
    return "BAT";
  default:
    return "?";
  }
}

uint64_t gpioWakeMask(gpio_num_t pin) {
  const int value = static_cast<int>(pin);
  if (value < 0 || value >= 64) {
    return 0;
  }
  return 1ULL << static_cast<unsigned>(value);
}

void configureRtcPullup(gpio_num_t pin) {
  if (static_cast<int>(pin) < 0) {
    return;
  }
  rtc_gpio_pullup_en(pin);
  rtc_gpio_pulldown_dis(pin);
}
} // namespace

namespace Board {
void configureSpeaker(m5::speaker_config_t &cfg, bool external, int gain) {
  externalSpeaker = external;
  M5.Power.setExtOutput(external);
  setAudioAmpEnabled(false);
  if (external) {
    // HAT SPK2 (MAX98357 I2S) on the StickS3 HAT header.
    // Header positions map StickC+2's G26/G25/G0 → StickS3's G0/G1/G8.
    cfg.pin_data_out = GPIO_NUM_1;
    cfg.pin_bck = GPIO_NUM_0;
    cfg.pin_ws = GPIO_NUM_8;
    cfg.pin_mck = I2S_PIN_NO_CHANGE;
    cfg.i2s_port = I2S_NUM_1;
    cfg.stereo = false;
    cfg.use_dac = false;
    cfg.buzzer = false;
    cfg.magnification = gain;
  } else {
    // StickS3 internal speaker defaults.
    cfg.pin_data_out = GPIO_NUM_14;
    cfg.pin_bck = GPIO_NUM_17;
    cfg.pin_ws = GPIO_NUM_15;
    cfg.pin_mck = GPIO_NUM_18;
    cfg.i2s_port = I2S_NUM_0;
    cfg.stereo = true;
    cfg.use_dac = false;
    cfg.buzzer = false;
  }
}

bool init() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.output_power = false;
  M5.begin(cfg);
  M5.Power.setExtOutput(false);
  setAudioAmpEnabled(false);

  const m5pm1_err_t pm1BeginRc = pm1.begin(&M5.In_I2C);
  if (pm1BeginRc == M5PM1_OK) {
    pm1Ready = true;
    pm1.setSingleResetDisable(true);
    const m5pm1_err_t chgRc = pm1.setChargeEnable(true);
    m5pm1_irq_btn_t drain;
    pm1.irqGetBtnStatusEnum(&drain, M5PM1_CLEAN_ALL);
    uint16_t vbat = 0, vin = 0;
    pm1.readVbat(&vbat);
    pm1.readVin(&vin);
    pm1.getPowerSource(&currentPowerSource);
    Serial.printf("[PM1] init OK; chargeEnable rc=%d vbat=%u vin=%u src=%d\n",
                  static_cast<int>(chgRc), vbat, vin,
                  static_cast<int>(currentPowerSource));
  } else {
    Serial.printf("[PM1] init failed rc=%d\n", static_cast<int>(pm1BeginRc));
  }

  currentBrightness = M5.Display.getBrightness();
  return true;
}

const DeviceCapabilities &capabilities() { return kCapabilities; }

void update() {
  M5.update();

  if (!pm1Ready || millis() - lastPm1PollMs <= 3000) {
    return;
  }

  lastPm1PollMs = millis();
  uint16_t vbat = 0, vin = 0, v5 = 0;
  pm1.readVbat(&vbat);
  pm1.readVin(&vin);
  pm1.read5VInOut(&v5);
  pm1.getPowerSource(&currentPowerSource);

  char ts[16];
  time_t now = time(nullptr);
  struct tm local;
  if (localtime_r(&now, &local) && local.tm_year + 1900 >= 2024) {
    strftime(ts, sizeof(ts), "%H:%M:%S", &local);
  } else {
    snprintf(ts, sizeof(ts), "up+%lus", millis() / 1000);
  }
  Serial.printf("[%s] [Pwr] vbat=%u mV level=%d vin=%u v5out=%u src=%s "
                "heap=%uK\n",
                ts, vbat, batteryLevel(), vin, v5,
                sourceLabel(currentPowerSource),
                static_cast<unsigned>(ESP.getFreeHeap() / 1024));
}

M5GFX &display() { return M5.Display; }

bool buttonAIsPressed() { return M5.BtnA.isPressed(); }

bool buttonBIsPressed() { return M5.BtnB.isPressed(); }

void setDisplayBrightness(uint8_t brightness) {
  currentBrightness = brightness;
  M5.Display.setBrightness(brightness);
}

uint8_t displayBrightness() { return currentBrightness; }

void setAudioAmpEnabled(bool enabled) {
  // PM1 GPIO3 drives the internal amp. Its mux is bits 6:7, not bit 3.
  auto &power = M5.Power.M5pm1;
  power.setGPIOOutput(m5::M5PM1_Class::gpio3, enabled && !externalSpeaker);
  power.setGPIODrive(m5::M5PM1_Class::gpio3, m5::M5PM1_Class::push_pull);
  power.setGPIOMode(m5::M5PM1_Class::gpio3, m5::M5PM1_Class::output);
  power.setGPIOFunction(m5::M5PM1_Class::gpio3, m5::M5PM1_Class::gpio);
}

int batteryLevel() { return M5.Power.getBatteryLevel(); }

uint16_t batteryVoltageMv() {
  if (!pm1Ready) {
    return 0;
  }
  uint16_t vbat = 0;
  pm1.readVbat(&vbat);
  return vbat;
}

uint16_t vbusVoltageMv() {
  if (!pm1Ready) {
    return 0;
  }
  uint16_t vin = 0;
  pm1.readVin(&vin);
  return vin;
}

bool usbConnected() {
  if (!pm1Ready) {
    return false;
  }
  pm1.getPowerSource(&currentPowerSource);
  return currentPowerSource == M5PM1_PWR_SRC_5VIN ||
         currentPowerSource == M5PM1_PWR_SRC_5VINOUT;
}

const char *powerSourceLabel() {
  if (!pm1Ready) {
    return "?";
  }
  pm1.getPowerSource(&currentPowerSource);
  return sourceLabel(currentPowerSource);
}

LightSleepWakeReason enterLightSleep(unsigned long wakeIntervalMs) {
  delay(wakeIntervalMs);
  return LightSleepWakeReason::Unsupported;
}

DeepSleepWakeReason deepSleepWakeReason() {
  const esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
  if (reason == ESP_SLEEP_WAKEUP_TIMER) {
    return DeepSleepWakeReason::Timer;
  }
  if (reason == ESP_SLEEP_WAKEUP_EXT1 || reason == ESP_SLEEP_WAKEUP_GPIO) {
    return DeepSleepWakeReason::Button;
  }
  if (reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
    return DeepSleepWakeReason::None;
  }
  return DeepSleepWakeReason::Other;
}

void enterDeepSleep(uint64_t sleepUs) {
  setAudioAmpEnabled(false);
  M5.Power.setExtOutput(false);
  esp_sleep_enable_timer_wakeup(sleepUs);
  configureRtcPullup(BUTTON_A_PIN);
  configureRtcPullup(BUTTON_B_PIN);
  const uint64_t buttonMask =
      gpioWakeMask(BUTTON_A_PIN) | gpioWakeMask(BUTTON_B_PIN);
  if (buttonMask != 0) {
    esp_sleep_enable_ext1_wakeup(buttonMask, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  esp_deep_sleep_start();
}

void powerOff() {
  setAudioAmpEnabled(false);
  M5.Power.setExtOutput(false);
  if (pm1Ready) {
    const m5pm1_err_t rc = pm1.shutdown();
    Serial.printf("[Power] PM1 shutdown rc=%d\n", static_cast<int>(rc));
    delay(200);
  }
  M5.Power.powerOff();
}
} // namespace Board
