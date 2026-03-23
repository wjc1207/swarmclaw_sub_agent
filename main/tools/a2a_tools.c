#include "a2a_tools.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "esp_dsp.h"
#include "img_converters.h"

#include "bthome_listener.h"
#include "camera_core.h"

#define FFT_SRC_W 640
#define FFT_SRC_H 480
#define FFT_PAD_W 1024
#define FFT_PAD_H 512
#define FFT_REPEAT_COUNT 101

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
             "\"capture_human\":\"/capture_human\",\"get_thome\":\"/get_thome\",\"image_fft\":\"/image_fft\"}");
}

static void tool_image_fft_execute(char *out, size_t out_size)
{
    // Acquire camera frame buffer
    camera_fb_t *fb = NULL;
    esp_err_t acq = camera_core_acquire_fb(&fb, 1, 30, pdMS_TO_TICKS(2000));
    if (acq != ESP_OK || fb == NULL) {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"camera_acquire_failed\",\"status\":\"failed\"}");
        return;
    }

    if (fb->width != FFT_SRC_W || fb->height != FFT_SRC_H) {
        snprintf(out, out_size,
                 "{\"ok\":false,\"error\":\"unsupported_resolution\",\"required\":\"640x480\",\"actual\":\"%dx%d\"}",
                 fb->width, fb->height);
        camera_core_release_fb(fb);
        return;
    }

    const char *decode_mode = "raw_to_gray";
    uint8_t *rgb888 = NULL;
    const uint8_t *src_rgb = NULL;

    if (fb->format == PIXFORMAT_JPEG) {
        decode_mode = "jpeg_to_rgb888";
        rgb888 = (uint8_t *)malloc((size_t)FFT_SRC_W * (size_t)FFT_SRC_H * 3);
        if (!rgb888) {
            snprintf(out, out_size, "{\"ok\":false,\"error\":\"oom_rgb\",\"status\":\"failed\"}");
            camera_core_release_fb(fb);
            return;
        }
        if (!fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb888)) {
            free(rgb888);
            camera_core_release_fb(fb);
            snprintf(out, out_size, "{\"ok\":false,\"error\":\"jpeg_decode_failed\",\"status\":\"failed\"}");
            return;
        }
        src_rgb = rgb888;
    }

    // Full 2D FFT with power-of-two padding: 640x480 -> 1024x512
    float *fft_mat = (float *)calloc((size_t)FFT_PAD_W * (size_t)FFT_PAD_H * 2, sizeof(float));
    float *col_buf = (float *)malloc(sizeof(float) * FFT_PAD_H * 2);
    if (!fft_mat || !col_buf) {
        free(rgb888);
        free(fft_mat);
        free(col_buf);
        camera_core_release_fb(fb);
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"oom_fft\",\"status\":\"failed\"}");
        return;
    }

    for (int y = 0; y < FFT_SRC_H; ++y) {
        for (int x = 0; x < FFT_SRC_W; ++x) {
            float gray = 0.0f;
            if (fb->format == PIXFORMAT_JPEG) {
                size_t p = ((size_t)y * (size_t)FFT_SRC_W + (size_t)x) * 3;
                float r = (float)src_rgb[p + 0];
                float g = (float)src_rgb[p + 1];
                float b = (float)src_rgb[p + 2];
                gray = 0.299f * r + 0.587f * g + 0.114f * b;
            } else if (fb->format == PIXFORMAT_RGB565) {
                size_t p = ((size_t)y * (size_t)FFT_SRC_W + (size_t)x) * 2;
                uint16_t rgb565 = ((uint16_t)fb->buf[p] << 8) | fb->buf[p + 1];
                float r = (float)((rgb565 >> 11) & 0x1F);
                float g = (float)((rgb565 >> 5) & 0x3F);
                float b = (float)(rgb565 & 0x1F);
                gray = 0.299f * r + 0.587f * g + 0.114f * b;
            } else if (fb->format == PIXFORMAT_GRAYSCALE) {
                size_t p = (size_t)y * (size_t)FFT_SRC_W + (size_t)x;
                gray = (float)fb->buf[p];
            } else {
                size_t p = (size_t)y * (size_t)FFT_SRC_W + (size_t)x;
                gray = (float)fb->buf[p];
            }

            size_t idx = ((size_t)y * (size_t)FFT_PAD_W + (size_t)x) * 2;
            fft_mat[idx] = gray;
            fft_mat[idx + 1] = 0.0f;
        }
    }

    for (int repeat = 0; repeat < FFT_REPEAT_COUNT; ++repeat) {
        dsps_fft2r_init_fc32(NULL, FFT_PAD_W);
        for (int y = 0; y < FFT_PAD_H; ++y) {
            float *row_ptr = &fft_mat[(size_t)y * (size_t)FFT_PAD_W * 2];
            dsps_fft2r_fc32_ansi(row_ptr, FFT_PAD_W);
        }

        dsps_fft2r_init_fc32(NULL, FFT_PAD_H);
        for (int x = 0; x < FFT_PAD_W; ++x) {
            for (int y = 0; y < FFT_PAD_H; ++y) {
                size_t src_idx = ((size_t)y * (size_t)FFT_PAD_W + (size_t)x) * 2;
                col_buf[(size_t)y * 2] = 1/sqrtf((float)(FFT_PAD_W)) * fft_mat[src_idx];
                col_buf[(size_t)y * 2 + 1] = 1/sqrtf((float)(FFT_PAD_W)) * fft_mat[src_idx + 1];
            }

            dsps_fft2r_fc32_ansi(col_buf, FFT_PAD_H);

            for (int y = 0; y < FFT_PAD_H; ++y) {
                size_t dst_idx = ((size_t)y * (size_t)FFT_PAD_W + (size_t)x) * 2;
                fft_mat[dst_idx] = 1/sqrtf((float)(FFT_PAD_H)) * col_buf[(size_t)y * 2];
                fft_mat[dst_idx + 1] = 1/sqrtf((float)(FFT_PAD_H)) * col_buf[(size_t)y * 2 + 1];
            }
        }

        // watchdog reset for long-running FFT
        vTaskDelay(1);
    }

    double mag_sum = 0.0;
    float max_mag = 0.0f;
    int max_x = 0;
    int max_y = 0;

    for (int y = 0; y < FFT_SRC_H; ++y) {
        for (int x = 0; x < FFT_SRC_W; ++x) {
            size_t idx = ((size_t)y * (size_t)FFT_PAD_W + (size_t)x) * 2;
            float real = fft_mat[idx];
            float imag = fft_mat[idx + 1];
            float mag = sqrtf(real * real + imag * imag);
            mag_sum += (double)mag;
            if (mag > max_mag) {
                max_mag = mag;
                max_x = x;
                max_y = y;
            }
        }
    }

    snprintf(out, out_size,
             "{\"ok\":true,\"status\":\"ok\",\"decode\":\"%s\",\"fft_mode\":\"full_2d\",\"src_w\":%d,\"src_h\":%d,\"pad_w\":%d,\"pad_h\":%d,\"repeat\":%d,\"mag_sum\":%.2f,\"max_mag\":%.2f,\"max_x\":%d,\"max_y\":%d}",
             decode_mode,
             FFT_SRC_W, FFT_SRC_H, FFT_PAD_W, FFT_PAD_H, FFT_REPEAT_COUNT,
             (float)mag_sum, max_mag, max_x, max_y);

    free(rgb888);
    free(fft_mat);
    free(col_buf);
    camera_core_release_fb(fb);
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
    cJSON_AddStringToObject(ble_fn, "description", "Get latest BLE BTHome sensor reading from this device.");
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

    // Tool: tool_image_fft (async candidate)
    cJSON *tool_fft = cJSON_CreateObject();
    cJSON *fft_fn = cJSON_CreateObject();
    cJSON *fft_params = cJSON_CreateObject();
    cJSON *fft_props = cJSON_CreateObject();
    cJSON *fft_required = cJSON_CreateArray();
    cJSON_AddStringToObject(tool_fft, "type", "function");
    cJSON_AddStringToObject(fft_fn, "name", "tool_image_fft");
    cJSON_AddStringToObject(fft_fn, "description", "Compute FFT analysis of latest captured image (async-capable). Returns magnitude spectrum summary.");
    cJSON_AddStringToObject(fft_params, "type", "object");
    cJSON_AddItemToObject(fft_params, "properties", fft_props);
    cJSON_AddItemToObject(fft_params, "required", fft_required);
    cJSON_AddItemToObject(fft_fn, "parameters", fft_params);
    cJSON_AddItemToObject(tool_fft, "function", fft_fn);
    cJSON_AddItemToArray(tools, tool_fft);

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

    if (strcmp(name, "tool_image_fft") == 0) {
        tool_image_fft_execute(out, out_size);
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
