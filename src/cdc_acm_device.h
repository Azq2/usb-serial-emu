#pragma once

#include <stddef.h>
#include <stdint.h>

#include "usbd_core.h"

void cdc_acm_task(void);
size_t cdc_acm_read(void *buffer, size_t size);
size_t cdc_acm_write(const void *buffer, size_t size);
void cdc_acm_reset(void);
void cdc_acm_configured(void);
void cdc_acm_transfer_complete(uint8_t endpoint, uint32_t transferred);
int cdc_acm_class_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length);
