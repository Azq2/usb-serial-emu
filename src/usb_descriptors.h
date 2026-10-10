#pragma once

#include <stdint.h>

const uint8_t *usb_descriptor_device(void);
const uint8_t *usb_descriptor_configuration(void);
const char *usb_descriptor_string(uint8_t index);
