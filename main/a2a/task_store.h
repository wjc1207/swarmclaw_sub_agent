#pragma once

#include <stdint.h>

#define A2A_TASK_STATE_QUEUED      "queued"
#define A2A_TASK_STATE_RUNNING     "running"
#define A2A_TASK_STATE_COMPLETED   "completed"
#define A2A_TASK_STATE_FAILED      "failed"

typedef struct {
    char id[40];
    char state[16];
    char input[320];
    char output[2048];
    char tool_name[64];
    uint64_t created_ms;
    uint64_t updated_ms;
} a2a_task_t;

void a2a_task_store_init(void);

const a2a_task_t *a2a_task_create(const char *input, const char *state, const char *tool_name);

const a2a_task_t *a2a_task_create_completed(const char *input, const char *output);

const a2a_task_t *a2a_task_get(const char *task_id);

int a2a_task_update(const char *task_id, const char *state, const char *output);

const a2a_task_t *a2a_task_find_next_queued(void);
