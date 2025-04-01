# OpenTelemetry Library for Arduino

## Introduction

Embedded devices have historically been a blind spot in modern observability stacks. Standard OpenTelemetry libraries are too resource-intensive for constrained IoT hardware, leaving a gap in end-to-end observability.

This library implements the OpenTelemetry protocol for Arduino-compatible microcontrollers, with a focus on IoT applications. It's possibly the first to enable resource-constrained embedded devices to emit OpenTelemetry traces and metrics using the OTLP protocol, bringing IoT devices into modern observability ecosystems.

Designed for hobbyist embedded developers and DevOps engineers who want to extend observability to their IoT projects without requiring deep expertise in observability protocols, this library provides a memory-optimized solution that balances functionality with resource constraints.

## What is OpenTelemetry?

OpenTelemetry (OTel) is an open-source observability framework for generating, collecting, and exporting telemetry data (metrics, logs, and traces). This library implements the OpenTelemetry Protocol (OTLP) for embedded devices, focusing on:

- **Metrics**: Numerical measurements over time (temperature, voltage, memory usage)
- **Traces**: Distributed traces showing the path of operations across systems
- **Attributes**: Key-value pairs adding context to metrics and traces

With this library, your ESP32 or similar device can send standardized telemetry to modern observability platforms like Splunk Observability Cloud, Grafana, New Relic, Datadog, and others that support the OpenTelemetry protocol.

## Features

- Send metrics to OpenTelemetry collectors via HTTP
- Create and manage traces with parent-child span relationships
- Add attributes to spans
- Batch multiple metrics in a single request
- Automatic trace ID generation
- Memory-optimized for resource-constrained devices
- Debug logging support
- Customizable time provider for accurate timestamps
- Customizable random seed provider for platform-specific entropy sources
- Robust error handling with safety wrappers

## Real-World Applications

This library enables numerous IoT observability scenarios:

- **Smart Home Monitoring**: Track temperature, humidity, and power usage patterns across multiple sensors with distributed tracing
- **Industrial IoT**: Monitor equipment performance and trace operations across manufacturing processes
- **Environmental Monitoring**: Collect and analyze environmental data with proper timestamps and context
- **Remote Diagnostics**: Troubleshoot device issues by tracing execution paths and performance bottlenecks
- **Battery Optimization**: Track power consumption patterns to optimize battery life

## Resource Consumption

This library is designed for resource-constrained devices:

- **Memory Usage**: Approximately 4-8KB RAM depending on configuration
- **Flash Usage**: Approximately 12-15KB of program memory
- **CPU Impact**: Minimal when using default settings; increases with logging verbosity
- **Battery Impact**: Configurable sending intervals to balance freshness vs. battery life
- **Network Usage**: Optimized payloads to reduce bandwidth consumption

These values may vary based on your specific hardware and configuration.

## Why Embedded OpenTelemetry Matters

Bringing OpenTelemetry to embedded devices bridges a critical gap in observability:

1. **Unified Observability**: Use the same tools and practices across cloud, edge, and embedded systems
2. **Democratizing IoT Analytics**: Access enterprise-grade analytics for hobbyist projects
3. **Simplified Debugging**: Trace issues across distributed IoT systems without proprietary solutions
4. **Future-Proofing**: Adopt the emerging standard for observability instrumentation
5. **Community Contribution**: Join the growing ecosystem of OpenTelemetry contributors and users

## Installation

1. Copy `opentelemetry.h` to your project's `src` directory
2. Include the required dependencies:
   - Arduino.h (for basic types and functions like millis())
   - WiFi.h (for network connectivity)
   - HTTPClient.h (for HTTP requests)
   - debug.h (for logging via debugLog function)
3. Configure your OpenTelemetry endpoints in `config.h`

The library relies on several core Arduino functions:
- `millis()` for default time tracking
- `random()` and `randomSeed()` for ID generation
- WiFi connectivity for sending data
- HTTP client for communicating with OpenTelemetry endpoints

### Debug.h Implementation

You'll need to provide a `debug.h` file with a `debugLog` function. Here's a simple example implementation:

```cpp
// debug.h - Simple debug logging implementation
#ifndef DEBUG_H
#define DEBUG_H

#include <Arduino.h>

// Debug control - uncomment to enable debug output
#define OTEL_DEBUG
//#define OTEL_DEBUG_VERBOSE  // Enables more detailed debug output

// Debug log function - prints formatted message with timestamp
void debugLog(const char* format, ...) {
#ifdef OTEL_DEBUG
    // Get current timestamp
    char timestamp[20];
    unsigned long ms = millis();
    unsigned long seconds = ms / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    
    sprintf(timestamp, "[%02lu:%02lu:%02lu.%03lu]", 
            hours % 24, minutes % 60, seconds % 60, ms % 1000);
    
    // Print timestamp
    Serial.print(timestamp);
    Serial.print(" [DEBUG] ");
    
    // Format and print the message
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    Serial.println(buffer);
#endif
}

#endif // DEBUG_H
```

You can customize this implementation to fit your needs, such as logging to a different output, adding log levels, or storing logs in flash memory.

## Configuration

The library requires the following configuration in your `config.h` file:

```cpp
// OpenTelemetry Configuration
#define OTEL_HOST "your.otel.host"           // Hostname/IP of your OpenTelemetry collector
#define OTEL_PORT "4318"                     // Port for OTLP/HTTP (ensure this stays in quotes)
#define OTEL_PATH "/v1/metrics"              // Path for metrics endpoint
#define OTEL_TRACES_PATH "/v1/traces"        // Path for traces endpoint
#define OTEL_SERVICE_NAME "m5stack-sensor"   // Service name to report
#define OTEL_SERVICE_VERSION "1.0.0"         // Service version to report

// Debug configuration
#define OTEL_DEBUG                           // Enable basic debug logging
//#define OTEL_DEBUG_VERBOSE                 // Enable verbose debug logging (uncomment if needed)

// Construct full URLs for OpenTelemetry endpoints
#define OTEL_METRICS_URL "http://" OTEL_HOST ":" OTEL_PORT OTEL_PATH
#define OTEL_TRACES_URL "http://" OTEL_HOST ":" OTEL_PORT OTEL_TRACES_PATH
```

## Memory Usage Constants

You can adjust the following constants to balance memory usage and functionality:

```cpp
#define MAX_METRICS 15           // Maximum number of metrics in a batch
#define MAX_SPANS 50             // Maximum number of spans to track
#define MAX_SPANS_PER_BATCH 15   // Maximum number of spans to send in one request
#define MAX_SPAN_ATTRS 10        // Maximum number of attributes per span
```

## Basic Usage

### Initialization

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "debug.h"
#include "opentelemetry.h"

// Create an instance
OpenTelemetry otel;

void setup() {
  Serial.begin(115200);
  debugLog("Starting OpenTelemetry example...");
  
  // Initialize with custom service info and endpoints (optional)
  otel.begin("my-service", "1.0.0", "http://otel-collector:4318/v1/metrics", "http://otel-collector:4318/v1/traces");
  
  // Or use the default values from config.h
  // otel.begin();
  
  // Set a custom time provider function (optional)
  otel.setTimeProvider(myCustomTimeFunction);
  
  // Set a custom random seed provider function (optional)
  otel.setRandomSeedProvider(myCustomRandomSeedFunction);
}

// Custom time provider function - returns nanoseconds since epoch
uint64_t myCustomTimeFunction() {
  // Example: get timestamp from RTC
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    time_t now = mktime(&timeinfo);
    return (uint64_t)now * 1000000000ULL;
  }
  return millis() * 1000000ULL; // Fallback
}

// Custom random seed provider function - returns 32-bit entropy value
uint32_t myCustomRandomSeedFunction() {
  // Example: get entropy from a hardware random number generator
  return myHardwareRNG.getRandomValue();
}
```

### Sending Metrics

```cpp
// Add metrics to the batch
otel.addMetric("temperature", 25.5, otel.getCurrentTimeNanos());
otel.addMetric("humidity", 65.0, otel.getCurrentTimeNanos());
otel.addMetric("pressure", 1013.25, otel.getCurrentTimeNanos());

// Send the metrics with robust error handling
bool success = otel.safeSendMetricsAndTraces();
if (!success) {
  Serial.print("Failed to send metrics: ");
  Serial.println(otel.getLastError());
}
```

### Working with Traces

```cpp
// Start a new trace
otel.startNewTrace();

// Start a root span
uint64_t rootSpanId = otel.startSpan("main_operation");

// Add attributes to the span
otel.addSpanAttribute(rootSpanId, "operation.name", "data_collection");
otel.addSpanAttribute(rootSpanId, "temperature", 25.5);

// Create a child span
uint64_t childSpanId = otel.startSpan("sub_operation", rootSpanId);

// Perform operations...

// End the child span
otel.endSpan(childSpanId);

// End the root span
otel.endSpan(rootSpanId);

// Send the traces with robust error handling
otel.safeFlushTraces();
```

### Combined Metrics and Traces

```cpp
// Add metrics and create spans as shown above

// Send both metrics and traces in one operation with robust error handling
bool success = otel.safeSendMetricsAndTraces();
if (!success) {
  Serial.print("Failed to send data: ");
  Serial.println(otel.getLastError());
}
```

## Visualizing Your Data

Once your device is sending OpenTelemetry data, you'll need a backend to collect and visualize it:

### Setting Up a Local Collector

For development and testing, you can run a local OpenTelemetry Collector:

```yaml
# otel-collector-config.yaml
receivers:
  otlp:
    protocols:
      http:
        endpoint: 0.0.0.0:4318

processors:
  batch:
    timeout: 1s
    send_batch_size: 1024

exporters:
  prometheus:
    endpoint: 0.0.0.0:8889
  jaeger:
    endpoint: jaeger:14250
    tls:
      insecure: true

service:
  pipelines:
    metrics:
      receivers: [otlp]
      processors: [batch]
      exporters: [prometheus]
    traces:
      receivers: [otlp]
      processors: [batch]
      exporters: [jaeger]
```

Start the collector with Docker:

```
docker run -v $(pwd)/otel-collector-config.yaml:/etc/otel-collector-config.yaml -p 4318:4318 -p 8889:8889 otel/opentelemetry-collector:latest --config=/etc/otel-collector-config.yaml
```

### Cloud Platforms

For production use, configure your device to connect to any of these OpenTelemetry-compatible platforms:

- Splunk Observability Cloud
- Grafana Cloud
- New Relic
- Datadog
- Dynatrace

### Sample Dashboards

In your visualization tool, you can create dashboards showing:

- Device metrics over time
- Trace visualization of device operations
- Correlation between different metrics and operations
- Alerts for anomalies or threshold violations

## Example Debug Output

When your device is running with debug enabled, you'll see output like this:

```
[2025-04-01 02:24:18.000] [DEBUG] Time to send metrics to OpenTelemetry (interval: 30000 ms, last send: 30035 ms ago)...
[2025-04-01 02:24:18.000] [DEBUG] Generated random ID: 7b728e061b08e020
[2025-04-01 02:24:18.000] [DEBUG] Generated new span ID: 7b728e061b08e020
[2025-04-01 02:24:18.000] [DEBUG] Started span [sensor_reading] id=7b728e061b08e020 parent=0000000000000000 trace=fa7e798a55dbda2da30cfc475ba37fcc (parts: fa7e798a55dbda2d a30cfc475ba37fcc) (total=1, active=1)
[2025-04-01 02:24:18.000] [DEBUG] Started sensor reading span: 7b728e061b08e020
[2025-04-01 02:24:18.000] [DEBUG] Querying sensors for fresh readings
[2025-04-01 02:24:18.000] [DEBUG] Using RTC hardware for timestamp
[2025-04-01 02:24:18.000] [DEBUG] Pressure reading: 1009.86 hPa (took 2 ms)
[2025-04-01 02:24:18.000] [DEBUG] Temperature: 25.37°C, Humidity: 48.92% (took 252 ms)
[2025-04-01 02:24:18.000] [DEBUG] Battery: 100%, 4.16V, Charging: No (took 1 ms)
[2025-04-01 02:24:18.000] [DEBUG] Total sensor query time: 281 ms
[2025-04-01 02:24:18.000] [DEBUG] Ended span [sensor_reading] id=7b728e061b08e020 trace=fa7e798a55dbda2da30cfc475ba37fcc duration=305163 µs (total=1, active=0)
```

The library also logs the complete OTLP payload so you can see exactly what's being sent:

```
[2025-04-01 02:24:19.000] [DEBUG] HTTP Request Details:
[2025-04-01 02:24:19.000] [DEBUG] POST http://192.168.1.81:4318/v1/traces
[2025-04-01 02:24:19.000] [DEBUG] Headers:
[2025-04-01 02:24:19.000] [DEBUG]   Content-Type: application/json
[2025-04-01 02:24:19.000] [DEBUG] Body:
[2025-04-01 02:24:19.000] [DEBUG] {"resourceSpans":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"m5stick-sensor"}},{"key":"service.version","value":{"stringValue":"1.0.0"}},{"key":"wifi.ssid","value":{"stringValue":"Morningwood-VLAN-24"}}]},"scopeSpans":[{"scope":{"name":"iototeldemo"},"spans":[{"traceId":"fa7e798a55dbda2da30cfc475ba37fcc","spanId":"7b728e061b08e020","name":"sensor_reading","startTimeUnixNano":"1743474271127384000","endTimeUnixNano":"1743474271432547000","kind":"SPAN_KIND_INTERNAL","attributes":[{"key":"temperature","value":{"doubleValue":25.37}},{"key":"humidity","value":{"doubleValue":48.92}},{"key":"pressure","value":{"doubleValue":1009.86}},{"key":"battery_level","value":{"doubleValue":100.00}}]}]}]}]}
```

## Complete Example

Here's a complete annotated example showing practical usage with a sensor platform:

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "debug.h"
#include "opentelemetry.h"

// WiFi credentials
const char* ssid = "your_wifi_ssid";
const char* password = "your_wifi_password";

// Create OpenTelemetry instance
OpenTelemetry otel;

// Sensor reading interval in milliseconds
const unsigned long SENSOR_INTERVAL = 30000; // 30 seconds
unsigned long lastReadingTime = 0;

// Custom time provider using RTC
uint64_t getRTCTimeNanos() {
  // This provides much more accurate timestamps than just using millis()
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    time_t now = mktime(&timeinfo);
    return (uint64_t)now * 1000000000ULL;
  }
  return millis() * 1000000ULL; // Fallback if RTC not set
}

// Custom random seed provider using ESP32's hardware RNG
uint32_t getESP32RandomSeed() {
#ifdef ESP32
  // Hardware RNG is better for trace ID generation and avoids WiFi/ADC conflicts
  return esp_random();
#else
  // Fallback for non-ESP32 platforms
  return (uint32_t)(millis() ^ micros());
#endif
}

void setup() {
  // Initialize serial first for debugging
  Serial.begin(115200);
  debugLog("Starting OpenTelemetry sensor demo...");
  
  // Connect to WiFi
  debugLog("Connecting to WiFi SSID: %s", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  debugLog("WiFi connected. IP address: %s", WiFi.localIP().toString().c_str());
  
  // Get time from NTP server - important for accurate timestamps
  debugLog("Configuring time via NTP...");
  configTime(0, 0, "pool.ntp.org");
  
  // Initialize OpenTelemetry
  debugLog("Initializing OpenTelemetry...");
  otel.begin();
  
  // Set custom time provider to use RTC synchronized via NTP
  otel.setTimeProvider(getRTCTimeNanos);
  
  // Set custom random seed provider to use ESP32's hardware RNG
  otel.setRandomSeedProvider(getESP32RandomSeed);
  
  // Start a trace for the setup process
  otel.startNewTrace();
  uint64_t setupSpanId = otel.startSpan("device_setup");
  otel.addSpanAttribute(setupSpanId, "device.type", "ESP32");
  otel.addSpanAttribute(setupSpanId, "wifi.ssid", ssid);
  otel.addSpanAttribute(setupSpanId, "wifi.rssi", (double)WiFi.RSSI());
  
  // Initialize sensor (example)
  uint64_t sensorInitSpanId = otel.startSpan("sensor_initialization", setupSpanId);
  // ... initialize your sensors here ...
  delay(100); // Simulating sensor initialization time
  otel.endSpan(sensorInitSpanId);
  
  // End setup span
  otel.endSpan(setupSpanId);
  
  // Start a new trace for the main program
  otel.startNewTrace();
  debugLog("Setup complete - starting main loop");
}

void loop() {
  unsigned long currentMillis = millis();
  
  // Check if it's time to read sensors
  if (currentMillis - lastReadingTime >= SENSOR_INTERVAL) {
    lastReadingTime = currentMillis;
    
    debugLog("Time to send metrics to OpenTelemetry (interval: %lu ms, last send: %lu ms ago)...", 
            SENSOR_INTERVAL, currentMillis - lastReadingTime + SENSOR_INTERVAL);

    // Create a span for sensor reading
    uint64_t sensorSpanId = otel.startSpan("sensor_reading");
    debugLog("Started sensor reading span: %016llx", sensorSpanId);
    
    // Read sensor values (example)
    debugLog("Querying sensors for fresh readings");
    
    // Record start time to measure sensor read duration
    unsigned long sensorStartTime = millis();
    
    debugLog("Using RTC hardware for timestamp");
    
    // Read pressure sensor (example)
    unsigned long pressureStartTime = millis();
    float pressure = 1009.86;  // Replace with actual sensor reading
    debugLog("Pressure reading: %.2f hPa (took %lu ms)", 
            pressure, millis() - pressureStartTime);
    
    // Read temperature and humidity (example)
    unsigned long tempStartTime = millis();
    float temperature = 25.37;  // Replace with actual sensor reading
    float humidity = 48.92;     // Replace with actual sensor reading
    debugLog("Temperature: %.2f°C, Humidity: %.2f%% (took %lu ms)", 
            temperature, humidity, millis() - tempStartTime);
    
    // Read battery status (example)
    unsigned long batteryStartTime = millis();
    int batteryLevel = 100;     // Replace with actual battery reading
    debugLog("Battery: %d%%, 4.16V, Charging: No (took %lu ms)", 
            batteryLevel, millis() - batteryStartTime);
    
    // Calculate total sensor query time
    unsigned long totalSensorTime = millis() - sensorStartTime;
    debugLog("Total sensor query time: %lu ms", totalSensorTime);
    
    // Add sensor values as span attributes
    otel.addSpanAttribute(sensorSpanId, "temperature", temperature);
    otel.addSpanAttribute(sensorSpanId, "humidity", humidity);
    otel.addSpanAttribute(sensorSpanId, "pressure", pressure);
    otel.addSpanAttribute(sensorSpanId, "battery_level", (double)batteryLevel);
    otel.addSpanAttribute(sensorSpanId, "wifi_rssi", (double)WiFi.RSSI());
    otel.addSpanAttribute(sensorSpanId, "sensor_read_time_ms", totalSensorTime);
    otel.addSpanAttribute(sensorSpanId, "free_heap", ESP.getFreeHeap());
    
    // End the sensor reading span
    otel.endSpan(sensorSpanId);
    debugLog("Completed sensor reading span");
    
    // Current timestamp from our custom time provider
    uint64_t timestamp = otel.getCurrentTimeNanos();
    
    // Add metrics
    otel.addMetric("temperature", temperature, timestamp);
    otel.addMetric("humidity", humidity, timestamp);
    otel.addMetric("pressure", pressure, timestamp);
    otel.addMetric("battery_level", batteryLevel, timestamp);
    otel.addMetric("wifi_rssi", WiFi.RSSI(), timestamp);
    otel.addMetric("sensor_read_time_ms", totalSensorTime, timestamp);
    otel.addMetric("free_heap", ESP.getFreeHeap(), timestamp);
    
    // Create a span for sending metrics
    uint64_t sendSpanId = otel.startSpan("metric_send");
    debugLog("Started metric send span: %016llx", sendSpanId);
    
    // Add pre-send information to the span
    otel.addSpanAttribute(sendSpanId, "wifi.rssi", (double)WiFi.RSSI());
    otel.addSpanAttribute(sendSpanId, "metrics_count", 7.0);  // We're sending 7 metrics
    otel.addSpanAttribute(sendSpanId, "all_metrics_added", "true");
    
    debugLog("Attempting to safely send metrics and traces with maximum error protection");
    
    // Get span stats for debugging
    uint8_t totalSpans, activeSpans, completedSpans;
    otel.getSpanStats(totalSpans, activeSpans, completedSpans);
    debugLog("Span stats: Total=%d, Active=%d, Completed=%d", 
            totalSpans, activeSpans, completedSpans);
    
    debugLog("--- Starting combined metrics and traces send operation ---");
    
    // Dump the current trace ID for debugging
    char traceIdHex[33];
    otel.getCurrentTraceIdHex(traceIdHex, sizeof(traceIdHex));
    debugLog("Current trace ID: %s", traceIdHex);
    
    debugLog("Sending %d metrics with %d spans queued...", 7, completedSpans);
    
    // Send both metrics and traces with robust error handling
    bool success = otel.safeSendMetricsAndTraces();
    
    // Add result to the send span
    otel.addSpanAttribute(sendSpanId, "success", success ? "true" : "false");
    if (!success) {
      otel.addSpanAttribute(sendSpanId, "error", otel.getLastError());
      otel.addSpanAttribute(sendSpanId, "http_code", (double)otel.getLastHttpCode());
    }
    
    // End the send span
    otel.endSpan(sendSpanId);
    debugLog("Completed metric send span");
    
    // Log the result
    debugLog("Metrics sent successfully");
    
    // After successfully sending, start a new trace for the next cycle
    if (success) {
      debugLog("Ending metrics collection trace and starting a new one");
      otel.startNewTrace();
      
      // Dump the new trace ID for debugging
      otel.getCurrentTraceIdHex(traceIdHex, sizeof(traceIdHex));
      debugLog("Started new trace: %s", traceIdHex);
    }
    
    // On ESP32, explicitly flush traces to avoid memory build-up
    debugLog("Regular trace flush triggered (%d completed spans)", completedSpans);
    otel.safeFlushTraces();
    
    // Get final span stats for debugging
    otel.getSpanStats(totalSpans, activeSpans, completedSpans);
    debugLog("SPAN STATS: Total=%d, Active=%d, Completed=%d", 
            totalSpans, activeSpans, completedSpans);
  }
  
  // Do other work here without blocking
  // ...
  
  // Yield to the ESP32 background tasks
  delay(10);
}
```

## API Reference

### Constructor

```cpp
OpenTelemetry()
```

Creates a new OpenTelemetry instance with default values from `config.h`.

### Initialization

```cpp
void begin(const char* svcName, const char* svcVersion, 
           const char* metEndpoint, const char* traceEndpoint = nullptr)
```

Initializes the OpenTelemetry instance with custom service information and endpoints.

- `svcName`: Service name to report
- `svcVersion`: Service version to report
- `metEndpoint`: Full URL for the metrics endpoint
- `traceEndpoint`: Full URL for the traces endpoint (optional, defaults to metrics endpoint)

### Time Management

```cpp
typedef uint64_t (*TimeProviderFunc)();

void setTimeProvider(TimeProviderFunc provider)
```

Sets a custom time provider function for timestamp generation.

- `provider`: Function pointer to a custom time function that returns nanoseconds since epoch

```cpp
uint64_t getCurrentTimeNanos()
```

Gets the current time in nanoseconds using the configured time provider.

- Returns: Current time in nanoseconds since epoch

### Random Seed Provider

```cpp
typedef uint32_t (*RandomSeedProviderFunc)();

void setRandomSeedProvider(RandomSeedProviderFunc provider)
```

Sets a custom random seed provider function for trace and span ID generation.

- `provider`: Function pointer to a custom function that returns a 32-bit entropy value

The default implementation uses the most appropriate entropy source for the platform:
- On ESP32, it uses the hardware random number generator via `esp_random()`
- On other platforms, it falls back to a software-based approach using `millis()`

Using a proper random seed provider is important for:
- Generating unique trace and span IDs
- Avoiding ID collisions in distributed systems
- Ensuring trace security and privacy
- Preventing conflicts when using ADC pins with WiFi on ESP32

### Metrics

```cpp
bool addMetric(const char* name, double value, uint64_t timestamp_nanos)
```

Adds a metric to the current batch.

- `name`: Name of the metric
- `value`: Numeric value of the metric
- `timestamp_nanos`: Timestamp in nanoseconds since epoch
- Returns: `true` if the metric was added, `false` if the batch is full

```cpp
bool sendMetrics()
```

Sends the current batch of metrics to the OpenTelemetry collector.

- Returns: `true` if successful, `false` otherwise

### Tracing

```cpp
void startNewTrace()
```

Starts a new trace with a new trace ID.

```cpp
uint64_t startSpan(const char* name, uint64_t parentSpanId = 0)
```

Starts a new span with the given name.

- `name`: Name of the span
- `parentSpanId`: Optional ID of the parent span (0 for no parent)
- Returns: Span ID for the new span, or 0 if creation failed

```cpp
bool addSpanAttribute(uint64_t spanId, const char* key, const char* value)
```

Adds a string attribute to the specified span.

- `spanId`: ID of the span
- `key`: Attribute key
- `value`: String attribute value
- Returns: `true` if successful, `false` otherwise

```cpp
bool addSpanAttribute(uint64_t spanId, const char* key, double value)
```

Adds a numeric attribute to the specified span.

- `spanId`: ID of the span
- `key`: Attribute key
- `value`: Numeric attribute value
- Returns: `true` if successful, `false` otherwise

```cpp
bool endSpan(uint64_t spanId)
```

Ends the specified span, recording its end time.

- `spanId`: ID of the span to end
- Returns: `true` if successful, `false` otherwise

```cpp
bool sendTraces()
```

Sends all completed spans as traces to the OpenTelemetry collector.

- Returns: `true` if successful, `false` otherwise

### Combined Operations

```cpp
bool sendMetricsAndTraces()
```

Sends both metrics and traces in a single operation.

- Returns: `true` if both metrics and traces were sent successfully, `false` otherwise

### Safety Wrappers

```cpp
bool safeFlushTraces()
```

Safely sends all completed spans with robust error handling.

- Returns: `true` if successful or if there are no traces to send, `false` otherwise

```cpp
bool safeSendMetricsAndTraces()
```

Safely sends both metrics and traces with robust error handling.

- Returns: `true` if successful, `false` otherwise

### Error Handling

```cpp
const char* getLastError()
```

Gets the last error message.

- Returns: String containing the last error message

```cpp
int getLastHttpCode()
```

Gets the HTTP status code from the last request.

- Returns: HTTP status code, or 0 if no request was made

### Debugging

```cpp
void getCurrentTraceIdHex(char* buffer, size_t bufferSize)
```

Gets the current trace ID as a hexadecimal string.

- `buffer`: Buffer to store the hexadecimal string
- `bufferSize`: Size of the buffer (must be at least 33 bytes)

```cpp
void getSpanStats(uint8_t& total, uint8_t& active, uint8_t& completed)
```

Gets statistics about the current spans.

- `total`: Will be set to the total number of spans
- `active`: Will be set to the number of active spans
- `completed`: Will be set to the number of completed spans

```cpp
void debugSpans()
```

Logs debug information about all spans.

```cpp
void debugSpanAttributes()
```

Logs debug information about span attributes.

## Best Practices

1. **Use a custom time provider for accurate timestamps**: Set a time provider that uses RTC or NTP synchronization.

2. **Use a custom random seed provider for better entropy**: Use a hardware RNG when available for better trace ID generation.

3. **Use the safety wrapper methods**: Always use `safeFlushTraces()` and `safeSendMetricsAndTraces()` for robust error handling.

4. **Use meaningful span names**: Choose span names that describe the operation being performed.

5. **Create a hierarchy of spans**: Use parent-child relationships to create a logical structure of operations.

6. **Add attributes for context**: Add relevant attributes to spans to provide context for debugging.

7. **End spans promptly**: Always end spans when the operation is complete to avoid memory leaks.

8. **Check for errors**: Always check the return values of operations.

9. **Start new traces for logical units**: Start a new trace for each logical unit of work.

10. **Manage memory usage**: Adjust the memory constants based on your device's capabilities.

11. **Avoid ADC-WiFi conflicts on ESP32**: Use the hardware RNG instead of analogRead() for random seed generation when using WiFi.

12. **Balance frequency with battery life**: Consider reducing telemetry frequency when operating on battery power.

## Limitations

- Limited to MAX_METRICS metrics per batch
- Limited to MAX_SPANS spans in memory at once
- Limited to MAX_SPANS_PER_BATCH spans per HTTP request
- Limited to MAX_SPAN_ATTRS attributes per span
- JSON payloads are limited to 4KB to conserve memory
- No protobuf support (uses JSON format for simplicity and debugging)
- No authentication mechanisms built-in (use in trusted networks)

## Troubleshooting

1. **Metrics not showing up in your visualization tool**:
   - Check that the metrics endpoint is correct
   - Verify that the HTTP status code is 200 (use `getLastHttpCode()`)
   - Check the collector logs for errors
   - Ensure the JSON payload is valid (check debug output)

2. **Traces not showing up in your visualization tool**:
   - Verify that spans are being properly ended with `endSpan()`
   - Check that the trace ID is valid (use `getCurrentTraceIdHex()`)
   - Inspect the trace payload with debug logging
   - Make sure spans have a valid parent-child relationship

3. **Memory issues**:
   - Reduce the MAX_SPANS constant
   - Send traces more frequently
   - Use shorter span and attribute names
   - Check for memory leaks using ESP32's heap monitoring

4. **Time-related issues**:
   - Implement a custom time provider using RTC or NTP
   - Ensure time sources are properly synchronized
   - Check if time jumps (e.g., from NTP sync) are affecting your spans

5. **ESP32 ADC and WiFi conflicts**:
   - Use the built-in hardware random number generator with `setRandomSeedProvider()`
   - Avoid reading from ADC2 pins (GPIO0, GPIO2, GPIO4, GPIO12-GPIO15) while WiFi is active
   - Use ADC1 pins (GPIO32-GPIO39) if you need analog readings with WiFi

6. **Network connectivity issues**:
   - Add robust WiFi reconnection handling
   - Implement retry logic for failed sends
   - Cache metrics/traces during network outages (if memory permits)

## Contributing

Contributions to improve this library are welcome! Here are some ideas:
- Add support for more OpenTelemetry features
- Optimize memory usage further
- Add more examples for different use cases
- Create visualization templates for popular platforms

## License

This library is released under the MIT License.