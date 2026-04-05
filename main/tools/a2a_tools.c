#include "a2a_tools.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "esp_log.h"
#include "img_converters.h"
#include "mbedtls/base64.h"

#include "bthome_listener.h"
#include "camera_core.h"

static const char *TAG = "a2a_tools";

#define TOOL_CAMERA_MAX_JPEG_BYTES  (64 * 1024)

static bool tool_parse_analyze_flag(const char *input)
{
    if (!input || input[0] == '\0') {
        return true;
    }

    cJSON *args = cJSON_Parse(input);
    if (!args) {
        return true;
    }

    cJSON *analyze = cJSON_GetObjectItemCaseSensitive(args, "analyze");
    bool enabled = true;
    if (cJSON_IsBool(analyze)) {
        enabled = cJSON_IsTrue(analyze);
    }

    cJSON_Delete(args);
    return enabled;
}

static void tool_ble_execute(char *out, size_t out_size)
{
    thome_reading_t reading = {0};
    bool has = bthome_listener_get_latest(&reading);
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;

    if (!has) {
        snprintf(out, out_size,
                 "{\"ok\":true,\"status\":\"no_data\",\"message\":\"No BLE BTHome data yet.\"}");
        return;
    }

    uint64_t age_ms = now_ms >= reading.last_seen_ms ? (now_ms - reading.last_seen_ms) : 0;
    snprintf(out, out_size,
             "{\"ok\":true,\"status\":\"ok\","
             "\"source_addr\":\"%s\","
             "\"age_ms\":%" PRIu64 ","
             "\"temperature\":%.2f,\"temperature_valid\":%s,"
             "\"humidity\":%.2f,\"humidity_valid\":%s,"
             "\"battery\":%d,\"battery_valid\":%s}",
             reading.source_addr,
             age_ms,
             reading.temperature_c,
             reading.temperature_valid ? "true" : "false",
             reading.humidity_percent,
             reading.humidity_valid ? "true" : "false",
             reading.battery_percent,
             reading.battery_valid ? "true" : "false");
}

static void tool_camera_execute(const char *input, char *out, size_t out_size)
{
    bool analyze_enabled = tool_parse_analyze_flag(input);
    camera_fb_t *fb = NULL;
    esp_err_t acq = camera_core_acquire_fb_latest(&fb, pdMS_TO_TICKS(1500));
    if (acq != ESP_OK || fb == NULL) {
        snprintf(out, out_size,
                 "{\"ok\":false,\"error\":\"capture_failed\",\"esp_err\":\"0x%x\"}",
                 (unsigned int)acq);
        return;
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool converted = false;

    if (fb->format == PIXFORMAT_JPEG) {
        jpg_buf = fb->buf;
        jpg_len = fb->len;
    } else {
        if (!fmt2jpg(fb->buf, fb->len, fb->width, fb->height, fb->format, 80, &jpg_buf, &jpg_len)) {
            camera_core_release_fb(fb);
            snprintf(out, out_size, "{\"ok\":false,\"error\":\"jpeg_encode_failed\"}");
            return;
        }
        converted = true;
    }

    if (jpg_len == 0 || jpg_len > TOOL_CAMERA_MAX_JPEG_BYTES) {
        if (converted) {
            free(jpg_buf);
        }
        camera_core_release_fb(fb);
        snprintf(out, out_size,
                 "{\"ok\":false,\"error\":\"image_too_large\",\"max_bytes\":%d,\"actual_bytes\":%u}",
                 TOOL_CAMERA_MAX_JPEG_BYTES,
                 (unsigned int)jpg_len);
        return;
    }

    size_t b64_len = ((jpg_len + 2) / 3) * 4;
    size_t data_url_len = strlen("data:image/jpeg;base64,") + b64_len;
    char *data_url = (char *)malloc(data_url_len + 1);
    if (!data_url) {
        if (converted) {
            free(jpg_buf);
        }
        camera_core_release_fb(fb);
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"oom\"}");
        return;
    }

    memcpy(data_url, "data:image/jpeg;base64,", strlen("data:image/jpeg;base64,"));
    size_t encoded_len = 0;
    int rc = mbedtls_base64_encode((unsigned char *)(data_url + strlen("data:image/jpeg;base64,")),
                                   b64_len + 1,
                                   &encoded_len,
                                   (const unsigned char *)jpg_buf,
                                   jpg_len);
    if (rc != 0) {
        free(data_url);
        if (converted) {
            free(jpg_buf);
        }
        camera_core_release_fb(fb);
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"base64_encode_failed\",\"rc\":%d}", rc);
        return;
    }
    data_url[strlen("data:image/jpeg;base64,") + encoded_len] = '\0';

    snprintf(out, out_size,
             "{\"ok\":true,\"status\":\"captured\",\"image_bytes\":%u,"
             "\"analysis_enabled\":%s,\"image_data_url\":\"%s\"}",
             (unsigned int)jpg_len,
             analyze_enabled ? "true" : "false",
             analyze_enabled ? data_url : "");

    free(data_url);

    if (converted) {
        free(jpg_buf);
    }
    camera_core_release_fb(fb);

    ESP_LOGI(TAG, "tool_camera captured image, bytes=%u analyze=%s",
             (unsigned int)jpg_len,
             analyze_enabled ? "true" : "false");
}

cJSON *a2a_tools_build_schema(void)
{
    cJSON *tools = cJSON_CreateArray();
    if (!tools) {
        return NULL;
    }

    // Tool: tool_ble
    cJSON *tool_ble = cJSON_CreateObject();
    cJSON *ble_fn = cJSON_CreateObject();
    cJSON *ble_params = cJSON_CreateObject();
    cJSON *ble_props = cJSON_CreateObject();
    cJSON *ble_required = cJSON_CreateArray();
    cJSON_AddStringToObject(tool_ble, "type", "function");
    cJSON_AddStringToObject(ble_fn, "name", "tool_ble");
    cJSON_AddStringToObject(ble_fn, "description", "Get latest BLE BTHome temperature/humidity reading from this device.");
    cJSON_AddStringToObject(ble_params, "type", "object");
    cJSON_AddItemToObject(ble_params, "properties", ble_props);
    cJSON_AddItemToObject(ble_params, "required", ble_required);
    cJSON_AddItemToObject(ble_fn, "parameters", ble_params);
    cJSON_AddItemToObject(tool_ble, "function", ble_fn);
    cJSON_AddItemToArray(tools, tool_ble);

    // Tool: tool_camera
    cJSON *tool_camera = cJSON_CreateObject();
    cJSON *cam_fn = cJSON_CreateObject();
    cJSON *cam_params = cJSON_CreateObject();
    cJSON *cam_props = cJSON_CreateObject();
    cJSON *cam_required = cJSON_CreateArray();
    cJSON_AddStringToObject(tool_camera, "type", "function");
    cJSON_AddStringToObject(cam_fn, "name", "tool_camera");
    cJSON_AddStringToObject(cam_fn, "description", "Capture a fresh camera image and return it for visual analysis.");
    cJSON *analyze_prop = cJSON_CreateObject();
    cJSON_AddStringToObject(analyze_prop, "type", "boolean");
    cJSON_AddStringToObject(analyze_prop, "description", "Whether to include image payload for LLM visual analysis. Default true.");
    cJSON_AddItemToObject(cam_props, "analyze", analyze_prop);
    cJSON_AddStringToObject(cam_params, "type", "object");
    cJSON_AddItemToObject(cam_params, "properties", cam_props);
    cJSON_AddItemToObject(cam_params, "required", cam_required);
    cJSON_AddItemToObject(cam_fn, "parameters", cam_params);
    cJSON_AddItemToObject(tool_camera, "function", cam_fn);
    cJSON_AddItemToArray(tools, tool_camera);

    return tools;
}

esp_err_t a2a_tools_execute(const char *name, char *out, size_t out_size)
{
    if (!name || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(name, "tool_ble") == 0) {
        tool_ble_execute(out, out_size);
        return ESP_OK;
    }

    if (strcmp(name, "tool_camera") == 0) {
        tool_camera_execute("{}", out, out_size);
        return ESP_OK;
    }

    snprintf(out, out_size, "{\"ok\":false,\"error\":\"unknown_tool\"}");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t a2a_tools_execute_async(const char *name, const char *input, char *out, size_t out_size)
{
    if (!name || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(name, "tool_camera") == 0) {
        tool_camera_execute(input, out, out_size);
        return ESP_OK;
    }

    return a2a_tools_execute(name, out, out_size);
}
