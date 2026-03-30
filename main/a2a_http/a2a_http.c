#include "a2a_http.h"

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

#include "config.h"
#include "camera_core.h"
#include "bthome_listener.h"
#include "a2a/a2a_rpc.h"
#include "a2a/task_manager.h"
#include "llm/llm_chat.h"
#include "tools/a2a_tools.h"

#include "esp_timer.h"
#include "esp_random.h"

static const char *TAG = "a2a_http";
static uint8_t *s_last_capture_jpg = NULL;
static size_t s_last_capture_jpg_len = 0;

#define MSG_REQ_MAX_BYTES   4096
#define MSG_RESP_MAX_BYTES  2048

#define TOKEN_SIZE          64
#define TOKEN_EXPIRY_MS     (10 * 60 * 1000)  // 10 minutes in milliseconds

static char current_token[TOKEN_SIZE + 1] = {0};
static uint64_t token_expiry_time = 0;

static void generate_new_token(void)
{
    // Generate random token using ESP32 hardware random
    uint8_t bytes[TOKEN_SIZE / 2];
    for (int i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (uint8_t)(esp_random() & 0xFF);
    }

    // Convert to hex string
    for (int i = 0; i < sizeof(bytes); i++) {
        sprintf(&current_token[i * 2], "%02x", bytes[i]);
    }
    current_token[TOKEN_SIZE] = '\0';

    // Set expiry time (10 minutes from now)
    token_expiry_time = esp_timer_get_time() / 1000ULL + TOKEN_EXPIRY_MS;
}

static bool check_auth(httpd_req_t *req)
{
    // If no token generated yet, initialize with random token
    if (current_token[0] == '\0') {
        generate_new_token();
    }

    // Check if token is expired - if expired, reject even if token is correct
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    if (now_ms >= token_expiry_time) {
        return false;
    }

    const char *expected = current_token;
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

// Export for tool use
const char *a2a_http_get_current_token(void)
{
    return current_token;
}

void a2a_http_regenerate_token(void)
{
    generate_new_token();
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

static esp_err_t capture_hr_handler(httpd_req_t *req)
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

static esp_err_t get_th_handler(httpd_req_t *req)
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
    // authentication removed per requirement

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

    ESP_LOGI(TAG, "message/send received: %.80s%s",
             msg_text,
             strlen(msg_text) > 80 ? "..." : "");

    // Always enqueue as async task - llm_chat_with_tools runs in task executor
    const a2a_task_t *task = a2a_task_create(msg_text, A2A_TASK_STATE_QUEUED);
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

    // Wait up to 10 seconds for task completion - short tasks return synchronously, long tasks remain async
    uint64_t start_ms = esp_timer_get_time() / 1000ULL;
    const a2a_task_t *updated_task = task;
    while (true) {
        uint64_t elapsed_ms = (esp_timer_get_time() / 1000ULL) - start_ms;
        if (elapsed_ms >= A2A_SYNC_TIMEOUT_MS) {
            // Timeout after 10s, return whatever we have
            updated_task = a2a_task_get(task->id);
            break;
        }

        updated_task = a2a_task_get(task->id);
        if (!updated_task) {
            break;
        }

        // If task is completed or failed, return immediately
        if (strcmp(updated_task->state, A2A_TASK_STATE_COMPLETED) == 0 ||
            strcmp(updated_task->state, A2A_TASK_STATE_FAILED) == 0) {
            break;
        }

        // Wait a bit before checking again
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    char *resp_str = a2a_build_task_result(id, updated_task);
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
    // authentication removed per requirement

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

        ESP_LOGI(TAG, "tasks/get received for task_id=%s", task_id);

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
    return httpd_resp_sendstr(req,
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32601,\"message\":\"Method not found\",\"data\":\"supported: tasks/get\"}}");
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
        "\"schema_version\":\"a2a-protocol-v1.0.0\","
        "\"id\":\"swarmclaw-sense\","
        "\"name\":\"SwarmClaw Sense\","
        "\"description\":\"ESP32 camera and BTHome edge agent for A2A agent-to-agent communication. All messages processed asynchronously by LLM with tool support.\","
        "\"endpoints\":{"
            "\"stream\":\"/stream\","
            "\"capture\":\"/capture\","
            "\"capture_hr\":\"/capture_hr\","
            "\"get_th\":\"/get_th\","
            "\"message_send\":\"/message/send\","
            "\"tasks_get\":\"/tasks/get\""
        "},"
        "\"usage\":{"
            "\"message/send\":{"
                "\"method\":\"POST\","
                "\"description\":\"Send a message to the agent for LLM processing. Waits up to 10 seconds - short tasks return result synchronously, long tasks return queued task ID for polling.\","
                "\"request_params\":{"
                    "\"jsonrpc\":\"2.0\","
                    "\"id\":\"<request-id>\","
                    "\"method\":\"message/send\","
                    "\"params\":{"
                        "\"message_text\":\"<user-message>\""
                    "}"
                "},"
                "\"response\":\"Returns task object with id and queued state. Poll result via /tasks/get\""
            "},"
            "\"tasks/get\":{"
                "\"method\":\"POST\","
                "\"description\":\"Get the current state and result of a previously queued task\","
                "\"request_params\":{"
                    "\"jsonrpc\":\"2.0\","
                    "\"id\":\"<request-id>\","
                    "\"method\":\"tasks/get\","
                    "\"params\":{"
                        "\"task_id\":\"<task-id-from-message-send>\""
                    "}"
                "},"
                "\"response\":\"Returns task object with current state and output if completed\""
            "}"
        "},"
        "\"message_send_params\":{"
            "\"type\":\"object\","
            "\"properties\":{"
                "\"message_text\":{\"type\":\"string\"}"
            "},"
            "\"required\":[\"message_text\"]"
        "},"
        "\"auth\":{"
            "\"type\":\"bearer_or_query_token\","
            "\"header\":\"Authorization: Bearer <token>\","
            "\"query\":\"token=<token>\","
            "\"description\":\"Authentication required for /stream, /capture, /capture_hr, /get_th. /message/send and /tasks/get do not require authentication. Token is dynamic and expires after 10 minutes. Regenerate via tool_get_token tool or serial CLI 'get_token' command.\""
        "},"
        "\"tools\":[\"tool_ble\",\"tool_camera\",\"tool_get_token\"],"
        "\"task_mode\":\"hybrid_sync_async\""
        "}";

    return httpd_resp_sendstr(req, body);
}

void a2a_http_start_server(void)
{
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

        httpd_uri_t capture_hr_uri = {
            .uri = "/capture_hr",
            .method = HTTP_GET,
            .handler = capture_hr_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &capture_hr_uri);

        httpd_uri_t get_th_uri = {
            .uri = "/get_th",
            .method = HTTP_GET,
            .handler = get_th_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &get_th_uri);

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
