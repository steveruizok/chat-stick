#pragma once

#include <Arduino.h>
#include <Preferences.h>

class SettingsStore {
public:
  void init();

  int brightness() const { return _brightness; }
  int volume() const { return _volume; }
  const String &chatId() const { return _chatId; }
  bool useExternalSpeaker() const { return _useExternalSpeaker; }
  int externalSpeakerGain() const { return _externalSpeakerGain; }
  const String &voice() const { return _voice; }

  void setBrightness(int brightness);
  void setVolume(int volume);
  void setChatId(const String &chatId);
  void clearChatId();
  void setUseExternalSpeaker(bool enabled);
  void setExternalSpeakerGain(int gain);
  void setVoice(const String &voice);
  void reset();

  static constexpr int kDefaultExternalGain = 24;
  static constexpr int kMinExternalGain = 1;
  static constexpr int kMaxExternalGain = 64;
  static constexpr const char *kDefaultVoice = "Aoede";

private:
  Preferences _prefs;
  int _brightness = 80;
  int _volume = 255;
  String _chatId;
  bool _useExternalSpeaker = false;
  int _externalSpeakerGain = kDefaultExternalGain;
  String _voice = kDefaultVoice;
  bool _ready = false;

  static constexpr const char *kNamespace = "chat-stick";
  static constexpr const char *kBrightnessKey = "brightness";
  static constexpr const char *kVolumeKey = "volume";
  static constexpr const char *kChatIdKey = "chat_id";
  static constexpr const char *kExternalSpeakerKey = "ext_spk";
  static constexpr const char *kExternalGainKey = "ext_gain";
  static constexpr const char *kVoiceKey = "voice";
};
