# ESP32-Based Real-Time Health Monitoring System Using FreeRTOS and Blynk IoT

## 1. Project Overview

This project implements an ESP32-based real-time health and environmental monitoring system using the FreeRTOS real-time operating system and Blynk IoT cloud platform.

The system integrates multiple hardware peripherals with independent FreeRTOS tasks to acquire sensor data, process measurements, control an LED indicator, and transmit telemetry to a remote dashboard over Wi-Fi.

The firmware follows a multitasking architecture in which sensor acquisition, cloud communication, and GPIO control operate independently.

### Key Features

- Real-time temperature and humidity monitoring using DHT11.
- Optical pulse and blood oxygen sensing using MAX30102.
- Heart rate and SpO₂ estimation from optical sensor samples.
- Independent LED status control using a dedicated FreeRTOS task.
- Wi-Fi connectivity using the ESP32 station interface.
- Cloud-based telemetry transmission through the Blynk IoT platform.
- Thread-safe shared telemetry using FreeRTOS mutex synchronization.
- I²C communication with the MAX30102 sensor.
- HTTPS-based communication with Blynk Cloud.
- Periodic sensor acquisition and cloud data updates.

## 2. Hardware Components

| Component | Purpose |
|---|---|
| ESP32 DevKit V1 (ESP32-WROOM-32) | Main microcontroller and processing unit |
| DHT11 | Temperature and humidity measurement |
| MAX30102 | Optical heart rate and SpO₂ sensing |
| LED | System status indication |
| Current-limiting resistor | LED current protection |
| Breadboard | Circuit prototyping |
| Jumper wires | Hardware interconnections |
| USB cable | Programming and power supply |

## 3. Hardware Pin Configuration

### ESP32 GPIO Mapping

| Peripheral | ESP32 GPIO | Function |
|---|---:|---|
| LED | GPIO18 | Digital output |
| DHT11 DATA | GPIO4 | Single-wire digital communication |
| MAX30102 SDA | GPIO21 | I²C data |
| MAX30102 SCL | GPIO22 | I²C clock |
| Common GND | GND | Common reference |
| Sensor supply | 3.3V / module-compatible supply | Power |

### DHT11 Connections

| DHT11 Pin | Connection |
|---|---|
| VCC | ESP32 3.3V |
| DATA | GPIO4 |
| NC | Not connected |
| GND | ESP32 GND |

For a bare four-pin DHT11 sensor, connect a 4.7 kΩ–10 kΩ pull-up resistor between DATA and 3.3V. Some breakout boards already include this resistor.

### MAX30102 Connections

| MAX30102 Pin | ESP32 Connection |
|---|---|
| VIN | Compatible supply according to breakout-board specifications |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |
| INT | Not connected |

**Note:** MAX30102 breakout boards differ in their voltage regulation and logic-level circuitry. Verify the module specifications before applying power. The I²C bus uses the 7-bit device address `0x57`.

### LED Connections

```text
ESP32 GPIO18
     |
     |
 Current-limiting resistor
     |
     |
 LED Anode (+)
 LED Cathode (-)
     |
    GND
```

The LED is controlled independently by the LED FreeRTOS task.

## 4. Software and Development Environment

| Technology | Application |
|---|---|
| Embedded C | Firmware development |
| ESP-IDF | ESP32 development framework |
| FreeRTOS | Real-time task scheduling |
| PlatformIO | Project management and build system |
| Wi-Fi (802.11 b/g/n) | Wireless connectivity |
| I²C | MAX30102 sensor communication |
| GPIO | LED and DHT11 interfacing |
| HTTPS | Secure cloud communication |
| Blynk IoT | Remote monitoring dashboard |
| Git and GitHub | Version control |

### Development Environment

- IDE: Visual Studio Code
- Framework: ESP-IDF
- Build system: PlatformIO
- Target board: ESP32 DevKit V1
- Serial monitor baud rate: 115200

## 5. System Architecture

The ESP32 operates as the central processing unit. Sensor acquisition and peripheral control are distributed across independent FreeRTOS tasks.

```text
                 +----------------------+
                 |       ESP32          |
                 |   ESP32-WROOM-32     |
                 +----------+-----------+
                            |
              +-------------+-------------+
              |             |             |
              v             v             v
         +---------+  +-----------+  +----------+
         | DHT11   |  | MAX30102  |  | LED      |
         | Task    |  | Task      |  | Task     |
         +----+----+  +-----+-----+  +----------+
              |             |
              v             v
         Temperature    Red / IR Samples
         Humidity             |
              |               v
              |         Signal Processing
              |               |
              +-------+-------+
                      |
                      v
              +---------------+
              | Shared        |
              | Telemetry     |
              | Structure     |
              +-------+-------+
                      |
                Mutex Protection
                      |
                      v
              +---------------+
              | Communication |
              | Task           |
              +-------+-------+
                      |
                  Wi-Fi / HTTPS
                      |
                      v
              +---------------+
              | Blynk IoT      |
              | Cloud          |
              +-------+-------+
                      |
                      v
              Mobile / Web Dashboard
```

## 6. FreeRTOS Task Architecture

The firmware uses four primary application tasks.

| Task | Priority | Stack Size | Responsibility |
|---|---:|---:|---|
| DHT11_Task | 5 | 4096 bytes | Temperature and humidity acquisition |
| MAX30102_Task | 6 | 6144 bytes | I²C sampling and optical signal processing |
| Communication_Task | 4 | 6144 bytes | HTTPS telemetry transmission |
| LED_Task | 3 | 2048 bytes | Periodic LED control |

### 6.1 DHT11 Task

The DHT11 task periodically acquires temperature and humidity measurements.

Operations:
1. Initiates communication with the DHT11.
2. Reads the 40-bit sensor response.
3. Extracts humidity and temperature data.
4. Validates the checksum.
5. Updates the shared telemetry structure.
6. Executes periodically at an interval of 2500 ms.

### 6.2 MAX30102 Task

The MAX30102 task handles optical sensor initialization, I²C communication, sample acquisition, and basic signal processing.

Operations:
1. Initializes the I²C master bus.
2. Configures the MAX30102 using its register interface.
3. Reads red and infrared samples from the FIFO.
4. Detects finger presence using the infrared signal threshold.
5. Maintains a sample window of 100 readings.
6. Estimates heart rate using threshold-based beat detection.
7. Estimates SpO₂ using a ratio-of-ratios approximation.
8. Updates the shared telemetry structure.

The sensor communicates using the I²C protocol at 100 kHz.

**Measurement note:** The current BPM detection and SpO₂ estimation algorithms are experimental implementations. They require calibration and validation against reference equipment before being considered accurate physiological measurements.

### 6.3 Communication Task

The communication task transfers the latest telemetry values to Blynk Cloud.

Operations:
1. Waits for the Wi-Fi connection event.
2. Reads the shared telemetry structure.
3. Formats sensor values into an HTTPS request.
4. Sends the request to the Blynk External API.
5. Checks the HTTP response status.
6. Repeats the process every 5000 ms.

### 6.4 LED Task

The LED task independently controls the external LED connected to GPIO18.

Operations:
1. Configures GPIO18 as an output.
2. Toggles the LED state.
3. Maintains a one-second delay between transitions.

## 7. Inter-Task Communication and Synchronization

Multiple tasks access a shared telemetry structure containing the latest sensor measurements and system status.

The structure contains:

| Variable | Data Type | Description |
|---|---|---|
| temperature | float | Temperature in °C |
| humidity | float | Relative humidity in % |
| bpm | float | Estimated heart rate in BPM |
| spo2 | float | Estimated oxygen saturation in % |
| dht_real | bool | DHT11 measurement validity |
| max_ok | bool | MAX30102 communication state |
| finger_detected | bool | Finger detection state |
| status | char array | System operating status |

A FreeRTOS mutex protects shared telemetry access.

This prevents simultaneous task operations from causing inconsistent updates or race conditions while reading and modifying shared data.

## 8. MAX30102 Register Configuration

The firmware uses the following MAX30102 registers:

| Register | Address | Purpose |
|---|---:|---|
| FIFO Write Pointer | 0x04 | FIFO write position |
| FIFO Overflow Counter | 0x05 | FIFO overflow tracking |
| FIFO Read Pointer | 0x06 | FIFO read position |
| FIFO Data | 0x07 | Sensor sample data |
| FIFO Configuration | 0x08 | FIFO configuration |
| Mode Configuration | 0x09 | Operating mode |
| SpO₂ Configuration | 0x0A | ADC and sampling configuration |
| LED1 Pulse Amplitude | 0x0C | Red LED current |
| LED2 Pulse Amplitude | 0x0D | IR LED current |
| Part ID | 0xFF | Device identification |

The firmware checks the part ID against `0x15` during initialization.

## 9. Blynk IoT Integration

Blynk provides the cloud dashboard used to visualize the ESP32 telemetry.

The ESP32 connects to a Wi-Fi network and communicates with Blynk through its HTTPS External API.

### Virtual Pin Configuration

| Virtual Pin | Parameter | Data Type | Unit |
|---|---|---|---|
| V0 | Temperature | Double | °C |
| V1 | Humidity | Double | % |
| V2 | Heart Rate | Double | BPM |
| V3 | SpO₂ | Integer | % |
| V4 | System Status | String | Text |

### Cloud Update Endpoint

```text
https://blynk.cloud/external/api/update
```

The firmware sends the following parameters:

```text
token
v0
v1
v2
v3
v4
```

Telemetry transmission occurs periodically every five seconds.

The dashboard can be accessed through the Blynk mobile application or web console.

## 10. Wi-Fi Connectivity

The ESP32 operates in Station Mode.

The firmware uses the ESP-IDF Wi-Fi event mechanism to handle:

- Wi-Fi initialization.
- Station connection.
- IP address acquisition.
- Disconnection detection.
- Automatic reconnection attempts.

A FreeRTOS Event Group is used to indicate successful Wi-Fi connectivity.

The communication task waits for the connection event before transmitting telemetry.

## 11. Project Directory Structure

```text
ESP32_FreeRTOS_Monitor/
│
├── src/
│   ├── main.c
│   └── CMakeLists.txt
│
├── CMakeLists.txt
├── platformio.ini
├── sdkconfig.defaults
├── secrets.h
├── secrets.example.h
├── .gitignore
└── README.md
```

The current firmware implementation is organized in `src/main.c`. The individual tasks and peripheral drivers are implemented as functions within this file.

## 12. Project Configuration

### platformio.ini

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
monitor_speed = 115200
upload_port = COM3
board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS=sdkconfig.defaults
```

Change `COM3` to match the serial port assigned to your ESP32.

### Root CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(Embedded_Monitoring_System)
```

### src/CMakeLists.txt

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        mbedtls
        nvs_flash
        esp_timer
        esp_rom
)
```

### sdkconfig.defaults

```ini
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
```

## 13. Credential Configuration

Private Wi-Fi credentials and the Blynk authentication token are maintained locally in `secrets.h`.

Example configuration:

```c
#ifndef SECRETS_H
#define SECRETS_H

#define WIFI_SSID       "YOUR_WIFI_NAME"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define BLYNK_TOKEN     "YOUR_BLYNK_AUTH_TOKEN"

#endif
```

Do not commit the actual `secrets.h` file to GitHub.

Use `secrets.example.h` as a public template containing placeholder values. Add `secrets.h` to `.gitignore`.

## 14. Building and Flashing the Firmware

### Requirements

- Visual Studio Code
- PlatformIO IDE extension
- ESP32 development board
- USB data cable
- Configured ESP-IDF project

### Build

1. Open the project in Visual Studio Code.
2. Open the PlatformIO sidebar.
3. Expand Project Tasks.
4. Select `esp32dev`.
5. Select General → Build.

### Upload

1. Connect the ESP32 to the computer.
2. Verify the serial port in `platformio.ini`.
3. Select General → Upload.
4. Wait for firmware flashing to complete.

### Monitor

1. Select General → Monitor.
2. Set the baud rate to 115200.
3. Observe initialization messages, task activity, sensor readings, and cloud communication logs.

## 15. Expected Serial Monitor Activity

Representative firmware log messages include:

```text
ESP32 FreeRTOS Health Monitor booting

DHT11 task started
MAX30102 task started
Communication task started
LED status task started

Starting FreeRTOS application tasks
All application tasks created successfully

WiFi connected!

Temperature = 27.0 C | Humidity = 55.0 %

MAX30102 initialized successfully

Uploading telemetry...
Blynk update successful

LED ON
LED OFF
```

Actual output depends on sensor availability, Wi-Fi connectivity, and the current system state.

## 16. Current Implementation Scope

The current firmware includes:

- Four independent FreeRTOS application tasks.
- DHT11 digital communication.
- MAX30102 I²C initialization and FIFO access.
- Basic heart rate detection and SpO₂ estimation algorithms.
- Shared telemetry synchronization using a mutex.
- Wi-Fi station connectivity and reconnection handling.
- HTTPS communication with Blynk Cloud.
- Periodic GPIO LED control.

The MAX30102 signal-processing algorithms are currently basic experimental implementations. Their accuracy and physiological validity have not been established through clinical or reference-device validation.

## 17. Future Enhancements

- Modular separation of sensor drivers and application tasks into `.c` and `.h` files.
- Improved MAX30102 signal filtering and beat detection.
- Validated SpO₂ estimation algorithms.
- More robust DHT11 timing and error handling.
- Improved sensor diagnostics and fault reporting.
- Watchdog monitoring and task health checks.
- Local data logging and historical trend visualization.
- Power optimization and low-power operating modes.
- PCB design and hardware integration.

## 18. Learning Outcomes

This project demonstrates practical implementation of:

- Embedded C programming.
- ESP32 peripheral interfacing.
- FreeRTOS task creation and scheduling.
- Mutex-based synchronization.
- GPIO programming.
- I²C master communication.
- Digital sensor interfacing.
- Basic optical signal processing.
- Wi-Fi networking.
- HTTPS-based IoT communication.
- Cloud dashboard integration.
- Embedded firmware debugging and development using ESP-IDF.


*This project is developed as an embedded systems and IoT engineering project for learning and demonstrating real-time firmware development, peripheral interfacing, task synchronization, and cloud connectivity.*
