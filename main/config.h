#pragma once

// Pull in secrets from secrets.h if present
#if __has_include("secrets.h")
#include "secrets.h"
#endif

// Default empty definitions if not defined in secrets.h
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

#ifndef CAM_API_TOKEN
#define CAM_API_TOKEN ""
#endif

#ifndef LLM_API_BASE
#define LLM_API_BASE ""
#endif

#ifndef LLM_API_KEY
#define LLM_API_KEY ""
#endif

#ifndef LLM_MODEL
#define LLM_MODEL ""
#endif
