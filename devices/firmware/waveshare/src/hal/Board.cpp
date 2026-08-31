#include "Board.h"

#include "../Config.h"
#include "diag/Log.h"
#include <Wire.h>
#include <XPowersLib.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

namespace {
/// QSPI bus used by the display panel.
Arduino_DataBus *displayBus =
    new Arduino_ESP32QSPI(LCD_CS_PIN, LCD_SCLK_PIN, LCD_SDIO0_PIN,
                          LCD_SDIO1_PIN, LCD_SDIO2_PIN, LCD_SDIO3_PIN);
/// Shared display panel instance. The concrete driver is selected per build
/// (see "Panel variant" in Config.h); everything outside this file only sees
/// the common Arduino_OLED interface.
#if defined(WAVESHARE_PANEL_CO5300)
Arduino_OLED *displayPanel = new Arduino_CO5300(
    displayBus, GFX_NOT_DEFINED, 0, SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX,
    LCD_PANEL_COL_OFFSET, 0, 0, 0);
#else
Arduino_OLED *displayPanel = new Arduino_SH8601(
    displayBus, GFX_NOT_DEFINED, 0, SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX,
    LCD_PANEL_COL_OFFSET, 0, 0, 0);
#endif

/// Power-management IC driver instance.
XPowersPMU pmu;
/// Whether the PMU initialized successfully.
bool pmuReady = false;
/// Latched state of the PMU power button.
bool pwrPressed = false;
/// Timestamp until which a PMU button pulse remains visible.
unsigned long pwrPulseUntilMs = 0;
/// Timestamp of the last PMU IRQ poll.
unsigned long lastPmuPollMs = 0;
/// Last brightness written to the display.
uint8_t currentBrightness = DEFAULT_BRIGHTNESS;
const DeviceCapabilities kCapabilities = {.externalSpeakerSwitch = false,
                                          .externalSpeakerGain = false,
                                          .lightSleep = true,
                                          .batteryLevel = true,
                                          .batteryVoltage = true,
                                          .usbPowerStatus = true,
                                          .endpointPreference = true,
                                          .bootDisplay =
                                              SHOW_BOOT_LOG_ON_DISPLAY,
                                          .debugDisplay =
                                              SHOW_DEBUG_TEXT_ON_DISPLAY};

/**
 * @brief Enable PMU ADC channels used for power telemetry.
 */
void enablePmuAdc() {
  pmu.enableTemperatureMeasure();
  pmu.enableBattDetection();
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
}

/**
 * @brief Poll PMU power-button IRQ state and update cached flags.
 */
void pollPmuButton() {
  if (!pmuReady || millis() - lastPmuPollMs < 20) {
    return;
  }
  const unsigned long now = millis();
  lastPmuPollMs = now;

  pmu.getIrqStatus();
  const bool negative = pmu.isPekeyNegativeIrq();
  const bool positive = pmu.isPekeyPositiveIrq();
  const bool shortPress = pmu.isPekeyShortPressIrq();
  const bool longPress = pmu.isPekeyLongPressIrq();

  if (negative) {
    pwrPressed = true;
    pwrPulseUntilMs = 0;
  }
  if (positive) {
    pwrPressed = false;
    pwrPulseUntilMs = 0;
  }
  if (shortPress) {
    if (negative || positive || pwrPressed) {
      pwrPressed = false;
      pwrPulseUntilMs = 0;
    } else {
      pwrPulseUntilMs = now + 180;
    }
  }
  if (longPress && !pwrPressed && !negative && !positive) {
    pwrPulseUntilMs = now + 1200;
  }
  pmu.clearIrqStatus();
}
} // namespace

namespace Board {
const DeviceCapabilities &capabilities() { return kCapabilities; }

/**
 * @brief Initialize board GPIO, I2C, PMU, and amplifier defaults.
 * @return True after initialization completes.
 */
bool init() {
  pinMode(BUTTON_A_PIN, INPUT_PULLUP);
  pinMode(AUDIO_PA_ENABLE_PIN, OUTPUT);
  digitalWrite(AUDIO_PA_ENABLE_PIN, HIGH);

  Log::client("Board", "panel=%s", PANEL_NAME);

  Wire.begin(BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
  Wire.setClock(400000);

  pmuReady = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, BOARD_I2C_SDA_PIN,
                       BOARD_I2C_SCL_PIN);
  if (pmuReady) {
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.setChargeTargetVoltage(3);
    // Let the system rail sag as far as the PMU allows (2.6V) before it cuts
    // power. With a higher threshold, a load transient on battery (WiFi burst
    // + AMOLED + codec) can dip VSYS across it and the AXP2101 shuts the
    // board down instantly — a clean "device just turned off", no brownout
    // reset logged.
    pmu.setSysPowerDownVoltage(2600);
    pmu.enableIRQ(
        XPOWERS_AXP2101_PKEY_POSITIVE_IRQ | XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ |
        XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
    enablePmuAdc();
    Log::client("Board",
                "AXP2101 online batt=%dmV pct=%d vbus=%dmV chg=%d vsysmin=%umV",
                pmu.getBattVoltage(), pmu.getBatteryPercent(),
                pmu.getVbusVoltage(), pmu.isCharging() ? 1 : 0,
                pmu.getSysPowerDownVoltage());
  } else {
    Log::client("Board", "AXP2101 not found");
  }

  return true;
}

/**
 * @brief Service board-level background polling.
 */
void update() { pollPmuButton(); }

/**
 * @brief Access the shared display driver.
 * @return Reference to the display panel.
 */
Arduino_OLED &display() { return *displayPanel; }

/**
 * @brief Read the current state of button A.
 * @return True when button A is pressed.
 */
bool buttonAIsPressed() { return digitalRead(BUTTON_A_PIN) == LOW; }

/**
 * @brief Read the current state of the PMU-backed button B.
 * @return True when the button is pressed or within a pulse window.
 */
bool buttonBIsPressed() { return pwrPressed || millis() < pwrPulseUntilMs; }

/**
 * @brief Apply display brightness and cache the requested value.
 * @param brightness Brightness level to apply.
 */
void setDisplayBrightness(uint8_t brightness) {
  currentBrightness = brightness;
  if (brightness == 0) {
    displayPanel->displayOff();
  } else {
    displayPanel->displayOn();
    displayPanel->setBrightness(brightness);
  }
}

/**
 * @brief Return the last requested display brightness.
 * @return Cached brightness level.
 */
uint8_t displayBrightness() { return currentBrightness; }

/**
 * @brief Enable or disable the external speaker amplifier.
 * @param enabled True to power the amplifier.
 */
void setAudioAmpEnabled(bool enabled) {
  digitalWrite(AUDIO_PA_ENABLE_PIN, enabled ? HIGH : LOW);
}

/**
 * @brief Report the battery charge percentage.
 * @return Battery percentage, or -1 if unavailable.
 */
int batteryLevel() {
  if (!pmuReady || !pmu.isBatteryConnect()) {
    return -1;
  }
  return pmu.getBatteryPercent();
}

/**
 * @brief Report the current battery voltage.
 * @return Battery voltage in millivolts, or 0 if unavailable.
 */
uint16_t batteryVoltageMv() {
  if (!pmuReady) {
    return 0;
  }
  return pmu.getBattVoltage();
}

/**
 * @brief Report the current USB VBUS voltage.
 * @return VBUS voltage in millivolts, or 0 if unavailable.
 */
uint16_t vbusVoltageMv() {
  if (!pmuReady) {
    return 0;
  }
  return pmu.getVbusVoltage();
}

/**
 * @brief Determine whether USB power is present.
 * @return True when VBUS is detected.
 */
bool usbConnected() { return pmuReady && pmu.isVbusIn(); }

/**
 * @brief Return a short label for the current power source.
 * @return "USB", "BAT", or "?" when unknown.
 */
const char *powerSourceLabel() {
  if (!pmuReady) {
    return "?";
  }
  if (usbConnected()) {
    return "USB";
  }
  if (pmu.isBatteryConnect()) {
    return "BAT";
  }
  return "?";
}

/**
 * @brief Enter one light-sleep interval and report why the device woke.
 * @param wakeIntervalMs Timer wake interval in milliseconds.
 * @return Wake reason used by shared power management.
 */
LightSleepWakeReason enterLightSleep(unsigned long wakeIntervalMs) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_enable(BUTTON_A_PIN, GPIO_INTR_LOW_LEVEL);
  if (BUTTON_B_PIN != GPIO_NUM_NC) {
    gpio_wakeup_enable(BUTTON_B_PIN, GPIO_INTR_LOW_LEVEL);
  }
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(wakeIntervalMs * 1000ULL);

  esp_light_sleep_start();

  const esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
  update();
  if (reason == ESP_SLEEP_WAKEUP_GPIO) {
    return LightSleepWakeReason::Button;
  }
  if (reason == ESP_SLEEP_WAKEUP_TIMER) {
    return buttonAIsPressed() || buttonBIsPressed() ? LightSleepWakeReason::Button
                                                    : LightSleepWakeReason::Timer;
  }
  return LightSleepWakeReason::Other;
}

/**
 * @brief Report why this boot resumed from deep sleep.
 * @return Deep sleep wake reason used by the shared controller.
 */
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

/**
 * @brief Enter deep sleep until the timer or wake-capable button fires.
 * @param sleepUs Timer wake interval in microseconds.
 */
void enterDeepSleep(uint64_t sleepUs) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup(sleepUs);
  if (BUTTON_A_PIN != GPIO_NUM_NC) {
    esp_sleep_enable_ext1_wakeup(1ULL << static_cast<unsigned>(BUTTON_A_PIN),
                                 ESP_EXT1_WAKEUP_ANY_LOW);
  }
  esp_deep_sleep_start();
}

/**
 * @brief Shut down display and audio, then enter deep sleep.
 */
void powerOff() {
  setAudioAmpEnabled(false);
  setDisplayBrightness(BRIGHTNESS_OFF);

  if (pmuReady) {
    pmu.shutdown();
    delay(200);
  }
  esp_deep_sleep_start();
}
} // namespace Board
