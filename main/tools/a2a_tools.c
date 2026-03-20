#include "a2a_tools.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "bthome_listener.h"

static void tool_ble_execute(char *out, size_t out_size)
{
    thome_reading_t reading = {0};
    bool has = bthome_listener_get_latest(&reading);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (!has) {
        snprintf(out, out_size, "{\"ok\":true,\"status\":\"no_data\"}");
        return;
    }

    snprintf(out, out_size,
             "{\"ok\":true,\"status\":\"ok\",\"source_addr\":\"%s\",\"last_seen_ms\":%" PRIu32 ","
             "\"encrypted\":%s,\"temperature\":%.2f,\"temperature_valid\":%s,\"humidity\":%.2f,"
             "\"humidity_valid\":%s,\"battery\":%d,\"battery_valid\":%s}",
             reading.source_addr,
             now_ms - reading.last_seen_ms,
             reading.encrypted ? "true" : "false",
             reading.temperature_c,
             reading.temperature_valid ? "true" : "false",
             reading.humidity_percent,
             reading.humidity_valid ? "true" : "false",
             reading.battery_percent,
             reading.battery_valid ? "true" : "false");
}

static void tool_camera_execute(char *out, size_t out_size)
{
    snprintf(out, out_size,
             "{\"ok\":true,\"status\":\"ok\",\"stream\":\"/stream\",\"capture\":\"/capture\","
             "\"capture_human\":\"/capture_human\",\"get_thome\":\"/get_thome\"}");
}

cJSON *a2a_tools_build_schema(void)
{
    cJSON *tools = cJSON_CreateArray();
    if (!tools) {
        return NULL;
    }

    cJSON *tool_ble = cJSON_CreateObject();
    cJSON *ble_fn = cJSON_CreateObject();
    cJSON *ble_params = cJSON_CreateObject();
    cJSON *ble_props = cJSON_CreateObject();
    cJSON *ble_required = cJSON_CreateArray();
    cJSON_AddStringToObject(tool_ble, "type", "function");
    cJSON_AddStringToObject(ble_fn, "name", "tool_ble");
    cJSON_AddStringToObject(ble_fn, "description", "Get latest BLE BTHome sensor reading from this device.");
    cJSON_AddStringToObject(ble_params, "type", "object");
    cJSON_AddItemToObject(ble_params, "properties", ble_props);
    cJSON_AddItemToObject(ble_params, "required", ble_required);
    cJSON_AddItemToObject(ble_fn, "parameters", ble_params);
    cJSON_AddItemToObject(tool_ble, "function", ble_fn);
    cJSON_AddItemToArray(tools, tool_ble);

    cJSON *tool_camera = cJSON_CreateObject();
    cJSON *cam_fn = cJSON_CreateObject();
    cJSON *cam_params = cJSON_CreateObject();
    cJSON *cam_props = cJSON_CreateObject();
    cJSON *cam_required = cJSON_CreateArray();
    cJSON_AddStringToObject(tool_camera, "type", "function");
    cJSON_AddStringToObject(cam_fn, "name", "tool_camera");
    cJSON_AddStringToObject(cam_fn, "description", "Get camera endpoint/status information from this device.");
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
        tool_camera_execute(out, out_size);
        return ESP_OK;
    }

    snprintf(out, out_size, "{\"ok\":false,\"error\":\"unknown_tool\"}");
    return ESP_ERR_NOT_FOUND;
}
