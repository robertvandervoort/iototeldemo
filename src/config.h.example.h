#ifndef CONFIG_H
#define CONFIG_H

// Set to 1 to show debug information on serial monitor
#define LCD_SHOW_DEBUG_INFO "1"

// Set your local WiFi username and password. Please use a 2.4GHz access point for the M5 Stick
#define WIFI_SSID     "your ssid goes here"
#define WIFI_PASSWORD "your wifi password goes here"

// Set the device hostname for network identification
#define WIFI_HOSTNAME "M5StickC-OTEL"

// WiFi Configuration
#define WIFI_CONNECT_TIMEOUT 60000  // Timeout for initial connection (60 seconds)
#define WIFI_RETRY_DELAY    5000    // Time between reconnection attempts (5 seconds)
#define WIFI_STABILIZE_DELAY 5000  // Delay after WiFi connection (ms) to ensure stability

// OpenTelemetry Configuration
#define OTEL_SERVICE_NAME    "m5stick-sensor"
#define OTEL_SERVICE_VERSION "1.0.0"
#define OTEL_HOST           "192.168.1.80"
#define OTEL_PORT           4318
#define OTEL_PROTOCOL       "http"
#define OTEL_ENDPOINT       "/v1/metrics"
#define OTEL_SEND_INTERVAL  30000  // Time between sending metrics (30 seconds)

// Construct the full OpenTelemetry URL
#define OTEL_URL            OTEL_PROTOCOL "://" OTEL_HOST ":" STR(OTEL_PORT) OTEL_ENDPOINT

// NTP Configuration
#define NTP_SERVER1         "pool.ntp.org"      // Primary NTP server
#define NTP_SERVER2         "time.google.com"    // Backup NTP server
#define NTP_SERVER3         "time.windows.com"   // Second backup NTP server
#define GMT_OFFSET_SEC      0                   // UTC timezone offset in seconds (0 = GMT) -21600 = US Central
#define DAYLIGHT_OFFSET_SEC 0                    // Daylight savings time offset in seconds (0 if not used)

// Helper macro to convert numbers to strings
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#endif // CONFIG_H