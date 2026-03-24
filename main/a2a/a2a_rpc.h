#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "task_manager.h"

bool a2a_parse_rpc_request(const char *payload,
                           cJSON **out_root,
                           cJSON **out_id,
                           const char **out_method,
                           cJSON **out_params,
                           char **out_error_json);

const char *a2a_extract_message_text(cJSON *params);
const char *a2a_extract_task_id(cJSON *params);

char *a2a_build_jsonrpc_error(cJSON *id, int code, const char *message, const char *data);
char *a2a_build_task_result(cJSON *id, const a2a_task_t *task);
