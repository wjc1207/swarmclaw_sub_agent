# Swarmclaw Sub Agent (ESP32-S3 Camera + BLE + HTTP)

This project runs on ESP32-S3 and provides:

- Wi-Fi station connection
- OV2640 camera capture and streaming HTTP APIs
- BTHome BLE passive listener (temperature, humidity, battery)

The firmware initializes camera and BLE scanner, joins Wi-Fi, then starts an HTTP server after getting an IP address.

## Hardware and Software

- Board: ESP32-S3
- Camera: OV2640
- Framework: ESP-IDF

## Quick Start

1. Clone the repository.
2. Create secrets file from template:

```bash
cp main/secrets.h.example main/secrets.h
```

3. Edit `main/secrets.h` and set:
	- `WIFI_SSID`
	- `WIFI_PASS`
	- `CAM_API_TOKEN` (optional, can be empty)
4. Build and flash:

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

5. In serial logs, find `Got IP: x.x.x.x`.
6. Open APIs from browser or curl.

## HTTP APIs

Base URL:

```text
http://<device_ip>
```

Routes:

- `GET /stream`: MJPEG live stream
- `GET /capture`: capture latest frame (JPEG)
- `GET /snapshot`: return cached last `/capture` image
- `GET /capture_human`: high-resolution still capture

## Authentication

If `CAM_API_TOKEN` is non-empty, requests must include either:

- Header: `Authorization: Bearer <token>`
- Query: `?token=<token>`

If `CAM_API_TOKEN` is empty, authentication is disabled.

Examples:

```bash
curl -H "Authorization: Bearer <token>" http://<device_ip>/capture -o capture.jpg
curl "http://<device_ip>/stream?token=<token>"
```

## BLE BTHome Listener

The firmware starts a NimBLE scanner for BTHome advertisements (UUID `0xFCD2`) and parses:

- Temperature
- Humidity
- Battery

Current target MAC address is configured in `main/main.c` in `bthome_listener_start("a4:c1:38:a0:0d:98")`.
Set empty string or `NULL` if you want to disable target filtering.

## Notes

- `main/secrets.h` is ignored by git and should never be committed.
- Camera pin mapping and quality/frame-size settings are in `main/camera_core/config.h`.
- Default stream frame size is VGA, while `/capture_human` uses UXGA.
