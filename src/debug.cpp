#include "debug.h"
#include "config.h"
#include <time.h>
#include <M5Unified.h>

void debugLog(const char* format, ...) {
    if (LCD_SHOW_DEBUG_INFO == "1") {
        // Get timestamp - try RTC first if available, fallback to ESP32 time
        char timestamp[32];  // Buffer for timestamp
        
        if (M5.Rtc.isEnabled()) {
            // Get time from RTC hardware
            auto dt = M5.Rtc.getDateTime();
            
            // Format timestamp with log4j style
            snprintf(timestamp, sizeof(timestamp), 
                     "%04d-%02d-%02d %02d:%02d:%02d.000", 
                     dt.date.year, dt.date.month, dt.date.date,
                     dt.time.hours, dt.time.minutes, dt.time.seconds);
        } else {
            // Fallback to ESP32 internal time
            time_t now;
            time(&now);
            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);
            
            // Format timestamp with log4j style
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S.000", &timeinfo);
        }
        
        // Format the message
        char message[256];
        va_list args;
        va_start(args, format);
        vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        
        // Combine with log4j style format
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "[%s] [DEBUG] %s", timestamp, message);
        
        Serial.println(buffer);
    }
} 