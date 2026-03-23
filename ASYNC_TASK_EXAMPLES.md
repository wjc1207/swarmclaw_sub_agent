# Async Task Support - Usage Examples

The SwarmClaw Sub Agent now supports both **synchronous** and **asynchronous** task execution for LLM processing and device tools.

## Task Modes

### Synchronous Tasks (Default)
- Request → Execute immediately → Response with result
- Blocks until completion
- Use for fast operations or when you need immediate results

### Asynchronous Tasks
- Request → Create task in "queued" state → Immediate response with task ID
- Execution happens in background
- Task state transitions: `queued` → `running` → `completed` (or `failed`)
- Poll with `tasks/get` to check status
- Use for long-running operations

## Examples

### 1. Synchronous LLM Request

**Request:**
```bash
curl -X POST http://localhost:80/message/send \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": "msg-001",
    "method": "message/send",
    "params": {
      "message": "What is the current temperature?"
    }
  }'
```

**Response (immediate, after LLM processing completes):**
```json
{
  "jsonrpc": "2.0",
  "id": "msg-001",
  "result": {
    "id": "task-1679123456789-1",
    "kind": "message",
    "status": "completed",
    "artifacts": [
      {
        "role": "assistant",
        "type": "text",
        "content": "The temperature is 22.5°C based on the latest sensor reading..."
      }
    ]
  }
}
```

---

### 2. Asynchronous LLM Request

**Request (with `isAsync: true`):**
```bash
curl -X POST http://localhost:80/message/send \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": "msg-002",
    "method": "message/send",
    "params": {
      "message": "Analyze the captured image and tell me what you see",
      "isAsync": true
    }
  }'
```

**Response (immediate, before processing completes):**
```json
{
  "jsonrpc": "2.0",
  "id": "msg-002",
  "result": {
    "id": "task-1679123456800-2",
    "kind": "message",
    "status": "queued",
    "artifacts": []
  }
}
```

Now poll for status:

**Polling Request:**
```bash
curl -X POST http://localhost:80/tasks/get \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": "status-001",
    "method": "tasks/get",
    "params": {
      "id": "task-1679123456800-2"
    }
  }'
```

**Response (while running):**
```json
{
  "jsonrpc": "2.0",
  "id": "status-001",
  "result": {
    "id": "task-1679123456800-2",
    "kind": "message",
    "status": "running",
    "artifacts": []
  }
}
```

**Response (after completion):**
```json
{
  "jsonrpc": "2.0",
  "id": "status-001",
  "result": {
    "id": "task-1679123456800-2",
    "kind": "message",
    "status": "completed",
    "artifacts": [
      {
        "role": "assistant",
        "type": "text",
        "content": "The image shows a bright room with blue walls, a desk, and office supplies..."
      }
    ]
  }
}
```

---

### 3. Async Tool Execution - Image FFT Analysis

**Request (async FFT computation):**
```bash
curl -X POST http://localhost:80/message/send \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": "msg-003",
    "method": "message/send",
    "params": {
      "message": "Perform FFT analysis on the latest image to check image quality",
      "isAsync": true
    }
  }'
```

**Immediate Response:**
```json
{
  "jsonrpc": "2.0",
  "id": "msg-003",
  "result": {
    "id": "task-1679123456801-3",
    "kind": "message",
    "status": "queued",
    "artifacts": []
  }
}
```

**Later, when checked:**
```json
{
  "jsonrpc": "2.0",
  "id": "status-002",
  "result": {
    "id": "task-1679123456801-3",
    "kind": "message",
    "status": "completed",
    "artifacts": [
      {
        "role": "assistant",
        "type": "text",
        "content": "FFT analysis complete: samples=256, mag_sum=1234.56, max_mag=567.89, max_freq=5"
      }
    ]
  }
}
```

---

## Task States

| State      | Description                                      |
|-----------|--------------------------------------------------|
| `queued`  | Task created, awaiting execution in background   |
| `running` | Task is currently being executed                 |
| `completed` | Task finished successfully                       |
| `failed`  | Task execution encountered an error              |

---

## Available Tools

All tools are registered with the LLM and can be called via message/send:

1. **tool_ble** - Get latest BLE BTHome sensor reading (temperature, humidity, battery)
2. **tool_camera** - Get camera status and endpoint URLs
3. **tool_image_fft** - Compute FFT analysis of latest captured image (async-capable)

---

## Implementation Details

- **Task Store**: Circular buffer with max 16 concurrent tasks
- **Background Executor**: FreeRTOS task that polls for queued tasks and executes them
- **Thread-Safe**: All task store operations protected by mutex
- **Task Retention**: Tasks remain in store until overwritten by new tasks (circular buffer wraps)

### Task IDs
Format: `task-{timestamp_ms}-{sequence_number}`  
Example: `task-1679123456789-5`

---

## Decision Flow

### Choose **Sync** if:
- Operation finishes quickly (< 1 second)
- Client can wait for response
- Simple request-response pattern

### Choose **Async** if:
- Operation is long-running (> 1 second)
- Client needs to continue and poll later
- Multiple parallel tasks needed
- Better UX for real-time applications
