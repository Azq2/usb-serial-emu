#include "ft232_device.h"

#include <string.h>

#include "byte_fifo.h"
#include "personality_switch.h"
#include "pico/time.h"
#include "usb_device.h"

enum {
	FT232_OUT_ENDPOINT = 0x02,
	FT232_IN_ENDPOINT = 0x81,
	FT232_PACKET_SIZE = 64,
	FT232_PAYLOAD_SIZE = 62,
	FTDI_REQ_RESET = 0x00,
	FTDI_REQ_MODEM_CTRL = 0x01,
	FTDI_REQ_SET_FLOW_CTRL = 0x02,
	FTDI_REQ_SET_BAUD_RATE = 0x03,
	FTDI_REQ_SET_DATA = 0x04,
	FTDI_REQ_GET_MODEM_STATUS = 0x05,
	FTDI_REQ_SET_EVENT_CHAR = 0x06,
	FTDI_REQ_SET_ERROR_CHAR = 0x07,
	FTDI_REQ_SET_LATENCY_TIMER = 0x09,
	FTDI_REQ_GET_LATENCY_TIMER = 0x0a,
	FTDI_REQ_SET_BITMODE = 0x0b,
	FTDI_REQ_READ_PINS = 0x0c,
	FTDI_REQ_READ_EEPROM = 0x90,
	FTDI_REQTYPE_OUT = 0x40,
	FTDI_REQTYPE_IN = 0xc0,
	FTDI_RESET_SIO = 0,
	FTDI_PURGE_RX = 1,
	FTDI_PURGE_TX = 2,
	FTDI_MODEM_DTR_VALUE = 0x0001,
	FTDI_MODEM_RTS_VALUE = 0x0002,
	FTDI_MODEM_DTR_MASK = 0x0100,
	FTDI_MODEM_RTS_MASK = 0x0200,
	FTDI_DATA_BREAK = 0x4000,
	FTDI_LINE_DATA_READY = 0x01,
};

typedef struct {
	bool opened;
	bool bulk_out_busy;
	bool bulk_in_busy;
	bool status_dirty;
	uint16_t bulk_in_payload;
	uint64_t last_bulk_in_ms;

	USB_MEM_ALIGNX uint8_t bulk_out_packet[FT232_PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t bulk_in_packet[FT232_PACKET_SIZE];
	USB_MEM_ALIGNX uint8_t control_response[2];

	byte_fifo_t rx_fifo;
	byte_fifo_t tx_fifo;

	uint16_t baud_divisor;
	uint16_t baud_divisor_index;
	uint16_t data_format;
	uint8_t pins;
	uint8_t latency_ms;
	uint8_t modem_status;
	uint8_t line_status;
	bool dtr;
	bool rts;
} ft232_state_t;

static ft232_state_t state;

static uint32_t decode_baud_rate(void) {
	static const uint8_t fraction_eighths[8] = {0, 4, 2, 1, 3, 5, 6, 7};
	uint8_t fraction_code = (uint8_t) (((state.baud_divisor_index & 1) << 2) | (state.baud_divisor >> 14));
	uint32_t divisor3 = (uint32_t) (state.baud_divisor & 0x3fff) * 8u + fraction_eighths[fraction_code];
	if (divisor3 == 0) {
		divisor3 = 8; /* Encoded zero is the special 3 Mbaud divisor. */
	}
	return (24000000u + divisor3 / 2u) / divisor3;
}

static void update_bootsel_command(void) {
	personality_switch_control_update(
		decode_baud_rate(), (uint8_t) (state.data_format & 0xff), (uint8_t) ((state.data_format >> 8) & 0x07),
		(uint8_t) ((state.data_format >> 11) & 0x07), state.dtr, state.rts, (state.data_format & FTDI_DATA_BREAK) != 0);
}

static uint64_t now_ms(void) {
	return time_us_64() / 1000u;
}

static void reset_state(void) {
	memset(&state, 0, sizeof(state));
	state.baud_divisor = 0x4138; /* 9600 baud on FT232R. */
	state.data_format = 8;		 /* 8N1. */
	state.latency_ms = 16;
	state.modem_status = 0xb1;
	state.line_status = 0x60;
	state.status_dirty = true;
	state.last_bulk_in_ms = now_ms();
}

static void reset_sio(void) {
	fifo_clear(&state.rx_fifo);
	fifo_clear(&state.tx_fifo);
	state.dtr = false;
	state.rts = false;
	state.status_dirty = true;
}

static void kick_bulk_out(void) {
	if (!state.opened || state.bulk_out_busy || fifo_free(&state.rx_fifo) < sizeof(state.bulk_out_packet)) {
		return;
	}

	if (usb_device_transfer(FT232_OUT_ENDPOINT, state.bulk_out_packet, sizeof(state.bulk_out_packet))) {
		state.bulk_out_busy = true;
	}
}

static void kick_bulk_in(void) {
	if (!state.opened || state.bulk_in_busy) {
		return;
	}

	uint64_t current_ms = now_ms();
	if (state.tx_fifo.count == 0 && !state.status_dirty && current_ms - state.last_bulk_in_ms < state.latency_ms) {
		return;
	}

	size_t payload = fifo_peek(&state.tx_fifo, &state.bulk_in_packet[2], FT232_PAYLOAD_SIZE);
	state.bulk_in_packet[0] = state.modem_status | 0x01;
	state.bulk_in_packet[1] = state.line_status;
	if (payload != 0) {
		state.bulk_in_packet[1] |= FTDI_LINE_DATA_READY;
	}

	if (usb_device_transfer(FT232_IN_ENDPOINT, state.bulk_in_packet, (uint32_t) (payload + 2))) {
		state.bulk_in_busy = true;
		state.bulk_in_payload = (uint16_t) payload;
		state.status_dirty = false;
		state.last_bulk_in_ms = current_ms;
	}
}

void ft232_task(void) {
	kick_bulk_out();
	kick_bulk_in();
}

size_t ft232_read(void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}
	size_t count = fifo_read(&state.rx_fifo, buffer, size);
	kick_bulk_out();
	return count;
}

size_t ft232_write(const void *buffer, size_t size) {
	if (buffer == NULL || size == 0) {
		return 0;
	}
	size_t count = fifo_write(&state.tx_fifo, buffer, size);
	kick_bulk_in();
	return count;
}

void ft232_reset(void) {
	reset_state();
}

void ft232_configured(void) {
	state.opened = true;
	state.status_dirty = true;
	state.last_bulk_in_ms = now_ms();
	kick_bulk_out();
	kick_bulk_in();
}

void ft232_transfer_complete(uint8_t endpoint, uint32_t transferred) {
	if (endpoint == FT232_OUT_ENDPOINT) {
		state.bulk_out_busy = false;
		if (transferred != 0) {
			(void) fifo_write(&state.rx_fifo, state.bulk_out_packet, transferred);
		}
		kick_bulk_out();
		return;
	}

	if (endpoint == FT232_IN_ENDPOINT) {
		state.bulk_in_busy = false;
		if (state.bulk_in_payload != 0) {
			fifo_drop(&state.tx_fifo, state.bulk_in_payload);
		}
		state.bulk_in_payload = 0;
		kick_bulk_in();
	}
}

static bool valid_channel(uint16_t index) {
	return (index & 0x00ff) == 0;
}

static bool handle_out_request(const struct usb_setup_packet *request) {
	if (request->wLength != 0) {
		return false;
	}

	switch (request->bRequest) {
		case FTDI_REQ_RESET:
			if (!valid_channel(request->wIndex) || request->wValue > FTDI_PURGE_TX) {
				return false;
			}
			if (request->wValue == FTDI_RESET_SIO) {
				reset_sio();
			} else if (request->wValue == FTDI_PURGE_RX) {
				fifo_clear(&state.tx_fifo);
			} else {
				fifo_clear(&state.rx_fifo);
			}
			return true;

		case FTDI_REQ_MODEM_CTRL:
			if (!valid_channel(request->wIndex)) {
				return false;
			}
			if (request->wValue & FTDI_MODEM_DTR_MASK) {
				state.dtr = (request->wValue & FTDI_MODEM_DTR_VALUE) != 0;
			}
			if (request->wValue & FTDI_MODEM_RTS_MASK) {
				state.rts = (request->wValue & FTDI_MODEM_RTS_VALUE) != 0;
			}
			update_bootsel_command();
			return true;

		case FTDI_REQ_SET_FLOW_CTRL:
			if (!valid_channel(request->wIndex)) {
				return false;
			}
			return true;

		case FTDI_REQ_SET_BAUD_RATE:
			/* On single-port BM/R devices bit 0 of wIndex is divisor bit 16. */
			if (request->wIndex > 1) {
				return false;
			}
			state.baud_divisor = request->wValue;
			state.baud_divisor_index = request->wIndex;
			update_bootsel_command();
			return true;

		case FTDI_REQ_SET_DATA:
			if (!valid_channel(request->wIndex)) {
				return false;
			}
			state.data_format = request->wValue;
			update_bootsel_command();
			return true;

		case FTDI_REQ_SET_EVENT_CHAR:
			if (!valid_channel(request->wIndex)) {
				return false;
			}
			return true;

		case FTDI_REQ_SET_ERROR_CHAR:
			if (!valid_channel(request->wIndex)) {
				return false;
			}
			return true;

		case FTDI_REQ_SET_LATENCY_TIMER:
			if (!valid_channel(request->wIndex) || request->wValue > 0xff) {
				return false;
			}
			state.latency_ms = request->wValue == 0 ? 1 : (uint8_t) request->wValue;
			return true;

		case FTDI_REQ_SET_BITMODE:
			if (!valid_channel(request->wIndex)) {
				return false;
			}
			state.pins = (uint8_t) request->wValue;
			return true;

		default:
			return false;
	}
}

static bool handle_in_request(const struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	switch (request->bRequest) {
		case FTDI_REQ_GET_MODEM_STATUS:
			if (!valid_channel(request->wIndex) || request->wValue != 0 || request->wLength != 2) {
				return false;
			}
			state.control_response[0] = state.modem_status | 0x01;
			state.control_response[1] = state.line_status;
			*data = state.control_response;
			*length = 2;
			return true;

		case FTDI_REQ_GET_LATENCY_TIMER:
			if (!valid_channel(request->wIndex) || request->wValue != 0 || request->wLength != 1) {
				return false;
			}
			state.control_response[0] = state.latency_ms;
			*data = state.control_response;
			*length = 1;
			return true;

		case FTDI_REQ_READ_PINS:
			if (!valid_channel(request->wIndex) || request->wValue != 0 || request->wLength != 1) {
				return false;
			}
			state.control_response[0] = state.pins;
			*data = state.control_response;
			*length = 1;
			return true;

		case FTDI_REQ_READ_EEPROM:
			if (request->wValue != 0 || request->wLength != 2) {
				return false;
			}
			/* Unprogrammed CBUS nibbles mean no pins are exported as GPIO. */
			state.control_response[0] = 0xff;
			state.control_response[1] = 0xff;
			*data = state.control_response;
			*length = 2;
			return true;

		default:
			return false;
	}
}

int ft232_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	if (request->bmRequestType == FTDI_REQTYPE_OUT) {
		return handle_out_request(request) ? 0 : -1;
	}

	if (request->bmRequestType == FTDI_REQTYPE_IN) {
		return handle_in_request(request, data, length) ? 0 : -1;
	}

	return -1;
}
