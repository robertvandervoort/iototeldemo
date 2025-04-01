#ifndef OPENTELEMETRY_H
#define OPENTELEMETRY_H

#include <Arduino.h>
#include <HTTPClient.h>
#include "config.h"
#include "debug.h"

// Define a maximum number of metrics to prevent unbounded growth
#define MAX_METRICS 15
// Define a maximum number of spans to prevent unbounded growth
#define MAX_SPANS 50
// Maximum number of spans to keep in memory at once for sending
#define MAX_SPANS_PER_BATCH 15
// Define a maximum number of span attributes
#define MAX_SPAN_ATTRS 10

class OpenTelemetry {
private:
    // Function pointer type for time retrieval
    typedef uint64_t (*TimeProviderFunc)();
    
    // Function pointer type for random seed generation
    typedef uint32_t (*RandomSeedProviderFunc)();
    
    // Default time provider - uses millis() which is available on all Arduino platforms
    static uint64_t defaultTimeProvider() {
        return (uint64_t)millis() * 1000000ULL; // Convert to nanoseconds
    }
    
    // Default random seed provider - uses ESP32's hardware RNG when available
    static uint32_t defaultRandomSeedProvider() {
#ifdef ESP32
        return esp_random();
#else
        // Fallback for non-ESP32 platforms - less ideal but portable
        return (uint32_t)millis();
#endif
    }
    
    // The current time provider
    TimeProviderFunc timeProvider = defaultTimeProvider;
    
    // The current random seed provider
    RandomSeedProviderFunc randomSeedProvider = defaultRandomSeedProvider;

    struct MetricPoint {
        const char* name;
        double value;
        uint64_t timestamp_nanos;
        MetricPoint() : name(nullptr), value(0), timestamp_nanos(0) {}
        MetricPoint(const char* n, double v, uint64_t ts) : name(n), value(v), timestamp_nanos(ts) {}
    };
    
    // Structure for span attributes
    struct SpanAttribute {
        const char* key;
        const char* stringValue;
        double doubleValue;
        bool isString;
        SpanAttribute() : key(nullptr), stringValue(nullptr), doubleValue(0), isString(true) {}
        SpanAttribute(const char* k, const char* v) : key(k), stringValue(v), doubleValue(0), isString(true) {}
        SpanAttribute(const char* k, double v) : key(k), stringValue(nullptr), doubleValue(v), isString(false) {}
    };
    
    // Structure for spans
    struct Span {
        char name[32];                       // Span name
        uint64_t traceId[2];                 // 128-bit trace ID (as 2 uint64_t)
        uint64_t spanId;                     // 64-bit span ID
        uint64_t parentSpanId;               // 64-bit parent span ID (0 if no parent)
        uint64_t startTimeNanos;             // Start time in nanoseconds
        uint64_t endTimeNanos;               // End time in nanoseconds
        SpanAttribute attributes[MAX_SPAN_ATTRS]; // Span attributes
        uint8_t attributeCount;              // Number of attributes
        bool isActive;                       // Whether the span is currently active
        
        Span() : spanId(0), parentSpanId(0), startTimeNanos(0), endTimeNanos(0), 
                 attributeCount(0), isActive(false) {
            name[0] = '\0';
            traceId[0] = 0;
            traceId[1] = 0;
        }
    };
    
    const char* serviceName;
    const char* serviceVersion;
    const char* metricsEndpoint;
    const char* tracesEndpoint;
    HTTPClient http;
    String lastErrorMessage;
    int lastHttpCode;
    
    // Fixed-size array instead of vector to avoid dynamic memory allocation
    MetricPoint batchMetrics[MAX_METRICS];
    uint8_t metricCount;
    
    // Spans for tracing
    Span spans[MAX_SPANS];
    uint8_t spanCount;
    uint8_t activeSpanCount;
    
    // Current trace ID (used for all spans in a single trace)
    uint64_t currentTraceId[2];
    
    // Pre-allocated buffer for JSON payload - reduced to save memory
    char jsonBuffer[4096]; // Reduced from 8192 to 4096
    
    bool appendToBuffer(char* buffer, size_t& position, const size_t maxSize, const char* format, ...) {
        va_list args;
        va_start(args, format);
        int written = vsnprintf(buffer + position, maxSize - position, format, args);
        va_end(args);
        
        if (written < 0 || written >= (int)(maxSize - position)) {
            // Buffer overflow would occur
            debugLog("Warning: JSON buffer overflow prevented");
            return false;
        }
        
        position += written;
        return true;
    }
    
    bool createBatchPayload() {
        size_t pos = 0;
        
        // Start the JSON structure
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), 
                "{\"resourceMetrics\":[{\"resource\":{\"attributes\":[")) {
            return false;
        }
        
        // Add resource attributes
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"%s\"}},", serviceName)) {
            return false;
        }
        
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                "{\"key\":\"service.version\",\"value\":{\"stringValue\":\"%s\"}},", serviceVersion)) {
            return false;
        }
        
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                "{\"key\":\"wifi.ssid\",\"value\":{\"stringValue\":\"%s\"}}", WIFI_SSID)) {
            return false;
        }
        
        // Continue building JSON
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                "]},"
                "\"scopeMetrics\":[{\"metrics\":[")) {
            return false;
        }
        
        // Add each metric
        for (uint8_t i = 0; i < metricCount; i++) {
            // Add comma if not the first metric
            if (i > 0) {
                if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), ",")) {
                    return false;
                }
            }
            
            // Add the metric point with its timestamp
            if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                    "{\"name\":\"%s\",\"gauge\":{\"dataPoints\":[{\"timeUnixNano\":\"%llu\",\"asDouble\":%.2f}]}}",
                    batchMetrics[i].name, batchMetrics[i].timestamp_nanos, batchMetrics[i].value)) {
                return false;
            }
        }
        
        // Close the JSON structure
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), "]}]}]}")) {
            return false;
        }
        
        debugLog("OpenTelemetry metrics payload created");

        return true;
    }
    
    // Create trace payload for limited number of completed spans
    bool createTracePayload() {
        if (!serviceName) serviceName = "default";
        if (!serviceVersion) serviceVersion = "0.0.0";
        
        // Count completed spans
        int completedSpanCount = 0;
        for (uint8_t i = 0; i < spanCount; i++) {
            if (!spans[i].isActive && spans[i].endTimeNanos > 0) {
                completedSpanCount++;
            }
        }
        
        if (completedSpanCount == 0) {
            return false; // No completed spans to send
        }
        
        // Limit the number of spans per batch to avoid buffer overflow
        // Use a smaller limit to ensure we don't overflow the buffer
        int spansToSend = min(completedSpanCount, MAX_SPANS_PER_BATCH);
        
        // To prevent buffer overflow, examine attribute density
        int totalAttributes = 0;
        for (uint8_t i = 0; i < spanCount; i++) {
            if (!spans[i].isActive && spans[i].endTimeNanos > 0) {
                totalAttributes += spans[i].attributeCount;
            }
        }
        
        // Calculate average attributes per span
        float avgAttributesPerSpan = completedSpanCount > 0 ? (float)totalAttributes / completedSpanCount : 0;
        
        // Adjust spans to send based on attribute density
        // Only limit spans if we have an extreme number of attributes (>15 per span on average)
        if (avgAttributesPerSpan > 15.0) {
            // Calculate how many spans we can send without overflowing
            // Each attribute takes up roughly 100 bytes in JSON
            // Buffer is 4096 bytes, with ~1000 bytes of overhead
            // So we have about 3000 bytes for attributes
            int maxAttributesInBatch = 3000 / 100; // ~30 attributes
            int maxSpansToSend = max(3, maxAttributesInBatch / max(1, (int)avgAttributesPerSpan));
            spansToSend = min(spansToSend, maxSpansToSend);
            
            debugLog("High attribute density (%.1f per span). Limiting batch to %d spans (total attrs: %d)", 
                    avgAttributesPerSpan, spansToSend, totalAttributes);
        } else {
            debugLog("Creating trace payload with %d/%d completed spans (total attrs: %d)", 
                    spansToSend, completedSpanCount, totalAttributes);
        }
        
        size_t pos = 0;
        
        // Start the JSON structure for traces
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), 
                "{\"resourceSpans\":[{\"resource\":{\"attributes\":[")) {
            return false;
        }
        
        // Add resource attributes
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"%s\"}},", serviceName)) {
            return false;
        }
        
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                "{\"key\":\"service.version\",\"value\":{\"stringValue\":\"%s\"}},", serviceVersion)) {
            return false;
        }
        
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                "{\"key\":\"wifi.ssid\",\"value\":{\"stringValue\":\"%s\"}}", WIFI_SSID)) {
            return false;
        }
        
        // Continue building JSON for scope spans
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                "]},"
                "\"scopeSpans\":[{\"scope\":{\"name\":\"iototeldemo\"},"
                "\"spans\":[")) {
            return false;
        }
        
        // Add completed spans (limited to MAX_SPANS_PER_BATCH)
        bool firstSpan = true;
        int spansSent = 0;
        
        for (uint8_t i = 0; i < spanCount && spansSent < spansToSend; i++) {
            if (spans[i].isActive || spans[i].endTimeNanos == 0) {
                continue; // Skip active or incomplete spans
            }
            
            // Add comma if not the first span
            if (!firstSpan) {
                if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), ",")) {
                    return false;
                }
            }
            firstSpan = false;
            
            // Format trace and span IDs as hex strings
            char traceIdHex[33]; // 128-bit (16 bytes) as 32 hex chars + null
            char spanIdHex[17];  // 64-bit (8 bytes) as 16 hex chars + null
            char parentSpanIdHex[17]; // 64-bit (8 bytes) as 16 hex chars + null
            
            // Convert IDs to hex strings
            sprintf(traceIdHex, "%016llx%016llx", spans[i].traceId[0], spans[i].traceId[1]);
            sprintf(spanIdHex, "%016llx", spans[i].spanId);
            sprintf(parentSpanIdHex, "%016llx", spans[i].parentSpanId);
            
            // Start span JSON
            if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                    "{\"traceId\":\"%s\",\"spanId\":\"%s\",", traceIdHex, spanIdHex)) {
                return false;
            }
            
            // Add parent span ID if there is one
            if (spans[i].parentSpanId != 0) {
                if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), 
                        "\"parentSpanId\":\"%s\",", parentSpanIdHex)) {
                    return false;
                }
            }
            
            // Add name, start and end times
            if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                    "\"name\":\"%s\",\"startTimeUnixNano\":\"%llu\",\"endTimeUnixNano\":\"%llu\",\"kind\":\"SPAN_KIND_INTERNAL\"",
                    spans[i].name, spans[i].startTimeNanos, spans[i].endTimeNanos)) {
                return false;
            }
            
            // Add attributes if there are any
            if (spans[i].attributeCount > 0) {
                if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), ",\"attributes\":[")) {
                    return false;
                }
                
                for (uint8_t j = 0; j < spans[i].attributeCount; j++) {
                    if (j > 0) {
                        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), ",")) {
                            return false;
                        }
                    }
                    
                    if (spans[i].attributes[j].isString) {
                        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                                "{\"key\":\"%s\",\"value\":{\"stringValue\":\"%s\"}}",
                                spans[i].attributes[j].key, spans[i].attributes[j].stringValue)) {
                            return false;
                        }
                    } else {
                        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer),
                                "{\"key\":\"%s\",\"value\":{\"doubleValue\":%.2f}}",
                                spans[i].attributes[j].key, spans[i].attributes[j].doubleValue)) {
                            return false;
                        }
                    }
                }
                
                if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), "]")) {
                    return false;
                }
            }
            
            // Close span JSON
            if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), "}")) {
                return false;
            }
            
            // Mark this span for removal after successful send
            spans[i].endTimeNanos = 1; // Special marker to remove this span
            spansSent++;
        }
        
        // Close the JSON structure
        if (!appendToBuffer(jsonBuffer, pos, sizeof(jsonBuffer), "]}]}]}")) {
            return false;
        }
        
        debugLog("OpenTelemetry trace payload created (%d bytes, %d spans)", pos, spansSent);
        return true;
    }
    
    // Generate a random 64-bit ID for trace and span IDs
    uint64_t generateRandomId() {
        // Seed the random number generator if not done already
        static bool randomSeeded = false;
        if (!randomSeeded) {
            randomSeed(randomSeedProvider());
            randomSeeded = true;
        }
        
        uint64_t id = 0;
        for (int i = 0; i < 8; i++) {
            id = (id << 8) | (uint8_t)random(256);
        }
        debugLog("Generated random ID: %016llx", id);
        return id;
    }
    
    // Remove spans marked for removal (those with endTimeNanos == 1)
    void removeMarkedSpans() {
        uint8_t newSpanCount = 0;
        uint8_t removed = 0;
        
        for (uint8_t i = 0; i < spanCount; i++) {
            if (!spans[i].isActive && spans[i].endTimeNanos == 1) {
                // Skip this span (it was sent)
                removed++;
            } else {
                // Keep this span
                if (i != newSpanCount) {
                    // Move the span to the new position
                    memcpy(&spans[newSpanCount], &spans[i], sizeof(Span));
                }
                newSpanCount++;
            }
        }
        
        spanCount = newSpanCount;
        if (removed > 0) {
            debugLog("Removed %d spans after successful trace send", removed);
        }
    }

    // Add this method to clean up old spans when we're getting close to the limit
    void cleanupOldSpans() {
        // If we've reached 60% of maximum capacity, force send completed traces
        if (spanCount >= (MAX_SPANS * 60 / 100)) {
            debugLog("Cleaning up spans, reached %d%% of capacity (%d/%d)", 
                    (spanCount * 100) / MAX_SPANS, spanCount, MAX_SPANS);
            sendTraces();
            
            // See if we still have too many spans
            if (spanCount >= (MAX_SPANS * 60 / 100)) {
                debugLog("Still have %d spans after sending traces", spanCount);
                
                // Count how many completed spans we have
                int completedCount = 0;
                for (uint8_t i = 0; i < spanCount; i++) {
                    if (!spans[i].isActive) {
                        completedCount++;
                    }
                }
                
                // If we have completed spans, remove them aggressively
                if (completedCount > 0) {
                    debugLog("Found %d completed spans to remove", completedCount);
                    
                    uint8_t newSpanCount = 0;
                    for (uint8_t i = 0; i < spanCount; i++) {
                        if (spans[i].isActive) {
                            // Keep active spans
                            if (i != newSpanCount) {
                                spans[newSpanCount] = spans[i];
                            }
                            newSpanCount++;
                        }
                    }
                    
                    uint8_t removedSpans = spanCount - newSpanCount;
                    spanCount = newSpanCount;
                    debugLog("Removed %d completed spans", removedSpans);
                }
                
                // If we still have too many spans, we have a leak of active spans
                if (spanCount >= (MAX_SPANS * 80 / 100)) {
                    debugLog("WARNING: Too many active spans (%d) - possible leak", spanCount);
                    
                    // Force end the oldest active spans
                    int activeEnded = 0;
                    for (uint8_t i = 0; i < spanCount && spanCount - activeEnded > (MAX_SPANS / 2); i++) {
                        if (spans[i].isActive) {
                            spans[i].isActive = false;
                            spans[i].endTimeNanos = getCurrentTimeNanos();
                            activeSpanCount--;
                            activeEnded++;
                            
                            debugLog("Force-ended active span: %s (ID: %016llx)", 
                                    spans[i].name, spans[i].spanId);
                        }
                    }
                    
                    if (activeEnded > 0) {
                        debugLog("Force-ended %d active spans to prevent memory leak", activeEnded);
                        
                        // Now send these ended spans
                        sendTraces();
                    }
                }
            }
        }
    }

public:
    OpenTelemetry() : serviceName(OTEL_SERVICE_NAME), serviceVersion(OTEL_SERVICE_VERSION), 
                     metricsEndpoint(OTEL_METRICS_URL), tracesEndpoint(OTEL_TRACES_URL),
                     lastHttpCode(0), metricCount(0), spanCount(0), activeSpanCount(0) {
        memset(currentTraceId, 0, sizeof(currentTraceId));
        debugLog("OpenTelemetry instance created");
        // Random seed will be initialized in generateRandomId when needed
    }
    
    void begin(const char* svcName, const char* svcVersion, const char* metEndpoint, const char* traceEndpoint = nullptr) {
        serviceName = svcName;
        serviceVersion = svcVersion;
        metricsEndpoint = metEndpoint;
        
        // If a separate trace endpoint is provided, use it, otherwise use the metrics endpoint
        if (traceEndpoint && strlen(traceEndpoint) > 0) {
            tracesEndpoint = traceEndpoint;
        } else {
            tracesEndpoint = metEndpoint;
        }
        
        lastErrorMessage = "None";
        lastHttpCode = 0;
        metricCount = 0;
        spanCount = 0;
        activeSpanCount = 0;
        
        // Initialize current trace ID
        currentTraceId[0] = generateRandomId();
        currentTraceId[1] = generateRandomId();
        
        debugLog("OpenTelemetry initialized with metrics endpoint: %s", metricsEndpoint);
        debugLog("OpenTelemetry initialized with traces endpoint: %s", tracesEndpoint);
    }
    
    bool addMetric(const char* name, double value, uint64_t timestamp_nanos) {
        if (metricCount >= MAX_METRICS) {
            debugLog("Warning: Maximum metrics count reached (%d). Metric not added.", MAX_METRICS);
            return false;
        }
        
        batchMetrics[metricCount++] = MetricPoint(name, value, timestamp_nanos);
        return true;
    }
    
    // Start a new trace (resets the current trace ID)
    void startNewTrace() {
        // Generate new trace ID
        currentTraceId[0] = generateRandomId();
        currentTraceId[1] = generateRandomId();
        
        char traceIdHex[33];
        sprintf(traceIdHex, "%016llx%016llx", currentTraceId[0], currentTraceId[1]);
        debugLog("Started new trace: %s (parts: %016llx %016llx)", 
                 traceIdHex, currentTraceId[0], currentTraceId[1]);
    }
    
    // Start a new span with the given name
    uint64_t startSpan(const char* name, uint64_t parentSpanId = 0) {
        // Clean up old spans if we're getting close to the limit
        if (spanCount >= (MAX_SPANS * 3 / 4)) {
            debugLog("Warning: Span count high (%d/%d), cleaning up old spans", spanCount, MAX_SPANS);
            cleanupOldSpans();
        }
        
        if (spanCount >= MAX_SPANS) {
            debugLog("Warning: Maximum span count reached (%d). Span not created.", MAX_SPANS);
            return 0;
        }
        
        // Generate span ID
        uint64_t spanId = generateRandomId();
        debugLog("Generated new span ID: %016llx", spanId);
        
        // Create new span
        Span& span = spans[spanCount++];
        strncpy(span.name, name, sizeof(span.name) - 1);
        span.name[sizeof(span.name) - 1] = '\0';
        span.traceId[0] = currentTraceId[0];
        span.traceId[1] = currentTraceId[1];
        span.spanId = spanId;
        span.parentSpanId = parentSpanId;
        span.startTimeNanos = getCurrentTimeNanos();
        span.endTimeNanos = 0;
        span.attributeCount = 0;
        span.isActive = true;
        
        activeSpanCount++;
        
        // Get trace ID as hex for logging
        char traceIdHex[33];
        sprintf(traceIdHex, "%016llx%016llx", span.traceId[0], span.traceId[1]);
        debugLog("Started span [%s] id=%016llx parent=%016llx trace=%s (parts: %016llx %016llx) (total=%d, active=%d)", 
                 name, spanId, parentSpanId, traceIdHex, span.traceId[0], span.traceId[1], spanCount, activeSpanCount);
        
        return spanId;
    }
    
    // Add string attribute to a span
    bool addSpanAttribute(uint64_t spanId, const char* key, const char* value) {
        if (spanId == 0) {
#ifdef OTEL_DEBUG_VERBOSE
            debugLog("Warning: Cannot add attribute to invalid span ID 0");
#endif
            return false;
        }

        for (uint8_t i = 0; i < spanCount; i++) {
            if (spans[i].spanId == spanId) {
                if (!spans[i].isActive) {
#ifdef OTEL_DEBUG_VERBOSE
                    // Get trace ID as hex for logging
                    char traceIdHex[33];
                    getCurrentTraceIdHex(traceIdHex, sizeof(traceIdHex));
                    debugLog("Warning: Cannot add attribute '%s' to ended span [%s] id=%016llx trace=%s", 
                             key, spans[i].name, spanId, traceIdHex);
#endif
                    return false;
                }
                
                if (spans[i].attributeCount >= MAX_SPAN_ATTRS) {
                    // Get trace ID as hex for logging
                    char traceIdHex[33];
                    getCurrentTraceIdHex(traceIdHex, sizeof(traceIdHex));
                    debugLog("Warning: Maximum attributes reached for span [%s] id=%016llx trace=%s", 
                             spans[i].name, spanId, traceIdHex);
                    return false;
                }
                
                SpanAttribute& attr = spans[i].attributes[spans[i].attributeCount++];
                attr.key = key;
                attr.stringValue = value;
                attr.doubleValue = 0;
                attr.isString = true;
                
#ifdef OTEL_DEBUG_VERBOSE
                // Get trace ID as hex for logging
                char traceIdHex[33];
                getCurrentTraceIdHex(traceIdHex, sizeof(traceIdHex));
                debugLog("Added attribute %s=\"%s\" to span [%s] id=%016llx trace=%s", 
                         key, value, spans[i].name, spanId, traceIdHex);
#endif
                return true;
            }
        }

#ifdef OTEL_DEBUG_VERBOSE
        debugLog("Warning: Span not found: %016llx", spanId);
#endif
        return false;
    }
    
    // Add numeric attribute to a span
    bool addSpanAttribute(uint64_t spanId, const char* key, double value) {
        if (spanId == 0) {
#ifdef OTEL_DEBUG_VERBOSE
            debugLog("Warning: Cannot add attribute to invalid span ID 0");
#endif
            return false;
        }

        for (uint8_t i = 0; i < spanCount; i++) {
            if (spans[i].spanId == spanId) {
                if (!spans[i].isActive) {
#ifdef OTEL_DEBUG_VERBOSE
                    // Get trace ID as hex for logging
                    char traceIdHex[33];
                    getCurrentTraceIdHex(traceIdHex, sizeof(traceIdHex));
                    debugLog("Warning: Cannot add attribute '%s' to ended span [%s] id=%016llx trace=%s", 
                             key, spans[i].name, spanId, traceIdHex);
#endif
                    return false;
                }
                
                if (spans[i].attributeCount >= MAX_SPAN_ATTRS) {
                    // Get trace ID as hex for logging
                    char traceIdHex[33];
                    getCurrentTraceIdHex(traceIdHex, sizeof(traceIdHex));
                    debugLog("Warning: Maximum attributes reached for span [%s] id=%016llx trace=%s", 
                             spans[i].name, spanId, traceIdHex);
                    return false;
                }
                
                SpanAttribute& attr = spans[i].attributes[spans[i].attributeCount++];
                attr.key = key;
                attr.stringValue = nullptr;
                attr.doubleValue = value;
                attr.isString = false;
                
#ifdef OTEL_DEBUG_VERBOSE
                // Get trace ID as hex for logging
                char traceIdHex[33];
                getCurrentTraceIdHex(traceIdHex, sizeof(traceIdHex));
                debugLog("Added attribute %s=%f to span [%s] id=%016llx trace=%s", 
                         key, value, spans[i].name, spanId, traceIdHex);
#endif
                return true;
            }
        }

#ifdef OTEL_DEBUG_VERBOSE
        debugLog("Warning: Span not found: %016llx", spanId);
#endif
        return false;
    }
    
    // End a span with the given ID
    bool endSpan(uint64_t spanId) {
        if (spanId == 0) {
            debugLog("Warning: Ignoring attempt to end invalid span ID 0");
            return false;
        }

        for (uint8_t i = 0; i < spanCount; i++) {
            if (spans[i].spanId == spanId) {
                // Check if span is active
                if (!spans[i].isActive) {
                    debugLog("Warning: Span %016llx [%s] already ended", spanId, spans[i].name);
                    return false;
                }
                
                spans[i].isActive = false;
                spans[i].endTimeNanos = getCurrentTimeNanos();
                activeSpanCount--;
                
                // Get trace ID as hex for logging
                char traceIdHex[33];
                sprintf(traceIdHex, "%016llx%016llx", spans[i].traceId[0], spans[i].traceId[1]);
                
                uint64_t durationMicros = (spans[i].endTimeNanos - spans[i].startTimeNanos) / 1000;
                debugLog("Ended span [%s] id=%016llx trace=%s duration=%llu µs (total=%d, active=%d)", 
                         spans[i].name, spanId, traceIdHex, durationMicros, spanCount, activeSpanCount);
                return true;
            }
        }

        debugLog("Warning: Span not found or not active: %016llx", spanId);
        return false;
    }
    
    // Send completed traces
    bool sendTraces() {
        // Make sure we have completed spans to send
        bool hasCompletedSpans = false;
        int completedSpanCount = 0;
        for (uint8_t i = 0; i < spanCount; i++) {
            if (!spans[i].isActive && spans[i].endTimeNanos > 0) {
                hasCompletedSpans = true;
                completedSpanCount++;
                debugLog("Found completed span [%s] id=%016llx endTime=%llu", 
                        spans[i].name, spans[i].spanId, spans[i].endTimeNanos);
            }
        }
        
        if (!hasCompletedSpans) {
            debugLog("No completed spans to send (total spans: %d, active: %d)", 
                    spanCount, activeSpanCount);
            return true; // No spans to send is not an error
        }
        
        debugLog("Found %d completed spans to send", completedSpanCount);
        
        // Make sure we have a valid endpoint
        if (!tracesEndpoint || strlen(tracesEndpoint) == 0) {
            lastErrorMessage = "No endpoint specified";
            debugLog("Error: %s", lastErrorMessage.c_str());
            return false;
        }
        
        debugLog("Using traces endpoint: %s", tracesEndpoint);
        
        // Make sure JSON buffer is initialized
        memset(jsonBuffer, 0, sizeof(jsonBuffer));
        
        // Create the JSON payload
        if (!createTracePayload()) {
            lastErrorMessage = "Failed to create trace payload";
            debugLog("Error: %s", lastErrorMessage.c_str());
            return false;
        }
        
        // Log the complete payload for debugging
        debugLog("Complete trace payload (%d bytes):", strlen(jsonBuffer));
        // Split the payload into chunks to avoid truncation
        const char* payload = jsonBuffer;
        int remaining = strlen(payload);
        int offset = 0;
        while (remaining > 0) {
            int chunkSize = min(200, remaining);
            char chunk[201];
            strncpy(chunk, payload + offset, chunkSize);
            chunk[chunkSize] = '\0';
            debugLog("%s", chunk);
            remaining -= chunkSize;
            offset += chunkSize;
        }
        
        // Send the data
        http.setTimeout(10000); // 10 second timeout for trace data
        http.begin(tracesEndpoint);
        http.addHeader("Content-Type", "application/json");
        
        // Log the complete request details
        debugLog("HTTP Request Details:");
        debugLog("POST %s", tracesEndpoint);
        debugLog("Headers:");
        debugLog("  Content-Type: application/json");
        debugLog("Body:");
        // Split the body into chunks to avoid truncation
        payload = jsonBuffer;
        remaining = strlen(payload);
        offset = 0;
        while (remaining > 0) {
            int chunkSize = min(200, remaining);
            char chunk[201];
            strncpy(chunk, payload + offset, chunkSize);
            chunk[chunkSize] = '\0';
            debugLog("%s", chunk);
            remaining -= chunkSize;
            offset += chunkSize;
        }
        
        // Send the request
        int httpCode = http.POST(jsonBuffer);
        lastHttpCode = httpCode;
        
        // Check for success (HTTP 200-299)
        bool success = (httpCode >= 200 && httpCode < 300);
        
        if (success) {
            debugLog("OpenTelemetry traces sent successfully (HTTP %d)", httpCode);
            debugLog("Response body: %s", http.getString().c_str());
            
            // Mark sent spans for cleanup
            int spansMoved = 0;
            for (uint8_t i = 0; i < spanCount; i++) {
                if (!spans[i].isActive && spans[i].endTimeNanos > 0) {
                    spans[i].spanId = 0; // Mark for cleanup
                    spansMoved++;
                    debugLog("Marked span [%s] id=%016llx for cleanup", 
                            spans[i].name, spans[i].spanId);
                    
                    // Avoid processing too many in one batch
                    if (spansMoved >= MAX_SPANS_PER_BATCH) {
                        break;
                    }
                }
            }
            
            // Clean up spans that were sent
            removeMarkedSpans();
            
            // If we still have more spans to send, send another batch
            for (uint8_t i = 0; i < spanCount; i++) {
                if (!spans[i].isActive && spans[i].endTimeNanos > 0) {
                    debugLog("More spans to send. Recursively calling sendTraces()");
                    sendTraces(); // Recursive call to send more spans
                    break;
                }
            }
            
            return true;
        } else {
            // Record error and log it
            lastErrorMessage = http.errorToString(httpCode);
            if (httpCode > 0) {
                String response = http.getString();
                debugLog("OpenTelemetry trace send failed: HTTP error %d: %s", httpCode, lastErrorMessage.c_str());
                debugLog("Response: %s", response.c_str());
            } else {
                debugLog("OpenTelemetry trace send failed: Connection error: %s", lastErrorMessage.c_str());
            }
            
            http.end();
            return false;
        }
    }
    
    bool sendMetrics() {
        if (metricCount == 0) {
            lastErrorMessage = "No metrics to send";
            debugLog("Cannot send metrics - No metrics in batch");
            return false;
        }

        if (!WiFi.isConnected()) {
            lastErrorMessage = "WiFi not connected";
            lastHttpCode = 0;
            debugLog("Cannot send metrics - WiFi not connected");
            return false;
        }
        
        // Create the JSON payload in our pre-allocated buffer
        if (!createBatchPayload()) {
            lastErrorMessage = "Failed to create payload (buffer overflow)";
            debugLog("Failed to create metrics payload - Buffer overflow");
            return false;
        }
        
        // Check if WiFi is still connected before sending
        if (WiFi.status() != WL_CONNECTED) {
            lastErrorMessage = "WiFi disconnected before send";
            lastHttpCode = 0;
            debugLog("Cannot send metrics - WiFi disconnected before sending");
            return false;
        }
        
        // Send the HTTP request
        http.begin(metricsEndpoint);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(10000); // Increase timeout to 10 seconds
        
        debugLog("Sending metrics data (%d bytes)...", strlen(jsonBuffer));
        unsigned long startTime = millis();
        lastHttpCode = http.POST(jsonBuffer);
        unsigned long sendTime = millis() - startTime;
        
        if (lastHttpCode < 200 || lastHttpCode >= 300) {
            lastErrorMessage = http.getString();
            if (lastErrorMessage.length() == 0) {
                lastErrorMessage = String("HTTP Error ") + lastHttpCode;
            }
            debugLog("Failed to send metrics: HTTP %d (%lums): %s", 
                    lastHttpCode, sendTime, lastErrorMessage.c_str());
        } else {
            lastErrorMessage = "None";
            debugLog("Metrics sent successfully in %lums (HTTP %d)", sendTime, lastHttpCode);
        }
        http.end();
        
        // Reset metrics count
        metricCount = 0;
        
        return lastHttpCode >= 200 && lastHttpCode < 300;
    }
    
    // Combined function to send both metrics and traces
    bool sendMetricsAndTraces() {
        bool metricsSuccess = false;
        bool tracesSuccess = false;
        
        if (!WiFi.isConnected()) {
            lastErrorMessage = "WiFi not connected";
            lastHttpCode = 0;
            debugLog("Cannot send metrics/traces - WiFi not connected");
            return false;
        }
        
        // Check if we have a valid endpoint
        if (!hasValidTracesEndpoint()) {
            lastErrorMessage = "No endpoint specified";
            lastHttpCode = 0;
            debugLog("Cannot send metrics/traces - No endpoint specified");
            return false;
        }
        
        debugLog("--- Starting combined metrics and traces send operation ---");
        
        // Log the current trace ID for debugging
        char traceIdHex[33];
        sprintf(traceIdHex, "%016llx%016llx", currentTraceId[0], currentTraceId[1]);
        debugLog("Current trace ID: %s", traceIdHex);
        
        // Count completed spans and metrics
        int completedSpanCount = 0;
        for (uint8_t i = 0; i < spanCount; i++) {
            if (!spans[i].isActive && spans[i].endTimeNanos > 0) {
                completedSpanCount++;
            }
        }
        
        debugLog("Sending %d metrics with %d spans queued...", metricCount, completedSpanCount);
        
        // First send metrics
        if (metricCount > 0) {
            metricsSuccess = sendMetrics();
            if (!metricsSuccess) {
                debugLog("Failed to send metrics: %s", lastErrorMessage.c_str());
            } else {
                debugLog("Metrics sent successfully");
            }
        } else {
            debugLog("No metrics to send");
            metricsSuccess = true; // No metrics is not an error
        }
        
        // Then send traces if there are any completed spans
        if (completedSpanCount > 0) {
            tracesSuccess = sendTraces();
            if (!tracesSuccess) {
                debugLog("Failed to send traces: %s", lastErrorMessage.c_str());
            } else {
                debugLog("Traces sent successfully");
            }
        } else {
            debugLog("No completed spans to send");
            tracesSuccess = true; // No spans is not an error
        }
        
        return metricsSuccess && tracesSuccess;
    }

    const char* getLastError() {
        return lastErrorMessage.c_str();
    }

    int getLastHttpCode() {
        return lastHttpCode;
    }
    
    // Gets the current trace ID as a hex string
    void getCurrentTraceIdHex(char* buffer, size_t bufferSize) {
        if (!buffer || bufferSize < 33) {
            debugLog("Error: Invalid buffer for trace ID hex conversion");
            if (buffer && bufferSize > 0) {
                buffer[0] = '\0';
            }
            return;
        }
        
        if (currentTraceId[0] == 0 && currentTraceId[1] == 0) {
            strncpy(buffer, "no_active_trace", bufferSize - 1);
            buffer[bufferSize - 1] = '\0';
            return;
        }
        
        // Format as 32-character hex string (16 bytes total, 8 bytes per uint64_t)
        snprintf(buffer, bufferSize, "%016llx%016llx", 
                 (unsigned long long)currentTraceId[0], 
                 (unsigned long long)currentTraceId[1]);
    }
    
    // Helper methods for common patterns
    
    // NOTE: The template-based traced call function was removed due to
    // C++ language compatibility issues. Use the startSpan/endSpan pattern directly instead.

    // Get span statistics for debugging
    void getSpanStats(uint8_t& total, uint8_t& active, uint8_t& completed) {
        total = spanCount;
        active = activeSpanCount;
        completed = spanCount - activeSpanCount;
    }
    
    // For debugging, print span information
    void debugSpans() {
        debugLog("Current span count: %d (Active: %d, Completed: %d)", 
                spanCount, activeSpanCount, spanCount - activeSpanCount);
        
        // Print details of all spans
        debugLog("Active spans:");
        int activeCount = 0;
        for (uint8_t i = 0; i < spanCount; i++) {
            if (spans[i].isActive) {
                debugLog("  %d: %s (ID: %016llx, Parent: %016llx)", 
                       i, spans[i].name, spans[i].spanId, spans[i].parentSpanId);
                activeCount++;
                if (activeCount >= 5) {
                    debugLog("  ... and %d more active spans", activeSpanCount - 5);
                    break;
                }
            }
        }
        
        if (activeCount == 0) {
            debugLog("  (None)");
        }
        
        debugLog("Completed spans (up to 5):");
        int completedCount = 0;
        for (uint8_t i = 0; i < spanCount; i++) {
            if (!spans[i].isActive) {
                debugLog("  %d: %s (ID: %016llx, endTime: %llu)", 
                       i, spans[i].name, spans[i].spanId, spans[i].endTimeNanos);
                completedCount++;
                if (completedCount >= 5) {
                    debugLog("  ... and %d more completed spans", spanCount - activeSpanCount - 5);
                    break;
                }
            }
        }
        
        if (completedCount == 0) {
            debugLog("  (None)");
        }
    }

    // Debug function to examine all span attributes
    void debugSpanAttributes() {
        debugLog("------ Span Attribute Analysis ------");
        
        // Count attribute usage
        int totalAttributes = 0;
        
        // Map to store attribute key frequencies (simplified)
        struct KeyCount {
            const char* key;
            int count;
        };
        
        KeyCount keyCounts[20]; // Track up to 20 different attribute keys
        int uniqueKeys = 0;
        
        // Scan all spans to count attributes
        for (uint8_t i = 0; i < spanCount; i++) {
            if (spans[i].attributeCount > 0) {
                totalAttributes += spans[i].attributeCount;
                
                // Log spans with many attributes
                if (spans[i].attributeCount > 5) {
                    debugLog("Span [%s] id=%016llx has %d attributes:", 
                          spans[i].name, spans[i].spanId, spans[i].attributeCount);
                    
                    // Show first few attributes
                    for (uint8_t j = 0; j < min(spans[i].attributeCount, (uint8_t)5); j++) {
                        if (spans[i].attributes[j].isString) {
                            debugLog("  - %s = \"%s\"", 
                                  spans[i].attributes[j].key, 
                                  spans[i].attributes[j].stringValue);
                        } else {
                            debugLog("  - %s = %f", 
                                  spans[i].attributes[j].key, 
                                  spans[i].attributes[j].doubleValue);
                        }
                    }
                    
                    if (spans[i].attributeCount > 5) {
                        debugLog("  - and %d more attributes", spans[i].attributeCount - 5);
                    }
                }
                
                // Count attribute key frequencies
                for (uint8_t j = 0; j < spans[i].attributeCount; j++) {
                    const char* key = spans[i].attributes[j].key;
                    bool found = false;
                    
                    for (int k = 0; k < uniqueKeys; k++) {
                        if (strcmp(keyCounts[k].key, key) == 0) {
                            keyCounts[k].count++;
                            found = true;
                            break;
                        }
                    }
                    
                    if (!found && uniqueKeys < 20) {
                        keyCounts[uniqueKeys].key = key;
                        keyCounts[uniqueKeys].count = 1;
                        uniqueKeys++;
                    }
                }
            }
        }
        
        float avgAttributes = (float)totalAttributes / spanCount;
        debugLog("Total attributes: %d across %d spans (avg: %.1f per span)", 
                totalAttributes, spanCount, avgAttributes);
        
        // Show most common attribute keys
        debugLog("Most common attribute keys:");
        for (int i = 0; i < uniqueKeys; i++) {
            debugLog("  - %s: %d occurrences", keyCounts[i].key, keyCounts[i].count);
        }
        
        debugLog("------------------------------------");
    }

    // Method to explicitly initialize or update the metrics endpoint
    void initializeMetricsEndpoint(const char* newEndpoint) {
        if (newEndpoint && strlen(newEndpoint) > 0) {
            metricsEndpoint = newEndpoint;
            debugLog("OpenTelemetry metrics endpoint initialized: %s", metricsEndpoint);
        } else {
            debugLog("Warning: Attempted to initialize metrics endpoint with NULL or empty string");
        }
    }
    
    // Method to explicitly initialize or update the traces endpoint
    void initializeTracesEndpoint(const char* newEndpoint) {
        if (newEndpoint && strlen(newEndpoint) > 0) {
            tracesEndpoint = newEndpoint;
            debugLog("OpenTelemetry traces endpoint initialized: %s", tracesEndpoint);
        } else {
            debugLog("Warning: Attempted to initialize traces endpoint with NULL or empty string");
        }
    }
    
    // Legacy compatibility method
    void initializeEndpoint(const char* newEndpoint) {
        initializeMetricsEndpoint(newEndpoint);
    }
    
    // Check if metrics endpoint is valid
    bool hasValidMetricsEndpoint() const {
        return metricsEndpoint && strlen(metricsEndpoint) > 0;
    }
    
    // Check if traces endpoint is valid
    bool hasValidTracesEndpoint() const {
        return tracesEndpoint && strlen(tracesEndpoint) > 0;
    }
    
    // Legacy compatibility method
    bool hasValidEndpoint() const {
        return hasValidMetricsEndpoint();
    }

    // Set custom time provider
    void setTimeProvider(TimeProviderFunc provider) {
        if (provider != nullptr) {
            timeProvider = provider;
            debugLog("Custom time provider set");
        } else {
            timeProvider = defaultTimeProvider;
            debugLog("Reset to default time provider");
        }
    }
    
    // Set custom random seed provider
    void setRandomSeedProvider(RandomSeedProviderFunc provider) {
        if (provider != nullptr) {
            randomSeedProvider = provider;
            debugLog("Custom random seed provider set");
        } else {
            randomSeedProvider = defaultRandomSeedProvider;
            debugLog("Reset to default random seed provider");
        }
    }
    
    // Get current timestamp using the registered provider
    uint64_t getCurrentTimeNanos() {
        return timeProvider();
    }
    
    // Function to safely flush OpenTelemetry traces with maximum error protection
    bool safeFlushTraces() {
        debugLog("Attempting to safely flush traces with maximum error protection");
        
        bool success = false;
        
        try {
            // Make sure we have a valid endpoint
            if (!hasValidTracesEndpoint()) {
                debugLog("Error: No valid endpoint for traces");
                return false;
            }
            
            // First check if we have any spans to send
            uint8_t total = 0, active = 0, completed = 0;
            
            // Get span stats - this should be safe
            try {
                getSpanStats(total, active, completed);
            } catch (...) {
                debugLog("Error getting span stats - assuming no spans to send");
                return true; // Consider it a success if we can't even get stats
            }
            
            if (completed == 0) {
                debugLog("No completed spans to send");
                return true; // Success - nothing to do
            }
            
            // Try to send traces with robust error handling
            try {
                success = sendTraces();
            } catch (...) {
                debugLog("Exception during trace sending - continuing without error");
                success = false; // Mark as failure
            }
            
            if (success) {
                debugLog("Traces sent successfully");
            } else {
                debugLog("Failed to send traces, but handled error gracefully: %s", getLastError());
            }
        } catch (...) {
            debugLog("Unhandled exception in safeFlushTraces - continuing execution");
            success = false;
        }
        
        return success;
    }
    
    // Function to safely send both metrics and traces with maximum error protection
    bool safeSendMetricsAndTraces() {
        debugLog("Attempting to safely send metrics and traces with maximum error protection");
        
        bool success = false;
        
        try {
            // Make sure we have valid endpoints
            if (!hasValidMetricsEndpoint()) {
                debugLog("Error: No valid endpoint for metrics");
                return false;
            }
            
            if (!hasValidTracesEndpoint()) {
                debugLog("Error: No valid endpoint for traces");
                return false;
            }
            
            // First check if we have any spans to send
            uint8_t total = 0, active = 0, completed = 0;
            
            // Get span stats - this should be safe
            try {
                getSpanStats(total, active, completed);
                debugLog("Span stats: Total=%d, Active=%d, Completed=%d", total, active, completed);
            } catch (...) {
                debugLog("Error getting span stats - continuing with send operation");
            }
            
            // Try to send metrics and traces with robust error handling
            try {
                success = sendMetricsAndTraces();
            } catch (...) {
                debugLog("Exception during metrics/traces sending - continuing without error");
                success = false; // Mark as failure
            }
            
            if (success) {
                debugLog("Metrics and traces sent successfully");
            } else {
                debugLog("Failed to send metrics and traces, but handled error gracefully: %s", 
                        getLastError());
            }
        } catch (...) {
            debugLog("Unhandled exception in safeSendMetricsAndTraces - continuing execution");
            success = false;
        }
        
        return success;
    }
};

#endif

