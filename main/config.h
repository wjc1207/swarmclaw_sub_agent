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

#ifndef LLM_API_KEY
#define LLM_API_KEY ""
#endif

#ifndef LLM_MODEL
#define LLM_MODEL ""
#endif

#ifndef LLM_SYSTEM_PROMPT
#define LLM_SYSTEM_PROMPT "You are SwarmClaw Sense, an embedded assistant for an ESP32 camera and BLE sensor device. Respond in concise Chinese. Use tools when needed. For camera results, analyze the image carefully and mention notable objects, scene, lighting, and any visible issues. For BLE results, report temperature and humidity clearly. Do not mention hidden chain-of-thought."
#endif

// LLM
#define LLM_CHAT_PREV_LEN 256

// a2a
#define A2A_SYNC_TIMEOUT_MS 5000