#include <M5Unified.h>
#include <M5UnitENV.h>
#include <WiFi.h>
#include "debug.h"
#include "opentelemetry.h"
#include "config.h"

// Create instance of the ENV III sensor unit
SHT3X sht3x;  // Humidity sensor in the ENVIII module
QMP6988 qmp;  // Temp and pressure sensor in the ENV3 module

// setup vars to receive sensor data and track connection
float temp = 0.0;
float hum = 0.0;
float pressure = 0.0;
int upload_fail_count = 0;
unsigned long last_wifi_reconnect = 0;
unsigned long last_wifi_check = 0;
bool wifi_was_connected = false;
const unsigned long WIFI_CHECK_INTERVAL = 1000;     // Check WiFi status every second
// Health check extension must be enabled in OpenTelemetry Collector
// For Splunk OpenTelemetry Collector, the extension is included but must be configured
// to listen on an external IP (not just localhost) in the extensions section
const unsigned long OTEL_PING_INTERVAL = 30000;     // Check OTel collector health every 30 seconds
unsigned long last_wifi_ping = 0;
bool wifi_ping_success = false;
unsigned long last_otel_send = 0;  // Track last time metrics were sent

// Screen state management
enum ScreenState {
    MAIN_SCREEN,
    NETWORK_SCREEN,
    OTEL_SCREEN,
    MAX_SCREENS
};
ScreenState currentScreen = MAIN_SCREEN;
String lastOtelError = "None"; // Store the last OpenTelemetry error message

// initialize our OTel object from the included library
OpenTelemetry otel;

// Function to display status with color
void displayStatus(const char* label, bool isOk, const char* message) {
    M5.Display.setTextColor(WHITE);
    M5.Display.printf("  %s: ", label);
    M5.Display.setTextColor(isOk ? GREEN : RED);
    M5.Display.printf("%s\n", message);
    M5.Display.setTextColor(WHITE);
    
    // Log status changes to serial
    debugLog("%s Status: %s (%s)", label, isOk ? "OK" : "FAIL", message);
}

// Function to display the main sensor readings screen
void displayMainScreen() {
    M5.Display.fillRect(0, 30, 240, 135, BLACK);  // Clear the display area
    M5.Display.setCursor(0, 40);
    M5.Display.printf("  Temp: %2.1f  \r\n  Humi: %2.0f%%  \r\n  Pressure:%2.0f hPa\r\n", temp, hum, pressure / 100);

    M5.Display.setCursor(0, 92);
    bool wifi_connected = (WiFi.status() == WL_CONNECTED);
    displayStatus("WiFi", wifi_connected, wifi_connected ? "Connected" : "Disconnected");
    
    M5.Display.setCursor(0, 110);
    char otel_status[32];
    if (!wifi_connected) {
        snprintf(otel_status, sizeof(otel_status), "No WiFi");
    } else if (upload_fail_count > 0) {
        snprintf(otel_status, sizeof(otel_status), "Failed (%d)", upload_fail_count);
    } else {
        snprintf(otel_status, sizeof(otel_status), "OK");
    }
    displayStatus("OTL", upload_fail_count == 0 && wifi_connected, otel_status);
}

// Function to display network details
void displayNetworkScreen() {
    M5.Display.fillRect(0, 30, 240, 135, BLACK);
    M5.Display.setCursor(0, 40);
    M5.Display.setTextColor(BLUE);
    M5.Display.printf("Network Info:\n");
    M5.Display.setTextColor(WHITE);
    
    bool wifi_connected = (WiFi.status() == WL_CONNECTED);
    if (wifi_connected) {
        M5.Display.printf("  IP: %s\n", WiFi.localIP().toString().c_str());
        M5.Display.printf("  SSID: %s\n", WIFI_SSID);
        M5.Display.printf("  Host: %s\n", WIFI_HOSTNAME);
        M5.Display.printf("  RSSI: %ddBm\n", WiFi.RSSI());
    } else {
        M5.Display.setTextColor(RED);
        M5.Display.printf("  Disconnected\n");
        M5.Display.printf("  Last attempt: %ds ago\n", (millis() - last_wifi_reconnect) / 1000);
    }
}

// Function to display OpenTelemetry details
void displayOtelScreen() {
    M5.Display.fillRect(0, 30, 240, 135, BLACK);
    M5.Display.setCursor(0, 40);
    M5.Display.setTextColor(BLUE);
    M5.Display.printf("OTEL Status:\n");
    M5.Display.setTextColor(WHITE);
    
    M5.Display.printf("  Endpoint:\n  %s\n", OTEL_URL);
    M5.Display.printf("  Service: %s\n", OTEL_SERVICE_NAME);
    
    if (upload_fail_count > 0) {
        M5.Display.setTextColor(RED);
        M5.Display.printf("  Failed uploads: %d\n", upload_fail_count);
        M5.Display.printf("  Last error:\n  %s", lastOtelError.c_str());
    } else {
        M5.Display.setTextColor(GREEN);
        M5.Display.printf("  Status: OK\n");
        M5.Display.printf("  Metrics sending\n");
    }
}

// Button event handlers
void handleButtonA() {
    debugLog("Button A released - cycling screen");
    // Force screen update immediately
    M5.Display.fillScreen(BLACK);  // Clear screen
    M5.Display.setCursor(10, 10);
    M5.Display.setTextColor(BLUE);
    M5.Display.printf("==  OTEL Demo ==");
    M5.Display.setTextColor(WHITE);
    
    // Cycle to next screen
    currentScreen = static_cast<ScreenState>((static_cast<int>(currentScreen) + 1) % static_cast<int>(MAX_SCREENS));
    debugLog("Screen changed to: %d (%s)", 
             static_cast<int>(currentScreen),
             currentScreen == MAIN_SCREEN ? "Main" :
             currentScreen == NETWORK_SCREEN ? "Network" : "OTEL");
             
    // Update the screen immediately
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

void handleButtonB() {
    debugLog("Button B released - no action assigned");
    // No action assigned yet
}

void handleButtonBLongPress() {
    debugLog("Button B long press (700ms) - no action assigned");
    // No action assigned yet
}

void handleButtons() {
    static bool buttonAPressed = false;
    
    // Check for Button A (Screen cycling)
    if (M5.BtnA.wasPressed()) {
        buttonAPressed = true;
        debugLog("Button A pressed");
    }
    
    if (M5.BtnA.wasReleased() && buttonAPressed) {
        buttonAPressed = false;
        handleButtonA();
    }
    
    // Check for Button B (Reserved for future use)
    if (M5.BtnB.wasReleased()) {
        handleButtonB();
    }
    
    if (M5.BtnB.wasReleaseFor(700)) {
        handleButtonBLongPress();
    }
}

// Function to perform a ping test to verify network connectivity
bool pingTest() {
    // First check if we have a valid IP address
    if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
        debugLog("Ping test failed: No valid IP address");
        return false;
    }

    HTTPClient http;
    // Use the health check endpoint of the OTel collector
    String healthUrl = "http://" + String(OTEL_HOST) + ":13133";
    http.begin(healthUrl);
    http.setTimeout(5000);  // 5 second timeout
    
    int httpCode = http.GET();
    String response = http.getString();
    http.end();
    
    // Log the response for debugging
    debugLog("OTEL health check response: %s", response.c_str());
    
    // Check for successful response and valid JSON content
    return (httpCode == 200 && response.indexOf("\"status\":\"Server available\"") != -1);
}

// Function to check WiFi status and handle state changes
bool checkWiFiConnection() {
    bool currently_connected = (WiFi.status() == WL_CONNECTED);
    
    // Log detailed WiFi status for debugging
    if (currently_connected) {
        static int last_rssi = 0;
        int current_rssi = WiFi.RSSI();
        if (current_rssi != last_rssi) {
            debugLog("WiFi RSSI changed: %d dBm", current_rssi);
            last_rssi = current_rssi;
        }
    }

    // Only perform ping test if WiFi shows connected
    if (currently_connected && (millis() - last_wifi_ping > OTEL_PING_INTERVAL)) {
        debugLog("Performing OTEL health check...");
        wifi_ping_success = pingTest();
        last_wifi_ping = millis();
        
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
            WiFi.disconnect(true);  // Disconnect with clearing settings
            delay(1000);  // Give it time to disconnect properly
            WiFi.mode(WIFI_STA);    // Ensure we're in station mode
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            last_wifi_reconnect = millis();
            wifi_ping_success = false;  // Reset ping status on reconnect
            consecutive_ping_failures = 0;  // Reset failure counter
        }
    }
    
    return !connection_lost;
}

// Function to establish and verify WiFi connection
bool establishWiFiConnection() {
    debugLog("Attempting to establish WiFi connection...");
    M5.Display.fillRect(0, 30, 240, 90, BLACK);
    M5.Display.setCursor(10, 30);
    M5.Display.setTextColor(YELLOW);
    M5.Display.printf("Connecting WiFi");
    
    while(true) {  // Keep trying until we get a stable connection
        WiFi.disconnect(true);
        delay(1000);
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(WIFI_HOSTNAME);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        
        unsigned long startAttempt = millis();
        bool initial_connection = false;
        int dots = 0;
        
        // Try to connect up to timeout
        while (millis() - startAttempt < WIFI_CONNECT_TIMEOUT) {
            if (WiFi.status() == WL_CONNECTED) {
                initial_connection = true;
                break;
            }
            
            // Update display with progress
            M5.Display.setCursor(10, 50);
            M5.Display.printf("%.*s", dots + 1, ".....");
            dots = (dots + 1) % 5;
            
            M5.Display.setCursor(10, 70);
            M5.Display.printf("Attempt: %ds", 
                (millis() - startAttempt) / 1000);
            
            delay(500);
        }
        
        if (initial_connection) {
            debugLog("Initial WiFi connection established");
            debugLog("IP: %s, RSSI: %d dBm", 
                    WiFi.localIP().toString().c_str(), 
                    WiFi.RSSI());
            
            // Show stabilization countdown
            debugLog("Waiting %d ms for stability...", WIFI_STABILIZE_DELAY);
            M5.Display.fillRect(0, 30, 240, 90, BLACK);
            M5.Display.setCursor(10, 30);
            M5.Display.setTextColor(GREEN);
            M5.Display.printf("WiFi Connected");
            M5.Display.setCursor(10, 50);
            M5.Display.setTextColor(WHITE);
            M5.Display.printf("Stabilizing...");
            
            delay(WIFI_STABILIZE_DELAY);
            
            // Verify connection is still stable
            if (WiFi.status() == WL_CONNECTED) {
                debugLog("WiFi connection stable");
                return true;
            }
            
            debugLog("Connection lost during stabilization");
        }
        
        // If we get here, either initial connection failed or stabilization failed
        debugLog("Connection attempt failed, retrying...");
        M5.Display.fillRect(0, 30, 240, 90, BLACK);
        M5.Display.setCursor(10, 30);
        M5.Display.setTextColor(RED);
        M5.Display.printf("WiFi Failed");
        M5.Display.setCursor(10, 50);
        M5.Display.setTextColor(WHITE);
        M5.Display.printf("Retrying...");
        delay(2000);  // Brief pause before next attempt
    }
}

// Function to configure and sync NTP
bool setupNTP() {
    if (WiFi.status() != WL_CONNECTED) {
        debugLog("Cannot setup NTP - WiFi not connected");
        return false;
    }

    debugLog("Configuring NTP...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    
    M5.Display.fillRect(0, 30, 240, 90, BLACK);
    M5.Display.setCursor(10, 30);
    M5.Display.setTextColor(YELLOW);
    M5.Display.printf("NTP Sync...");
    
    // Wait for NTP to sync
    time_t now = time(nullptr);
    int ntp_tries = 0;
    const int MAX_NTP_TRIES = 10;
    
    while (now < 24 * 3600 && ntp_tries < MAX_NTP_TRIES) {
        // Verify WiFi is still connected during NTP sync
        if (WiFi.status() != WL_CONNECTED) {
            debugLog("Lost WiFi during NTP sync");
            return false;
        }
        
        debugLog("Waiting for NTP sync... attempt %d", ntp_tries + 1);
        M5.Display.setCursor(10, 50);
        M5.Display.printf("Attempt: %d/%d", ntp_tries + 1, MAX_NTP_TRIES);
        delay(1000);
        now = time(nullptr);
        ntp_tries++;
    }
    
    if (now < 24 * 3600) {
        debugLog("NTP sync failed");
        return false;
    }
    
    debugLog("NTP synchronized successfully");
    return true;
}

// Function to verify OpenTelemetry endpoint health
bool verifyOtelHealth() {
    debugLog("Verifying OpenTelemetry endpoint health...");
    M5.Display.fillRect(0, 30, 240, 90, BLACK);
    M5.Display.setCursor(10, 30);
    M5.Display.setTextColor(YELLOW);
    M5.Display.printf("Testing OTEL");
    
    const int MAX_RETRIES = 5;
    const int RETRY_DELAY = 5000;  // 5 seconds
    
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        debugLog("Health check attempt %d of %d", attempt, MAX_RETRIES);
        M5.Display.setCursor(10, 50);
        M5.Display.printf("Attempt %d/%d", attempt, MAX_RETRIES);
        
        HTTPClient http;
        String healthUrl = String("http://") + OTEL_HOST + ":13133";
        debugLog("Checking URL: %s", healthUrl.c_str());
        
        http.begin(healthUrl);
        http.setTimeout(5000);
        
        int httpCode = http.GET();
        String response = http.getString();
        http.end();
        
        debugLog("HTTP Code: %d", httpCode);
        debugLog("Response: %s", response.c_str());
        
        if (httpCode == 200 && response.indexOf("\"status\":\"Server available\"") != -1) {
            debugLog("OpenTelemetry endpoint healthy");
            M5.Display.fillRect(0, 30, 240, 90, BLACK);
            M5.Display.setCursor(10, 30);
            M5.Display.setTextColor(GREEN);
            M5.Display.printf("OTEL Ready");
            delay(1000);
            return true;
        }
        
        if (attempt < MAX_RETRIES) {
            debugLog("Retrying in %d ms...", RETRY_DELAY);
            delay(RETRY_DELAY);
        }
    }
    
    debugLog("OpenTelemetry endpoint unavailable after %d attempts", MAX_RETRIES);
    M5.Display.fillRect(0, 30, 240, 90, BLACK);
    M5.Display.setCursor(10, 30);
    M5.Display.setTextColor(RED);
    M5.Display.printf("OTEL Failed");
    M5.Display.setCursor(10, 50);
    M5.Display.setTextColor(WHITE);
    M5.Display.printf("Waiting 60s...");
    delay(60000);  // Wait one minute before allowing retry
    return false;
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

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);    // Init M5Stack with default config
    
    Serial.begin(115200);
    debugLog("Debug mode enabled");
    debugLog("Device hostname: %s", WIFI_HOSTNAME);
    
    // setup the display
    M5.Display.setRotation(3);    // Rotate the screen
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);

    M5.Display.setCursor(10, 10);
    M5.Display.setTextColor(BLUE);
    M5.Display.printf("== OTEL Demo  ==");
    M5.Display.setTextColor(WHITE);

    // Initialize I2C for ENV sensor with proper parameters
    debugLog("Initializing I2C sensors on pins 32,33");
    Wire.begin(32, 33);
    
    if (!qmp.begin(&Wire, QMP6988_SLAVE_ADDRESS_L, 32, 33, 400000U)) {
        debugLog("ERROR: QMP6988 initialization failed!");
        M5.Display.setCursor(10, 30);
        M5.Display.printf("QMP6988 init failed!");
        while (1) delay(1);
    }
    debugLog("QMP6988 sensor initialized successfully");

    if (!sht3x.begin(&Wire, SHT3X_I2C_ADDR, 32, 33, 400000U)) {
        debugLog("ERROR: SHT3X initialization failed!");
        M5.Display.setCursor(10, 30);
        M5.Display.printf("SHT3X init failed!");
        while (1) delay(1);
    }
    debugLog("SHT3X sensor initialized successfully");

    // Initialize OpenTelemetry
    debugLog("Initializing OpenTelemetry with endpoint: %s", OTEL_URL);
    otel.begin(OTEL_SERVICE_NAME, OTEL_SERVICE_VERSION, OTEL_URL);
    
    // Initial connection setup
    while (true) {
        // First establish WiFi connection
        if (!establishWiFiConnection()) {
            debugLog("WiFi connection failed, retrying...");
            continue;
        }
        debugLog("WiFi connection established and stable");
        
        // Once WiFi is stable, setup NTP
        if (!setupNTP()) {
            debugLog("NTP setup failed, restarting connection process");
            continue;
        }
        debugLog("NTP synchronized successfully");
        
        // Only try OTel health check after WiFi and NTP are ready
        if (!verifyOtelHealth()) {
            debugLog("OTel health check failed, restarting connection process");
            continue;
        }
        debugLog("OTel health check passed");
        break;  // All systems ready
    }
}

void loop() {
    M5.update();  // Read the press state of the buttons
    handleButtons();  // Handle any button events
    
    // Check WiFi status
    if (WiFi.status() != WL_CONNECTED) {
        debugLog("WiFi connection lost, restarting connection process");
        while (true) {
            if (!establishWiFiConnection()) continue;
            if (!setupNTP()) continue;
            if (!verifyOtelHealth()) continue;
            break;
        }
        return;  // Start fresh loop iteration
    }
    
    // Get sensor readings
    if (qmp.update()) {
        pressure = qmp.pressure;
        debugLog("Pressure reading: %.2f hPa", pressure / 100);
    } else {
        debugLog("Failed to read QMP6988 sensor");
    }

    if (sht3x.update()) {
        temp = sht3x.cTemp;
        hum = sht3x.humidity;
        debugLog("Temperature: %.2f°C, Humidity: %.2f%%", temp, hum);
    } else {
        debugLog("Failed to read SHT3X sensor");
        temp = 0;
        hum = 0;
    }

    // Gather battery and power information
    auto power = M5.Power;
    int battery_level = power.getBatteryLevel();
    float battery_voltage = power.getBatteryVoltage();
    bool is_charging = power.isCharging();

    // Only send metrics if enough time has passed since last send
    if (millis() - last_otel_send >= OTEL_SEND_INTERVAL) {
        debugLog("Sending metrics to OpenTelemetry...");
        
        // Add metrics to the batch
        otel.addMetric("temperature", temp);
        otel.addMetric("humidity", hum);
        otel.addMetric("pressure", pressure/100); // convert to hPa
        otel.addMetric("battery_level", battery_level);
        otel.addMetric("battery_voltage", battery_voltage/1000); // convert to volts
        otel.addMetric("battery_charging", is_charging ? 1 : 0);
        otel.addMetric("wifi.rssi", WiFi.RSSI());

        // Send the metrics
        bool metrics_ok = otel.sendMetrics();

        if (!metrics_ok) {
            upload_fail_count++;
            lastOtelError = otel.getLastError();
            debugLog("Failed to send metrics (attempt %d failed)", upload_fail_count);
            debugLog("HTTP Code: %d", otel.getLastHttpCode());
            debugLog("Error: %s", lastOtelError.c_str());
            return;  // Start loop over to check connectivity
        }

        debugLog("Metrics sent successfully");
        upload_fail_count = 0;
        lastOtelError = "None";
        last_otel_send = millis();
    }

    // Update the display
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

    delay(1000);  // Update display and check buttons every second
}