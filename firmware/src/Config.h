#pragma once

#include <Arduino.h>

// ============= Server Configuration =============
// The dev and prod server addresses are per-user and live in credentials.h
// (gitignored). credentials.h defines: DEVELOPMENT_SERVER_ADDRESS,
// DEVELOPMENT_SERVER_PORT, PRODUCTION_SERVER_ADDRESS, SERVER_ENDPOINTS[],
// and SERVER_ENDPOINT_COUNT.
struct ServerEndpoint {
  const char *host;
  int port;
  const char *ca_cert;
};

// Google Trust Services root used by the deployed workers.dev endpoint.
// Source: https://pki.goog/ demo certificate chain for GTS Root R4.
constexpr const char *GTS_ROOT_R4_CA =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

constexpr const char *SERVER_PATH = "/ws";

// ============= WiFi Networks =============
struct WiFiNetwork {
  const char *ssid;
  const char *password;
  const char *label;
};

// WiFi credentials are in credentials.h (gitignored).
// Copy credentials.h.example to credentials.h and fill in your networks.
// credentials.h defines: WIFI_NETWORKS[] and WIFI_NETWORK_COUNT
constexpr int WIFI_CONNECT_TIMEOUT_SEC = 10;

// ============= Device =============
constexpr const char *DEVICE_ID = "m5s3-live";
constexpr int FIRMWARE_VERSION = 1;

// ============= Audio =============
constexpr int MIC_SAMPLE_RATE = 16000;  // 16 kHz input (Gemini Live API)
constexpr int MIC_CHUNK_MS = 100;       // Send a chunk every 100 ms
constexpr int PLAY_SAMPLE_RATE = 24000; // 24 kHz output (Gemini Live API)
constexpr int MAX_PLAYBACK_SEC = 30;    // Max response buffer

// ============= Display =============
constexpr int SCREEN_WIDTH_PX = 240;
constexpr int SCREEN_HEIGHT_PX = 135;
constexpr int DEFAULT_BRIGHTNESS = 80;  // lower = longer battery; plenty readable indoors
constexpr int DEFAULT_VOLUME = 255;

// ============= Hardware (M5StickS3) =============
// StickS3 button pin map from M5Stack docs: KEY1=G11, KEY2=G12.
constexpr gpio_num_t BUTTON_A_PIN = GPIO_NUM_11;
constexpr gpio_num_t BUTTON_B_PIN = GPIO_NUM_12;

// ============= Clock =============
constexpr const char *NTP_SERVER = "pool.ntp.org";
constexpr const char *LOCAL_TZ = "PST8PDT,M3.2.0,M11.1.0";

// ============= Power Management =============
constexpr unsigned long IDLE_DIM_MS = 60 * 1000;
constexpr unsigned long IDLE_SCREEN_OFF_MS = 2 * 60 * 1000;
constexpr unsigned long IDLE_LIGHT_SLEEP_MS = 5 * 60 * 1000;
constexpr unsigned long IDLE_POWER_OFF_MS = 10 * 60 * 1000;
constexpr unsigned long LIGHT_SLEEP_WAKE_INTERVAL_MS = 60 * 1000;
constexpr int BRIGHTNESS_DIM = 48;
constexpr int BRIGHTNESS_OFF = 0;
