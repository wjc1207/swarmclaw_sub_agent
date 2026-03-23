#include "a2a_rpc.h"

#include <string.h>

char *a2a_build_jsonrpc_error(cJSON *id, int code, const char *message, const char *data)
{
    cJSON *err_resp = cJSON_CreateObject();
    cJSON *err_obj = cJSON_CreateObject();
    if (!err_resp || !err_obj) {
        cJSON_Delete(err_resp);
        cJSON_Delete(err_obj);
        return NULL;
    }

    cJSON_AddStringToObject(err_resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(err_resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddNumberToObject(err_obj, "code", code);
    cJSON_AddStringToObject(err_obj, "message", message ? message : "Error");
    if (data) {
        cJSON_AddStringToObject(err_obj, "data", data);
    }
    cJSON_AddItemToObject(err_resp, "error", err_obj);

    char *resp_str = cJSON_PrintUnformatted(err_resp);
    cJSON_Delete(err_resp);
    return resp_str;
}

char *a2a_build_task_result(cJSON *id, const a2a_task_t *task)
{
    if (!task) {
        return NULL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *task_obj = cJSON_CreateObject();
    cJSON *status = cJSON_CreateObject();
    cJSON *artifacts = cJSON_CreateArray();
    cJSON *artifact = cJSON_CreateObject();
    cJSON *parts = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();
    if (!resp || !task_obj || !status || !artifacts || !artifact || !parts || !part) {
        cJSON_Delete(resp);
        cJSON_Delete(task_obj);
        cJSON_Delete(status);
        cJSON_Delete(artifacts);
        cJSON_Delete(artifact);
        cJSON_Delete(parts);
        cJSON_Delete(part);
        return NULL;
    }

    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());

    cJSON_AddStringToObject(task_obj, "id", task->id);
    cJSON_AddStringToObject(task_obj, "kind", "task");

    cJSON_AddStringToObject(status, "state", task->state);
    cJSON_AddNumberToObject(status, "timestamp", (double)task->updated_ms);
    cJSON_AddItemToObject(task_obj, "status", status);

    cJSON_AddStringToObject(artifact, "name", "assistant_response");
    cJSON_AddStringToObject(artifact, "mimeType", "text/plain");
    cJSON_AddStringToObject(part, "type", "text");
    cJSON_AddStringToObject(part, "text", task->output);
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToObject(artifact, "parts", parts);
    cJSON_AddItemToArray(artifacts, artifact);
    cJSON_AddItemToObject(task_obj, "artifacts", artifacts);

    cJSON_AddItemToObject(resp, "result", task_obj);

    char *resp_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return resp_str;
}

const char *a2a_extract_message_text(cJSON *params)
{
    cJSON *message = cJSON_GetObjectItem(params, "message");
    cJSON *text = cJSON_GetObjectItem(params, "text");

    if (cJSON_IsString(message) && message->valuestring) {
        return message->valuestring;
    }

    if (cJSON_IsObject(message)) {
        cJSON *m_content = cJSON_GetObjectItem(message, "content");
        cJSON *m_text = cJSON_GetObjectItem(message, "text");
        cJSON *m_parts = cJSON_GetObjectItem(message, "parts");
        if (cJSON_IsString(m_content) && m_content->valuestring) {
            return m_content->valuestring;
        }
        if (cJSON_IsString(m_text) && m_text->valuestring) {
            return m_text->valuestring;
        }
        if (cJSON_IsArray(m_parts) && cJSON_GetArraySize(m_parts) > 0) {
            cJSON *part0 = cJSON_GetArrayItem(m_parts, 0);
            cJSON *p_text = cJSON_IsObject(part0) ? cJSON_GetObjectItem(part0, "text") : NULL;
            if (cJSON_IsString(p_text) && p_text->valuestring) {
                return p_text->valuestring;
            }
        }
    }

    if (cJSON_IsString(text) && text->valuestring) {
        return text->valuestring;
    }

    return NULL;
}

const char *a2a_extract_task_id(cJSON *params)
{
    if (!params || !cJSON_IsObject(params)) {
        return NULL;
    }

    cJSON *id = cJSON_GetObjectItem(params, "id");
    if (cJSON_IsString(id) && id->valuestring) {
        return id->valuestring;
    }

    cJSON *task_id = cJSON_GetObjectItem(params, "taskId");
    if (cJSON_IsString(task_id) && task_id->valuestring) {
        return task_id->valuestring;
    }

    return NULL;
}

bool a2a_parse_rpc_request(const char *payload,
                           cJSON **out_root,
                           cJSON **out_id,
                           const char **out_method,
                           cJSON **out_params,
                           char **out_error_json)
{
    if (!payload || !out_root || !out_id || !out_method || !out_params || !out_error_json) {
        return false;
    }

    *out_root = NULL;
    *out_id = NULL;
    *out_method = NULL;
    *out_params = NULL;
    *out_error_json = NULL;

    cJSON *root = cJSON_Parse(payload);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *out_error_json = a2a_build_jsonrpc_error(NULL, -32700, "Parse error", "invalid_json");
        return false;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *jsonrpc = cJSON_GetObjectItem(root, "jsonrpc");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0) {
        *out_error_json = a2a_build_jsonrpc_error(id, -32600, "Invalid Request", "jsonrpc must be 2.0");
        cJSON_Delete(root);
        return false;
    }

    if (!cJSON_IsString(method) || !method->valuestring || method->valuestring[0] == '\0') {
        *out_error_json = a2a_build_jsonrpc_error(id, -32600, "Invalid Request", "method is required");
        cJSON_Delete(root);
        return false;
    }

    if (!params || !cJSON_IsObject(params)) {
        *out_error_json = a2a_build_jsonrpc_error(id, -32602, "Invalid params", "params object is required");
        cJSON_Delete(root);
        return false;
    }

    *out_root = root;
    *out_id = id;
    *out_method = method->valuestring;
    *out_params = params;
    return true;
}
