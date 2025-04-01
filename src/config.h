#ifndef CONFIG_H
#define CONFIG_H

// Set to 1 to show debug information on serial monitor
#define LCD_SHOW_DEBUG_INFO "1"

// Uncomment to enable detailed OpenTelemetry debugging
// #define OTEL_DEBUG_VERBOSE

// Set your local WiFi username and password. Please use a 2.4GHz access point for the M5 Stick
#define WIFI_SSID     "Morningwood-VLAN-24"
#define WIFI_PASSWORD "306Pineview"

// Set the device hostname for network identification
#define WIFI_HOSTNAME "M5StickC-OTEL"

// WiFi Configuration
#define WIFI_CONNECT_TIMEOUT 60000  // Timeout for initial connection (60 seconds)
#define WIFI_RETRY_DELAY    5000    // Time between reconnection attempts (5 seconds)
#define WIFI_STABILIZE_DELAY 5000  // Delay after WiFi connection (ms) to ensure stability
#define WIFI_REBOOT_ON_FAIL false    // Whether to reboot the device when WiFi connection fails

// OpenTelemetry Configuration
#define OTEL_SERVICE_NAME    "m5stick-sensor"
#define OTEL_SERVICE_VERSION "1.0.0"
#define OTEL_HOST           "192.168.1.81"
#define OTEL_PORT           "4318"
#define OTEL_PROTOCOL       "http"
#define OTEL_METRICS_ENDPOINT "/v1/metrics"
#define OTEL_TRACES_ENDPOINT  "/v1/traces"
#define OTEL_SEND_INTERVAL  30000  // Time between sending metrics (30 seconds)

// Construct the full OpenTelemetry URLs
#define OTEL_METRICS_URL    OTEL_PROTOCOL "://" OTEL_HOST ":" OTEL_PORT OTEL_METRICS_ENDPOINT
#define OTEL_TRACES_URL     OTEL_PROTOCOL "://" OTEL_HOST ":" OTEL_PORT OTEL_TRACES_ENDPOINT

// NTP Configuration
#define NTP_SERVER1         "pool.ntp.org"      // Primary NTP server
#define NTP_SERVER2         "time.google.com"   // Secondary NTP server (or empty string if not used)
#define NTP_SERVER3         "time.windows.com"  // Tertiary NTP server (or empty string if not used)
// NTP timeout settings
#define NTP_SYNC_TIMEOUT    10000   // Timeout for NTP sync in milliseconds (10 seconds)
#define NTP_MAX_RETRIES     3       // Maximum number of NTP sync retries before giving up

// Power Management Options
// Enable this to only apply power saving when on battery
#define ENABLE_POWER_SAVE_ON_BATTERY true
// If set to true, device will only enter light sleep and use WiFi power saving when on battery
// If false, power saving is always enabled regardless of charging status

// Tracing Configuration
#define ENABLE_TRACING_ON_BATTERY false  // Set to false to disable tracing when on battery
#define TRACE_FLUSH_INTERVAL 30000  // Flush traces every 30 seconds

#endif // CONFIG_H