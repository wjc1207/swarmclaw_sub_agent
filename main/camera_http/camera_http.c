#include "camera_http.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "esp_camera.h"
#include "img_converters.h"

#include "secrets.h"
#include "camera_core.h"
#include "bthome_listener.h"
#include "a2a/a2a_rpc.h"
#include "a2a/task_store.h"
#include "llm/llm_chat.h"
#include "tools/a2a_tools.h"

#include "esp_timer.h"

static const char *TAG = "camera_sta";
static uint8_t *s_last_capture_jpg = NULL;
static size_t s_last_capture_jpg_len = 0;
static SemaphoreHandle_t s_task_executor_running = NULL;

#define MSG_REQ_MAX_BYTES   4096
#define MSG_RESP_MAX_BYTES  2048

typedef enum {
    TASK_TYPE_LLM = 0,
    TASK_TYPE_CAPTURE,
    TASK_TYPE_BLE_THEMO,
    TASK_TYPE_IMAGE_FFT,
} task_type_t;

static bool check_auth(httpd_req_t *req)
{
    const char *expected = CAM_API_TOKEN;
    if (expected[0] == '\0') {
        return true;
    }

    char auth_buf[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf)) == ESP_OK) {
        if (strncmp(auth_buf, "Bearer ", 7) == 0 && strcmp(auth_buf + 7, expected) == 0) {
            return true;
        }
    }

    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char token_val[160];
        if (httpd_query_key_value(query, "token", token_val, sizeof(token_val)) == ESP_OK &&
            strcmp(token_val, expected) == 0) {
            return true;
        }
    }

    return false;
}

static void task_executor_thread(void *arg)
{
    (void)arg;
    while (true) {
        // Find next queued task
        const a2a_task_t *task = a2a_task_find_next_queued();
        if (!task) {
            // No queued tasks, sleep briefly
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Copy task ID and tool name (to avoid dangling pointers)
        char task_id[40];
        char tool_name[64];
        strlcpy(task_id, task->id, sizeof(task_id));
        strlcpy(tool_name, task->tool_name, sizeof(tool_name));

        ESP_LOGI(TAG, "Executing async task %s with tool %s", task_id, tool_name);

        // Mark task as running
        a2a_task_update(task_id, A2A_TASK_STATE_RUNNING, NULL);

        // Execute async task by task type/tool name
        char *result = (char *)calloc(1, 2048);
        if (!result) {
            a2a_task_update(task_id, A2A_TASK_STATE_FAILED, "{\"ok\":false,\"error\":\"oom\"}");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        esp_err_t exec_ret = ESP_ERR_NOT_FOUND;
        if (strcmp(tool_name, "tool_image_fft") == 0 ||
            strcmp(tool_name, "tool_ble") == 0 ||
            strcmp(tool_name, "tool_camera") == 0) {
            exec_ret = a2a_tools_execute(tool_name, result, 2048);
        } else if (strcmp(tool_name, "llm_message") == 0) {
            exec_ret = llm_chat_with_tools(task->input, result, 2048);
        } else {
            snprintf(result, 2048, "{\"ok\":false,\"error\":\"unknown_task_type\",\"tool\":\"%s\"}", tool_name);
        }

        // Mark task as completed or failed
        const char *final_state = (exec_ret == ESP_OK) ? A2A_TASK_STATE_COMPLETED : A2A_TASK_STATE_FAILED;
        a2a_task_update(task_id, final_state, result);
        free(result);

        ESP_LOGI(TAG, "Async task %s completed with state %s", task_id, final_state);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static task_type_t a2a_extract_task_type(cJSON *params, const char *msg_text)
{
    cJSON *task_type_obj = params ? cJSON_GetObjectItem(params, "taskType") : NULL;
    if (cJSON_IsString(task_type_obj) && task_type_obj->valuestring) {
        const char *t = task_type_obj->valuestring;
        if (strcmp(t, "image_fft") == 0) {
            return TASK_TYPE_IMAGE_FFT;
        }
        if (strcmp(t, "capture") == 0) {
            return TASK_TYPE_CAPTURE;
        }
        if (strcmp(t, "ble_themo") == 0 || strcmp(t, "ble_thermo") == 0) {
            return TASK_TYPE_BLE_THEMO;
        }
        if (strcmp(t, "llm") == 0) {
            return TASK_TYPE_LLM;
        }
    }

    if (!msg_text) {
        return TASK_TYPE_LLM;
    }

    if (strstr(msg_text, "image_fft") || strstr(msg_text, "fft") || strstr(msg_text, "FFT")) {
        return TASK_TYPE_IMAGE_FFT;
    }
    if (strstr(msg_text, "ble_themo") || strstr(msg_text, "ble_thermo") || strstr(msg_text, "ble thermo")) {
        return TASK_TYPE_BLE_THEMO;
    }
    if (strstr(msg_text, "capture") || strstr(msg_text, "拍照")) {
        return TASK_TYPE_CAPTURE;
    }

    return TASK_TYPE_LLM;
}

static const char *task_type_to_name(task_type_t task_type)
{
    switch (task_type) {
        case TASK_TYPE_CAPTURE:
            return "capture";
        case TASK_TYPE_BLE_THEMO:
            return "ble_themo";
        case TASK_TYPE_IMAGE_FFT:
            return "image_fft";
        case TASK_TYPE_LLM:
        default:
            return "llm";
    }
}

static const char *task_type_to_tool(task_type_t task_type)
{
    switch (task_type) {
        case TASK_TYPE_CAPTURE:
            return "tool_camera";
        case TASK_TYPE_BLE_THEMO:
            return "tool_ble";
        case TASK_TYPE_IMAGE_FFT:
            return "tool_image_fft";
        case TASK_TYPE_LLM:
        default:
            return "llm_message";
    }
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"camera\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace; boundary=frame";
    static const char *_STREAM_BOUNDARY = "\r\n--frame\r\n";
    static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    while (true) {
        camera_fb_t *fb = NULL;
        esp_err_t acq = camera_core_acquire_fb(&fb, 3, 30, pdMS_TO_TICKS(1000));
        if (acq != ESP_OK || fb == NULL) {
            ESP_LOGE(TAG, "Camera capture failed");
            return ESP_FAIL;
        }

        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;

        if (fb->format == PIXFORMAT_JPEG) {
            jpg_buf = fb->buf;
            jpg_len = fb->len;
        } else {
            if (!fmt2jpg(fb->buf, fb->len, fb->width, fb->height, fb->format, 80, &jpg_buf, &jpg_len)) {
                ESP_LOGE(TAG, "JPEG compression failed");
                camera_core_release_fb(fb);
                return ESP_FAIL;
            }
        }

        char part_buf[64];
        size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, jpg_len);

        if (httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY)) != ESP_OK ||
            httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len) != ESP_OK) {
            if (fb->format != PIXFORMAT_JPEG) {
                free(jpg_buf);
            }
            camera_core_release_fb(fb);
            break;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            free(jpg_buf);
        }
        camera_core_release_fb(fb);

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    return ESP_OK;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"camera\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    camera_fb_t *fb = NULL;
    esp_err_t acq = camera_core_acquire_fb_latest(&fb, pdMS_TO_TICKS(1000));
    if (acq != ESP_OK || fb == NULL) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;

    if (fb->format == PIXFORMAT_JPEG) {
        jpg_buf = fb->buf;
        jpg_len = fb->len;
    } else {
        if (!fmt2jpg(fb->buf, fb->len, fb->width, fb->height, fb->format, 90, &jpg_buf, &jpg_len)) {
            ESP_LOGE(TAG, "JPEG compression failed");
            camera_core_release_fb(fb);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    esp_err_t res = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);

    if (res == ESP_OK && jpg_len > 0) {
        uint8_t *copy = (uint8_t *)malloc(jpg_len);
        if (copy != NULL) {
            memcpy(copy, jpg_buf, jpg_len);
            free(s_last_capture_jpg);
            s_last_capture_jpg = copy;
            s_last_capture_jpg_len = jpg_len;
        } else {
            ESP_LOGW(TAG, "No memory to update snapshot cache");
        }
    }

    if (fb->format != PIXFORMAT_JPEG) {
        free(jpg_buf);
    }
    camera_core_release_fb(fb);

    return res;
}

static esp_err_t capture_human_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"camera\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    camera_fb_t *fb = NULL;
    esp_err_t acq = camera_core_acquire_fb_human(&fb, pdMS_TO_TICKS(2000));
    if (acq != ESP_OK || fb == NULL) {
        ESP_LOGE(TAG, "Human capture failed, err=0x%x", (unsigned int)acq);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Human capture failed", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture_human.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_core_release_fb_human(fb);
    return res;
}

static esp_err_t get_thome_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"camera\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    thome_reading_t reading = {0};
    bool has = bthome_listener_get_latest(&reading);

    uint32_t now_ms = esp_timer_get_time() / 1000;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    if (!has) {
        return httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"no_data\"}");
    }

    char body[384];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"status\":\"ok\","
             "\"source_addr\":\"%s\","
             "\"last_seen_ms\":%" PRIu32 ","
             "\"encrypted\":%s,"
             "\"temperature\":%0.2f,\"temperature_valid\":%s,"
             "\"humidity\":%0.2f,\"humidity_valid\":%s,"
             "\"battery\":%d,\"battery_valid\":%s}",
             reading.source_addr,
             now_ms - reading.last_seen_ms,
             reading.encrypted ? "true" : "false",
             reading.temperature_c,
             reading.temperature_valid ? "true" : "false",
             reading.humidity_percent,
             reading.humidity_valid ? "true" : "false",
             reading.battery_percent,
             reading.battery_valid ? "true" : "false");

    return httpd_resp_sendstr(req, body);
}

static esp_err_t message_send_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"camera\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > MSG_REQ_MAX_BYTES) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_sendstr(req, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Invalid Request\",\"data\":\"invalid_body_length\"}}");
    }

    char *payload = (char *)calloc(1, req->content_len + 1);
    if (!payload) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_sendstr(req,
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"oom\"}}");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, payload + received, req->content_len - received);
        if (ret <= 0) {
            free(payload);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            return httpd_resp_sendstr(req,
                "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"recv_failed\"}}");
        }
        received += ret;
    }

    cJSON *root = NULL;
    cJSON *id = NULL;
    const char *method = NULL;
    cJSON *params = NULL;
    char *parse_error_json = NULL;
    if (!a2a_parse_rpc_request(payload, &root, &id, &method, &params, &parse_error_json)) {
        free(payload);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        if (!parse_error_json) {
            parse_error_json = a2a_build_jsonrpc_error(NULL, -32603, "Internal error", "oom");
        }
        if (!parse_error_json) {
            return httpd_resp_sendstr(req,
                "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
        }
        esp_err_t ret = httpd_resp_sendstr(req, parse_error_json);
        free(parse_error_json);
        return ret;
    }

    if (strcmp(method, "tasks/get") == 0) {
        const char *task_id = a2a_extract_task_id(params);
        if (!task_id || task_id[0] == '\0') {
            char *err = a2a_build_jsonrpc_error(id, -32602, "Invalid params", "task id is required");
            cJSON_Delete(root);
            free(payload);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            if (!err) {
                return httpd_resp_sendstr(req,
                    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
            }
            esp_err_t ret = httpd_resp_sendstr(req, err);
            free(err);
            return ret;
        }

        const a2a_task_t *task = a2a_task_get(task_id);
        if (!task) {
            char *err = a2a_build_jsonrpc_error(id, -32004, "Task not found", task_id);
            cJSON_Delete(root);
            free(payload);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            if (!err) {
                return httpd_resp_sendstr(req,
                    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
            }
            esp_err_t ret = httpd_resp_sendstr(req, err);
            free(err);
            return ret;
        }

        char *resp_str = a2a_build_task_result(id, task);
        cJSON_Delete(root);
        free(payload);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_hdr(req, "Expires", "0");
        if (!resp_str) {
            return httpd_resp_sendstr(req,
                "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"oom\"}}");
        }
        esp_err_t ret = httpd_resp_sendstr(req, resp_str);
        free(resp_str);
        return ret;
    }

    if (strcmp(method, "message/send") != 0) {
        char *err = a2a_build_jsonrpc_error(id, -32601, "Method not found", "supported: message/send, tasks/get");
        cJSON_Delete(root);
        free(payload);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        if (!err) {
            return httpd_resp_sendstr(req,
                "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
        }
        esp_err_t ret = httpd_resp_sendstr(req, err);
        free(err);
        return ret;
    }

    const char *msg_text = a2a_extract_message_text(params);
    if (!msg_text || msg_text[0] == '\0') {
        char *err = a2a_build_jsonrpc_error(id, -32602, "Invalid params", "message text is required");
        cJSON_Delete(root);
        free(payload);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        if (!err) {
            return httpd_resp_sendstr(req,
                "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
        }
        esp_err_t ret = httpd_resp_sendstr(req, err);
        free(err);
        return ret;
    }

    task_type_t task_type = a2a_extract_task_type(params, msg_text);
    bool is_async = (task_type == TASK_TYPE_IMAGE_FFT);
    const char *task_type_name = task_type_to_name(task_type);
    const char *tool_name = task_type_to_tool(task_type);

    ESP_LOGI(TAG, "message/send received type=%s mode=%s: %.80s%s",
             task_type_name,
             is_async ? "async" : "sync",
             msg_text,
             strlen(msg_text) > 80 ? "..." : "");

    const a2a_task_t *task = NULL;
    char answer[MSG_RESP_MAX_BYTES] = {0};

    if (is_async) {
        // image_fft is always async: enqueue and return task id immediately
        task = a2a_task_create(msg_text, A2A_TASK_STATE_QUEUED, tool_name);
        if (!task) {
            char *err = a2a_build_jsonrpc_error(id, -32603, "Internal error", "task_create_failed");
            cJSON_Delete(root);
            free(payload);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            if (!err) {
                return httpd_resp_sendstr(req,
                    "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
            }
            esp_err_t ret = httpd_resp_sendstr(req, err);
            free(err);
            return ret;
        }
    } else {
        // capture / ble_themo / default llm are sync
        esp_err_t exec_ret = ESP_OK;
        if (task_type == TASK_TYPE_CAPTURE || task_type == TASK_TYPE_BLE_THEMO) {
            exec_ret = a2a_tools_execute(tool_name, answer, sizeof(answer));
        } else {
            exec_ret = llm_chat_with_tools(msg_text, answer, sizeof(answer));
        }

        if (exec_ret != ESP_OK) {
            snprintf(answer, sizeof(answer),
                     "{\"ok\":false,\"error\":\"exec_failed\",\"task_type\":\"%s\",\"esp_err\":%d}",
                     task_type_name, (int)exec_ret);
        }
        task = a2a_task_create_completed(msg_text, answer);
    }

    char *resp_str = a2a_build_task_result(id, task);
    cJSON_Delete(root);
    free(payload);

    if (!resp_str) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_sendstr(req,
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"oom\"}}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    esp_err_t send_ret = httpd_resp_sendstr(req, resp_str);
    free(resp_str);
    return send_ret;
}

static esp_err_t tasks_get_handler(httpd_req_t *req)
{
    return message_send_handler(req);
}

static esp_err_t agent_well_known_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    static const char *body =
        "{"
        "\"schema_version\":\"a2a-sim-1\","
        "\"id\":\"swarmclaw-sub-agent\","
        "\"name\":\"SwarmClaw Sub Agent\","
        "\"description\":\"ESP32 camera and BTHome edge agent for A2A simulation with async task support\","
        "\"endpoints\":{"
            "\"stream\":\"/stream\"," 
            "\"capture\":\"/capture\"," 
            "\"capture_human\":\"/capture_human\"," 
            "\"get_thome\":\"/get_thome\"," 
            "\"message_send\":\"/message/send\"," 
            "\"tasks_get\":\"/tasks/get\""
        "},"
        "\"auth\":{"
            "\"type\":\"bearer_or_query_token\","
            "\"header\":\"Authorization: Bearer <token>\","
            "\"query\":\"token=<token>\""
        "},"
        "\"tools\":[\"tool_ble\",\"tool_camera\",\"tool_image_fft\"],"
        "\"task_modes\":[\"sync\",\"async\"]"
        "}";

    return httpd_resp_sendstr(req, body);
}

void camera_http_start_server(void)
{
    a2a_task_store_init();

    // Spawn background task executor thread
    if (!s_task_executor_running) {
        s_task_executor_running = xSemaphoreCreateBinary();
        xTaskCreate(task_executor_thread, "task_executor", 8192, NULL, 3, NULL);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = stream_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &stream_uri);

        httpd_uri_t capture_uri = {
            .uri = "/capture",
            .method = HTTP_GET,
            .handler = capture_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &capture_uri);

        httpd_uri_t capture_human_uri = {
            .uri = "/capture_human",
            .method = HTTP_GET,
            .handler = capture_human_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &capture_human_uri);

        httpd_uri_t get_thome_uri = {
            .uri = "/get_thome",
            .method = HTTP_GET,
            .handler = get_thome_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &get_thome_uri);

        httpd_uri_t message_send_uri = {
            .uri = "/message/send",
            .method = HTTP_POST,
            .handler = message_send_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &message_send_uri);

        httpd_uri_t tasks_get_uri = {
            .uri = "/tasks/get",
            .method = HTTP_POST,
            .handler = tasks_get_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &tasks_get_uri);

        httpd_uri_t well_known_agent_card_uri = {
            .uri = "/.well-known/agent-card.json",
            .method = HTTP_GET,
            .handler = agent_well_known_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &well_known_agent_card_uri);

        // For compatibility with older versions of the spec, also serve the agent info at /well-known/agent.json
        httpd_uri_t well_known_agent_uri = {
            .uri = "/.well-known/agent.json",
            .method = HTTP_GET,
            .handler = agent_well_known_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &well_known_agent_uri);
    }
}
