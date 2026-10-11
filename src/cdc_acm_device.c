#include "cdc_acm_device.h"

#include <stdbool.h>
#include <string.h>

#include "byte_fifo.h"
#include "personality_switch.h"
#include "runtime_profile.h"
#include "usb_device.h"

enum {
	CDC_OUT_ENDPOINT = 0x02,
	CDC_IN_ENDPOINT = 0x82,
	CDC_PACKET_SIZE = 64,
	CDC_NOTIFICATION_ENDPOINT = 0x81,
	CDC_SET_LINE_CODING = 0x20,
	CDC_GET_LINE_CODING = 0x21,
	CDC_SET_CONTROL_LINE_STATE = 0x22,
	CDC_SEND_BREAK = 0x23,
	EVENT_QUEUE_CAPACITY = 16,
	SERIAL_STATE_QUEUE_CAPACITY = 16,
};

typedef struct {
	uint32_t baud;
	uint8_t stop_bits;
	uint8_t parity;
	uint8_t data_bits;
} __attribute__((packed)) line_coding_t;

typedef struct {
	uint8_t request_type;
	uint8_t notification;
	uint16_t value;
	uint16_t index;
	uint16_t length;
	uint16_t serial_state;
} __attribute__((packed)) serial_state_notification_t;

typedef struct {
	bool opened;
	bool out_busy;
	bool in_busy;
	bool in_need_zlp;
	bool notification_busy;
	uint16_t in_size;
	USB_MEM_ALIGNX uint8_t out_packet[CDC_PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t in_packet[CDC_PACKET_SIZE];
	USB_MEM_ALIGNX serial_state_notification_t notification;
	byte_fifo_t rx;
	byte_fifo_t tx;
	line_coding_t line_coding;
	bool dtr;
	bool rts;
	bool break_enabled;
	cdc_test_event_t events[EVENT_QUEUE_CAPACITY];
	uint8_t event_head;
	uint8_t event_tail;
	uint8_t event_count;
	bool event_overflow;
	uint16_t serial_states[SERIAL_STATE_QUEUE_CAPACITY];
	uint8_t serial_state_head;
	uint8_t serial_state_tail;
	uint8_t serial_state_count;
} cdc_acm_state_t;

static cdc_acm_state_t state;
static uint32_t reset_generation;

_Static_assert(sizeof(serial_state_notification_t) == 10, "invalid CDC serial-state notification");

static void push_event(const cdc_test_event_t *event) {
	if (!runtime_profile_is_cdc_test()) {
		return;
	}
	if (state.event_count == EVENT_QUEUE_CAPACITY) {
		state.event_overflow = true;
		return;
	}
	state.events[state.event_head] = *event;
	state.event_head = (state.event_head + 1u) % EVENT_QUEUE_CAPACITY;
	++state.event_count;
}

static void update_management_command(void) {
	if (runtime_profile_is_cdc_test()) {
		return;
	}
	personality_switch_control_update(state.line_coding.baud, state.line_coding.data_bits, state.line_coding.parity,
									  state.line_coding.stop_bits, state.dtr, state.rts, state.break_enabled);
}

static void kick_notification(void) {
	if (!state.opened || state.notification_busy || state.serial_state_count == 0) {
		return;
	}
	state.notification = (serial_state_notification_t){
		.request_type = 0xa1,
		.notification = 0x20,
		.length = 2,
		.serial_state = state.serial_states[state.serial_state_tail],
	};
	if (usb_device_transfer(CDC_NOTIFICATION_ENDPOINT, &state.notification, sizeof(state.notification))) {
		state.notification_busy = true;
	}
}

static void kick_out(void) {
	if (!state.opened || state.out_busy || fifo_free(&state.rx) < sizeof(state.out_packet)) {
		return;
	}
	if (usb_device_transfer(CDC_OUT_ENDPOINT, state.out_packet, sizeof(state.out_packet))) {
		state.out_busy = true;
	}
}

static void kick_in(void) {
	if (!state.opened || state.in_busy) {
		return;
	}
	if (state.tx.count != 0) {
		size_t count = fifo_peek(&state.tx, state.in_packet, sizeof(state.in_packet));
		if (usb_device_transfer(CDC_IN_ENDPOINT, state.in_packet, (uint32_t) count)) {
			state.in_busy = true;
			state.in_size = (uint16_t) count;
		}
		return;
	}
	if (state.in_need_zlp && usb_device_transfer(CDC_IN_ENDPOINT, NULL, 0)) {
		state.in_need_zlp = false;
		state.in_busy = true;
		state.in_size = 0;
	}
}

void cdc_acm_reset(void) {
	memset(&state, 0, sizeof(state));
	++reset_generation;
	state.line_coding.baud = 115200;
	state.line_coding.data_bits = 8;
	push_event(&(cdc_test_event_t){.type = CDC_TEST_EVENT_USB_RESET});
}

void cdc_acm_configured(void) {
	state.opened = true;
	push_event(&(cdc_test_event_t){.type = CDC_TEST_EVENT_USB_CONFIGURED});
	kick_out();
}

void cdc_acm_suspended(void) {
	push_event(&(cdc_test_event_t){.type = CDC_TEST_EVENT_USB_SUSPEND});
}

void cdc_acm_resumed(void) {
	push_event(&(cdc_test_event_t){.type = CDC_TEST_EVENT_USB_RESUME});
}

void cdc_acm_task(void) {
	kick_out();
	kick_in();
	kick_notification();
}

size_t cdc_acm_read(void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}
	size_t count = fifo_read(&state.rx, buffer, size);
	kick_out();
	return count;
}

size_t cdc_acm_write(const void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}
	size_t count = fifo_write(&state.tx, buffer, size);
	kick_in();
	return count;
}

void cdc_acm_transfer_complete(uint8_t endpoint, uint32_t transferred) {
	if (endpoint == CDC_OUT_ENDPOINT) {
		state.out_busy = false;
		(void) fifo_write(&state.rx, state.out_packet, transferred);
		kick_out();
		return;
	}
	if (endpoint == CDC_IN_ENDPOINT) {
		state.in_busy = false;
		fifo_drop(&state.tx, state.in_size);
		if (state.in_size == CDC_PACKET_SIZE && state.tx.count == 0) {
			state.in_need_zlp = true;
		}
		state.in_size = 0;
		kick_in();
		return;
	}
	if (endpoint == CDC_NOTIFICATION_ENDPOINT) {
		state.notification_busy = false;
		if (state.serial_state_count != 0) {
			state.serial_state_tail = (state.serial_state_tail + 1u) % SERIAL_STATE_QUEUE_CAPACITY;
			--state.serial_state_count;
		}
		kick_notification();
	}
}

int cdc_acm_class_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	if (request->wIndex != 0) {
		return -1;
	}
	switch (request->bRequest) {
		case CDC_SET_LINE_CODING:
			if (request->bmRequestType != 0x21 || request->wLength != sizeof(state.line_coding)) {
				return -1;
			}
			memcpy(&state.line_coding, *data, sizeof(state.line_coding));
			push_event(&(cdc_test_event_t){
				.type = CDC_TEST_EVENT_LINE_CODING,
				.baud = state.line_coding.baud,
				.data_bits = state.line_coding.data_bits,
				.parity = state.line_coding.parity,
				.stop_bits = state.line_coding.stop_bits,
			});
			update_management_command();
			return 0;
		case CDC_GET_LINE_CODING:
			if (request->bmRequestType != 0xa1 || request->wLength != sizeof(state.line_coding)) {
				return -1;
			}
			*data = (uint8_t *) &state.line_coding;
			*length = sizeof(state.line_coding);
			return 0;
		case CDC_SET_CONTROL_LINE_STATE:
			if (request->bmRequestType != 0x21 || request->wLength != 0) {
				return -1;
			}
			state.dtr = (request->wValue & 1) != 0;
			state.rts = (request->wValue & 2) != 0;
			push_event(&(cdc_test_event_t){
				.type = CDC_TEST_EVENT_CONTROL_LINES,
				.dtr = state.dtr,
				.rts = state.rts,
			});
			update_management_command();
			return 0;
		case CDC_SEND_BREAK:
			if (request->bmRequestType != 0x21 || request->wLength != 0) {
				return -1;
			}
			state.break_enabled = request->wValue != 0;
			push_event(&(cdc_test_event_t){.type = CDC_TEST_EVENT_HOST_BREAK, .value = request->wValue});
			update_management_command();
			return 0;
		default:
			return -1;
	}
}

size_t cdc_acm_write_free(void) {
	return fifo_free(&state.tx);
}

bool cdc_acm_take_event(cdc_test_event_t *event) {
	if (event == NULL) {
		return false;
	}
	if (state.event_count != 0) {
		*event = state.events[state.event_tail];
		state.event_tail = (state.event_tail + 1u) % EVENT_QUEUE_CAPACITY;
		--state.event_count;
		return true;
	}
	if (state.event_overflow) {
		state.event_overflow = false;
		*event = (cdc_test_event_t){.type = CDC_TEST_EVENT_OVERFLOW};
		return true;
	}
	return false;
}

void cdc_acm_set_serial_state(uint16_t serial_state) {
	uint16_t last = 0xffffu;
	if (state.serial_state_count != 0) {
		uint8_t index = (state.serial_state_head + SERIAL_STATE_QUEUE_CAPACITY - 1u) % SERIAL_STATE_QUEUE_CAPACITY;
		last = state.serial_states[index];
	} else if (state.notification_busy) {
		last = state.notification.serial_state;
	}
	if (serial_state == last || state.serial_state_count == SERIAL_STATE_QUEUE_CAPACITY) {
		return;
	}
	state.serial_states[state.serial_state_head] = serial_state;
	state.serial_state_head = (state.serial_state_head + 1u) % SERIAL_STATE_QUEUE_CAPACITY;
	++state.serial_state_count;
	kick_notification();
}

bool cdc_acm_tx_idle(void) {
	return state.tx.count == 0 && !state.in_busy && !state.in_need_zlp;
}

uint32_t cdc_acm_reset_generation(void) {
	return reset_generation;
}
