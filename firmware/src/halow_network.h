#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using halow_ready_callback_t = void (*)();

bool halow_channel_supported(const char *country, uint8_t channel);
esp_err_t halow_connect(const char *mesh_id, const char *passphrase,
                        const char *country, uint8_t channel,
                        halow_ready_callback_t on_ready);
