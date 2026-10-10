#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usbd_core.h"

void pl2303_task(void);

size_t pl2303_read(void *buffer, size_t size);
size_t pl2303_write(const void *buffer, size_t size);

void pl2303_reset(void);
void pl2303_configured(void);
void pl2303_transfer_complete(uint8_t endpoint, uint32_t transferred);
int pl2303_class_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length);
int pl2303_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length);
