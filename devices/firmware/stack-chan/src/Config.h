#pragma once

#include <Arduino.h>

namespace Config {

constexpr const char *kDeviceName = "Stack-Chan";
constexpr const char *kFirmwareDevice = "stack-chan";
constexpr int kFirmwareVersion = 1;

constexpr uint8_t kDisplayBrightness = 96;
constexpr uint8_t kSpeakerVolume = 96;
constexpr uint32_t kFrameIntervalMs = 16;
constexpr bool kBlinkEnabled = false;
constexpr uint32_t kBlinkIntervalMinMs = 2400;
constexpr uint32_t kBlinkIntervalMaxMs = 5200;
constexpr uint32_t kBlinkDurationMs = 130;

// The BSP expresses angles in tenths of a degree. Keep pitch inside the
// manufacturer's recommended 5-85 degree range; the physical end stops can
// stall and damage the vertical servo.
constexpr int kYawMinTenths = -1200;
constexpr int kYawMaxTenths = 1200;
constexpr int kPitchMinTenths = 50;
constexpr int kPitchMaxTenths = 850;
constexpr int kNeutralYawTenths = 0;
constexpr int kNeutralPitchTenths = 450;
constexpr int kDefaultMotionSpeed = 350;

// ============= Local control website =============
constexpr const char *kControlHostname = "stack-chan";
constexpr const char *kControlApSsid = "stack-chan-control";
constexpr const char *kControlApPassword = "stack-chan";
// Keep the robot from advertising a network that phones and laptops may
// automatically join. Set true only for deliberate, offline setup sessions.
constexpr bool kEnableControlAccessPoint = false;
// The Arduino camera driver and Wi-Fi currently compete for the same small
// internal-RAM pool on CoreS3. Leave video off until the native esp_video
// service is integrated; controls and telemetry remain stable this way.
#ifndef STACK_CHAN_ENABLE_CAMERA
#define STACK_CHAN_ENABLE_CAMERA 0
#endif
constexpr bool kEnableCamera = STACK_CHAN_ENABLE_CAMERA != 0;
constexpr uint16_t kControlHttpPort = 80;
constexpr uint16_t kCameraStreamPort = 81;
constexpr uint16_t kAudioHttpPort = 82;
constexpr uint32_t kWifiConnectTimeoutMs = 10000;

// Audio is intentionally half-duplex on CoreS3: its microphone and speaker
// share the codec path and cannot be active at the same time.
constexpr uint32_t kAudioSampleRate = 16000;
constexpr int kDefaultDeviceRecordingSeconds = 5;
constexpr int kMaxDeviceRecordingSeconds = 5;
constexpr int kMaxWebsiteRecordingSeconds = 10;

} // namespace Config
