#include "llm_chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "secrets.h"
#include "a2a_tools.h"

static const char *TAG = "llm_chat";

#ifndef LLM_API_KEY
#define LLM_API_KEY ""
#endif
#ifndef LLM_MODEL
#define LLM_MODEL "qwen3.5-plus-2026-02-15"
#endif
#ifndef LLM_API_URL
#define LLM_API_URL "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
#endif

#define LLM_RESP_BUF_SIZE   (24 * 1024)
#define LLM_MAX_TOOL_ITERS  3

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_accum_t;

static esp_err_t http_accum_event_handler(esp_http_client_event_t *evt)
{
    http_accum_t *acc = (http_accum_t *)evt->user_data;
    if (!acc || evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t need = acc->len + evt->data_len + 1;
    if (need > acc->cap) {
        size_t new_cap = acc->cap ? acc->cap : 1024;
        while (new_cap < need) {
            new_cap *= 2;
        }
        char *tmp = (char *)realloc(acc->data, new_cap);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        acc->data = tmp;
        acc->cap = new_cap;
    }

    memcpy(acc->data + acc->len, evt->data, evt->data_len);
    acc->len += evt->data_len;
    acc->data[acc->len] = '\0';
    return ESP_OK;
}

static esp_err_t llm_http_post_json(const char *body_json, char **out_resp, int *out_status)
{
    if (!body_json || !out_resp || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_resp = NULL;
    *out_status = 0;

    http_accum_t acc = {0};
    acc.cap = 2048;
    acc.data = (char *)calloc(1, acc.cap);
    if (!acc.data) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = LLM_API_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_accum_event_handler,
        .user_data = &acc,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(acc.data);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (LLM_API_KEY[0] != '\0') {
        char auth[320];
        snprintf(auth, sizeof(auth), "Bearer %s", LLM_API_KEY);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, body_json, strlen(body_json));

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(acc.data);
        return err;
    }

    if (acc.len >= LLM_RESP_BUF_SIZE) {
        free(acc.data);
        return ESP_ERR_NO_MEM;
    }

    *out_resp = acc.data;
    return ESP_OK;
}

esp_err_t llm_chat_with_tools(const char *user_text, char *reply, size_t reply_size)
{
    if (!user_text || !reply || reply_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (LLM_API_KEY[0] == '\0') {
        snprintf(reply, reply_size, "LLM_API_KEY is not configured.");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *messages = cJSON_CreateArray();
    cJSON *tools = a2a_tools_build_schema();
    if (!messages || !tools) {
        cJSON_Delete(messages);
        cJSON_Delete(tools);
        return ESP_ERR_NO_MEM;
    }

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_text);
    cJSON_AddItemToArray(messages, user_msg);

    esp_err_t final_ret = ESP_FAIL;
    for (int iter = 0; iter < LLM_MAX_TOOL_ITERS; iter++) {
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "model", LLM_MODEL);
        cJSON_AddNumberToObject(payload, "max_tokens", 1024);
        cJSON_AddStringToObject(payload, "tool_choice", "auto");
        cJSON_AddItemToObject(payload, "messages", cJSON_Duplicate(messages, 1));
        cJSON_AddItemToObject(payload, "tools", cJSON_Duplicate(tools, 1));

        char *body = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        if (!body) {
            final_ret = ESP_ERR_NO_MEM;
            break;
        }

        char *resp = NULL;
        int status = 0;
        esp_err_t http_ret = llm_http_post_json(body, &resp, &status);
        free(body);
        if (http_ret != ESP_OK || !resp || status < 200 || status >= 300) {
            ESP_LOGE(TAG, "LLM HTTP failed: err=%s status=%d", esp_err_to_name(http_ret), status);
            free(resp);
            final_ret = ESP_FAIL;
            break;
        }

        cJSON *root = cJSON_Parse(resp);
        free(resp);
        if (!root) {
            final_ret = ESP_FAIL;
            break;
        }

        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *message = cJSON_IsObject(choice0) ? cJSON_GetObjectItem(choice0, "message") : NULL;
        cJSON *content = cJSON_IsObject(message) ? cJSON_GetObjectItem(message, "content") : NULL;
        cJSON *tool_calls = cJSON_IsObject(message) ? cJSON_GetObjectItem(message, "tool_calls") : NULL;

        if (!tool_calls || !cJSON_IsArray(tool_calls) || cJSON_GetArraySize(tool_calls) == 0) {
            const char *text = (content && cJSON_IsString(content) && content->valuestring)
                                   ? content->valuestring
                                   : "";
            strlcpy(reply, text, reply_size);
            cJSON_Delete(root);
            final_ret = ESP_OK;
            break;
        }

        cJSON *assistant_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(assistant_msg, "role", "assistant");
        if (content && cJSON_IsString(content) && content->valuestring) {
            cJSON_AddStringToObject(assistant_msg, "content", content->valuestring);
        } else {
            cJSON_AddStringToObject(assistant_msg, "content", "");
        }
        cJSON_AddItemToObject(assistant_msg, "tool_calls", cJSON_Duplicate(tool_calls, 1));
        cJSON_AddItemToArray(messages, assistant_msg);

        int call_count = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < call_count; i++) {
            cJSON *call = cJSON_GetArrayItem(tool_calls, i);
            cJSON *id = cJSON_GetObjectItem(call, "id");
            cJSON *fn = cJSON_GetObjectItem(call, "function");
            cJSON *name = cJSON_IsObject(fn) ? cJSON_GetObjectItem(fn, "name") : NULL;

            char tool_result[512] = {0};
            if (name && cJSON_IsString(name) && name->valuestring) {
                (void)a2a_tools_execute(name->valuestring, tool_result, sizeof(tool_result));
            } else {
                snprintf(tool_result, sizeof(tool_result), "{\"ok\":false,\"error\":\"invalid_tool_call\"}");
            }

            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            if (id && cJSON_IsString(id) && id->valuestring) {
                cJSON_AddStringToObject(tool_msg, "tool_call_id", id->valuestring);
            } else {
                cJSON_AddStringToObject(tool_msg, "tool_call_id", "tool_call_unknown");
            }
            cJSON_AddStringToObject(tool_msg, "content", tool_result);
            cJSON_AddItemToArray(messages, tool_msg);
        }

        cJSON_Delete(root);
    }

    if (final_ret != ESP_OK && reply[0] == '\0') {
        strlcpy(reply, "Sorry, I cannot reach cloud LLM right now.", reply_size);
    }

    cJSON_Delete(messages);
    cJSON_Delete(tools);
    return final_ret;
}
