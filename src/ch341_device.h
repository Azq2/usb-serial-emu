#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usbd_core.h"

void ch341_task(void);
size_t ch341_read(void *buffer, size_t size);
size_t ch341_write(const void *buffer, size_t size);
void ch341_reset(void);
void ch341_configured(void);
void ch341_transfer_complete(uint8_t endpoint, uint32_t transferred);
int ch341_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length);
