#pragma once

#include <Arduino.h>
#include <Preferences.h>

class SettingsStore {
public:
  void init();

  int brightness() const { return _brightness; }
  int volume() const { return _volume; }
  const String &chatId() const { return _chatId; }

  void setBrightness(int brightness);
  void setVolume(int volume);
  void setChatId(const String &chatId);
  void clearChatId();
  void reset();

private:
  Preferences _prefs;
  int _brightness = 80;
  int _volume = 255;
  String _chatId;
  bool _ready = false;

  static constexpr const char *kNamespace = "chat-stick";
  static constexpr const char *kBrightnessKey = "brightness";
  static constexpr const char *kVolumeKey = "volume";
  static constexpr const char *kChatIdKey = "chat_id";
};
