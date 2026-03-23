#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

cJSON *a2a_tools_build_schema(void);
esp_err_t a2a_tools_execute(const char *name, char *out, size_t out_size);

// For async execution: execute tool and store result
esp_err_t a2a_tools_execute_async(const char *name, const char *input, char *out, size_t out_size);
