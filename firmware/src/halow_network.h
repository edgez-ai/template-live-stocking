#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using halow_ready_callback_t = void (*)();
using halow_beacon_callback_t = void (*)(const uint8_t *data, size_t length);

struct HalowBeaconDebugStats {
  uint32_t scan_results;
  uint32_t scan_edgez_ies;
  uint32_t vendor_callbacks;
  int startup_stage;
  int library_status;
};

bool halow_channel_supported(const char *country, uint8_t channel);
void halow_set_beacon_callback(halow_beacon_callback_t callback);
HalowBeaconDebugStats halow_beacon_debug_stats();
esp_err_t halow_connect(const char *mesh_id, const char *passphrase,
                        const char *country, uint8_t channel, bool wifi_upstream,
                        halow_ready_callback_t on_ready);
