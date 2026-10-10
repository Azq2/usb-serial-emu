#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "usbd_core.h"

bool usb_device_transfer(uint8_t endpoint, void *buffer, uint32_t size);
uint16_t usb_device_ep0_size(void);
void usb_device_init(void);
