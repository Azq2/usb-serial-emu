#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usbd_core.h"

void cp210x_task(void);
size_t cp210x_read(void *buffer, size_t size);
size_t cp210x_write(const void *buffer, size_t size);
void cp210x_reset(void);
void cp210x_configured(void);
void cp210x_transfer_complete(uint8_t endpoint, uint32_t transferred);
int cp210x_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length);
