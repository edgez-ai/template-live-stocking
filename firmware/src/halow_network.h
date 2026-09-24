#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using halow_ready_callback_t = void (*)();
using halow_beacon_callback_t = void (*)(const uint8_t *data, size_t length,
                                         const uint8_t source_mac[6],
                                         int16_t rssi_dbm, bool rssi_valid);

bool halow_channel_supported(const char *country, uint8_t channel);
void halow_set_beacon_callback(halow_beacon_callback_t callback);
bool halow_get_peer_rssi(const uint8_t peer_mac[6], int16_t *rssi_dbm);
esp_err_t halow_connect(const char *mesh_id, const char *passphrase,
                        const char *country, uint8_t channel, bool wifi_upstream,
                        halow_ready_callback_t on_ready);
