# WiFi Sensor Hub

A WiFi-enabled sensor hub built in C for the ESP32 WROOM board. This project provides a flexible platform for collecting data from multiple sensors and transmitting it over WiFi for remote monitoring and data logging.

## Overview

The WiFi Sensor Hub leverages the ESP32 WROOM's dual-core processor and built-in WiFi capabilities to create a versatile IoT sensor platform. It can interface with various sensors (temperature, humidity, motion, etc.) and send data to a remote server or local network for processing and visualization.

## Hardware Requirements

- **ESP32 WROOM** development board
- **Sensors** (examples):
  - DHT22 (Temperature & Humidity)
  - PIR Motion Sensor
  - Light Sensor (LDR)
  - Any I2C/SPI compatible sensors
- **USB cable** for programming and power
- **Power supply** (5V via USB or battery pack)

## Software Requirements

- **ESP-IDF** (Espressif IoT Development Framework)
- **Toolchain** for ESP32 (Xtensa compiler)
- **USB-to-Serial drivers** for your ESP32 board

## Getting Started

### Installation

1. Install ESP-IDF following the [official guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
2. Clone this repository:
   ```bash
   git clone https://github.com/alexanderpulido/wifi-sensor-hub.git
   cd wifi-sensor-hub
   ```
3. Configure WiFi credentials and sensor settings

### Building

```bash
idf.py build
```

### Flashing

```bash
idf.py -p /dev/ttyUSB0 flash
```

### Monitoring

```bash
idf.py -p /dev/ttyUSB0 monitor
```

## Features

- Multi-sensor support
- WiFi connectivity for remote data transmission
- Low power consumption modes
- Real-time data streaming
- Configurable sampling rates
- OTA (Over-The-Air) updates support

## Configuration

Edit the configuration file to set:
- WiFi SSID and password
- Sensor pins and types
- Data transmission intervals
- Server endpoints

## Project Structure

```
wifi-sensor-hub/
├── main/           # Main application code
├── components/     # Reusable components
├── include/        # Header files
└── README.md       # This file
```

## License

See LICENSE file for details.

## Contributing

Contributions are welcome! Please open an issue or submit a pull request.