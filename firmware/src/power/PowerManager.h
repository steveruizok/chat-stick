#pragma once

#include <Arduino.h>
#include <functional>

enum class PowerState {
  Active,
  Dimmed,
  ScreenOff,
  Waking,
  LightSleep,
  PowerOff,
};

const char *powerStateName(PowerState state);

class PowerManager {
public:
  PowerManager();

  void update();
  void registerActivity();

  PowerState getState() const { return _state; }
  unsigned long getIdleTime() const;

  bool isInterruptible() const {
    return _state == PowerState::Dimmed || _state == PowerState::ScreenOff ||
           _state == PowerState::LightSleep;
  }

  bool isWaking() const { return _state == PowerState::Waking; }

  void beginWaking();
  void finishWaking();
  void restoreActive();

  void setSavedBrightness(int brightness) { _savedBrightness = brightness; }

  void onBrightnessChange(std::function<void(int)> callback) {
    _brightnessCallback = callback;
  }

  void onWiFiStateChange(std::function<void(bool)> callback) {
    _wifiCallback = callback;
  }

  void onPowerOff(std::function<void()> callback) { _powerOffCallback = callback; }

private:
  PowerState _state;
  unsigned long _lastActivityMs;
  int _savedBrightness;

  std::function<void(int)> _brightnessCallback;
  std::function<void(bool)> _wifiCallback;
  std::function<void()> _powerOffCallback;

  void transitionTo(PowerState newState);
  void enterLightSleep();
};
