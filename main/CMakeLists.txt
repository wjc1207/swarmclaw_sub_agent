idf_component_register(
    SRCS "main.c"
        "ble/bthome_listener.c"
        "camera_core/camera_core.c"
        "camera_http/camera_http.c"
        "llm/llm_chat.c"
        "tools/a2a_tools.c"
        "wifi_sta/wifi_sta.c"
    INCLUDE_DIRS "."
              "ble"
              "camera_core"
              "camera_http"
              "llm"
              "tools"
              "wifi_sta"
    PRIV_REQUIRES 
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_server
        esp_http_client
        esp-tls
        json
        esp_psram
        esp32-camera
        bt
)
