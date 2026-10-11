#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	CDC_TEST_EVENT_USB_RESET,
	CDC_TEST_EVENT_USB_CONFIGURED,
	CDC_TEST_EVENT_USB_SUSPEND,
	CDC_TEST_EVENT_USB_RESUME,
	CDC_TEST_EVENT_LINE_CODING,
	CDC_TEST_EVENT_CONTROL_LINES,
	CDC_TEST_EVENT_HOST_BREAK,
	CDC_TEST_EVENT_OVERFLOW,
} cdc_test_event_type_t;

typedef struct {
	cdc_test_event_type_t type;
	uint32_t baud;
	uint16_t value;
	uint8_t data_bits;
	uint8_t parity;
	uint8_t stop_bits;
	bool dtr;
	bool rts;
} cdc_test_event_t;

size_t cdc_test_device_read(void *buffer, size_t size);
size_t cdc_test_device_write(const void *buffer, size_t size);
size_t cdc_test_device_write_free(void);
bool cdc_test_device_take_event(cdc_test_event_t *event);
void cdc_test_device_set_serial_state(uint16_t serial_state);
bool cdc_test_device_tx_idle(void);
uint32_t cdc_test_device_reset_generation(void);
bool cdc_test_device_personality_id(const char *name, uint32_t *id);
const char *cdc_test_device_personality_names(void);
void cdc_test_device_select_personality(uint32_t id, bool persist);
