#include "cp210x_device.h"

#include <string.h>

#include "byte_fifo.h"
#include "personality_switch.h"
#include "usb_device.h"

enum {
	OUT_ENDPOINT = 0x01,
	IN_ENDPOINT = 0x81,
	PACKET_SIZE = 64,
	REQTYPE_OUT_INTERFACE = 0x41,
	REQTYPE_IN_INTERFACE = 0xc1,
	REQTYPE_IN_DEVICE = 0xc0,
	REQ_IFC_ENABLE = 0x00,
	REQ_SET_BAUDDIV = 0x01,
	REQ_GET_BAUDDIV = 0x02,
	REQ_SET_LINE_CTL = 0x03,
	REQ_GET_LINE_CTL = 0x04,
	REQ_SET_BREAK = 0x05,
	REQ_SET_MHS = 0x07,
	REQ_GET_MDMSTS = 0x08,
	REQ_GET_CHARS = 0x0e,
	REQ_GET_PROPS = 0x0f,
	REQ_GET_COMM_STATUS = 0x10,
	REQ_PURGE = 0x12,
	REQ_SET_FLOW = 0x13,
	REQ_GET_FLOW = 0x14,
	REQ_EMBED_EVENTS = 0x15,
	REQ_SET_CHARS = 0x19,
	REQ_GET_BAUDRATE = 0x1d,
	REQ_SET_BAUDRATE = 0x1e,
	REQ_VENDOR_SPECIFIC = 0xff,
	VALUE_GET_PARTNUM = 0x370b,
	CONTROL_DTR = 0x0001,
	CONTROL_RTS = 0x0002,
	CONTROL_CTS = 0x0010,
	CONTROL_DSR = 0x0020,
	CONTROL_DCD = 0x0080,
	CONTROL_WRITE_DTR = 0x0100,
	CONTROL_WRITE_RTS = 0x0200,
};

typedef struct {
	bool opened;
	bool out_busy;
	bool in_busy;
	bool in_need_zlp;
	uint16_t in_size;
	USB_MEM_ALIGNX uint8_t out_packet[PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t in_packet[PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t control[64];
	byte_fifo_t rx;
	byte_fifo_t tx;
	uint32_t baud;
	uint16_t line_control;
	uint16_t baud_divisor;
	uint8_t modem_inputs;
	uint8_t special_chars[6];
	uint8_t flow_control[16];
	bool enabled;
	bool dtr;
	bool rts;
	bool break_enabled;
} cp210x_state_t;

static cp210x_state_t state;

static uint32_t get_le32(const uint8_t *data) {
	return (uint32_t) data[0] | ((uint32_t) data[1] << 8) | ((uint32_t) data[2] << 16) | ((uint32_t) data[3] << 24);
}

static void put_le16(uint8_t *data, uint16_t value) {
	data[0] = (uint8_t) value;
	data[1] = (uint8_t) (value >> 8);
}

static void put_le32(uint8_t *data, uint32_t value) {
	data[0] = (uint8_t) value;
	data[1] = (uint8_t) (value >> 8);
	data[2] = (uint8_t) (value >> 16);
	data[3] = (uint8_t) (value >> 24);
}

static void update_management_command(void) {
	uint8_t data_bits = (uint8_t) ((state.line_control >> 8) & 0x0f);
	uint8_t parity = (uint8_t) ((state.line_control >> 4) & 0x0f);
	uint8_t stop_bits = (state.line_control & 0x0f) == 2 ? 2 : 0;
	personality_switch_control_update(state.baud, data_bits, parity, stop_bits, state.dtr, state.rts,
									  state.break_enabled);
}

static void reset_state(void) {
	memset(&state, 0, sizeof(state));
	state.baud = 9600;
	state.baud_divisor = 384;
	state.line_control = 0x0800;
	state.modem_inputs = CONTROL_CTS | CONTROL_DSR | CONTROL_DCD;
	state.special_chars[4] = 0x11;
	state.special_chars[5] = 0x13;
	put_le32(&state.flow_control[0], 1);
	put_le32(&state.flow_control[4], 0x40);
	put_le32(&state.flow_control[8], 128);
	put_le32(&state.flow_control[12], 128);
}

static void kick_out(void) {
	if (!state.opened || !state.enabled || state.out_busy || fifo_free(&state.rx) < sizeof(state.out_packet)) {
		return;
	}
	if (usb_device_transfer(OUT_ENDPOINT, state.out_packet, sizeof(state.out_packet))) {
		state.out_busy = true;
	}
}

static void kick_in(void) {
	if (!state.opened || !state.enabled || state.in_busy) {
		return;
	}
	if (state.tx.count != 0) {
		size_t count = fifo_peek(&state.tx, state.in_packet, sizeof(state.in_packet));
		if (usb_device_transfer(IN_ENDPOINT, state.in_packet, (uint32_t) count)) {
			state.in_busy = true;
			state.in_size = (uint16_t) count;
		}
		return;
	}
	if (state.in_need_zlp && usb_device_transfer(IN_ENDPOINT, NULL, 0)) {
		state.in_need_zlp = false;
		state.in_busy = true;
		state.in_size = 0;
	}
}

void cp210x_task(void) {
	kick_out();
	kick_in();
}

size_t cp210x_read(void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}
	size_t count = fifo_read(&state.rx, buffer, size);
	kick_out();
	return count;
}

size_t cp210x_write(const void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}
	size_t count = fifo_write(&state.tx, buffer, size);
	kick_in();
	return count;
}

void cp210x_reset(void) {
	reset_state();
}

void cp210x_configured(void) {
	state.opened = true;
	kick_out();
}

void cp210x_transfer_complete(uint8_t endpoint, uint32_t transferred) {
	if (endpoint == OUT_ENDPOINT) {
		state.out_busy = false;
		(void) fifo_write(&state.rx, state.out_packet, transferred);
		kick_out();
		return;
	}
	if (endpoint == IN_ENDPOINT) {
		state.in_busy = false;
		fifo_drop(&state.tx, state.in_size);
		if (state.in_size == sizeof(state.in_packet) && state.tx.count == 0) {
			state.in_need_zlp = true;
		}
		state.in_size = 0;
		kick_in();
	}
}

static uint8_t modem_status(void) {
	return state.modem_inputs | (state.dtr ? CONTROL_DTR : 0) | (state.rts ? CONTROL_RTS : 0);
}

static bool handle_in(const struct usb_setup_packet *request, uint8_t **data, uint32_t *response_length) {
	if (request->wLength > sizeof(state.control)) {
		return false;
	}
	memset(state.control, 0, sizeof(state.control));
	uint16_t length = request->wLength;
	switch (request->bRequest) {
		case REQ_VENDOR_SPECIFIC:
			if (request->bmRequestType != REQTYPE_IN_DEVICE || request->wValue != VALUE_GET_PARTNUM ||
				request->wIndex != 0 || length == 0) {
				return false;
			}
			state.control[0] = 0x02;
			/* A one-byte short reply to the two-byte quirk probe disables event
			   insertion, matching supported CP2102 clones in the Linux driver. */
			*data = state.control;
			*response_length = 1;
			return true;
		case REQ_GET_BAUDDIV:
			put_le16(state.control, state.baud_divisor);
			length = 2;
			break;
		case REQ_GET_LINE_CTL:
			put_le16(state.control, state.line_control);
			length = 2;
			break;
		case REQ_GET_MDMSTS:
			state.control[0] = modem_status();
			length = 1;
			break;
		case REQ_GET_CHARS:
			memcpy(state.control, state.special_chars, sizeof(state.special_chars));
			length = sizeof(state.special_chars);
			break;
		case REQ_GET_PROPS:
			/* Linux does not require this for CP2102; return a zeroed block. */
			break;
		case REQ_GET_COMM_STATUS:
			length = 19;
			break;
		case REQ_GET_FLOW:
			memcpy(state.control, state.flow_control, sizeof(state.flow_control));
			length = sizeof(state.flow_control);
			break;
		case REQ_GET_BAUDRATE:
			put_le32(state.control, state.baud);
			length = 4;
			break;
		default:
			return false;
	}
	if (request->bmRequestType != REQTYPE_IN_INTERFACE || request->wIndex != 0 || request->wValue != 0 ||
		request->wLength != length) {
		return false;
	}
	*data = state.control;
	*response_length = length;
	return true;
}

static void set_modem_lines(uint16_t value) {
	if (value & CONTROL_WRITE_DTR) {
		state.dtr = (value & CONTROL_DTR) != 0;
	}
	if (value & CONTROL_WRITE_RTS) {
		state.rts = (value & CONTROL_RTS) != 0;
	}
}

int cp210x_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	if (request->bmRequestType & USB_REQUEST_DIR_MASK) {
		return handle_in(request, data, length) ? 0 : -1;
	}
	if (request->bmRequestType != REQTYPE_OUT_INTERFACE || request->wIndex != 0) {
		return -1;
	}
	if (request->wLength != 0) {
		if (request->bRequest == REQ_SET_BAUDRATE && request->wLength == 4) {
			state.baud = get_le32(*data);
			state.baud_divisor = state.baud == 0 ? 0 : (uint16_t) (0x384000u / state.baud);
		} else if (request->bRequest == REQ_SET_FLOW && request->wLength == 16) {
			memcpy(state.flow_control, *data, sizeof(state.flow_control));
			state.dtr = (get_le32(&state.flow_control[0]) & 0x03) == 1;
			state.rts = (get_le32(&state.flow_control[4]) & 0xc0) == 0x40;
		} else if (request->bRequest == REQ_SET_CHARS && request->wLength == 6) {
			memcpy(state.special_chars, *data, sizeof(state.special_chars));
		} else {
			return -1;
		}
		update_management_command();
		return 0;
	}

	switch (request->bRequest) {
		case REQ_IFC_ENABLE:
			state.enabled = request->wValue != 0;
			break;
		case REQ_SET_BAUDDIV:
			state.baud_divisor = request->wValue;
			if (state.baud_divisor != 0) {
				state.baud = 0x384000u / state.baud_divisor;
			}
			break;
		case REQ_SET_LINE_CTL:
			state.line_control = request->wValue;
			break;
		case REQ_SET_BREAK:
			state.break_enabled = request->wValue != 0;
			break;
		case REQ_SET_MHS:
			set_modem_lines(request->wValue);
			break;
		case REQ_PURGE:
			if (request->wValue & 0x05) {
				fifo_clear(&state.rx);
			}
			if (request->wValue & 0x0a) {
				fifo_clear(&state.tx);
			}
			break;
		case REQ_EMBED_EVENTS:
			/* The part-number quirk probe disables this path. */
			break;
		default:
			return -1;
	}
	update_management_command();
	return 0;
}
