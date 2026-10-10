#include "cdc_acm_device.h"

#include <stdbool.h>
#include <string.h>

#include "byte_fifo.h"
#include "personality_switch.h"
#include "usb_device.h"

enum {
	CDC_OUT_ENDPOINT = 0x02,
	CDC_IN_ENDPOINT = 0x82,
	CDC_PACKET_SIZE = 64,
	CDC_SET_LINE_CODING = 0x20,
	CDC_GET_LINE_CODING = 0x21,
	CDC_SET_CONTROL_LINE_STATE = 0x22,
	CDC_SEND_BREAK = 0x23,
};

typedef struct {
	uint32_t baud;
	uint8_t stop_bits;
	uint8_t parity;
	uint8_t data_bits;
} __attribute__((packed)) line_coding_t;

typedef struct {
	bool opened;
	bool out_busy;
	bool in_busy;
	bool in_need_zlp;
	uint16_t in_size;
	USB_MEM_ALIGNX uint8_t out_packet[CDC_PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t in_packet[CDC_PACKET_SIZE];
	byte_fifo_t rx;
	byte_fifo_t tx;
	line_coding_t line_coding;
	bool dtr;
	bool rts;
	bool break_enabled;
} cdc_acm_state_t;

static cdc_acm_state_t state;

static void update_management_command(void) {
	personality_switch_control_update(state.line_coding.baud, state.line_coding.data_bits, state.line_coding.parity,
									  state.line_coding.stop_bits, state.dtr, state.rts, state.break_enabled);
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
	state.line_coding.baud = 115200;
	state.line_coding.data_bits = 8;
}

void cdc_acm_configured(void) {
	state.opened = true;
	kick_out();
}

void cdc_acm_task(void) {
	kick_out();
	kick_in();
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
			update_management_command();
			return 0;
		case CDC_SEND_BREAK:
			if (request->bmRequestType != 0x21 || request->wLength != 0) {
				return -1;
			}
			state.break_enabled = request->wValue != 0;
			update_management_command();
			return 0;
		default:
			return -1;
	}
}
