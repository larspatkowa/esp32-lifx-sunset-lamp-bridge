#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_rmaker_core.h"

esp_err_t sunset_lamp_ble_start(esp_rmaker_param_t *power_param);
void sunset_lamp_ble_set_power(bool is_on);
void sunset_lamp_ble_set_brightness(int brightness);
void sunset_lamp_ble_set_hue(int hue);
void sunset_lamp_ble_set_saturation(int saturation);
