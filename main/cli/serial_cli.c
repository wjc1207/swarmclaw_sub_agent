#include "serial_cli.h"
#include "a2a_http/a2a_http.h"
#include "wifi_sta.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"

static const char *TAG = "serial_cli";

/* --- get_token command --- */
static struct {
    struct arg_end *end;
} get_token_args;

static int cmd_get_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&get_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, get_token_args.end, argv[0]);
        return 1;
    }

    // Regenerate token
    a2a_http_regenerate_token();
    const char *token = a2a_http_get_current_token();

    char ip_str[16] = {0};
    wifi_sta_get_ip(ip_str, sizeof(ip_str));

    printf("\n");
    printf("New API token generated:\n");
    printf("Token: %s\n", token);
    printf("Expires in: 10 minutes (600 seconds)\n");
    printf("\n");
    printf("Usage examples:\n");
    if (ip_str[0] != '\0') {
        printf("  curl -H \"Authorization: Bearer %s\" http://%s/stream\n", token, ip_str);
        printf("  curl http://%s/capture?token=%s\n", ip_str, token);
    } else {
        printf("  curl -H \"Authorization: Bearer %s\" http://<device-ip>/stream\n", token);
        printf("  curl http://<device-ip>/capture?token=%s\n", token);
    }
    printf("\n");

    return 0;
}

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "swarmclaw> ";
    repl_config.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    // Register built-in help command
    esp_console_register_help_command();

    // Register get_token command
    get_token_args.end = arg_end(0);
    const esp_console_cmd_t get_token_cmd = {
        .command = "get_token",
        .help = "Generate a new API token and print it. New token is valid for 10 minutes.",
        .hint = NULL,
        .func = &cmd_get_token,
        .argtable = &get_token_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&get_token_cmd));

    // Start REPL
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial CLI started");

    return ESP_OK;
}
