#pragma once

#include "esp_err.h"

esp_err_t display_init();
void display_show(const char *title, const char *message);
void display_show_message(const char *message, const char *channel);
