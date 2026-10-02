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
uint8_t currentBrightness = DEFAULT_BRIGHTNESS;
m5pm1_pwr_src_t currentPowerSource = M5PM1_PWR_SRC_UNKNOWN;

const DeviceCapabilities kCapabilities = {.externalSpeakerSwitch = false,
                                          .externalSpeakerGain = false,
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
void configureSpeaker(m5::speaker_config_t &, bool, int) {
  // Preserve M5Unified's StopWatch ES8311 pins, I2S format and gain.
}
bool initDisplay() { return M5.Display.width() == 466; }
void drawDisplayBitmap(int x, int y, uint16_t *pixels, int w, int h) {
  // The shared framebuffer holds native RGB565 words. M5GFX's untyped
  // uint16_t overload defaults to byte-swapped data: gray 0x7BEF would
  // become near-white 0xEF7B, hiding the recording/thinking dim state.
  M5.Display.pushImage(x, y, w, h,
                      reinterpret_cast<const lgfx::rgb565_t *>(pixels));
}
bool init() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.fallback_board = m5::board_t::board_M5StopWatch;
  M5.begin(cfg);
  M5.Display.setRotation(0);
  pinMode(BUTTON_A_PIN, INPUT_PULLUP);
  pinMode(BUTTON_B_PIN, INPUT_PULLUP);
  Serial.printf("[Board] StopWatch display=%dx%d PSRAM=%u A=G2 B=G1\n",
                M5.Display.width(), M5.Display.height(), ESP.getPsramSize());

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

bool buttonAIsPressed() { return digitalRead(BUTTON_A_PIN) == LOW; }

bool buttonBIsPressed() { return digitalRead(BUTTON_B_PIN) == LOW; }

void setDisplayBrightness(uint8_t brightness) {
  currentBrightness = brightness;
  M5.Display.setBrightness(brightness);
}

uint8_t displayBrightness() { return currentBrightness; }

void setAudioAmpEnabled(bool) {}

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
  // Some PM1 revisions report an undocumented source value. The measured
  // USB input is still valid; use it to keep idle shutdown off while plugged in.
  return vbusVoltageMv() >= 4000;
}

const char *powerSourceLabel() {
  if (!pm1Ready) {
    return "?";
  }
  pm1.getPowerSource(&currentPowerSource);
  return usbConnected() ? "USB" : "BAT";
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
  if (pm1Ready) {
    const m5pm1_err_t rc = pm1.shutdown();
    Serial.printf("[Power] PM1 shutdown rc=%d\n", static_cast<int>(rc));
    delay(200);
  }
  M5.Power.powerOff();
}
} // namespace Board
