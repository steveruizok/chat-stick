#include "SettingsStore.h"

#include "../Config.h"

void SettingsStore::init() {
  _brightness = DEFAULT_BRIGHTNESS;
  _volume = DEFAULT_VOLUME;
  _chatId = "";

  _ready = _prefs.begin(kNamespace, false);
  if (!_ready) {
    Serial.println("[Settings] Preferences init failed; using defaults");
    return;
  }

  _brightness =
      constrain(_prefs.getUChar(kBrightnessKey, DEFAULT_BRIGHTNESS), 0, 255);
  _volume = constrain(_prefs.getUChar(kVolumeKey, DEFAULT_VOLUME), 0, 255);
  _chatId = _prefs.getString(kChatIdKey, "");

  Serial.printf("[Settings] Loaded brightness=%d volume=%d chat=%s\n",
                _brightness, _volume,
                _chatId.isEmpty() ? "(none)" : _chatId.c_str());
}

void SettingsStore::setBrightness(int brightness) {
  _brightness = constrain(brightness, 0, 255);
  if (_ready) {
    _prefs.putUChar(kBrightnessKey, static_cast<uint8_t>(_brightness));
  }
}

void SettingsStore::setVolume(int volume) {
  _volume = constrain(volume, 0, 255);
  if (_ready) {
    _prefs.putUChar(kVolumeKey, static_cast<uint8_t>(_volume));
  }
}

void SettingsStore::setChatId(const String &chatId) {
  _chatId = chatId;
  if (_ready) {
    _prefs.putString(kChatIdKey, _chatId);
  }
}

void SettingsStore::clearChatId() {
  _chatId = "";
  if (_ready) {
    _prefs.remove(kChatIdKey);
  }
}
