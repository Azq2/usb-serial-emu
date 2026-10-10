#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/structs/usb.h"
#include "usbd_core.h"

/* CherryUSB 1.6.1 keeps this state global but declares its layout privately. */
struct rp2040_ep_state {
	uint16_t ep_mps;
	uint8_t ep_type;
	uint8_t ep_stalled;
	uint8_t ep_enable;
	uint8_t ep_addr;
	uint8_t *xfer_buf;
	uint32_t xfer_len;
	uint32_t actual_xfer_len;
	volatile uint32_t *endpoint_control;
	volatile uint32_t *buffer_control;
	uint8_t *data_buffer;
	uint8_t next_pid;
};

struct rp2040_udc {
	volatile uint8_t dev_addr;
	struct rp2040_ep_state in_ep[USB_NUM_ENDPOINTS];
	struct rp2040_ep_state out_ep[USB_NUM_ENDPOINTS];
	struct usb_setup_packet setup;
};

_Static_assert(sizeof(void *) == 4, "RP2040 USB state requires 32-bit pointers");
_Static_assert(sizeof(struct rp2040_ep_state) == 36, "CherryUSB endpoint state layout changed");
_Static_assert(offsetof(struct rp2040_ep_state, buffer_control) == 24, "CherryUSB endpoint state layout changed");
_Static_assert(offsetof(struct rp2040_ep_state, next_pid) == 32, "CherryUSB endpoint state layout changed");
_Static_assert(offsetof(struct rp2040_udc, out_ep) == 4 + 36 * USB_NUM_ENDPOINTS,
			   "CherryUSB controller state layout changed");

extern struct rp2040_udc g_rp2040_udc;

int __wrap_usbd_ep_clear_stall(uint8_t bus, uint8_t endpoint) {
	(void) bus;

	uint8_t index = USB_EP_GET_IDX(endpoint);
	if (index == 0) {
		return 0;
	}

	struct rp2040_ep_state *state =
		USB_EP_DIR_IS_OUT(endpoint) ? &g_rp2040_udc.out_ep[index] : &g_rp2040_udc.in_ep[index];
	uint32_t control = *state->buffer_control;
	bool active = (control & USB_BUF_CTRL_AVAIL) != 0;

	/* USB requires DATA0 after CLEAR_FEATURE(HALT).  Preserve an already
	 * armed transfer; the upstream RP2040 port overwrites it with ~STALL. */
	control &= ~(USB_BUF_CTRL_STALL | USB_BUF_CTRL_DATA1_PID);
	*state->buffer_control = active ? control : 0;
	state->next_pid = active ? 1 : 0;
	return 0;
}
