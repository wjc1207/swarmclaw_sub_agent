#pragma once

#include "esp_netif.h"

void wifi_init_sta(void);

// Get current IP address as string
bool wifi_sta_get_ip(char *buf, size_t buf_size);
