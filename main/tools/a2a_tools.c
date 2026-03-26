#include "a2a_tools.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "esp_timer.h"

#include "bthome_listener.h"
#include "camera_core.h"
#include "wifi_sta.h"
#include "a2a_http/a2a_http.h"

static void tool_ble_execute(char *out, size_t out_size)
{
    char ip_str[16] = {0};
    wifi_sta_get_ip(ip_str, sizeof(ip_str));

    snprintf(out, out_size, "{\"ok\":true,\"status\":\"connected\",\"url\":\"http://%s/get_th\",\"ip\":\"%s\",\"note\":\"Requires valid access token, call tool_get_token to get a new token\"}",
             ip_str, ip_str);
}

static void tool_camera_execute(char *out, size_t out_size)
{
    char ip_str[16] = {0};
    wifi_sta_get_ip(ip_str, sizeof(ip_str));

    snprintf(out, out_size, "{\"ok\":true,\"status\":\"connected\",\"url\":\"http://%s/capture\",\"ip\":\"%s\",\"note\":\"Requires valid access token, call tool_get_token to get a new token\"}",
             ip_str, ip_str);
}

static void tool_get_token_execute(char *out, size_t out_size)
{
    // Regenerate token immediately
    a2a_http_regenerate_token();
    const char *token = a2a_http_get_current_token();
    uint64_t expiry_ms = 10 * 60 * 1000; // 10 minutes

    snprintf(out, out_size,
             "{\"ok\":true,"
             "\"token\":\"%s\","
             "\"expires_in_ms\":%" PRIu64 ","
             "\"expires_in_minutes\":%.1f}",
             token, expiry_ms, (double)expiry_ms / 60000.0);
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
    cJSON_AddStringToObject(ble_fn, "description", "Get BTHome temperature/humidity endpoint information from this device.");
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
    cJSON_AddStringToObject(cam_fn, "description", "Get camera endpoint/status information from this device.");
    cJSON_AddStringToObject(cam_params, "type", "object");
    cJSON_AddItemToObject(cam_params, "properties", cam_props);
    cJSON_AddItemToObject(cam_params, "required", cam_required);
    cJSON_AddItemToObject(cam_fn, "parameters", cam_params);
    cJSON_AddItemToObject(tool_camera, "function", cam_fn);
    cJSON_AddItemToArray(tools, tool_camera);

    // Tool: tool_get_token
    cJSON *tool_get_token = cJSON_CreateObject();
    cJSON *gt_fn = cJSON_CreateObject();
    cJSON *gt_params = cJSON_CreateObject();
    cJSON *gt_props = cJSON_CreateObject();
    cJSON *gt_required = cJSON_CreateArray();
    cJSON_AddStringToObject(tool_get_token, "type", "function");
    cJSON_AddStringToObject(gt_fn, "name", "tool_get_token");
    cJSON_AddStringToObject(gt_fn, "description", "Generate a new API access token. The new token is valid for 10 minutes and invalidates any previous token. Returns the new token.");
    cJSON_AddStringToObject(gt_params, "type", "object");
    cJSON_AddItemToObject(gt_params, "properties", gt_props);
    cJSON_AddItemToObject(gt_params, "required", gt_required);
    cJSON_AddItemToObject(gt_fn, "parameters", gt_params);
    cJSON_AddItemToObject(tool_get_token, "function", gt_fn);
    cJSON_AddItemToArray(tools, tool_get_token);

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

    if (strcmp(name, "tool_get_token") == 0) {
        tool_get_token_execute(out, out_size);
        return ESP_OK;
    }

    snprintf(out, out_size, "{\"ok\":false,\"error\":\"unknown_tool\"}");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t a2a_tools_execute_async(const char *name, const char *input, char *out, size_t out_size)
{
    // For now, all tools execute synchronously
    // In a future enhancement, long-running tools could be identified and 
    // spawned as background tasks
    (void)input;  // input parameter unused for now
    return a2a_tools_execute(name, out, out_size);
}
