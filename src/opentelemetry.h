#ifndef OPENTELEMETRY_H
#define OPENTELEMETRY_H

#include <Arduino.h>
#include <HTTPClient.h>
#include "config.h"
#include "debug.h"

class OpenTelemetry {
private:
    struct MetricPoint {
        String name;
        double value;
        MetricPoint(const char* n, double v) : name(n), value(v) {}
    };
    
    String serviceName;
    String serviceVersion;
    String endpoint;
    HTTPClient http;
    String lastErrorMessage;
    int lastHttpCode;
    bool timeConfigured;
    std::vector<MetricPoint> batchMetrics;
    
    String createBatchPayload(uint64_t timestamp_nanos) {
        String payload = "{\"resourceMetrics\":[{";
        payload += "\"resource\":{\"attributes\":[";
        payload += "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"" + serviceName + "\"}},";
        payload += "{\"key\":\"service.version\",\"value\":{\"stringValue\":\"" + serviceVersion + "\"}},";
        payload += "{\"key\":\"wifi.ssid\",\"value\":{\"stringValue\":\"" + String(WIFI_SSID) + "\"}}";
        payload += "]},";
        payload += "\"scopeMetrics\":[{\"metrics\":[";
        
        for (size_t i = 0; i < batchMetrics.size(); i++) {
            if (i > 0) payload += ",";
            payload += "{";
            payload += "\"name\":\"" + batchMetrics[i].name + "\",";
            payload += "\"gauge\":{\"dataPoints\":[{";
            payload += "\"timeUnixNano\":\"" + String(timestamp_nanos) + "\",";
            payload += "\"asDouble\":" + String(batchMetrics[i].value, 2);
            payload += "}]}";
            payload += "}";
        }
        
        payload += "]}]}]}";
        return payload;
    }

public:
    void begin(const char* svcName, const char* svcVersion, const char* endpointUrl) {
        serviceName = String(svcName);
        serviceVersion = String(svcVersion);
        endpoint = String(endpointUrl);
        lastErrorMessage = "None";
        lastHttpCode = 0;
        timeConfigured = false;
        
        // Configure time with multiple NTP servers and timezone settings from config.h
        debugLog("Configuring NTP with servers: %s, %s, %s", NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
        
        // Wait for NTP sync (with timeout)
        unsigned long startAttempt = millis();
        const unsigned long NTP_TIMEOUT = 10000; // 10 second timeout
        time_t now;
        
        while (millis() - startAttempt < NTP_TIMEOUT) {
            time(&now);
            if (now > 1600000000) { // Time is after Sept 2020, probably valid
                timeConfigured = true;
                debugLog("NTP sync successful. Current time: %lu", now);
                break;
            }
            delay(100);
        }
        
        if (!timeConfigured) {
            debugLog("Warning: NTP sync timed out. Timestamps may be inaccurate.");
        }
    }
    
    void addMetric(const char* name, double value) {
        batchMetrics.push_back(MetricPoint(name, value));
    }
    
    bool sendMetrics() {
        if (batchMetrics.empty()) {
            lastErrorMessage = "No metrics to send";
            return false;
        }

        if (!WiFi.isConnected()) {
            lastErrorMessage = "WiFi not connected";
            lastHttpCode = 0;
            return false;
        }

        // Get current timestamp
        time_t now;
        time(&now);
        uint64_t timestamp_nanos = (uint64_t)now * 1000000000ULL;
        
        String payload = createBatchPayload(timestamp_nanos);
        
        http.begin(endpoint);
        http.addHeader("Content-Type", "application/json");
        
        lastHttpCode = http.POST(payload);
        if (lastHttpCode < 200 || lastHttpCode >= 300) {
            lastErrorMessage = http.getString();
            if (lastErrorMessage.length() == 0) {
                lastErrorMessage = String("HTTP Error ") + lastHttpCode;
            }
        } else {
            lastErrorMessage = "None";
        }
        http.end();
        
        // Clear the batch after sending
        batchMetrics.clear();
        
        return lastHttpCode >= 200 && lastHttpCode < 300;
    }

    const char* getLastError() {
        return lastErrorMessage.c_str();
    }

    int getLastHttpCode() {
        return lastHttpCode;
    }
    
    bool isTimeConfigured() {
        return timeConfigured;
    }
};

#endif

