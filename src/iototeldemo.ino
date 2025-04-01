#include <M5Unified.h>
#include <M5UnitENV.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include "debug.h"
#include "opentelemetry.h"
#include "config.h"

// Default watchdog timeout is 5 seconds
#ifndef WDT_TIMEOUT
#define WDT_TIMEOUT 10  // Extend timeout to 10 seconds for WiFi operations
#endif

// Constants for display and power management
#ifndef BATT_X
#define BATT_X 195
#endif
#ifndef BATT_Y
#define BATT_Y 2
#endif
#ifndef DISPLAY_TIMEOUT
#define DISPLAY_TIMEOUT 30000  // 30 seconds with no activity turns off display
#endif
#ifndef DEFAULT_SCREEN_BRIGHTNESS
#define DEFAULT_SCREEN_BRIGHTNESS 10  // Default screen brightness (0-15)
#endif

// Constants for WiFi connectivity
#ifndef WIFI_CONNECT_TIMEOUT
#define WIFI_CONNECT_TIMEOUT 15000  // 15 seconds timeout
#endif
#ifndef WIFI_STABILIZE_DELAY
#define WIFI_STABILIZE_DELAY 5000   // 5 second delay to let connection stabilize
#endif
#ifndef CONNECTION_TIMEOUT
#define CONNECTION_TIMEOUT WIFI_CONNECT_TIMEOUT // Alias for consistency
#endif
#ifndef WIFI_HOSTNAME
#define WIFI_HOSTNAME "M5Stack-IoT"
#endif
#ifndef WIFI_RETRY_DELAY
#define WIFI_RETRY_DELAY 5000  // 5 seconds delay between reconnection attempts
#endif

// Timing constants
#ifndef WIFI_CHECK_INTERVAL
#define WIFI_CHECK_INTERVAL 1000    // Check WiFi status every second
#endif
#ifndef OTEL_PING_INTERVAL
#define OTEL_PING_INTERVAL 30000    // Check OTel collector health every 30 seconds
#endif

// Button pin definitions for M5Stack
#ifndef BUTTON_A_PIN
#define BUTTON_A_PIN 39
#endif
#ifndef BUTTON_B_PIN
#define BUTTON_B_PIN 38
#endif
#ifndef BUTTON_C_PIN
#define BUTTON_C_PIN 37
#endif

// Global state variables
bool display_on = true;
unsigned long last_activity_time = 0;
unsigned long last_otel_send_time = 0;
bool otel_initialized = false;
bool has_sent_first_metrics = false;
String lastOtelError = "";
int last_rssi = 0;  // Track last WiFi signal strength

// OpenTelemetry instance
OpenTelemetry otel;

// Create instance of the ENV III sensor unit
SHT3X sht3x;  // Humidity sensor in the ENVIII module
QMP6988 qmp;  // Temp and pressure sensor in the ENV3 module

// Setup vars to receive sensor data and track connection
float temp = 0.0;
float hum = 0.0;
float pressure = 0.0;
int g_battery_level = 0;
float g_battery_voltage = 0.0;
bool g_is_charging = false;
int upload_fail_count = 0;
unsigned long last_wifi_reconnect = 0;
unsigned long last_wifi_check = 0;
bool wifi_was_connected = false;
unsigned long last_wifi_ping = 0;
bool wifi_ping_success = false;
unsigned long last_otel_send = 0;  // Track last time metrics were sent
unsigned long last_sensor_query = 0;  // Track last time sensors were queried
uint64_t sensor_reading_timestamp = 0; // Timestamp when sensors were last read

// Power management settings
unsigned long last_button_press = 0;  // Track last button press for display timeout

// Screen state management
enum ScreenState {
    MAIN_SCREEN,
    NETWORK_SCREEN,
    OTEL_SCREEN,
    MAX_SCREENS
};
ScreenState currentScreen = MAIN_SCREEN;

// Track previous values to avoid redundant display updates
float prev_temp = 0.0;
float prev_hum = 0.0;
float prev_pressure = 0.0;
bool prev_wifi_connected = false;
int prev_upload_fail_count = 0;
bool display_needs_full_refresh = true;

// Network configuration is defined in config.h

// Add a constant for trace flush interval - make it more frequent
#ifndef TRACE_FLUSH_INTERVAL
#define TRACE_FLUSH_INTERVAL 30000  // Flush traces every 30 seconds
#endif

// Add a periodic span debug log
#ifndef SPAN_DEBUG_INTERVAL
#define SPAN_DEBUG_INTERVAL 30000  // Log span stats every 30 seconds
#endif

// In the global variables section, add:
unsigned long last_trace_flush = 0;  // Track last time traces were flushed
unsigned long last_span_debug = 0;   // Track last time we logged span stats

// First, let's add a custom time provider function that uses the RTC
uint64_t getDeviceTimeNanos() {
    // Use high-resolution time if available
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ULL + (uint64_t)tv.tv_usec * 1000ULL;
}

// Function to configure power management settings for better battery life
void configurePowerManagement() {
    debugLog("Configuring power management for optimal battery life");
    
    // Set screen brightness to lower value to save power
    M5.Display.setBrightness(DEFAULT_SCREEN_BRIGHTNESS * 16); // Brightness 0-255

}

// Function to turn off the display to save power
void turnOffDisplay() {
    if (display_on) {
        debugLog("Turning off display to save power");
        M5.Display.setBrightness(0); // Turn off backlight
        display_on = false;
    }
}

// Function to turn on the display
void turnOnDisplay() {
    if (!display_on) {
        debugLog("Turning on display");
        M5.Display.setBrightness(DEFAULT_SCREEN_BRIGHTNESS * 16); // Brightness 0-255
        display_on = true;
        display_needs_full_refresh = true;
    }
    last_button_press = millis();
}

// Function to enter light sleep mode for a specified time
void enterLightSleep(uint32_t sleep_time_ms) {
    // Always feed watchdog before going to sleep
    esp_task_wdt_reset();
    
    debugLog("Entering light sleep for %u ms", sleep_time_ms);
    
    // Check if we should enable power saving
    bool enable_power_saving = shouldEnablePowerSaving();
    
    // Decision to disable WiFi completely is based on:
    // 1. Power saving enabled
    // 2. Metrics interval >= 60 seconds (for short intervals, keep WiFi connected)
    // 3. Sleep time > 5 seconds (for very short sleeps, not worth disconnecting)
    bool should_disable_wifi = enable_power_saving && 
                               (OTEL_SEND_INTERVAL >= 60000 && sleep_time_ms > 5000);
    
    // Get power state before sleep for comparison
    auto power = M5.Power;
    int pre_sleep_battery = power.getBatteryLevel();
    bool pre_sleep_charging = power.isCharging();
    
    if (should_disable_wifi) {
        // For longer sleep periods, completely disable WiFi to save maximum power
        debugLog("WiFi will be disconnected during sleep to save power");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    } else if (enable_power_saving) {
        // For shorter sleep periods, just use modem sleep to save some power
        // while keeping the connection
        debugLog("WiFi modem sleep enabled during light sleep");
        WiFi.setSleep(true);
    } else {
        // No power saving, keep WiFi fully active
        debugLog("WiFi kept fully active during sleep (power saving disabled)");
        WiFi.setSleep(false);
    }
    
    // Configure wake sources for light sleep
    esp_sleep_enable_timer_wakeup(sleep_time_ms * 1000); // Convert to microseconds
    
    // Enable button wake sources
    gpio_wakeup_enable((gpio_num_t)BUTTON_A_PIN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BUTTON_B_PIN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BUTTON_C_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    
    // Enter light sleep mode - execution stops here until wake
    esp_light_sleep_start();
    
    // Get wake reason
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    String reason_str;
    
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_TIMER:
            reason_str = "timer";
            break;
        case ESP_SLEEP_WAKEUP_GPIO:
            reason_str = "button";
            break;
        default:
            reason_str = "other";
            break;
    }
    
    // Get power state after sleep for comparison
    int post_sleep_battery = power.getBatteryLevel();
    bool post_sleep_charging = power.isCharging();
    int battery_change = post_sleep_battery - pre_sleep_battery;
    
    debugLog("Woke up from light sleep (reason: %s, battery: %d%% -> %d%%, change: %d%%)", 
             reason_str.c_str(), pre_sleep_battery, post_sleep_battery, battery_change);
    
    // Always feed watchdog right after waking
    esp_task_wdt_reset();
    
    // Handle WiFi reconnection if needed
    if (should_disable_wifi) {
        debugLog("Restoring WiFi after sleep");
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        
        // Set a short timeout for initial reconnection
        unsigned long start_time = millis();
        bool reconnected = false;
        
        // Try for WIFI_RETRY_DELAY seconds to reconnect after wake
        while (millis() - start_time < WIFI_RETRY_DELAY) {
            if (WiFi.status() == WL_CONNECTED) {
                reconnected = true;
                break;
            }
            esp_task_wdt_reset();
            delay(1000);
        }
        
        if (reconnected) {
            debugLog("WiFi successfully reconnected after sleep in %llu ms", millis() - start_time);
        } else {
            debugLog("Initial WiFi reconnection failed after sleep");
        }
    } else {
        // For modem sleep mode, check if the connection was maintained
        if (WiFi.status() == WL_CONNECTED) {
            debugLog("WiFi connection maintained during sleep");
            // Explicitly wake up the WiFi modem from sleep mode
            WiFi.setSleep(false);
            debugLog("WiFi modem woken up from sleep mode");
        } else {
            debugLog("WiFi connection lost during sleep despite modem sleep mode");
            // Try to reconnect since connection was lost
            WiFi.setSleep(false);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            
            unsigned long start_time = millis();
            bool reconnected = false;
            
            // Try for 5 seconds to reconnect
            while (millis() - start_time < 5000) {
                if (WiFi.status() == WL_CONNECTED) {
                    reconnected = true;
                    break;
                }
                esp_task_wdt_reset();
                delay(500);
            }
            
            if (reconnected) {
                debugLog("WiFi successfully reconnected after modem sleep in %llu ms", millis() - start_time);
            } else {
                debugLog("WiFi reconnection failed after modem sleep");
            }
        }
    }
    
    // If we woke up due to a button press, turn on the display
    if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO && !display_on) {
        turnOnDisplay();
    }
}

// Function to display status with color
void displayStatus(const char* label, bool isOk, const char* message) {
    static String lastLabels[5];
    static bool lastStates[5];
    static String lastMessages[5];
    static int statusCount = 0;
    
    // Find if we've seen this label before
    int labelIndex = -1;
    for (int i = 0; i < statusCount; i++) {
        if (lastLabels[i] == label) {
            labelIndex = i;
            break;
        }
    }
    
    // If it's a new label or the status changed, log it
    bool statusChanged = false;
    if (labelIndex == -1) {
        // New label we haven't seen before
        if (statusCount < 5) {  // Prevent array overflow
            labelIndex = statusCount;
            lastLabels[labelIndex] = label;
            lastStates[labelIndex] = !isOk;  // Force it to be different to trigger logging
            lastMessages[labelIndex] = "";
            statusCount++;
            statusChanged = true;
        }
    } else if (lastStates[labelIndex] != isOk || lastMessages[labelIndex] != message) {
        // Status changed for existing label
        statusChanged = true;
    }
    
    // Update display regardless of change
    M5.Display.setTextColor(WHITE);
    M5.Display.printf("  %s: ", label);
    M5.Display.setTextColor(isOk ? GREEN : RED);
    M5.Display.printf("%s\n", message);
    M5.Display.setTextColor(WHITE);
    
    // But only log if the status changed
    if (statusChanged) {
        debugLog("%s Status: %s (%s)", label, isOk ? "OK" : "FAIL", message);
        lastStates[labelIndex] = isOk;
        lastMessages[labelIndex] = message;
    }
}

// Function to display the main sensor readings screen
void displayMainScreen() {
    bool wifi_connected = (WiFi.status() == WL_CONNECTED);
    
    // Only do a full redraw if this is the first time or if the screen was changed
    if (display_needs_full_refresh) {
        M5.Display.fillRect(0, 30, 240, 135, BLACK);  // Clear the display area
        display_needs_full_refresh = false;
    }
    
    // Only update temperature if it changed
    if (abs(temp - prev_temp) >= 0.1) {
        M5.Display.fillRect(0, 40, 240, 15, BLACK);  // Clear just the temperature line
        M5.Display.setCursor(0, 40);
        M5.Display.printf("  Temp: %2.1f  \r\n", temp);
        prev_temp = temp;
    }
    
    // Only update humidity if it changed
    if (abs(hum - prev_hum) >= 1.0) {
        M5.Display.fillRect(0, 55, 240, 15, BLACK);  // Clear just the humidity line
        M5.Display.setCursor(0, 55);
        M5.Display.printf("  Humi: %2.0f%%  \r\n", hum);
        prev_hum = hum;
    }
    
    // Only update pressure if it changed
    if (abs(pressure/100 - prev_pressure/100) >= 1.0) {
        M5.Display.fillRect(0, 70, 240, 15, BLACK);  // Clear just the pressure line
        M5.Display.setCursor(0, 70);
        M5.Display.printf("  Pressure:%2.0f hPa\r\n", pressure / 100);
        prev_pressure = pressure;
    }
    
    // Calculate and display time until next reading
    static unsigned long prev_time_to_next = 0;
    unsigned long time_to_next = 0;
    
    if (millis() > last_otel_send) {
        time_to_next = (last_otel_send + OTEL_SEND_INTERVAL - millis()) / 1000;
    }
    
    // Update every 5 seconds
    if (abs((long)(time_to_next - prev_time_to_next)) >= 5 || display_needs_full_refresh) {
        M5.Display.fillRect(0, 85, 240, 15, BLACK);
        M5.Display.setCursor(0, 85);
        M5.Display.setTextColor(YELLOW);
        M5.Display.printf("  Next in: %ds\r\n", time_to_next);
        M5.Display.setTextColor(WHITE);
        prev_time_to_next = time_to_next;
    }
    
    // Only update WiFi status if it changed
    if (wifi_connected != prev_wifi_connected) {
        M5.Display.fillRect(0, 100, 240, 15, BLACK);  // Clear the line
        M5.Display.setCursor(0, 100);
        displayStatus("WiFi", wifi_connected, wifi_connected ? "Connected" : "Disconnected");
        prev_wifi_connected = wifi_connected;
    }
    
    // Only update OTel status if it changed
    if (upload_fail_count != prev_upload_fail_count || wifi_connected != prev_wifi_connected) {
        M5.Display.fillRect(0, 115, 240, 15, BLACK);  // Clear the line
        M5.Display.setCursor(0, 115);
        char otel_status[32];
        if (!wifi_connected) {
            snprintf(otel_status, sizeof(otel_status), "No WiFi");
        } else if (upload_fail_count > 0) {
            snprintf(otel_status, sizeof(otel_status), "Failed (%d)", upload_fail_count);
        } else {
            snprintf(otel_status, sizeof(otel_status), "OK");
        }
        displayStatus("OTel", upload_fail_count == 0 && wifi_connected, otel_status);
        prev_upload_fail_count = upload_fail_count;
    }
    
    // Display battery status on last line
    static int prev_battery_level = -1;
    static bool prev_charging_status = !g_is_charging;
    
    if (prev_battery_level != g_battery_level || prev_charging_status != g_is_charging || display_needs_full_refresh) {
        M5.Display.fillRect(0, 130, 240, 15, BLACK);  // Clear the line
        M5.Display.setCursor(0, 130);
        char batt_status[32];
        snprintf(batt_status, sizeof(batt_status), "%d%% %.1fV %s", 
                 g_battery_level, g_battery_voltage/1000.0, 
                 g_is_charging ? "(Charging)" : "");
        
        M5.Display.setTextColor(g_battery_level > 20 ? GREEN : RED);
        M5.Display.printf("  Batt: %s\r\n", batt_status);
        M5.Display.setTextColor(WHITE);
        
        prev_battery_level = g_battery_level;
        prev_charging_status = g_is_charging;
    }
}

// Function to display Network information screen
void displayNetworkScreen() {
    if (display_needs_full_refresh) {
        M5.Display.fillScreen(BLACK);
        display_needs_full_refresh = false;
    }
    
    M5.Display.setCursor(0, 0);
    M5.Display.setTextColor(BLUE, BLACK);
    M5.Display.printf("== Network Info ==");
    
    M5.Display.setCursor(0, 15);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.printf("SSID: %s", WIFI_SSID);
    
    M5.Display.setCursor(0, 30);
    
    // Color code based on connection status
    if (WiFi.status() == WL_CONNECTED) {
        M5.Display.setTextColor(GREEN, BLACK);
        M5.Display.printf("Status: Connected");
        
        M5.Display.setCursor(0, 45);
        M5.Display.setTextColor(WHITE, BLACK);
        M5.Display.printf("IP: %s", WiFi.localIP().toString().c_str());
        
        M5.Display.setCursor(0, 60);
        
        // Color code RSSI based on signal strength
        int rssi = WiFi.RSSI();
        if (rssi > -70) { // Good signal
            M5.Display.setTextColor(GREEN, BLACK);
        } else if (rssi > -85) { // Medium signal
            M5.Display.setTextColor(YELLOW, BLACK);
        } else { // Poor signal
            M5.Display.setTextColor(RED, BLACK);
        }
        M5.Display.printf("RSSI: %d dBm", rssi);
        
        debugLog("Network screen updated: Connected, RSSI: %d", rssi);
    } else {
        M5.Display.setTextColor(RED, BLACK);
        M5.Display.printf("Status: Disconnected");
        M5.Display.setCursor(0, 45);
        M5.Display.printf("Error code: %d", WiFi.status());
        
        debugLog("Network screen updated: Disconnected, code: %d", WiFi.status());
    }
    
    // Show health check status
    M5.Display.setCursor(0, 75);
    if (wifi_ping_success) {
        M5.Display.setTextColor(GREEN, BLACK);
        M5.Display.printf("OTEL Health: OK");
    } else {
        M5.Display.setTextColor(RED, BLACK);
        M5.Display.printf("OTEL Health: Fail");
    }
    
    // Bottom info bar with navigation
    M5.Display.setCursor(0, 120);
    M5.Display.setTextColor(BLUE, BLACK);
    M5.Display.printf("[A:Back] [B:Next]");
}

// Function to display OpenTelemetry details
void displayOtelScreen() {
    static int prev_otel_fail_count = -1;
    static String prev_error_message = "";
    
    // Only clear and redraw header if it's a full refresh
    if (display_needs_full_refresh) {
        M5.Display.fillRect(0, 30, 240, 135, BLACK);
        M5.Display.setCursor(0, 40);
        M5.Display.setTextColor(BLUE);
        M5.Display.printf("OTEL Status:\n");
        M5.Display.setTextColor(WHITE);
        
        M5.Display.printf("  Metrics:\n  %s\n", OTEL_METRICS_URL);
        M5.Display.printf("  Traces:\n  %s\n", OTEL_TRACES_URL);
        M5.Display.printf("  Service: %s\n", OTEL_SERVICE_NAME);
        
        // Show sensor interval
        M5.Display.printf("  Sample: %ds\n", OTEL_SEND_INTERVAL / 1000);
        
        display_needs_full_refresh = false;
        prev_otel_fail_count = -1;  // Force status update
    }
    
    // Calculate and display time until next reading
    static unsigned long prev_time_to_next = 0;
    unsigned long time_to_next = 0;
    
    if (millis() > last_otel_send) {
        time_to_next = (last_otel_send + OTEL_SEND_INTERVAL - millis()) / 1000;
    }
    
    // Update every 5 seconds
    if (abs((long)(time_to_next - prev_time_to_next)) >= 5 || display_needs_full_refresh) {
        M5.Display.fillRect(0, 100, 160, 15, BLACK);
        M5.Display.setCursor(0, 100);
        M5.Display.setTextColor(YELLOW);
        M5.Display.printf("  Next in: %ds", time_to_next);
        M5.Display.setTextColor(WHITE);
        prev_time_to_next = time_to_next;
    }
    
    // Only update the status section if values changed
    if (prev_otel_fail_count != upload_fail_count || 
        prev_error_message != lastOtelError) {
        
        // Clear just the status area
        M5.Display.fillRect(0, 115, 240, 50, BLACK);
        M5.Display.setCursor(0, 115);
        
        if (upload_fail_count > 0) {
            M5.Display.setTextColor(RED);
            M5.Display.printf("  Failed uploads: %d\n", upload_fail_count);
            M5.Display.printf("  Last error:\n  %s", lastOtelError.c_str());
        } else {
            M5.Display.setTextColor(GREEN);
            M5.Display.printf("  Status: OK\n");
            M5.Display.printf("  Metrics sending\n");
        }
        
        prev_otel_fail_count = upload_fail_count;
        prev_error_message = lastOtelError;
    }
}

// Function to handle button presses
void handleButtons() {
    if (M5.BtnA.wasPressed()) {
        // Handle Button A press (navigate screens, back, etc.)
        last_button_press = millis();
        if (!display_on) {
            turnOnDisplay();
            debugLog("Button A pressed: turning on display");
        } else {
            // Navigate to previous screen
            currentScreen = (ScreenState)((currentScreen + MAX_SCREENS - 1) % MAX_SCREENS);
            display_needs_full_refresh = true;
            debugLog("Button A pressed: navigated to screen %d", currentScreen);
        }
    }
    
    if (M5.BtnB.wasPressed()) {
        // Handle Button B press (navigate screens, next, etc.)
        last_button_press = millis();
        if (!display_on) {
            turnOnDisplay();
            debugLog("Button B pressed: turning on display");
        } else {
            // Navigate to next screen
            currentScreen = (ScreenState)((currentScreen + 1) % MAX_SCREENS);
            display_needs_full_refresh = true;
            debugLog("Button B pressed: navigated to screen %d", currentScreen);
        }
    }
}

// Function to perform a OTel collector health API to verify network connectivity
bool pingTest() {
    debugLog("Checking OTel Collector Health to verify network connectivity");
    HTTPClient http;
    http.setTimeout(5000);
    
    // The health endpoint of the OpenTelemetry collector
    String healthUrl = "http://" + String(OTEL_HOST) + ":13133";
    http.begin(healthUrl);
    
    int httpCode = http.GET();
    bool success = false;
    
    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            String response = http.getString();
            debugLog("Health check successful: %s", response.c_str());
            success = true;
        } else {
            debugLog("Health check returned non-OK status: %d", httpCode);
        }
    } else {
        debugLog("Health check failed: %s", http.errorToString(httpCode).c_str());
    }
    
    http.end();
    return success;
}

// Function to check WiFi status and handle state changes
bool checkWiFiConnection() {
    bool currently_connected = (WiFi.status() == WL_CONNECTED);
    
    // Log detailed WiFi status for debugging
    if (currently_connected) {
        int current_rssi = WiFi.RSSI();
        if (current_rssi != last_rssi) {
            debugLog("WiFi RSSI changed: %d dBm", current_rssi);
            last_rssi = current_rssi;
        }
    }

    // Only perform ping test if WiFi shows connected
    if (currently_connected && (millis() - last_wifi_ping > OTEL_PING_INTERVAL)) {
        // Enable full WiFi power before performing health check
        WiFi.setSleep(false);
        debugLog("Performing OTEL health check...");
        wifi_ping_success = pingTest();
        last_wifi_ping = millis();
        
        // Check if power saving should be enabled
        bool enable_power_saving = shouldEnablePowerSaving();
        
        // Re-enable modem sleep to save power only if:
        // 1. Power saving is enabled
        // 2. Metrics interval is >= 60 seconds
        if (enable_power_saving && OTEL_SEND_INTERVAL >= 60000) {
            WiFi.setSleep(true);
            debugLog("WiFi returned to sleep mode after health check (power saving enabled)");
        } else {
            if (!enable_power_saving) {
                debugLog("WiFi kept active after health check (power saving disabled)");
            } else {
                debugLog("WiFi kept active after health check (metrics interval < 60s)");
            }
        }
        
        if (!wifi_ping_success) {
            debugLog("OTEL health check failed while WiFi connected (status: %d)", WiFi.status());
        }
    }
    
    // Consider connection lost only if:
    // 1. WiFi is disconnected OR
    // 2. WiFi is connected but we've failed multiple ping tests
    static int consecutive_ping_failures = 0;
    if (!currently_connected) {
        consecutive_ping_failures = 0;  // Reset counter when WiFi is disconnected
    } else if (!wifi_ping_success) {
        consecutive_ping_failures++;
        debugLog("Consecutive ping failures: %d", consecutive_ping_failures);
    } else {
        consecutive_ping_failures = 0;  // Reset on successful ping
    }
    
    bool connection_lost = !currently_connected || (consecutive_ping_failures >= 3);
    
    // Check if state changed
    if (connection_lost != !wifi_was_connected) {
        if (!connection_lost) {
            debugLog("WiFi connected! IP: %s, RSSI: %d dBm", 
                    WiFi.localIP().toString().c_str(), 
                    WiFi.RSSI());
            // Enable modem sleep to save power when connected but idle, only if metrics interval is >= 60 seconds
            if (OTEL_SEND_INTERVAL >= 60000) {
                WiFi.setSleep(true);
                debugLog("WiFi set to sleep mode (metrics interval >= 60s)");
            } else {
                WiFi.setSleep(false);
                debugLog("WiFi kept active (metrics interval < 60s)");
            }
        } else {
            debugLog("WiFi connection lost! Details:");
            debugLog("- WiFi Status: %d", WiFi.status());
            debugLog("- IP Address: %s", WiFi.localIP().toString().c_str());
            debugLog("- RSSI: %d dBm", WiFi.RSSI());
            debugLog("- Ping Success: %s", wifi_ping_success ? "Yes" : "No");
            debugLog("- Consecutive Failures: %d", consecutive_ping_failures);
        }
        wifi_was_connected = !connection_lost;
    }
    
    // Handle reconnection if needed
    if (connection_lost) {
        if (millis() - last_wifi_reconnect > WIFI_RETRY_DELAY) {
            debugLog("Attempting WiFi reconnection...");
            WiFi.setSleep(false); // Full power for reconnection
            WiFi.disconnect(true);  // Disconnect with clearing settings
            delay(1000);  // Give it time to disconnect properly
            WiFi.mode(WIFI_STA);    // Ensure we're in station mode
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            last_wifi_reconnect = millis();
            wifi_ping_success = false;  // Reset ping status on reconnect
            consecutive_ping_failures = 0;  // Reset failure counter
            
            // Set a timeout for the reconnection
            unsigned long reconnect_start = millis();
            bool reconnected = false;
            
            while (millis() - reconnect_start < 5000) {
                if (WiFi.status() == WL_CONNECTED) {
                    reconnected = true;
                    break;
                }
                esp_task_wdt_reset();
                delay(500);
            }
            
            if (reconnected) {
                debugLog("WiFi reconnection successful");
            } else {
                debugLog("WiFi reconnection attempt failed");
            }
        }
    }
    
    return !connection_lost;
}

// Function to establish and verify WiFi connection
bool establishWiFiConnection() {
    debugLog("WiFi connection attempt started");
    
    if (!WiFi.isConnected()) {
        // Set WiFi power save mode off for more reliable connection
        WiFi.setSleep(false);
        
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        
        unsigned long startTime = millis();
        bool connectionSuccess = false;
        
        // Try to connect for up to CONNECTION_TIMEOUT ms
        while (millis() - startTime < CONNECTION_TIMEOUT) {
            if (WiFi.status() == WL_CONNECTED) {
                connectionSuccess = true;
                break;
            }
            
            // Feed watchdog during connection attempts
            esp_task_wdt_reset();
            
            // Short delay between connection attempts
            delay(500);
        }
        
        if (connectionSuccess) {
            int rssi = WiFi.RSSI();
            unsigned long connectionTime = millis() - startTime;
            debugLog("WiFi connected - IP: %s, RSSI: %d dBm, Time: %lu ms", 
                    WiFi.localIP().toString().c_str(), 
                    rssi,
                    connectionTime);
            return connectionSuccess;
        } else {
            unsigned long connectionAttemptTime = millis() - startTime;
            debugLog("WiFi connection failed after %lu ms", connectionAttemptTime);
            return connectionSuccess;
        }
    } else {
        debugLog("WiFi already connected");
        return true;
    }
}

// Function to configure and sync NTP
bool setupNTP() {
    unsigned long startTime = millis();
    
    if (WiFi.status() != WL_CONNECTED) {
        debugLog("Cannot setup NTP - WiFi not connected");
        return false;
    }

    // Additional check for valid IP address
    if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
        debugLog("Cannot setup NTP - No valid IP address assigned");
        return false;
    }

    debugLog("Performing NTP sync...");
    
    M5.Display.fillRect(0, 30, 240, 90, BLACK);
    M5.Display.setCursor(10, 30);
    M5.Display.setTextColor(YELLOW);
    M5.Display.printf("NTP Sync...");
    
    // Log NTP servers being used
    debugLog("Using NTP servers: %s, %s, %s", 
            NTP_SERVER1, 
            strlen(NTP_SERVER2) > 0 ? NTP_SERVER2 : "none", 
            strlen(NTP_SERVER3) > 0 ? NTP_SERVER3 : "none");
    
    // Configure time with NTP servers - using UTC (no timezone/DST offset)
    if (strlen(NTP_SERVER2) > 0 && strlen(NTP_SERVER3) > 0) {
        configTime(0, 0, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
    } else if (strlen(NTP_SERVER2) > 0) {
        configTime(0, 0, NTP_SERVER1, NTP_SERVER2);
    } else {
        configTime(0, 0, NTP_SERVER1);
    }
    
    // Try NTP sync with retries
    for (int retry = 0; retry < NTP_MAX_RETRIES; retry++) {
        if (retry > 0) {
            debugLog("NTP sync retry %d of %d", retry + 1, NTP_MAX_RETRIES);
            M5.Display.setCursor(10, 50);
            M5.Display.printf("Retry %d/%d", retry + 1, NTP_MAX_RETRIES);
        }
        
        // Wait for NTP sync with timeout
        unsigned long startAttempt = millis();
        time_t now;
        
        while (millis() - startAttempt < NTP_SYNC_TIMEOUT) {
            time(&now);
            if (now > 1600000000) { // Time is after Sept 2020, probably valid
                unsigned long syncTime = millis() - startAttempt;
                unsigned long totalTime = millis() - startTime;
                debugLog("NTP sync successful. Current time: %lu UTC (took %lu ms, total %lu ms with %d attempts)", 
                        now, syncTime, totalTime, retry + 1);
                
                // Wait for the next second to ensure accurate synchronization
                time_t next_second = now + 1;
                while (time(nullptr) < next_second) {
                    delay(10);
                }
                
                // Get the current time again after synchronization
                time_t synced_time = time(nullptr);
                
                // Set the RTC to the synchronized time (if RTC is available)
                if (M5.Rtc.isEnabled()) {
                    // Convert time_t to tm structure in UTC
                    struct tm timeinfo;
                    gmtime_r(&synced_time, &timeinfo);
                    
                    // Set the RTC with the synchronized time
                    M5.Rtc.setDateTime(&timeinfo);
                    debugLog("Time synchronized and saved to RTC hardware");
                }
                
                M5.Display.fillRect(0, 30, 240, 90, BLACK);
                M5.Display.setCursor(10, 30);
                M5.Display.setTextColor(GREEN);
                M5.Display.printf("Time Synced");
                delay(1000);
                return true;
            }
            delay(100);
        }
        
        // If we reached the last retry, log failure
        if (retry == NTP_MAX_RETRIES - 1) {
            unsigned long totalTime = millis() - startTime;
            debugLog("NTP sync failed after %d attempts (total time: %lu ms)", 
                    NTP_MAX_RETRIES, totalTime);
            M5.Display.fillRect(0, 30, 240, 90, BLACK);
            M5.Display.setCursor(10, 30);
            M5.Display.setTextColor(RED);
            M5.Display.printf("NTP Failed");
            return false;
        }
    }
    
    return false; // Should never reach here due to the retry loop
}

// Function to verify OpenTelemetry collector health
bool verifyOtelHealth() {
    debugLog("Verifying OpenTelemetry collector health...");
    
    // Feed watchdog before potentially slow network operation
    esp_task_wdt_reset();
    
    // Try to ping the Collector health check endpoint
    bool success = pingTest();
    
    if (!success) {
        debugLog("OpenTelemetry collector health check failed");
    } else {
        debugLog("OpenTelemetry collector health check succeeded");
    }
    
    return success;
}

// Main connection management function
bool ensureConnectivity() {
    while (true) {
        // First establish WiFi connection
        if (!establishWiFiConnection()) {
            continue;  // Keep trying WiFi connection
        }
        
        // Once WiFi is stable, setup NTP
        if (!setupNTP()) {
            debugLog("NTP setup failed, restarting connection process");
            continue;  // Go back to establishing WiFi
        }
        
        // Only try OTel health check after WiFi and NTP are ready
        if (verifyOtelHealth()) {
            return true;  // Everything is good
        }
        
        // If OTel check failed, verify WiFi is still connected
        if (WiFi.status() != WL_CONNECTED) {
            debugLog("WiFi disconnected during OTel check");
            continue;  // Go back to establishing WiFi
        }
    }
}

// Function to query all sensors and update readings
void querySensors() {
    unsigned long startTime = millis();
    debugLog("Querying sensors for fresh readings");
    last_sensor_query = millis();
    
    // Capture timestamp when sensor data is collected (in nanoseconds)
    time_t now;
    
    // Use RTC if available, otherwise fall back to ESP32 time
    bool using_rtc = false;
    if (M5.Rtc.isEnabled()) {
        // Get time from RTC hardware which is more accurate
        auto dt = M5.Rtc.getDateTime();
        
        // Convert RTC datetime to time_t format
        struct tm timeinfo;
        timeinfo.tm_year = dt.date.year - 1900;
        timeinfo.tm_mon = dt.date.month - 1;
        timeinfo.tm_mday = dt.date.date;
        timeinfo.tm_hour = dt.time.hours;
        timeinfo.tm_min = dt.time.minutes;
        timeinfo.tm_sec = dt.time.seconds;
        timeinfo.tm_isdst = 0; // No DST
        
        now = mktime(&timeinfo);
        using_rtc = true;
        debugLog("Using RTC hardware for timestamp");
    } else {
        // Fallback to ESP32 internal time
        debugLog("RTC hardware not detected, using ESP32 internal time");
        time(&now);
    }
    
    sensor_reading_timestamp = (uint64_t)now * 1000000000ULL;
    
    // Track sensor readings success
    bool pressure_success = false;
    bool temphum_success = false;
    
    // Get pressure reading - measure time taken
    unsigned long pressure_start = millis();
    if (qmp.update()) {
        unsigned long pressure_time = millis() - pressure_start;
        pressure = qmp.pressure;
        pressure_success = true;
        debugLog("Pressure reading: %.2f hPa (took %lu ms)", pressure / 100, pressure_time);
    } else {
        unsigned long pressure_time = millis() - pressure_start;
        debugLog("Failed to read QMP6988 sensor (after %lu ms)", pressure_time);
        pressure_success = false;
    }

    // Get temperature and humidity readings - measure time taken
    unsigned long temphum_start = millis();
    if (sht3x.update()) {
        unsigned long temphum_time = millis() - temphum_start;
        temp = sht3x.cTemp;
        hum = sht3x.humidity;
        temphum_success = true;
        debugLog("Temperature: %.2f°C, Humidity: %.2f%% (took %lu ms)", temp, hum, temphum_time);
    } else {
        unsigned long temphum_time = millis() - temphum_start;
        debugLog("Failed to read SHT3X sensor (after %lu ms)", temphum_time);
        temp = 0;
        hum = 0;
        temphum_success = false;
    }

    // Gather battery and power information
    unsigned long battery_start = millis();
    auto power = M5.Power;
    int battery_level = power.getBatteryLevel();
    float battery_voltage = power.getBatteryVoltage();
    bool is_charging = power.isCharging();
    unsigned long battery_time = millis() - battery_start;
    
    // Store these as global variables for metrics and display
    g_battery_level = battery_level;
    g_battery_voltage = battery_voltage;
    g_is_charging = is_charging;
    
    debugLog("Battery: %d%%, %.2fV, Charging: %s (took %lu ms)", 
             battery_level, 
             battery_voltage/1000.0, // convert to volts for display
             is_charging ? "Yes" : "No",
             battery_time);
    
    unsigned long total_time = millis() - startTime;
    debugLog("Total sensor query time: %lu ms", total_time);
    
    // If tracing is enabled, these attributes will be added to the span by the caller
    if (shouldEnableTracing()) {
        // Add overall sensor status to the main span
        otel.addSpanAttribute(sensor_reading_timestamp, "pressure_success", pressure_success ? "true" : "false");
        otel.addSpanAttribute(sensor_reading_timestamp, "temphum_success", temphum_success ? "true" : "false");
        otel.addSpanAttribute(sensor_reading_timestamp, "using_rtc", using_rtc ? "true" : "false");
        otel.addSpanAttribute(sensor_reading_timestamp, "total_time_ms", (double)total_time);
        otel.addSpanAttribute(sensor_reading_timestamp, "battery_level", (double)battery_level);
        otel.addSpanAttribute(sensor_reading_timestamp, "is_charging", is_charging ? "true" : "false");
    }
}

// Function to send OpenTelemetry metrics
bool sendOtelMetrics() {
    // Create a span for the entire send operation
    uint64_t spanId = otel.startSpan("send_otel_metrics");
    
    // Get current time for metrics
    time_t now;
    time(&now);
    uint64_t current_time_nanos = (uint64_t)now * 1000000000ULL;
    
    // Add current battery level and connection info to metrics
    otel.addMetric("device.battery_level", M5.Power.getBatteryLevel(), current_time_nanos);
    otel.addMetric("device.is_charging", M5.Power.isCharging() ? 1.0 : 0.0, current_time_nanos);
    
    if (WiFi.isConnected()) {
        otel.addMetric("network.rssi", WiFi.RSSI(), current_time_nanos);
        if (spanId != 0) {
            otel.addSpanAttribute(spanId, "wifi.connected", "true");
            otel.addSpanAttribute(spanId, "wifi.rssi", (double)WiFi.RSSI());
        }
    } else {
        // Don't even try to send if we're not connected
        debugLog("Cannot send metrics, WiFi not connected");
        if (spanId != 0) {
            otel.addSpanAttribute(spanId, "error", "wifi_not_connected");
            otel.endSpan(spanId);
        }
        return false;
    }
    
    // Create a child span for the actual sending
    uint64_t metricSendSpanId = 0;
    if (spanId != 0) {
        metricSendSpanId = otel.startSpan("metric_send", spanId);
    }
    
    // Send metrics and get the result
    bool success = otel.sendMetrics();
    
    if (success) {
        debugLog("OpenTelemetry metrics sent successfully");
        if (metricSendSpanId != 0) {
            otel.addSpanAttribute(metricSendSpanId, "success", "true");
        }
    } else {
        lastOtelError = otel.getLastError();
        debugLog("Failed to send OpenTelemetry metrics: %s", lastOtelError.c_str());
        
        const char* errorMsg = "send_failed";
        if (metricSendSpanId != 0) {
            otel.addSpanAttribute(metricSendSpanId, "success", "false");
            otel.addSpanAttribute(metricSendSpanId, "error_message", errorMsg);
        }
    }
    
    // End the metric send span
    if (metricSendSpanId != 0) {
        otel.endSpan(metricSendSpanId);
    }
    
    // End the main send metrics span
    if (spanId != 0) {
        otel.endSpan(spanId);
    }
    
    return success;
}

// Function to check if power saving should be enabled
bool shouldEnablePowerSaving() {
    // Static variables to track previous state
    static bool prev_on_battery = false;
    static int prev_battery_level = -1;
    static bool prev_is_charging = false;
    static unsigned long last_state_change = 0;
    
    // If the feature is disabled, always use power saving
    if (!ENABLE_POWER_SAVE_ON_BATTERY) {
        return true;
    }
    
    // If the feature is enabled, only use power saving when on battery
    // (not charging AND less than 100% battery)
    auto power = M5.Power;
    int battery_level = power.getBatteryLevel();
    bool is_charging = power.isCharging();
    
    // Device is considered "on battery" if it's not charging and battery level is less than 100%
    bool on_battery = !is_charging && battery_level < 100;
    
    // Check if state has changed
    bool state_changed = (on_battery != prev_on_battery) || 
                         (battery_level != prev_battery_level && (battery_level == 100 || prev_battery_level == 100)) || 
                         (is_charging != prev_is_charging);
    
    // Only log state changes or at most once per minute
    unsigned long state_log_interval = 60000; // 1 minute
    if (state_changed || (millis() - last_state_change >= state_log_interval)) {
        debugLog("Power state changed: level=%d%%, charging=%s, power saving=%s", 
                battery_level, 
                is_charging ? "yes" : "no", 
                on_battery ? "enabled" : "disabled");
        
        // Update last state change timestamp
        last_state_change = millis();
    }
    
    // Update previous state
    prev_on_battery = on_battery;
    prev_battery_level = battery_level;
    prev_is_charging = is_charging;
    
    return on_battery;
}

// Function to determine if tracing should be enabled
bool shouldEnableTracing() {
    // Static variables to track previous state for logging
    static bool prev_tracing_enabled = true;
    static unsigned long last_state_change = 0;
    
    // Check if the device is running on battery
    auto power = M5.Power;
    int battery_level = power.getBatteryLevel();
    bool is_charging = power.isCharging();
    bool on_battery = !is_charging && battery_level < 100;
    
    // Determine tracing state based on configuration and battery status
    bool tracing_enabled = !on_battery || ENABLE_TRACING_ON_BATTERY;
    
    // Log state changes or periodically
    if (tracing_enabled != prev_tracing_enabled || (millis() - last_state_change >= 60000)) {
        debugLog("Tracing state: %s (on battery: %s, config: %s)", 
                tracing_enabled ? "enabled" : "disabled",
                on_battery ? "yes" : "no", 
                ENABLE_TRACING_ON_BATTERY ? "allow on battery" : "disable on battery");
        
        last_state_change = millis();
    }
    
    prev_tracing_enabled = tracing_enabled;
    return tracing_enabled;
}

// Setup runs once at board startup
void setup() {
    // Start a trace for the entire setup process
    uint64_t setupSpanId = 0;
    
    // Initialize M5StickC-Plus
    auto cfg = M5.config();
    cfg.external_rtc = true;  // Enable the onboard RTC hardware
    M5.begin(cfg);    // Init M5Stack with modified config
    
    Serial.begin(115200);
    debugLog("Debug mode enabled");
    debugLog("M5StickC-Plus IoT OpenTelemetry Demo");
    
    // Set our custom time provider that uses the RTC
    otel.setTimeProvider(getDeviceTimeNanos);
    
    // Start a trace for the entire setup process - only if tracing is enabled
    try {
        otel.startNewTrace();
        setupSpanId = otel.startSpan("device_setup");
        debugLog("Starting device setup trace: %016llx", setupSpanId);
    } catch (...) {
        debugLog("Error starting device setup trace - continuing without tracing");
    }
    
    // Set up the display - create a child span
    uint64_t displaySpanId = 0;
    try {
        displaySpanId = otel.startSpan("display_initialization", setupSpanId);
        debugLog("Starting display initialization span: %016llx", displaySpanId);
    } catch (...) {
        debugLog("Error starting display span - continuing without span");
    }
    
    M5.Display.setRotation(3);  // Landscape mode
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);  // Changed from 1 to 2 for larger text
    M5.Display.setTextColor(WHITE);
    M5.Display.setCursor(0, 0);
    M5.Display.println("IoT OTel Demo");
    
    // Configure power management for best battery life
    debugLog("Configuring power management for optimal battery life");
    configurePowerManagement();
    last_button_press = millis();
    
    // End display span safely
    if (displaySpanId != 0) {
        try {
            otel.endSpan(displaySpanId);
            debugLog("Display initialization span completed");
        } catch (...) {
            debugLog("Error ending display span - continuing");
        }
    }

    // Initialize the sensor units - create a child span
    uint64_t sensorInitSpanId = 0;
    try {
        sensorInitSpanId = otel.startSpan("sensor_initialization", setupSpanId);
        debugLog("Starting sensor initialization span: %016llx", sensorInitSpanId);
    } catch (...) {
        debugLog("Error starting sensor initialization span - continuing without span");
    }
    
    debugLog("Initializing sensors");
    Wire.begin(32, 33); // SDA, SCL pins for M5StickC-Plus
    
    // Try to initialize the QMP6988 pressure sensor
    if (qmp.begin(&Wire, QMP6988_SLAVE_ADDRESS_L, 32, 33, 400000U)) {
        debugLog("QMP6988 pressure sensor initialized");
        try {
            if (sensorInitSpanId != 0) otel.addSpanAttribute(sensorInitSpanId, "qmp6988_init", "success");
        } catch (...) { /* Ignore attribute errors */ }
    } else {
        debugLog("Failed to initialize QMP6988 pressure sensor");
        try {
            if (sensorInitSpanId != 0) otel.addSpanAttribute(sensorInitSpanId, "qmp6988_init", "failed");
        } catch (...) { /* Ignore attribute errors */ }
    }
    
    // Try to initialize the SHT30 temperature/humidity sensor
    if (sht3x.begin(&Wire, SHT3X_I2C_ADDR, 32, 33, 400000U)) {
        debugLog("SHT3X temperature/humidity sensor initialized");
        try {
            if (sensorInitSpanId != 0) otel.addSpanAttribute(sensorInitSpanId, "sht3x_init", "success");
        } catch (...) { /* Ignore attribute errors */ }
    } else {
        debugLog("Failed to initialize SHT3X temperature/humidity sensor");
        try {
            if (sensorInitSpanId != 0) otel.addSpanAttribute(sensorInitSpanId, "sht3x_init", "failed");
        } catch (...) { /* Ignore attribute errors */ }
    }
    
    if (sensorInitSpanId != 0) {
        try {
            otel.endSpan(sensorInitSpanId);
            debugLog("Sensor initialization span completed");
        } catch (...) {
            debugLog("Error ending sensor initialization span - continuing");
        }
    }
    
    // Set up the watchdog timer - create a child span
    uint64_t watchdogSpanId = 0;
    try {
        watchdogSpanId = otel.startSpan("watchdog_setup", setupSpanId);
        debugLog("Starting watchdog setup span: %016llx", watchdogSpanId);
    } catch (...) {
        debugLog("Error starting watchdog setup span - continuing without span");
    }
    
    debugLog("Configuring watchdog timer with %d second timeout", WDT_TIMEOUT);
    esp_task_wdt_init(WDT_TIMEOUT, true); // Initialize with timeout and panic mode
    esp_task_wdt_add(NULL);  // Add current thread to watchdog
    esp_task_wdt_reset();    // Reset timer
    
    if (watchdogSpanId != 0) {
        try {
            otel.endSpan(watchdogSpanId);
            debugLog("Watchdog setup span completed");
        } catch (...) {
            debugLog("Error ending watchdog setup span - continuing");
        }
    }
    
    // Connect to WiFi - create a child span
    uint64_t wifiSpanId = 0;
    try {
        wifiSpanId = otel.startSpan("wifi_connection", setupSpanId);
        debugLog("Starting WiFi connection span: %016llx", wifiSpanId);
    } catch (...) {
        debugLog("Error starting WiFi connection span - continuing without span");
    }
    
    bool connected = establishWiFiConnection();
    
    // Add WiFi connection results to span
    if (wifiSpanId != 0) {
        try {
            otel.addSpanAttribute(wifiSpanId, "success", connected ? "true" : "false");
            if (connected) {
                otel.addSpanAttribute(wifiSpanId, "ip_address", WiFi.localIP().toString().c_str());
                otel.addSpanAttribute(wifiSpanId, "rssi", (double)WiFi.RSSI());
            } else {
                otel.addSpanAttribute(wifiSpanId, "error", "connection_failed");
            }
        } catch (...) {
            debugLog("Error adding WiFi span attributes - continuing");
        }
    }
    
    if (wifiSpanId != 0) {
        try {
            otel.endSpan(wifiSpanId);
            debugLog("WiFi connection span completed");
        } catch (...) {
            debugLog("Error ending WiFi connection span - continuing");
        }
    }
    
    if (!connected) {
        debugLog("Failed to establish WiFi connection.");
        try {
            uint64_t setupSpan = otel.startSpan("device_setup", setupSpanId);
            if (setupSpan != 0) {
                otel.addSpanAttribute(setupSpan, "error", "wifi_connection_failed");
            }
        } catch (...) {
            debugLog("Error adding WiFi error attribute to setup span - continuing");
        }
        
        if (WIFI_REBOOT_ON_FAIL) {
            debugLog("WIFI_REBOOT_ON_FAIL is enabled. Rebooting...");
            // Safely use the safer method for trace flushing
            bool success = otel.safeFlushTraces();
            if (success) {
                debugLog("Setup trace completed");
                esp_restart();
            } else {
                debugLog("Error sending traces before reboot - continuing with reboot");
                esp_restart();
            }
        } else {
            debugLog("WIFI_REBOOT_ON_FAIL is disabled. Continuing without WiFi connection.");
        }
    }
    
    // Sync time with NTP server - create a child span
    uint64_t ntpSpanId = 0;
    try {
        ntpSpanId = otel.startSpan("ntp_sync", setupSpanId);
        debugLog("Starting NTP sync span: %016llx", ntpSpanId);
    } catch (...) {
        debugLog("Error starting NTP sync span - continuing without span");
    }
    
    bool ntpSuccess = setupNTP();
    
    // Add NTP results to span
    if (ntpSpanId != 0) {
        try {
            otel.addSpanAttribute(ntpSpanId, "success", ntpSuccess ? "true" : "false");
            if (!ntpSuccess) {
                otel.addSpanAttribute(ntpSpanId, "error", "ntp_sync_failed");
                
                // Try to add error to main setup span
                try {
                    uint64_t setupSpan = otel.startSpan("device_setup", setupSpanId);
                    if (setupSpan != 0) {
                        otel.addSpanAttribute(setupSpan, "error", "ntp_sync_failed");
                    }
                } catch (...) {
                    debugLog("Error adding NTP error attribute to setup span - continuing");
                }
            }
        } catch (...) {
            debugLog("Error adding NTP span attributes - continuing");
        }
    }
    
    if (ntpSpanId != 0) {
        try {
            otel.endSpan(ntpSpanId);
            debugLog("NTP sync span completed");
        } catch (...) {
            debugLog("Error ending NTP sync span - continuing");
        }
    }
    
    // Get initial sensor readings - create a child span
    uint64_t sensorDataSpanId = 0;
    try {
        sensorDataSpanId = otel.startSpan("initial_sensor_reading", setupSpanId);
        debugLog("Starting initial sensor reading span: %016llx", sensorDataSpanId);
    } catch (...) {
        debugLog("Error starting initial sensor reading span - continuing without span");
    }
    
    debugLog("Getting initial sensor readings");
    querySensors();
    
    // Add sensor reading results to span
    if (sensorDataSpanId != 0) {
        try {
            otel.addSpanAttribute(sensorDataSpanId, "temperature", temp);
            otel.addSpanAttribute(sensorDataSpanId, "humidity", hum);
            otel.addSpanAttribute(sensorDataSpanId, "pressure", pressure/100);
            otel.addSpanAttribute(sensorDataSpanId, "battery_level", (double)g_battery_level);
        } catch (...) {
            debugLog("Error adding sensor data span attributes - continuing");
        }
    }
    
    if (sensorDataSpanId != 0) {
        try {
            otel.endSpan(sensorDataSpanId);
            debugLog("Initial sensor reading span completed");
        } catch (...) {
            debugLog("Error ending initial sensor reading span - continuing");
        }
    }
    
    // Initialize OpenTelemetry with the endpoint and service information
    debugLog("Initializing OpenTelemetry client");
    otel.begin(OTEL_SERVICE_NAME, OTEL_SERVICE_VERSION, OTEL_METRICS_URL, OTEL_TRACES_URL);
    
    // Explicitly set the endpoints to ensure they're properly initialized
    otel.initializeMetricsEndpoint(OTEL_METRICS_URL);
    otel.initializeTracesEndpoint(OTEL_TRACES_URL);
    
    if (otel.hasValidMetricsEndpoint() && otel.hasValidTracesEndpoint()) {
        debugLog("OpenTelemetry endpoints configured: Metrics=%s, Traces=%s", OTEL_METRICS_URL, OTEL_TRACES_URL);
        otel_initialized = true;
    } else {
        debugLog("Warning: OpenTelemetry endpoints not properly configured");
        otel_initialized = false;
    }

    // Set up the display
    displayMainScreen();
    
    debugLog("Setup complete");
    
    // Safely handle trace completion - let's wrap this in try/catch logic
    // to prevent crash if there's an issue with the OpenTelemetry implementation
    uint8_t total = 0, active = 0, completed = 0;
    
    // First check if we have any spans to send
    try {
        otel.getSpanStats(total, active, completed);
        if (completed > 0) {
            debugLog("Attempting to send %d completed spans", completed);
            // Use the safe method instead of direct call
            otel.safeSendMetricsAndTraces();
        } else {
            debugLog("No completed spans to send");
        }
    } catch (...) {
        // If there's any issue, just log and continue
        debugLog("Error during trace handling - continuing without sending traces");
    }
    
    debugLog("Setup completed successfully");
    
    // End the setup span if we created one
    if (setupSpanId != 0) {
        try {
            otel.endSpan(setupSpanId);
            debugLog("Device setup trace completed");
        } catch (...) {
            debugLog("Error ending setup span - continuing");
        }
    }
    
    // Start a new trace for the first metrics collection cycle
    try {
        otel.startNewTrace();
        debugLog("Starting initial metrics collection trace");
    } catch (...) {
        debugLog("Error starting initial metrics collection trace - continuing without tracing");
    }
}

void loop() {
    // We no longer create a trace for each loop iteration
    
    // Record loop start time
    unsigned long loop_start = millis();
    
    // Feed the watchdog timer
    esp_task_wdt_reset();
    
    M5.update();  // Read the press state of the buttons
    handleButtons();  // Handle any button events
    
    // Handle display timeout to save power, but only after first metrics are sent
    if (display_on && (millis() - last_button_press > DISPLAY_TIMEOUT) && last_otel_send > 0) {
        turnOffDisplay();
    }
    
    // Check if tracing is enabled and we need to flush traces (even if no metrics are ready)
    bool tracing_enabled = shouldEnableTracing();
    if (tracing_enabled && WiFi.status() == WL_CONNECTED && (millis() - last_trace_flush >= TRACE_FLUSH_INTERVAL)) {
        // Only attempt to flush if there are completed spans to send
        uint8_t total, active, completed;
        otel.getSpanStats(total, active, completed);
        
        if (completed > 0) {
            debugLog("Regular trace flush triggered (%d completed spans)", completed);
            bool success = otel.safeFlushTraces();
            if (success) {
                debugLog("Trace flush successful");
            } else {
                debugLog("Trace flush failed: %s", otel.getLastError());
            }
        }
        
        last_trace_flush = millis();
    }
    
    // Periodically log span statistics for debugging
    if (tracing_enabled && (millis() - last_span_debug >= SPAN_DEBUG_INTERVAL)) {
        uint8_t total, active, completed;
        otel.getSpanStats(total, active, completed);
        debugLog("SPAN STATS: Total=%d, Active=%d, Completed=%d", total, active, completed);
        
        // If we have more than 10 spans, print detailed info
        if (total > 10) {
            otel.debugSpans();
        }
        
        last_span_debug = millis();
    }
    
    // Check WiFi status - no more tracing inside this function
    if (WiFi.status() != WL_CONNECTED) {
        debugLog("WiFi connection lost, restarting connection process");
        
        bool connected = false;
        int reconnectAttempts = 0;
        const int MAX_RECONNECT_ATTEMPTS = 3;
        
        while (!connected && reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
            // Feed the watchdog timer during reconnection attempts
            esp_task_wdt_reset();
            
            reconnectAttempts++;
            debugLog("Reconnect attempt %d of %d", reconnectAttempts, MAX_RECONNECT_ATTEMPTS);
            
            if (!establishWiFiConnection()) {
                debugLog("WiFi reconnection failed, attempt %d", reconnectAttempts);
                delay(WIFI_RETRY_DELAY); // Short delay before next attempt
                continue;
            }
            
            // Feed the watchdog timer
            esp_task_wdt_reset();
            
            // Only try OTel health check after WiFi is connected
            if (!verifyOtelHealth()) {
                debugLog("OTel health check failed after reconnection");
                continue;
            }
            
            connected = true;
            debugLog("WiFi reconnection successful");
        }
        
        if (!connected) {
            debugLog("Failed to reconnect after %d attempts.", MAX_RECONNECT_ATTEMPTS);
            // Feed the watchdog one last time
            esp_task_wdt_reset();
            
            if (WIFI_REBOOT_ON_FAIL) {
                debugLog("WIFI_REBOOT_ON_FAIL is enabled. Restarting device...");
                
                // Send any pending traces before restart if tracing is enabled
                if (tracing_enabled) {
                    otel.safeFlushTraces();
                }
                
                // It's better to restart cleanly than let the watchdog trigger
                ESP.restart();
            } else {
                debugLog("WIFI_REBOOT_ON_FAIL is disabled. Continuing without WiFi connection.");
                // Will continue the loop but with limited functionality
            }
        }
        
        return;  // Start fresh loop iteration
    }
    
    // Calculate time until next metric send
    unsigned long time_to_next_send = 0;
    if (millis() > last_otel_send) {
        time_to_next_send = (last_otel_send + OTEL_SEND_INTERVAL - millis());
    }
    
    // Sleep strategy based on metric intervals
    const unsigned long WIFI_RECONNECT_TIME = 30000; // 30 seconds for reconnect+stabilize+verify
    
    // Check if power saving is enabled based on battery state
    bool enable_power_saving = shouldEnablePowerSaving();
    
    // Only enter light sleep if:
    // 1. Display is off
    // 2. We have time before next metric send (>5s)
    // 3. Power saving is enabled or we always use power saving
    bool should_sleep = !display_on && time_to_next_send > 5000 && enable_power_saving;
    
    if (should_sleep) {
        uint32_t sleep_time;
        
        if (OTEL_SEND_INTERVAL < 60000) {
            // For short metric intervals (<60s):
            // - Keep sleep periods very short to stay responsive
            // - Never disconnect WiFi since reconnection takes too long
            sleep_time = min(time_to_next_send - 1000, 2000UL); // Max 2 seconds
            debugLog("Short metric interval - brief light sleep with WiFi in modem sleep");
        } else {
            // For longer intervals (≥60s):
            // - Longer sleep periods are fine
            // - But wake up with enough time to reestablish connectivity if needed
            if (time_to_next_send > WIFI_RECONNECT_TIME + 5000) {
                // We have plenty of time before next metrics send
                sleep_time = min(time_to_next_send - WIFI_RECONNECT_TIME, 30000UL); // Max 30 seconds, leave time for reconnect
                debugLog("Long metric interval - extended light sleep (WiFi may disconnect)");
            } else {
                // We're close to next metrics send
                sleep_time = min(time_to_next_send - 2000, 3000UL); // Max 3 seconds, stay connected
                debugLog("Approaching next metrics send - brief light sleep");
            }
        }
        
        // Enter light sleep
        if (sleep_time > 500) { // Only sleep if we have at least 500ms to save
            debugLog("Starting light sleep for %llu ms (time to next metrics: %llu ms)", 
                    sleep_time, time_to_next_send);
            
            // Enter light sleep - execution stops here until wake
            enterLightSleep(sleep_time);
        }
    }
    
    // Feed the watchdog timer before potentially long operation
    esp_task_wdt_reset();
    
    // Only send metrics if enough time has passed since last send
    if (millis() - last_otel_send >= OTEL_SEND_INTERVAL) {
        debugLog("Time to send metrics to OpenTelemetry (interval: %lu ms, last send: %lu ms ago)...", 
                OTEL_SEND_INTERVAL, millis() - last_otel_send);
        
        // Create a span for sensor reading
        uint64_t sensorSpanId = 0;
        if (tracing_enabled) {
            try {
                sensorSpanId = otel.startSpan("sensor_reading");
                debugLog("Started sensor reading span: %016llx", sensorSpanId);
            } catch (...) {
                debugLog("Error starting sensor reading span - continuing without span");
            }
        }
        
        // Query sensors right before sending metrics
        querySensors();
        
        // End the sensor reading span
        if (sensorSpanId != 0) {
            try {
                otel.addSpanAttribute(sensorSpanId, "temperature", temp);
                otel.addSpanAttribute(sensorSpanId, "humidity", hum);
                otel.addSpanAttribute(sensorSpanId, "pressure", pressure/100);
                otel.addSpanAttribute(sensorSpanId, "battery_level", (double)g_battery_level);
                otel.endSpan(sensorSpanId);
                debugLog("Completed sensor reading span");
            } catch (...) {
                debugLog("Error ending sensor reading span - continuing");
            }
        }
        
        // Ensure WiFi is fully awake before sending metrics
        if (WiFi.getSleep()) {
            debugLog("Waking up WiFi from sleep mode before sending metrics");
            WiFi.setSleep(false);
            delay(100); // Brief delay to ensure modem is awake
        }
        
        // Make sure WiFi is actually connected
        if (WiFi.status() != WL_CONNECTED) {
            debugLog("WiFi disconnected before sending metrics, attempting to reconnect");
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            
            // Try to reconnect for up to 5 seconds
            unsigned long reconnect_start = millis();
            bool reconnected = false;
            while (millis() - reconnect_start < 5000) {
                if (WiFi.status() == WL_CONNECTED) {
                    reconnected = true;
                    debugLog("WiFi reconnected successfully before sending metrics");
                    break;
                }
                esp_task_wdt_reset();
                delay(500);
            }
            
            if (!reconnected) {
                debugLog("Failed to reconnect WiFi, skipping metrics send");
                // Don't update last_otel_send here to allow retry on next loop
                upload_fail_count++;
                lastOtelError = "WiFi reconnection failed";
                return; // Skip this sending attempt
            }
        }
        
        // Feed the watchdog timer before network operation
        esp_task_wdt_reset();

        // Add metrics to the batch with timestamp from when sensors were read
        bool all_metrics_added = true;
        all_metrics_added &= otel.addMetric("temperature", temp, sensor_reading_timestamp);
        all_metrics_added &= otel.addMetric("humidity", hum, sensor_reading_timestamp);
        all_metrics_added &= otel.addMetric("pressure", pressure/100, sensor_reading_timestamp); // convert to hPa
        all_metrics_added &= otel.addMetric("battery_level", g_battery_level, sensor_reading_timestamp);
        all_metrics_added &= otel.addMetric("battery_voltage", g_battery_voltage/1000, sensor_reading_timestamp); // convert to volts
        all_metrics_added &= otel.addMetric("battery_charging", g_is_charging ? 1 : 0, sensor_reading_timestamp);
        all_metrics_added &= otel.addMetric("wifi.rssi", WiFi.RSSI(), sensor_reading_timestamp);
        all_metrics_added &= otel.addMetric("FreeHeap", ESP.getFreeHeap(), sensor_reading_timestamp); // added for testing

        if (!all_metrics_added) {
            debugLog("Warning: Some metrics weren't added due to buffer constraints");
        }

        // Create a span for metrics sending if tracing is enabled
        uint64_t metricsSpanId = 0;
        if (tracing_enabled) {
            try {
                metricsSpanId = otel.startSpan("metric_send");
                debugLog("Started metric send span: %016llx", metricsSpanId);
                
                // Add context to span
                otel.addSpanAttribute(metricsSpanId, "wifi.rssi", (double)WiFi.RSSI());
                otel.addSpanAttribute(metricsSpanId, "metrics_count", 7.0); // Number of metrics we're sending
                otel.addSpanAttribute(metricsSpanId, "all_metrics_added", all_metrics_added ? "true" : "false");
                
                if (!all_metrics_added) {
                    otel.addSpanAttribute(metricsSpanId, "error", "buffer_constraints");
                }
            } catch (...) {
                debugLog("Error starting metric send span - continuing without span");
            }
        }

        // Send both metrics and traces
        bool success = otel.safeSendMetricsAndTraces();
        
        // Add result to span and end it
        if (metricsSpanId != 0) {
            try {
                otel.addSpanAttribute(metricsSpanId, "success", success ? "true" : "false");
                
                if (!success) {
                    otel.addSpanAttribute(metricsSpanId, "error", otel.getLastError());
                    otel.addSpanAttribute(metricsSpanId, "http_code", (double)otel.getLastHttpCode());
                }
                
                // End the span
                otel.endSpan(metricsSpanId);
                debugLog("Completed metric send span");
            } catch (...) {
                debugLog("Error ending metric send span - continuing");
            }
        }

        if (success) {
            debugLog("Metrics sent successfully");
            upload_fail_count = 0;
            lastOtelError = "None";
            last_otel_send = millis();
            
            // End the current metrics collection trace and start a new one
            if (tracing_enabled) {
                try {
                    debugLog("Ending metrics collection trace and starting a new one");
                    otel.startNewTrace();
                } catch (...) {
                    debugLog("Error cycling metrics collection trace - continuing");
                }
            }
            
            // Force a refresh of the main screen after metrics are sent
            if (currentScreen == MAIN_SCREEN && display_on) {
                // Reset temperature values to force redraw
                prev_temp = -999.0;
                prev_hum = -999.0;
                prev_pressure = -999.0;
            }
        } else {
            debugLog("Failed to send metrics: %s", otel.getLastError());
            upload_fail_count++;
            lastOtelError = otel.getLastError();
            // Don't update last_otel_send on failure to allow retry
        }
    }

    // Update the display only if it's on
    if (display_on) {
        switch (currentScreen) {
            case NETWORK_SCREEN:
                displayNetworkScreen();
                break;
            case OTEL_SCREEN:
                displayOtelScreen();
                break;
            case MAIN_SCREEN:
            default:
                displayMainScreen();
                break;
        }
    }

    // Short delay to prevent CPU from running at full speed constantly
    delay(50);
}