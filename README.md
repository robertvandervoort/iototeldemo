# IoT OpenTelemetry Demo

This project demonstrates sending IoT sensor data to OpenTelemetry using an M5Stack device with ENV III sensor unit.

## Hardware Requirements

- M5StickC-Plus ```https://docs.m5stack.com/en/core/m5stickc_plus```
- ENV III Sensor Unit ```https://docs.m5stack.com/en/unit/envIII```
- USB-C cable for programming/power


## Software Requirements

-  Visual Studio Code or Cursor with:
  - PlatformIO IDE
  - Espressif 32 platform installed in PlatformIO
  - M5Unified library
  - M5Unit-ENV library

## Configuration

Copy `config.h.example` to `config.h` and update with your settings:

```cpp
// WiFi Configuration
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"
#define WIFI_HOSTNAME "m5stack-otel"

// OpenTelemetry Configuration
#define OTEL_HOST "your.otel.host"  // Hostname/IP of your OpenTelemetry collector
#define OTEL_PORT "4318"            // Port for OTLP/HTTP ensure this stays in quotes
#define OTEL_PATH "/v1/metrics"     // Path for metrics endpoint
#define OTEL_SERVICE_NAME "m5stack-env"
#define OTEL_SERVICE_VERSION "1.0.0"

// Debug Configuration
#define LCD_SHOW_DEBUG_INFO "1"     // Show debug info in serial terminal output (1=yes, 0=no)
```

## NTP Configuration

The device uses NTP (Network Time Protocol) to synchronize its RTC (Real Time Clock) to ensure accurate timestamps for sensor data. Configure NTP settings in `config.h`:

```cpp
// NTP Configuration
#define NTP_SERVER1         "pool.ntp.org"      // Primary NTP server
#define NTP_SERVER2         "time.google.com"   // Secondary NTP server (or empty string if not used)
#define NTP_SERVER3         "time.windows.com"  // Tertiary NTP server (or empty string if not used)
// NTP timeout settings
#define NTP_SYNC_TIMEOUT    10000   // Timeout for NTP sync in milliseconds (10 seconds)
#define NTP_MAX_RETRIES     3       // Maximum number of NTP sync retries before giving up
```

For isolated networks without internet access, configure these with local NTP server addresses. All timestamps are in UTC.

### Timestamp Precision

When sending data to OpenTelemetry, the device:
1. Captures timestamps at the exact moment sensor data is collected (not when data is sent)
2. Uses the hardware RTC when available for improved precision
3. Sends timestamps in nanoseconds since epoch (UTC) as required by OpenTelemetry

This ensures accurate time alignment of sensor data even in environments with intermittent connectivity.

## OpenTelemetry Health Check Requirements

This project uses the OpenTelemetry Collector's health check extension to monitor collector availability. The health check endpoint must be properly configured:

### For Splunk OpenTelemetry Collector:
1. The health check extension is included by default
2. You must configure it to listen on an external IP (not just localhost)
3. Add the following to your OTel collector's configuration:

```yaml
# Health check extension used by the M5Stick to verify collector availability
extensions:
  health_check:
    endpoint: "0.0.0.0:13133"

# OTLP receiver configuration - the M5Stick connects to port 4318 via HTTP
receivers:
  otlp:
    protocols:
      grpc:
        endpoint: "${SPLUNK_LISTEN_INTERFACE}:4317"
      http:
        endpoint: "${SPLUNK_LISTEN_INTERFACE}:4318"

# Core processors used in the metrics and traces pipelines
processors:
  batch:
    metadata_keys:
      - X-SF-Token
  memory_limiter:
    check_interval: 2s
    limit_mib: ${SPLUNK_MEMORY_LIMIT_MIB}
  resourcedetection:
    detectors: [gcp, ecs, ec2, azure, system]
    override: true
  resource/add_environment:
    attributes:
      - action: insert
        value: rv-lab
        key: deployment.environment

# Service definition showing which components are used in each pipeline
service:
  extensions: [health_check]
  pipelines:
    traces:
      receivers: [otlp]
      processors:
      - memory_limiter
      - batch
      - resourcedetection
      - resource/add_environment
      exporters: [otlphttp, signalfx]
    metrics:
      receivers: [otlp]
      processors:
      - memory_limiter
      - batch
      - resourcedetection
      - resource/add_environment
      exporters: [signalfx]
```

A complete Splunk Observability Cloud OpenTelemetry agent configuration example is available in this codebase at `iototeldemo\agent-config.yaml.splunk.yml`. This file includes all necessary settings for running the collector with the M5Stick device.

### For Other OpenTelemetry Collectors:
1. Enable the health check extension in your collector configuration
2. Configure it to listen on an external IP
3. Ensure port 13133 is accessible from your device

The health check endpoint should return a response like:
```json
{"status":"Server available","upSince":"2024-03-24T16:40:45.887236615Z","uptime":"41m39.21715837s"}
```

## Building and Flashing

1. Open the project in PlatformIO
2. Connect your M5Stack device via USB
3. Build and upload the project
4. Monitor the serial output for debugging

## Operation

The device will:
1. Connect to configured WiFi network
2. Initialize the ENV III sensor
3. Begin collecting temperature, humidity, and pressure readings
4. Send metrics to OpenTelemetry collector at configured interval
5. Display current readings and connection status on screen

### Button Controls

- Button A: Cycle through display screens (Main/Network/OpenTelemetry)
- Button B: Reserved for future use

### Display Screens

1. Main Screen
   - Current temperature
   - Current humidity
   - Current pressure
   - Connection status

2. Network Screen
   - WiFi SSID
   - IP Address
   - Signal strength
   - Connection status

3. OpenTelemetry Screen
   - Endpoint details
   - Service name
   - Upload status
   - Last error (if any)

## Metrics

The following metrics are sent to OpenTelemetry:

- `temperature`: Temperature reading in degrees Celsius (°C)
- `humidity`: Relative humidity percentage (%)
- `pressure`: Atmospheric pressure in hectopascals (hPa)
- `battery_level`: Current battery charge level (%)
- `battery_voltage`: Battery voltage in volts (V)
- `battery_charging`: Binary indicator if device is charging (1) or not (0)
- `wifi.rssi`: WiFi signal strength in decibel-milliwatts (dBm), typically ranges from -30 (excellent) to -90 (poor)

Each metric includes:
- Precise timestamp of when the sensor reading was taken
- Service name and version as resource attributes
- WiFi SSID as a resource attribute

Metrics are sent at regular intervals configured by `OTEL_SEND_INTERVAL` (default: 30 seconds).

## Examples in Splunk Observability Cloud

### Distributed Tracing

This screenshot shows the distributed traces captured in Splunk APM. Each trace represents a complete operation cycle, with spans for sensor reading, data processing, and metric transmission. This provides deep visibility into the device's internal operations and helps identify performance bottlenecks.

![Distributed Traces in Splunk APM](Screenshot%202025-03-31%20214506.png)

### Metrics Dashboard

The following screenshot shows the M5StickC sensor data visualized in a Splunk Observability Cloud dashboard. The dashboard displays temperature, humidity, pressure, and battery metrics over time, allowing for easy monitoring of environmental conditions and device status.

![Metrics Dashboard in Splunk Observability Cloud](Screenshot%202025-03-31%20214539.png)

A complete dashboard JSON export (`dashboard_RV - IoT Dashboard Level 1.json`) is included in this project. You can import this file directly into your Splunk Observability Cloud instance to quickly set up a similar dashboard for monitoring your own devices.

## Troubleshooting

1. Check serial output for detailed debug information
2. Verify WiFi credentials are correct
3. Ensure OpenTelemetry collector is accessible
4. Check collector logs for any ingestion errors

## Contributing

Pull requests are welcome. For major changes, please open an issue first to discuss what you would like to change.

## License

[MIT](https://choosealicense.com/licenses/mit/) 