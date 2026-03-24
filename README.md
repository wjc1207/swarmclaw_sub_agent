# Swarmclaw Sub Agent (ESP32-S3 Camera + BLE + A2A Agent)

This project runs on ESP32-S3 and provides an A2A (Agent-to-Agent) compatible HTTP server with:

- Wi-Fi station connection
- OV2640 camera capture and streaming HTTP APIs
- BTHome BLE passive listener (temperature, humidity, battery)
- Cloud LLM inference with local tool calling capability
- Async task execution for long-running LLM requests

## Architecture

```
main/
├── a2a/
│   ├── a2a_rpc.c/h         - JSON-RPC 2.0 utilities
│   └── task_manager.c/h    - Async task storage/management
├── a2a_http/
│   └── a2a_http.c/h        - A2A HTTP server implementation
├── ble/
│   └── bthome_listener.c/h - BTHome BLE scanner
├── camera_core/            - Camera driver and initialization
├── llm/
│   └── llm_chat.c/h        - LLM API client (llm_chat_with_tools)
├── tools/
│   └── a2a_tools.c/h       - Local tool implementations
├── wifi_sta/
│   └── wifi_sta.c/h        - Wi-Fi station connection
├── config.h                - Configuration (pulls in secrets.h if present)
├── secrets.h.example       - Example secrets template
└── main.c                  - Entry point: init everything, starts task executor
```

## Hardware and Software

- Board: ESP32-S3 (with PSRAM)
- Camera: OV2640
- BLE: Xiaomi Mijia Bluetooth Temperature and Humidity Sensor (LYWSD03MMC)
- need BLE OTA firmware update to BTHome broadcast
- https://pvvx.github.io/ATC_MiThermometer/TelinkMiFlasher.html
- Wi-Fi: Connects to configured SSID on boot
- Framework: ESP-IDF

## Quick Start

1. Clone the repository.
2. (Optional) Create secrets file from template if you don't want to edit `config.h` directly:

```bash
cp main/secrets.h.example main/secrets.h
```

3. Edit your configuration - either edit `main/secrets.h` or directly in `main/config.h`:
	- `WIFI_SSID` - Your Wi-Fi name
	- `WIFI_PASS` - Your Wi-Fi password
	- `CAM_API_TOKEN` - HTTP API authentication token (optional, can be empty)
	- `LLM_API_BASE` - Cloud LLM API base URL
	- `LLM_API_KEY` - Cloud LLM API key
	- `LLM_MODEL` - LLM model name to use

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

Camera routes:

- `GET /stream`: MJPEG live stream
- `GET /capture`: capture latest frame (JPEG)
- `GET /snapshot`: return cached last `/capture` image
- `GET /capture_human`: high-resolution still capture
- `GET /get_thome`: latest BLE BTHome reading as JSON

## A2A Agent (JSON-RPC 2.0)

This firmware exposes a lightweight A2A-compatible agent endpoint:

- Agent discovery:
	- `GET /.well-known/agent.json` - Get agent card metadata

### JSON-RPC Methods

#### `message/send` - Send a message for LLM processing

**Request format:**
```json
{
	"jsonrpc": "2.0",
	"id": "req-1",
	"method": "message/send",
	"params": {
		"message_text": "What is the latest temperature reading?"
	}
}
```

**Response:**
```json
{
	"jsonrpc": "2.0",
	"id": "req-1",
	"result": {
		"id": "task-123456789-1",
		"state": "queued",
		...
	}
}
```

All messages are **always asynchronous**. The request returns immediately with a queued task ID.
The actual LLM chat with tool calling executes in a background task executor thread.

#### `tasks/get` - Get task status and result

**Request format:**
```json
{
	"jsonrpc": "2.0",
	"id": "status-1",
	"method": "tasks/get",
	"params": {
		"task_id": "task-123456789-1"
	}
}
```

**Response (completed):**
```json
{
	"jsonrpc": "2.0",
	"id": "status-1",
	"result": {
		"id": "task-123456789-1",
		"state": "completed",
		"input": "What is the latest temperature?",
		"output": "{...LLM response...}",
		"created_ms": 123456789,
		"updated_ms": 123456795
	}
}
```

Task states:
- `queued` - Waiting in execution queue
- `running` - Currently executing
- `completed` - Completed successfully, `output` has result
- `failed` - Execution failed, `output` has error info

## Available Local Tools

The LLM can call these local tools when needed:

| Tool | Description |
|------|-------------|
| `tool_ble` | Get latest BTHome sensor reading from this device (temperature, humidity, battery) |
| `tool_camera` | Get camera endpoint/status information from this device |

Example tool call by LLM will automatically execute the tool and return results to the LLM for final response synthesis.

## cURL Examples

Send a message:

```bash
curl -X POST "http://<device_ip>/message/send?token=<token>" \
	-H "Content-Type: application/json" \
	-d '{
		"jsonrpc":"2.0",
		"id":"req-1",
		"method":"message/send",
		"params":{"message_text":"What is the latest temperature reading?"}
	}'
```

Poll task status:

```bash
curl -X POST "http://<device_ip>/tasks/get?token=<token>" \
	-H "Content-Type: application/json" \
	-d '{
		"jsonrpc":"2.0",
		"id":"status-1",
		"method":"tasks/get",
		"params":{"task_id":"task-1679123456789-1"}
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

## BLE BTHome Listener

The firmware starts a BLE scanner for BTHome advertisements (UUID `0xFCD2`) and parses:

- Temperature (°C)
- Humidity (% RH)
- Battery percentage

Current target MAC address is configured in [`main/main.c`](main/main.c) in `bthome_listener_start("a4:c1:38:a0:0d:98")`.
Set `NULL` if you want to disable target filtering (accepts any BTHome advertisement).

## Configuration

All configuration goes through [`main/config.h`](main/config.h):

- If `secrets.h` exists, it is included and overrides defaults
- If `secrets.h` does not exist, all configs default to empty strings
- `secrets.h` is git-ignored and should never contain committed secrets

## Notes

- `main/secrets.h` is ignored by git and should never be committed.
- Camera pin mapping and quality/frame-size settings are in `main/camera_core/config.h`.
- Default stream frame size is VGA, while `/capture_human` uses UXGA.
- Async task execution is handled by a dedicated FreeRTOS task spawned from `main.c`.
