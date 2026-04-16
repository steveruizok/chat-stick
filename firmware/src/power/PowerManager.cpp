#include "PowerManager.h"

#include "../Config.h"
#include <M5Unified.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

const char *powerStateName(PowerState state) {
  switch (state) {
  case PowerState::Active:
    return "Active";
  case PowerState::Dimmed:
    return "Dimmed";
  case PowerState::ScreenOff:
    return "ScreenOff";
  case PowerState::Waking:
    return "Waking";
  case PowerState::LightSleep:
    return "LightSleep";
  case PowerState::PowerOff:
    return "PowerOff";
  default:
    return "Unknown";
  }
}

PowerManager::PowerManager()
    : _state(PowerState::Active), _lastActivityMs(millis()),
      _savedBrightness(DEFAULT_BRIGHTNESS),
      _timeouts({IDLE_DIM_MS, IDLE_SCREEN_OFF_MS, IDLE_LIGHT_SLEEP_MS,
                 IDLE_POWER_OFF_MS}) {}

void PowerManager::update() {
  if (_state == PowerState::Waking || _state == PowerState::PowerOff) {
    return;
  }

  const unsigned long idle = getIdleTime();

  PowerState target = PowerState::Active;
  if (idle >= _timeouts.powerOffMs) {
    target = PowerState::PowerOff;
  } else if (idle >= _timeouts.lightSleepMs) {
    target = PowerState::LightSleep;
  } else if (idle >= _timeouts.screenOffMs) {
    target = PowerState::ScreenOff;
  } else if (idle >= _timeouts.dimMs) {
    target = PowerState::Dimmed;
  }

  if (target > _state) {
    transitionTo(target);
  }
}

void PowerManager::registerActivity() {
  _lastActivityMs = millis();

  if (isInterruptible()) {
    restoreActive();
  }
}

void PowerManager::setTimeouts(const PowerTimeouts &timeouts) {
  _timeouts.dimMs = max(1000UL, timeouts.dimMs);
  _timeouts.screenOffMs = max(_timeouts.dimMs + 1000UL, timeouts.screenOffMs);
  _timeouts.lightSleepMs =
      max(_timeouts.screenOffMs + 1000UL, timeouts.lightSleepMs);
  _timeouts.powerOffMs =
      max(_timeouts.lightSleepMs + 1000UL, timeouts.powerOffMs);
  Serial.printf("[Power] Updated timeouts dim=%lu screen=%lu sleep=%lu off=%lu\n",
                _timeouts.dimMs, _timeouts.screenOffMs, _timeouts.lightSleepMs,
                _timeouts.powerOffMs);
}

void PowerManager::beginWaking() {
  if (!isInterruptible()) {
    return;
  }

  const PowerState previous = _state;
  _state = PowerState::Waking;
  _lastActivityMs = millis();

  Serial.printf("[Power] %s -> Waking\n", powerStateName(previous));

  if (_brightnessCallback) {
    _brightnessCallback(_savedBrightness);
  }
}

void PowerManager::finishWaking() {
  if (_state != PowerState::Waking) {
    return;
  }

  _state = PowerState::Active;
  _lastActivityMs = millis();
  Serial.println("[Power] Waking -> Active");
}

void PowerManager::restoreActive() {
  if (_state == PowerState::Active || _state == PowerState::Waking) {
    return;
  }

  Serial.printf("[Power] %s -> Active\n", powerStateName(_state));
  _state = PowerState::Active;

  if (_brightnessCallback) {
    _brightnessCallback(_savedBrightness);
  }
}

unsigned long PowerManager::getIdleTime() const {
  return millis() - _lastActivityMs;
}

void PowerManager::transitionTo(PowerState newState) {
  if (newState == _state) {
    return;
  }

  const PowerState oldState = _state;
  _state = newState;

  Serial.printf("[Power] %s -> %s\n", powerStateName(oldState),
                powerStateName(newState));

  switch (newState) {
  case PowerState::Active:
    if (_brightnessCallback) {
      _brightnessCallback(_savedBrightness);
    }
    break;

  case PowerState::Dimmed:
    if (_brightnessCallback) {
      _brightnessCallback(BRIGHTNESS_DIM);
    }
    break;

  case PowerState::ScreenOff:
    if (_brightnessCallback) {
      _brightnessCallback(BRIGHTNESS_OFF);
    }
    break;

  case PowerState::LightSleep:
    enterLightSleep();
    break;

  case PowerState::PowerOff:
    if (_brightnessCallback) {
      _brightnessCallback(BRIGHTNESS_OFF);
    }
    if (_wifiCallback) {
      _wifiCallback(false);
    }
    if (_powerOffCallback) {
      _powerOffCallback();
    }
    break;

  case PowerState::Waking:
    break;
  }
}

void PowerManager::enterLightSleep() {
  Serial.println("[Power] Entering light sleep...");

  if (_brightnessCallback) {
    _brightnessCallback(BRIGHTNESS_OFF);
  }
  M5.Display.sleep();

  if (_wifiCallback) {
    _wifiCallback(false);
  }

  while (true) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_wakeup_enable(BUTTON_A_PIN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(BUTTON_B_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_WAKE_INTERVAL_MS * 1000ULL);

    esp_light_sleep_start();

    const esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
    if (reason == ESP_SLEEP_WAKEUP_GPIO) {
      Serial.println("[Power] Light sleep wake: button");
      M5.Display.wakeup();
      if (_wifiCallback) {
        _wifiCallback(true);
      }
      _state = PowerState::Waking;
      _lastActivityMs = millis();
      if (_brightnessCallback) {
        _brightnessCallback(_savedBrightness);
      }
      return;
    }

    if (reason == ESP_SLEEP_WAKEUP_TIMER) {
      const unsigned long idle = getIdleTime();
      Serial.printf("[Power] Light sleep timer wake, idle=%lu ms\n", idle);
      if (idle >= _timeouts.powerOffMs) {
        _state = PowerState::PowerOff;
        transitionTo(PowerState::PowerOff);
        return;
      }
    }
  }
}
