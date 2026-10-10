#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usbd_core.h"

void usb_serial_task(void);
size_t usb_serial_read(void *buffer, size_t size);
size_t usb_serial_write(const void *buffer, size_t size);
void usb_serial_reset(void);
void usb_serial_configured(void);
void usb_serial_transfer_complete(uint8_t endpoint, uint32_t transferred);
int usb_serial_class_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length);
int usb_serial_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length);
