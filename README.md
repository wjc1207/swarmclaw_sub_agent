# Swarmclaw Sub Agent (ESP32-S3 Camera + BLE + HTTP)

This project runs on ESP32-S3 and provides:

- Wi-Fi station connection
- OV2640 camera capture and streaming HTTP APIs
- BTHome BLE passive listener (temperature, humidity, battery)

The firmware initializes camera and BLE scanner, joins Wi-Fi, then starts an HTTP server after getting an IP address.

## Hardware and Software

- Board: ESP32-S3
- Camera: OV2640
- BLE: Xiaomi Mijia Bluetooth Temperature and Humidity Sensor (LYWSD03MMC)
- need BLE OTA firmware update to BTHome broadcast 
- https://pvvx.github.io/ATC_MiThermometer/TelinkMiFlasher.html
- Wi-Fi: Connects to configured SSID on boot
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
- `GET /get_thome`: latest BLE BTHome reading as JSON
- `POST /message/send`: A2A-style JSON-RPC 2.0 message endpoint (LLM + tools)

## A2A Server (JSON-RPC 2.0)

This firmware exposes a lightweight A2A-compatible server surface:

- Discovery:
	- `GET /.well-known/agent.json`
	- `GET /.well-known/agent-card.json`
- Message endpoints:
	- `POST /message/send` (create task - sync or async)
	- `POST /tasks/get` (check task status)

The `/message/send` endpoint accepts JSON-RPC 2.0 requests and returns JSON-RPC 2.0 responses.
Internally it calls a cloud LLM and allows tool calls for:

- `tool_ble`: read latest BTHome sensor data
- `tool_camera`: read camera endpoint/status info
- `tool_image_fft`: compute FFT analysis of latest captured image (async-capable)

### Task Execution Modes

**Synchronous (default):**
```json
{
	"jsonrpc": "2.0",
	"id": "req-1",
	"method": "message/send",
	"params": {
		"message": "What is the latest temperature?"
	}
}
```
Returns result immediately after LLM completes processing.

**Asynchronous (set `isAsync: true`):**
```json
{
	"jsonrpc": "2.0",
	"id": "req-2",
	"method": "message/send",
	"params": {
		"message": "Analyze the captured image in detail",
		"isAsync": true
	}
}
```
Returns task ID immediately, execution happens in background. Poll with `tasks/get` to check status.

### Request Example

```json
{
	"jsonrpc": "2.0",
	"id": "req-1",
	"method": "message/send",
	"params": {
		"message": {
			"parts": [
				{ "type": "text", "text": "What is the latest temperature?" }
			]
		}
	}
}
```

### Success Response Example

```json
{
	"jsonrpc": "2.0",
	"id": "req-1",
	"result": {
		"message": {
			"role": "assistant",
			"parts": [
				{ "type": "text", "text": "Latest temperature is 25.31 C." }
			]
		}
	}
}
```

### Error Response Example

```json
{
	"jsonrpc": "2.0",
	"id": "req-1",
	"error": {
		"code": -32602,
		"message": "Invalid params",
		"data": "message text is required"
	}
}
```

### cURL Example

```bash
curl -X POST "http://<device_ip>/message/send?token=<token>" \
	-H "Content-Type: application/json" \
	-d '{
		"jsonrpc":"2.0",
		"id":"req-1",
		"method":"message/send",
		"params":{"text":"Summarize current BLE status"}
	}'
```

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

### Task Status Polling

For async tasks, use `tasks/get` to check status:

```bash
curl -X POST "http://<device_ip>/tasks/get?token=<token>" \
	-H "Content-Type: application/json" \
	-d '{
		"jsonrpc":"2.0",
		"id":"status-1",
		"method":"tasks/get",
		"params":{"id":"task-1679123456789-1"}
	}'
```

Response includes task state: `queued`, `running`, `completed`, or `failed`.

### Image FFT Analysis Tool

The `tool_image_fft` performs Fast Fourier Transform analysis on the latest captured image:

```bash
curl -X POST "http://<device_ip>/message/send?token=<token>" \
	-H "Content-Type: application/json" \
	-d '{
		"jsonrpc":"2.0",
		"id":"req-3",
		"method":"message/send",
		"params":{
			"message":"Analyze image quality using FFT",
			"isAsync":true
		}
	}'
```

Returns FFT magnitude spectrum summary (samples count, max frequency, magnitude sum) which can be used for:
- Image quality assessment
- Blur detection
- Frequency content analysis

**See [ASYNC_TASK_EXAMPLES.md](ASYNC_TASK_EXAMPLES.md) for detailed examples of sync/async task usage.**

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
