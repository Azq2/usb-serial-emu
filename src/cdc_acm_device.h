#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdc_test_device.h"
#include "usbd_core.h"

void cdc_acm_task(void);
size_t cdc_acm_read(void *buffer, size_t size);
size_t cdc_acm_write(const void *buffer, size_t size);
void cdc_acm_reset(void);
void cdc_acm_configured(void);
void cdc_acm_suspended(void);
void cdc_acm_resumed(void);
void cdc_acm_transfer_complete(uint8_t endpoint, uint32_t transferred);
int cdc_acm_class_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length);
size_t cdc_acm_write_free(void);
bool cdc_acm_take_event(cdc_test_event_t *event);
void cdc_acm_set_serial_state(uint16_t serial_state);
bool cdc_acm_tx_idle(void);
uint32_t cdc_acm_reset_generation(void);
