#include "ch341_device.h"

#include <string.h>

#include "byte_fifo.h"
#include "personality_switch.h"
#include "usb_device.h"

enum {
	OUT_ENDPOINT = 0x02,
	IN_ENDPOINT = 0x82,
	INTERRUPT_ENDPOINT = 0x81,
	PACKET_SIZE = 32,
	STATUS_SIZE = 8,
	REQ_READ_VERSION = 0x5f,
	REQ_WRITE_REG = 0x9a,
	REQ_READ_REG = 0x95,
	REQ_SERIAL_INIT = 0xa1,
	REQ_MODEM_CTRL = 0xa4,
	REQTYPE_OUT = 0x40,
	REQTYPE_IN = 0xc0,
	REG_BREAK = 0x05,
	REG_PRESCALER = 0x12,
	REG_DIVISOR = 0x13,
	REG_LCR = 0x18,
	BIT_RTS = 0x40,
	BIT_DTR = 0x20,
};

typedef struct {
	bool opened;
	bool out_busy;
	bool in_busy;
	bool interrupt_busy;
	bool status_dirty;
	uint16_t in_size;
	USB_MEM_ALIGNX uint8_t out_packet[PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t in_packet[PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t interrupt_packet[STATUS_SIZE];
	USB_MEM_ALIGNX uint8_t response[2];
	byte_fifo_t rx;
	byte_fifo_t tx;
	uint8_t registers[256];
	uint8_t modem_inputs;
	bool dtr;
	bool rts;
	bool break_enabled;
} ch341_state_t;

static ch341_state_t state;

static uint32_t baud_rate(void) {
	uint8_t prescaler = state.registers[REG_PRESCALER];
	unsigned ps = prescaler & 0x03;
	unsigned fact = (prescaler >> 2) & 0x01;
	unsigned divisor = 0x100u - state.registers[REG_DIVISOR];
	if (divisor < 2) {
		divisor = 2;
	}
	unsigned shift = 12u - 3u * ps - fact;
	return 48000000u / ((1u << shift) * divisor);
}

static void update_management_command(void) {
	uint8_t lcr = state.registers[REG_LCR];
	uint8_t parity = 0;
	if (lcr & 0x08) {
		if (lcr & 0x20) {
			parity = (lcr & 0x10) ? 4 : 3;
		} else {
			parity = (lcr & 0x10) ? 2 : 1;
		}
	}
	personality_switch_control_update(baud_rate(), (uint8_t) (5 + (lcr & 0x03)), parity, (lcr & 0x04) ? 2 : 0,
									  state.dtr, state.rts, state.break_enabled);
}

static void reset_state(void) {
	memset(&state, 0, sizeof(state));
	state.registers[REG_PRESCALER] = 0x02;
	state.registers[REG_DIVISOR] = 0xb2;
	state.registers[REG_LCR] = 0xc3;
	state.registers[REG_BREAK] = 0x01;
	state.modem_inputs = 0x0b;
	state.status_dirty = true;
}

static void kick_out(void) {
	if (!state.opened || state.out_busy || fifo_free(&state.rx) < sizeof(state.out_packet)) {
		return;
	}
	if (usb_device_transfer(OUT_ENDPOINT, state.out_packet, sizeof(state.out_packet))) {
		state.out_busy = true;
	}
}

static void kick_in(void) {
	if (!state.opened || state.in_busy || state.tx.count == 0) {
		return;
	}
	size_t count = fifo_peek(&state.tx, state.in_packet, sizeof(state.in_packet));
	if (usb_device_transfer(IN_ENDPOINT, state.in_packet, (uint32_t) count)) {
		state.in_busy = true;
		state.in_size = (uint16_t) count;
	}
}

static void kick_interrupt(void) {
	if (!state.opened || state.interrupt_busy || !state.status_dirty) {
		return;
	}
	memset(state.interrupt_packet, 0, sizeof(state.interrupt_packet));
	state.interrupt_packet[0] = 0x08;
	state.interrupt_packet[2] = (uint8_t) ~state.modem_inputs;
	if (usb_device_transfer(INTERRUPT_ENDPOINT, state.interrupt_packet, sizeof(state.interrupt_packet))) {
		state.interrupt_busy = true;
		state.status_dirty = false;
	}
}

void ch341_task(void) {
	kick_out();
	kick_in();
	kick_interrupt();
}

size_t ch341_read(void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}
	size_t count = fifo_read(&state.rx, buffer, size);
	kick_out();
	return count;
}

size_t ch341_write(const void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}
	size_t count = fifo_write(&state.tx, buffer, size);
	kick_in();
	return count;
}

void ch341_reset(void) {
	reset_state();
}

void ch341_configured(void) {
	state.opened = true;
	kick_out();
	kick_interrupt();
}

void ch341_transfer_complete(uint8_t endpoint, uint32_t transferred) {
	if (endpoint == OUT_ENDPOINT) {
		state.out_busy = false;
		(void) fifo_write(&state.rx, state.out_packet, transferred);
		kick_out();
		return;
	}
	if (endpoint == IN_ENDPOINT) {
		state.in_busy = false;
		fifo_drop(&state.tx, state.in_size);
		state.in_size = 0;
		kick_in();
		return;
	}
	if (endpoint == INTERRUPT_ENDPOINT) {
		state.interrupt_busy = false;
		kick_interrupt();
	}
}

static void write_register_pair(uint16_t addresses, uint16_t values) {
	uint8_t low_address = (uint8_t) addresses;
	uint8_t high_address = (uint8_t) (addresses >> 8);
	state.registers[low_address] = (uint8_t) values;
	state.registers[high_address] = (uint8_t) (values >> 8);
	if (low_address == REG_BREAK || high_address == REG_BREAK) {
		state.break_enabled = (state.registers[REG_BREAK] & 0x01) == 0;
	}
	update_management_command();
}

int ch341_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	if (request->bmRequestType == REQTYPE_OUT && request->wLength == 0) {
		switch (request->bRequest) {
			case REQ_SERIAL_INIT:
				return 0;
			case REQ_WRITE_REG:
				write_register_pair(request->wValue, request->wIndex);
				return 0;
			case REQ_MODEM_CTRL: {
				uint8_t lines = (uint8_t) ~request->wValue;
				state.dtr = (lines & BIT_DTR) != 0;
				state.rts = (lines & BIT_RTS) != 0;
				update_management_command();
				return 0;
			}
			default:
				return -1;
		}
	}

	if (request->bmRequestType != REQTYPE_IN || request->wLength != 2) {
		return -1;
	}
	if (request->bRequest == REQ_READ_VERSION && request->wValue == 0 && request->wIndex == 0) {
		state.response[0] = 0x30;
		state.response[1] = 0x00;
		*data = state.response;
		*length = 2;
		return 0;
	}
	if (request->bRequest == REQ_READ_REG && request->wIndex == 0) {
		uint8_t low_address = (uint8_t) request->wValue;
		uint8_t high_address = (uint8_t) (request->wValue >> 8);
		if (request->wValue == 0x0706) {
			state.response[0] = (uint8_t) ~state.modem_inputs;
			state.response[1] = 0;
		} else {
			state.response[0] = state.registers[low_address];
			state.response[1] = state.registers[high_address];
		}
		*data = state.response;
		*length = 2;
		return 0;
	}
	return -1;
}
