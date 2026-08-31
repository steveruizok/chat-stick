#pragma once

#include <Arduino.h>

// ============= Server Configuration =============
// The dev and prod server addresses are per-user and live in credentials.h
// (gitignored). credentials.h defines: DEVELOPMENT_SERVER_ADDRESS,
// DEVELOPMENT_SERVER_PORT, PRODUCTION_SERVER_ADDRESS, SERVER_ENDPOINTS[],
// and SERVER_ENDPOINT_COUNT.
/**
 * @brief TLS endpoint configuration for a chat server deployment.
 */
struct ServerEndpoint {
  /// Hostname of the server.
  const char *host;

  /// TCP port used for HTTPS or WebSocket connections.
  int port;

  /// PEM certificate authority bundle used to verify the endpoint.
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
/**
 * @brief Built-in WiFi credential entry compiled from credentials.h.
 */
struct WiFiNetwork {
  /// Network SSID.
  const char *ssid;

  /// Network password.
  const char *password;

  /// Human-readable label shown in logs and UI.
  const char *label;
};

// WiFi credentials are in credentials.h (gitignored).
// Copy credentials.h.example to credentials.h and fill in your networks.
// credentials.h defines: WIFI_NETWORKS[] and WIFI_NETWORK_COUNT
constexpr int WIFI_CONNECT_TIMEOUT_SEC = 10;
constexpr bool WIFI_DISABLE_PERSISTENT_STORAGE = false;
constexpr bool WIFI_DISABLE_SLEEP_DURING_CONNECT = false;
constexpr bool WIFI_SCAN_BEFORE_FALLBACK_CONNECT = false;
constexpr bool WIFI_USE_FAST_CONNECT_HINTS = false;
constexpr bool WIFI_SKIP_SAVED_CONFIGURED_DUPLICATES = false;
constexpr bool WIFI_LOG_CONNECT_DETAILS = true;
constexpr unsigned long WIFI_CONNECT_POLL_MS = 250;
// Cap radio TX power in quarter-dBm (44 = 11dBm, 78 = 19.5dBm full power) to
// tame battery current spikes; 0 disables the cap. Tried at 44 for the
// on-battery power-off issue: it did NOT fix it and made streamed audio very
// choppy (weak link -> retransmits), so it stays off.
constexpr int WIFI_MAX_TX_POWER_QDBM = 0;

// ============= Panel variant =============
// The Waveshare ESP32-S3-Touch-AMOLED-1.8 shipped with two different AMOLED
// driver ICs. Exactly one of these is defined by the PlatformIO env
// (see platformio.ini): WAVESHARE_PANEL_SH8601 (original) or
// WAVESHARE_PANEL_CO5300 (V2). The panel ID cannot be read back over the
// write-only QSPI bus, so the variant is chosen at build time.
#if defined(WAVESHARE_PANEL_CO5300) && defined(WAVESHARE_PANEL_SH8601)
#error "Define only one of WAVESHARE_PANEL_SH8601 / WAVESHARE_PANEL_CO5300"
#elif !defined(WAVESHARE_PANEL_CO5300) && !defined(WAVESHARE_PANEL_SH8601)
#error "Build with a waveshare-v1 or waveshare-v2 PlatformIO env (see platformio.ini)"
#endif

// ============= Device =============
// FIRMWARE_DEVICE is the OTA lineage: the worker serves
// chat-stick/firmware/<FIRMWARE_DEVICE>/firmware-v<N>.bin, so each panel
// variant needs its own name or a board would pull a binary built for the
// other controller.
#if defined(WAVESHARE_PANEL_CO5300)
constexpr const char *FIRMWARE_DEVICE = "waveshare-v2";
constexpr const char *PANEL_NAME = "CO5300";
#else
constexpr const char *FIRMWARE_DEVICE = "waveshare";
constexpr const char *PANEL_NAME = "SH8601";
#endif
constexpr const char *DEVICE_ID = "waveshare-amoled18-live";
constexpr int FIRMWARE_VERSION = 11;

// ============= Audio =============
constexpr int MIC_SAMPLE_RATE = 16000;  // 16 kHz input (Gemini Live API)
constexpr int MIC_CHUNK_MS = 100;       // Send a chunk every 100 ms
constexpr int PLAY_SAMPLE_RATE = 24000; // 24 kHz output (Gemini Live API)
constexpr int MAX_PLAYBACK_SEC = 30;    // Max response buffer

// Speaker volume calibration (measured 2026-08-30 with the waveshare-*-volcal
// harness): levels below 100 are inaudible on this board's speaker and levels
// above 230 are uncomfortably loud for speech. Volume levels keep the raw
// 0-255 scale, but setVolume() clamps nonzero requests into this window
// (0 stays mute); the raw maximum is reserved for alarm/alert tones via
// playAlarmMelody(). The server's set_volume tool description mirrors these
// numbers so "minimum volume" means 100 and "max volume" means 230.
constexpr int VOLUME_RAW_AUDIBLE_FLOOR = 100;
constexpr int VOLUME_RAW_SPEECH_CEILING = 230;
constexpr int VOLUME_RAW_ALERT = 255;

// ============= Display =============
constexpr int SCREEN_WIDTH_PX = 368;
constexpr int SCREEN_HEIGHT_PX = 448;

// Target pixel size for server-generated images. Waveshare fills the whole
// screen; the server picks the closest Imagen aspect ratio and dithers to this
// size before sending. Old saved images keep whatever dimensions they were
// generated at.
constexpr int IMAGE_TARGET_WIDTH = SCREEN_WIDTH_PX;
constexpr int IMAGE_TARGET_HEIGHT = SCREEN_HEIGHT_PX;
constexpr int DEFAULT_BRIGHTNESS =
    80; // lower = longer battery; plenty readable indoors
constexpr int DEFAULT_VOLUME = 200;
constexpr bool SHOW_BOOT_LOG_ON_DISPLAY = false;
constexpr bool SHOW_DEBUG_TEXT_ON_DISPLAY = false;

// Serial debug commands: 'D' dumps the framebuffer as hex rows, 'R' forces a
// full-screen repaint. For diagnosing panel/GRAM issues; harmless to leave on.
#define FRAMEBUFFER_DEBUG_COMMANDS 1

// ============= Hardware (Waveshare ESP32-S3-Touch-AMOLED-1.8) =============
constexpr int LCD_SDIO0_PIN = 4;
constexpr int LCD_SDIO1_PIN = 5;
constexpr int LCD_SDIO2_PIN = 6;
constexpr int LCD_SDIO3_PIN = 7;
constexpr int LCD_SCLK_PIN = 11;
constexpr int LCD_CS_PIN = 12;
// First visible column in the controller's address space. The CO5300 addresses
// 480 columns and the V2 panel's 368 visible pixels start at column 16; the
// SH8601 panel is addressed from column 0.
#if defined(WAVESHARE_PANEL_CO5300)
constexpr uint8_t LCD_PANEL_COL_OFFSET = 16;
#else
constexpr uint8_t LCD_PANEL_COL_OFFSET = 0;
#endif

constexpr int BOARD_I2C_SDA_PIN = 15;
constexpr int BOARD_I2C_SCL_PIN = 14;

constexpr gpio_num_t BUTTON_A_PIN = GPIO_NUM_0; // BOOT, active low
// PWR is read through AXP2101 IRQs. On this board AXP_IRQ is wired to the
// TCA9554 expander (EXIO5), not a wake-capable ESP32 GPIO.
constexpr gpio_num_t BUTTON_B_PIN = GPIO_NUM_NC;

constexpr int AUDIO_I2S_MCLK_PIN = 16;
constexpr int AUDIO_I2S_BCLK_PIN = 9;
constexpr int AUDIO_I2S_DIN_PIN = 10; // ES8311 ADC -> ESP32
constexpr int AUDIO_I2S_WS_PIN = 45;
constexpr int AUDIO_I2S_DOUT_PIN = 8; // ESP32 -> ES8311 DAC
constexpr int AUDIO_PA_ENABLE_PIN = 46;

// ============= Clock =============
constexpr const char *NTP_SERVER = "pool.ntp.org";
constexpr const char *LOCAL_TZ = "PST8PDT,M3.2.0,M11.1.0";

// ============= Timers / Alarms =============
constexpr int MAX_TIMERS = 4;
constexpr int TIMER_NAME_MAX_LEN = 24;
constexpr int TIMER_MIN_DURATION_SEC = 1;
constexpr int TIMER_MAX_DURATION_SEC = 24 * 60 * 60; // 24 hours
// `time(nullptr)` below this epoch is treated as "clock not yet synced",
// so timers can't be created or fired blindly. ~2024-01-01 UTC.
constexpr time_t TIMER_MIN_VALID_EPOCH = 1704067200;

// ============= Power Management =============
constexpr int CPU_ACTIVE_MHZ = 240;
constexpr int CPU_IDLE_MHZ = 80;
constexpr unsigned long IDLE_DIM_MS = 60 * 1000;
constexpr unsigned long IDLE_SCREEN_OFF_MS = 2 * 60 * 1000;
constexpr unsigned long IDLE_LIGHT_SLEEP_MS = 5 * 60 * 1000;
constexpr unsigned long IDLE_POWER_OFF_MS = 10 * 60 * 1000;
constexpr unsigned long LIGHT_SLEEP_WAKE_INTERVAL_MS = 250;
constexpr bool IDLE_POWER_OFF_WHILE_USB_CONNECTED = false;
constexpr bool IDLE_DEEP_SLEEP_ENABLED = false;
constexpr uint32_t IDLE_FULL_POWER_OFF_SEC = 0;
constexpr uint32_t IDLE_DEEP_SLEEP_SHUTDOWN_SEC = 0;
constexpr int BRIGHTNESS_DIM = 48;
constexpr int BRIGHTNESS_OFF = 0;
