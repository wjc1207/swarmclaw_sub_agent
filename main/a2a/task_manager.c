#include "task_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "llm_chat.h"

#define A2A_TASK_CAPACITY 16
static const char *TAG = "task_manager";

static a2a_task_t s_tasks[A2A_TASK_CAPACITY];
static int s_next_idx = 0;
static uint32_t s_seq = 0;
static SemaphoreHandle_t s_task_mutex = NULL;

void a2a_task_manager_init(void)
{
    if (!s_task_mutex) {
        s_task_mutex = xSemaphoreCreateMutex();
    }

    xSemaphoreTake(s_task_mutex, portMAX_DELAY);
    memset(s_tasks, 0, sizeof(s_tasks));
    s_next_idx = 0;
    s_seq = 0;
    xSemaphoreGive(s_task_mutex);
}

const a2a_task_t *a2a_task_create(const char *input, const char *state)
{
    if (!s_task_mutex) {
        return NULL;
    }

    xSemaphoreTake(s_task_mutex, portMAX_DELAY);

    a2a_task_t *task = &s_tasks[s_next_idx];
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    s_seq++;
    snprintf(task->id, sizeof(task->id), "task-%llu-%lu",
             (unsigned long long)now_ms,
             (unsigned long)s_seq);
    strlcpy(task->state, state ? state : A2A_TASK_STATE_QUEUED, sizeof(task->state));
    strlcpy(task->input, input ? input : "", sizeof(task->input));
    memset(task->output, 0, sizeof(task->output));
    task->created_ms = now_ms;
    task->updated_ms = now_ms;

    const a2a_task_t *result = task;
    s_next_idx = (s_next_idx + 1) % A2A_TASK_CAPACITY;

    xSemaphoreGive(s_task_mutex);
    return result;
}

const a2a_task_t *a2a_task_create_completed(const char *input, const char *output)
{
    if (!s_task_mutex) {
        return NULL;
    }

    xSemaphoreTake(s_task_mutex, portMAX_DELAY);

    a2a_task_t *task = &s_tasks[s_next_idx];
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

    s_seq++;
    snprintf(task->id, sizeof(task->id), "task-%llu-%lu",
             (unsigned long long)now_ms,
             (unsigned long)s_seq);
    strlcpy(task->state, A2A_TASK_STATE_COMPLETED, sizeof(task->state));
    strlcpy(task->input, input ? input : "", sizeof(task->input));
    strlcpy(task->output, output ? output : "", sizeof(task->output));
    task->created_ms = now_ms;
    task->updated_ms = now_ms;

    const a2a_task_t *result = task;
    s_next_idx = (s_next_idx + 1) % A2A_TASK_CAPACITY;

    xSemaphoreGive(s_task_mutex);
    return result;
}

const a2a_task_t *a2a_task_get(const char *task_id)
{
    if (!task_id || task_id[0] == '\0' || !s_task_mutex) {
        ESP_LOGE(TAG, "Invalid task ID or mutex");
        return NULL;
    }

    xSemaphoreTake(s_task_mutex, portMAX_DELAY);

    for (int i = 0; i < A2A_TASK_CAPACITY; i++) {
        if (s_tasks[i].id[0] != '\0' && strcmp(s_tasks[i].id, task_id) == 0) {
            const a2a_task_t *result = &s_tasks[i];
            xSemaphoreGive(s_task_mutex);
            return result;
        }
    }

    xSemaphoreGive(s_task_mutex);
    return NULL;
}

int a2a_task_update(const char *task_id, const char *state, const char *output)
{
    if (!task_id || task_id[0] == '\0' || !s_task_mutex) {
        ESP_LOGE(TAG, "Invalid task ID or mutex");
        return -1;
    }

    xSemaphoreTake(s_task_mutex, portMAX_DELAY);

    for (int i = 0; i < A2A_TASK_CAPACITY; i++) {
        if (s_tasks[i].id[0] != '\0' && strcmp(s_tasks[i].id, task_id) == 0) {
            if (state) {
                strlcpy(s_tasks[i].state, state, sizeof(s_tasks[i].state));
            }
            if (output) {
                strlcpy(s_tasks[i].output, output, sizeof(s_tasks[i].output));
            }
            s_tasks[i].updated_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            xSemaphoreGive(s_task_mutex);
            return 0;
        }
    }

    xSemaphoreGive(s_task_mutex);
    return -1;
}

const a2a_task_t *a2a_task_find_next_queued(void)
{
    if (!s_task_mutex) {
        return NULL;
    }

    xSemaphoreTake(s_task_mutex, portMAX_DELAY);

    for (int i = 0; i < A2A_TASK_CAPACITY; i++) {
        if (s_tasks[i].id[0] != '\0' && strcmp(s_tasks[i].state, A2A_TASK_STATE_QUEUED) == 0) {
            const a2a_task_t *result = &s_tasks[i];
            xSemaphoreGive(s_task_mutex);
            return result;
        }
    }

    xSemaphoreGive(s_task_mutex);
    return NULL;
}

bool a2a_task_cancel(const char *task_id)
{
    if (!task_id || task_id[0] == '\0' || !s_task_mutex) {
        ESP_LOGE(TAG, "Invalid task ID or mutex");
        return false;
    }

    xSemaphoreTake(s_task_mutex, portMAX_DELAY);

    for (int i = 0; i < A2A_TASK_CAPACITY; i++) {
        if (s_tasks[i].id[0] != '\0' && strcmp(s_tasks[i].id, task_id) == 0) {
            // Only allow canceling queued or running tasks
            if (strcmp(s_tasks[i].state, A2A_TASK_STATE_QUEUED) == 0 ||
                strcmp(s_tasks[i].state, A2A_TASK_STATE_RUNNING) == 0) {
                strlcpy(s_tasks[i].state, "canceled", sizeof(s_tasks[i].state));
                s_tasks[i].updated_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
                xSemaphoreGive(s_task_mutex);
                return true;
            }
            // Already completed/failed - can't cancel
            xSemaphoreGive(s_task_mutex);
            return false;
        }
    }

    xSemaphoreGive(s_task_mutex);
    return false;
}