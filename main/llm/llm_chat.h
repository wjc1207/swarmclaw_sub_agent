#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t llm_chat_with_tools(const char *user_text, char *reply, size_t reply_size);
