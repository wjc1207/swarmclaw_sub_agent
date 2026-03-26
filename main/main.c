#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>

#include "esp_psram.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "camera_core.h"
#include "bthome_listener.h"
#include "wifi_sta.h"
#include "a2a/task_manager.h"
#include "a2a_http.h"
#include "llm_chat.h"
#include "cli/serial_cli.h"

static const char *TAG = "camera_sta";
static SemaphoreHandle_t s_task_executor_running = NULL;

static void task_executor_thread(void *arg)
{
    (void)arg;
    while (true) {
        // Find next queued task
        const a2a_task_t *task = a2a_task_find_next_queued();
        if (!task) {
            // No queued tasks, sleep briefly
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Copy task ID (to avoid dangling pointers)
        char task_id[40];
        strlcpy(task_id, task->id, sizeof(task_id));

        ESP_LOGI(TAG, "Executing async task %s with llm_chat_with_tools", task_id);

        // Mark task as running
        a2a_task_update(task_id, A2A_TASK_STATE_RUNNING, NULL);

        // Allocate result buffer
        char *result = (char *)calloc(1, 2048);
        if (!result) {
            a2a_task_update(task_id, A2A_TASK_STATE_FAILED, "{\"ok\":false,\"error\":\"oom\"}");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // All tasks go through llm_chat_with_tools - it handles tool calling internally
        esp_err_t exec_ret = llm_chat_with_tools(task->input, result, 2048);

        // Mark task as completed or failed
        const char *final_state = (exec_ret == ESP_OK) ? A2A_TASK_STATE_COMPLETED : A2A_TASK_STATE_FAILED;
        a2a_task_update(task_id, final_state, result);
        free(result);

        ESP_LOGI(TAG, "Async task %s completed with state %s", task_id, final_state);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM size: %d bytes", psram_size);

    if (ESP_OK != camera_core_init()) {
        ESP_LOGE(TAG, "Camera init failed");
        return;
    }

    esp_err_t ble_ret = bthome_listener_start("a4:c1:38:a0:0d:98");
    if (ble_ret != ESP_OK) {
        ESP_LOGW(TAG, "BTHome listener start failed: 0x%x", (unsigned int)ble_ret);
    }

    a2a_task_manager_init();

    // Spawn background task executor thread - created in main.c as requested
    if (!s_task_executor_running) {
        s_task_executor_running = xSemaphoreCreateBinary();
        xTaskCreate(task_executor_thread, "task_executor", 8192, NULL, 3, NULL);
    }

    wifi_init_sta();
    a2a_http_start_server();
    ESP_ERROR_CHECK(serial_cli_init());
}
