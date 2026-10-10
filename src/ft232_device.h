#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usbd_core.h"

void ft232_task(void);
size_t ft232_read(void *buffer, size_t size);
size_t ft232_write(const void *buffer, size_t size);

void ft232_reset(void);
void ft232_configured(void);
void ft232_transfer_complete(uint8_t endpoint, uint32_t transferred);
int ft232_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length);
