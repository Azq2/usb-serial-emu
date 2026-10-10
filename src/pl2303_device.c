#include "pl2303_device.h"

#include <string.h>

#include "byte_fifo.h"
#include "personality_switch.h"
#include "runtime_profile.h"
#include "usb_device.h"

enum {
	PL2303_INTERFACE_NUMBER = 0,
	PL2303_INTERRUPT_ENDPOINT = 0x81,
	PL2303_OUT_ENDPOINT = 0x02,
	PL2303_IN_ENDPOINT = 0x83,
	PL2303_BULK_PACKET_SIZE = 64,
	PL2303_STATUS_PACKET_SIZE = 10,
	PL2303_REQ_SET_LINE = 0x20,
	PL2303_REQ_GET_LINE = 0x21,
	PL2303_REQ_SET_CONTROL = 0x22,
	PL2303_REQ_BREAK = 0x23,
	PL2303_REQ_VENDOR_LEGACY = 0x01,
	PL2303_REQ_VENDOR_HXN_WRITE = 0x80,
	PL2303_REQ_VENDOR_HXN_READ = 0x81,
	PL2303_REQTYPE_CLASS_OUT = 0x21,
	PL2303_REQTYPE_CLASS_IN = 0xa1,
	PL2303_REQTYPE_VENDOR_OUT = 0x40,
	PL2303_REQTYPE_VENDOR_IN = 0xc0,
	PL2303_HX_STATUS_PROBE = 0x8080,
};

typedef struct {
	bool opened;

	bool bulk_out_busy;
	bool bulk_in_busy;
	bool bulk_in_need_zlp;
	bool interrupt_busy;
	bool status_dirty;
	uint16_t bulk_in_last_size;

	USB_MEM_ALIGNX uint8_t bulk_out_packet[PL2303_BULK_PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t bulk_in_packet[PL2303_BULK_PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t interrupt_packet[PL2303_STATUS_PACKET_SIZE];

	byte_fifo_t rx_fifo;
	byte_fifo_t tx_fifo;

	uint8_t line_coding[7];
	uint8_t registers[256];
	uint8_t vendor_response;
	uint8_t control_lines;
	uint8_t uart_status;
	bool break_enabled;
} pl2303_state_t;

static pl2303_state_t state;

static void update_bootsel_command(void) {
	uint32_t baud = (uint32_t) state.line_coding[0] | ((uint32_t) state.line_coding[1] << 8) |
					((uint32_t) state.line_coding[2] << 16) | ((uint32_t) state.line_coding[3] << 24);
	personality_switch_control_update(baud, state.line_coding[6], state.line_coding[5], state.line_coding[4],
									  (state.control_lines & 0x01) != 0, (state.control_lines & 0x02) != 0,
									  state.break_enabled);
}

static void reset_protocol_state(void) {
	memset(&state, 0, sizeof(state));

	/* 9600 8N1, little-endian baud. */
	state.line_coding[0] = 0x80;
	state.line_coding[1] = 0x25;
	state.line_coding[2] = 0x00;
	state.line_coding[3] = 0x00;
	state.line_coding[4] = 0x00;
	state.line_coding[5] = 0x00;
	state.line_coding[6] = 0x08;

	if (runtime_profile_is_pl2303_hxn()) {
		state.registers[0x0a] = 0x1c;
	}

	state.uart_status = 0x83;
	state.status_dirty = true;
}

static void kick_bulk_out(void) {
	if (!state.opened || state.bulk_out_busy || fifo_free(&state.rx_fifo) < sizeof(state.bulk_out_packet)) {
		return;
	}

	if (usb_device_transfer(PL2303_OUT_ENDPOINT, state.bulk_out_packet, sizeof(state.bulk_out_packet))) {
		state.bulk_out_busy = true;
	}
}

static void kick_bulk_in(void) {
	if (!state.opened || state.bulk_in_busy) {
		return;
	}

	if (state.tx_fifo.count != 0) {
		size_t count = fifo_peek(&state.tx_fifo, state.bulk_in_packet, sizeof(state.bulk_in_packet));
		if (usb_device_transfer(PL2303_IN_ENDPOINT, state.bulk_in_packet, (uint32_t) count)) {
			state.bulk_in_busy = true;
			state.bulk_in_last_size = (uint16_t) count;
		}
		return;
	}

	if (state.bulk_in_need_zlp && usb_device_transfer(PL2303_IN_ENDPOINT, NULL, 0)) {
		state.bulk_in_busy = true;
		state.bulk_in_last_size = 0;
		state.bulk_in_need_zlp = false;
	}
}

static void kick_interrupt(void) {
	if (!state.opened || state.interrupt_busy || !state.status_dirty) {
		return;
	}

	memset(state.interrupt_packet, 0, sizeof(state.interrupt_packet));
	if (runtime_profile_pl2303_status_at_index_zero()) {
		state.interrupt_packet[0] = state.uart_status;
	} else {
		/* CDC-style serial-state notification used by normal PL2303 devices. */
		state.interrupt_packet[0] = 0xa1;
		state.interrupt_packet[1] = 0x20;
		state.interrupt_packet[4] = runtime_profile_pl2303_split_interfaces() ? 1 : 0;
		state.interrupt_packet[6] = 0x02;
		state.interrupt_packet[8] = state.uart_status;
	}

	if (usb_device_transfer(PL2303_INTERRUPT_ENDPOINT, state.interrupt_packet, sizeof(state.interrupt_packet))) {
		state.interrupt_busy = true;
		state.status_dirty = false;
	}
}

void pl2303_task(void) {
	kick_bulk_out();
	kick_bulk_in();
	kick_interrupt();
}

size_t pl2303_read(void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}

	size_t count = fifo_read(&state.rx_fifo, buffer, size);
	kick_bulk_out();
	return count;
}

size_t pl2303_write(const void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}

	size_t count = fifo_write(&state.tx_fifo, buffer, size);
	kick_bulk_in();
	return count;
}

void pl2303_reset(void) {
	reset_protocol_state();
}

void pl2303_configured(void) {
	state.opened = true;
	state.status_dirty = true;
	kick_bulk_out();
	kick_interrupt();
}

int pl2303_class_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	if (request->wIndex != PL2303_INTERFACE_NUMBER && !runtime_profile_pl2303_split_interfaces()) {
		return -1;
	}

	switch (request->bRequest) {
		case PL2303_REQ_GET_LINE:
			if (request->bmRequestType != PL2303_REQTYPE_CLASS_IN || request->wValue != 0 || request->wLength != 7) {
				return -1;
			}
			if (runtime_profile_is_pl2303_hxd_clone()) {
				return -1;
			}
			*data = state.line_coding;
			*length = sizeof(state.line_coding);
			return 0;

		case PL2303_REQ_SET_LINE:
			if (request->bmRequestType != PL2303_REQTYPE_CLASS_OUT || request->wValue != 0 || request->wLength != 7) {
				return -1;
			}
			memcpy(state.line_coding, *data, sizeof(state.line_coding));
			update_bootsel_command();
			return 0;

		case PL2303_REQ_SET_CONTROL:
			if (request->bmRequestType != PL2303_REQTYPE_CLASS_OUT || request->wLength != 0) {
				return -1;
			}
			state.control_lines = (uint8_t) (request->wValue & 0x03);
			state.status_dirty = true;
			update_bootsel_command();
			return 0;

		case PL2303_REQ_BREAK:
			if (request->bmRequestType != PL2303_REQTYPE_CLASS_OUT || request->wLength != 0 ||
				(request->wValue != 0x0000 && request->wValue != 0xffff)) {
				return -1;
			}
			if (runtime_profile_is_pl2303_hxd_clone()) {
				return -1;
			}
			state.break_enabled = request->wValue == 0xffff;
			update_bootsel_command();
			return 0;

		default:
			return -1;
	}
}

void pl2303_transfer_complete(uint8_t endpoint, uint32_t transferred) {
	if (endpoint == PL2303_OUT_ENDPOINT) {
		state.bulk_out_busy = false;
		if (transferred != 0) {
			(void) fifo_write(&state.rx_fifo, state.bulk_out_packet, transferred);
		}
		kick_bulk_out();
		return;
	}

	if (endpoint == PL2303_IN_ENDPOINT) {
		state.bulk_in_busy = false;
		fifo_drop(&state.tx_fifo, state.bulk_in_last_size);
		if (state.bulk_in_last_size != 0 && state.bulk_in_last_size % PL2303_BULK_PACKET_SIZE == 0 &&
			state.tx_fifo.count == 0) {
			state.bulk_in_need_zlp = true;
		}
		kick_bulk_in();
		return;
	}

	if (endpoint == PL2303_INTERRUPT_ENDPOINT) {
		state.interrupt_busy = false;
		kick_interrupt();
	}
}

int pl2303_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	if (request->bmRequestType == PL2303_REQTYPE_VENDOR_IN && request->bRequest == PL2303_REQ_VENDOR_LEGACY &&
		request->wValue == PL2303_HX_STATUS_PROBE && request->wIndex == 0 && request->wLength == 1) {
		if (!runtime_profile_supports_hx_status()) {
			return -1;
		}
		state.vendor_response = 0;
		*data = &state.vendor_response;
		*length = 1;
		return 0;
	}

	if (runtime_profile_is_pl2303_hxn()) {
		if (request->bmRequestType == PL2303_REQTYPE_VENDOR_IN && request->bRequest == PL2303_REQ_VENDOR_HXN_READ &&
			request->wIndex == 0 && request->wLength == 1) {
			state.vendor_response = state.registers[request->wValue & 0xff];
			*data = &state.vendor_response;
			*length = 1;
			return 0;
		}

		if (request->bmRequestType == PL2303_REQTYPE_VENDOR_OUT && request->bRequest == PL2303_REQ_VENDOR_HXN_WRITE &&
			request->wLength == 0) {
			state.registers[request->wValue & 0xff] = (uint8_t) request->wIndex;
			return 0;
		}
		return -1;
	}

	if (request->bmRequestType == PL2303_REQTYPE_VENDOR_IN && request->bRequest == PL2303_REQ_VENDOR_LEGACY &&
		request->wIndex == 0 && request->wLength == 1) {
		if (request->wValue >= 0x80 && request->wValue <= 0xff) {
			state.vendor_response = state.registers[request->wValue & 0x7f];
		} else {
			state.vendor_response = 0;
		}
		*data = &state.vendor_response;
		*length = 1;
		return 0;
	}

	if (request->bmRequestType == PL2303_REQTYPE_VENDOR_OUT && request->bRequest == PL2303_REQ_VENDOR_LEGACY &&
		request->wLength == 0) {
		if (request->wValue <= 0xff) {
			state.registers[request->wValue] = (uint8_t) request->wIndex;
		}
		return 0;
	}

	return -1;
}
