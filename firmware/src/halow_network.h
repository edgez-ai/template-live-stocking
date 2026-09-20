#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

struct HalowNetwork {
  char ssid[33];
  uint8_t bssid[6];
  int16_t rssi;
  uint8_t bandwidth_mhz;
  uint32_t frequency_khz;
  bool secured;
};

using halow_ready_callback_t = void (*)();

esp_err_t halow_scan(HalowNetwork *networks, size_t capacity, size_t *count);
esp_err_t halow_connect(const char *ssid, const char *password,
                        const uint8_t bssid[6], halow_ready_callback_t on_ready);
