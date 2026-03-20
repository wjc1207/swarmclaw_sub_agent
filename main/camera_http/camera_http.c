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
#include "llm/llm_chat.h"

#include "esp_timer.h"

static const char *TAG = "camera_sta";
static uint8_t *s_last_capture_jpg = NULL;
static size_t s_last_capture_jpg_len = 0;

#define MSG_REQ_MAX_BYTES   4096
#define MSG_RESP_MAX_BYTES  2048

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
        return httpd_resp_sendstr(req,
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Invalid Request\",\"data\":\"invalid_body_length\"}}");
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

    cJSON *root = cJSON_Parse(payload);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        free(payload);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_sendstr(req,
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\",\"data\":\"invalid_json\"}}");
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *jsonrpc = cJSON_GetObjectItem(root, "jsonrpc");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0) {
        cJSON *err_resp = cJSON_CreateObject();
        cJSON *err_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(err_resp, "jsonrpc", "2.0");
        cJSON_AddItemToObject(err_resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
        cJSON_AddNumberToObject(err_obj, "code", -32600);
        cJSON_AddStringToObject(err_obj, "message", "Invalid Request");
        cJSON_AddStringToObject(err_obj, "data", "jsonrpc must be 2.0");
        cJSON_AddItemToObject(err_resp, "error", err_obj);
        char *resp_str = cJSON_PrintUnformatted(err_resp);
        cJSON_Delete(err_resp);
        cJSON_Delete(root);
        free(payload);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        if (!resp_str) {
            return httpd_resp_sendstr(req,
                "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
        }
        esp_err_t ret = httpd_resp_sendstr(req, resp_str);
        free(resp_str);
        return ret;
    }

    if (!cJSON_IsString(method) || strcmp(method->valuestring, "message/send") != 0) {
        cJSON *err_resp = cJSON_CreateObject();
        cJSON *err_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(err_resp, "jsonrpc", "2.0");
        cJSON_AddItemToObject(err_resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
        cJSON_AddNumberToObject(err_obj, "code", -32601);
        cJSON_AddStringToObject(err_obj, "message", "Method not found");
        cJSON_AddStringToObject(err_obj, "data", "expected method=message/send");
        cJSON_AddItemToObject(err_resp, "error", err_obj);
        char *resp_str = cJSON_PrintUnformatted(err_resp);
        cJSON_Delete(err_resp);
        cJSON_Delete(root);
        free(payload);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        if (!resp_str) {
            return httpd_resp_sendstr(req,
                "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
        }
        esp_err_t ret = httpd_resp_sendstr(req, resp_str);
        free(resp_str);
        return ret;
    }

    if (!params || !cJSON_IsObject(params)) {
        cJSON *err_resp = cJSON_CreateObject();
        cJSON *err_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(err_resp, "jsonrpc", "2.0");
        cJSON_AddItemToObject(err_resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
        cJSON_AddNumberToObject(err_obj, "code", -32602);
        cJSON_AddStringToObject(err_obj, "message", "Invalid params");
        cJSON_AddStringToObject(err_obj, "data", "params object is required");
        cJSON_AddItemToObject(err_resp, "error", err_obj);
        char *resp_str = cJSON_PrintUnformatted(err_resp);
        cJSON_Delete(err_resp);
        cJSON_Delete(root);
        free(payload);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        if (!resp_str) {
            return httpd_resp_sendstr(req,
                "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
        }
        esp_err_t ret = httpd_resp_sendstr(req, resp_str);
        free(resp_str);
        return ret;
    }

    cJSON *message = cJSON_GetObjectItem(params, "message");
    cJSON *text = cJSON_GetObjectItem(params, "text");
    const char *msg_text = NULL;
    if (cJSON_IsString(message) && message->valuestring) {
        msg_text = message->valuestring;
    } else if (cJSON_IsObject(message)) {
        cJSON *m_content = cJSON_GetObjectItem(message, "content");
        cJSON *m_text = cJSON_GetObjectItem(message, "text");
        cJSON *m_parts = cJSON_GetObjectItem(message, "parts");
        if (cJSON_IsString(m_content) && m_content->valuestring) {
            msg_text = m_content->valuestring;
        } else if (cJSON_IsString(m_text) && m_text->valuestring) {
            msg_text = m_text->valuestring;
        } else if (cJSON_IsArray(m_parts) && cJSON_GetArraySize(m_parts) > 0) {
            cJSON *part0 = cJSON_GetArrayItem(m_parts, 0);
            cJSON *p_text = cJSON_IsObject(part0) ? cJSON_GetObjectItem(part0, "text") : NULL;
            if (cJSON_IsString(p_text) && p_text->valuestring) {
                msg_text = p_text->valuestring;
            }
        }
    } else if (cJSON_IsString(text) && text->valuestring) {
        msg_text = text->valuestring;
    }

    if (!msg_text || msg_text[0] == '\0') {
        cJSON *err_resp = cJSON_CreateObject();
        cJSON *err_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(err_resp, "jsonrpc", "2.0");
        cJSON_AddItemToObject(err_resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
        cJSON_AddNumberToObject(err_obj, "code", -32602);
        cJSON_AddStringToObject(err_obj, "message", "Invalid params");
        cJSON_AddStringToObject(err_obj, "data", "message text is required");
        cJSON_AddItemToObject(err_resp, "error", err_obj);
        char *resp_str = cJSON_PrintUnformatted(err_resp);
        cJSON_Delete(err_resp);
        cJSON_Delete(root);
        free(payload);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        if (!resp_str) {
            return httpd_resp_sendstr(req,
                "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
        }
        esp_err_t ret = httpd_resp_sendstr(req, resp_str);
        free(resp_str);
        return ret;
    }

    ESP_LOGI(TAG, "message/send received: %.80s%s",
             msg_text,
             strlen(msg_text) > 80 ? "..." : "");

    char answer[MSG_RESP_MAX_BYTES] = {0};
    esp_err_t llm_ret = llm_chat_with_tools(msg_text, answer, sizeof(answer));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    if (llm_ret == ESP_OK) {
        cJSON *result = cJSON_CreateObject();
        cJSON *msg = cJSON_CreateObject();
        cJSON *parts = cJSON_CreateArray();
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "assistant");
        cJSON_AddStringToObject(part, "type", "text");
        cJSON_AddStringToObject(part, "text", answer);
        cJSON_AddItemToArray(parts, part);
        cJSON_AddItemToObject(msg, "parts", parts);
        cJSON_AddItemToObject(result, "message", msg);
        cJSON_AddItemToObject(resp, "result", result);
    } else {
        cJSON *err_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(err_obj, "code", -32000);
        cJSON_AddStringToObject(err_obj, "message", "LLM execution failed");
        cJSON_AddStringToObject(err_obj, "data", answer);
        cJSON_AddItemToObject(resp, "error", err_obj);
    }
    char *resp_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);

    free(payload);

    if (!resp_str) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
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
        "\"description\":\"ESP32 camera and BTHome edge agent for A2A simulation\","
        "\"endpoints\":{"
            "\"stream\":\"/stream\"," 
            "\"capture\":\"/capture\"," 
            "\"capture_human\":\"/capture_human\"," 
            "\"get_thome\":\"/get_thome\"," 
            "\"message_send\":\"/message/send\""
        "},"
        "\"auth\":{"
            "\"type\":\"bearer_or_query_token\","
            "\"header\":\"Authorization: Bearer <token>\","
            "\"query\":\"token=<token>\""
        "},"
        "\"tools\":[\"tool_ble\",\"tool_camera\"]"
        "}";

    return httpd_resp_sendstr(req, body);
}

void camera_http_start_server(void)
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

        httpd_uri_t well_known_agent_card_uri = {
            .uri = "/.well-known/agent-card.json",
            .method = HTTP_GET,
            .handler = agent_well_known_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &well_known_agent_card_uri);

        // compatibility with simple version 
        httpd_uri_t well_known_agent_uri = {
            .uri = "/.well-known/agent.json",
            .method = HTTP_GET,
            .handler = agent_well_known_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &well_known_agent_uri);
    }
}
