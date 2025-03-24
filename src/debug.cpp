#include "debug.h"
#include "config.h"

void debugLog(const char* format, ...) {
    if (LCD_SHOW_DEBUG_INFO == "1") {
        char buffer[256];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        Serial.println(buffer);
    }
} 