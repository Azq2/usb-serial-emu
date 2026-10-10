#include "personality_switch.h"

#include <stddef.h>

#include "pico/bootrom.h"
#include "pico/time.h"
#include "runtime_profile.h"

typedef enum {
	SWITCH_ACTION_NONE,
	SWITCH_ACTION_BOOTSEL,
	SWITCH_ACTION_PROFILE,
} switch_action_t;

static switch_action_t pending_action;
static usb_profile_id_t pending_profile;
static bool pending_profile_persist;
static uint64_t action_deadline_us;

static bool baud_matches(uint32_t actual, uint32_t service) {
	uint32_t tolerance = service / 100u + 1u;
	return actual >= service - tolerance && actual <= service + tolerance;
}

static bool profile_for_service_baud(uint32_t baud, usb_profile_id_t *profile) {
	static const struct {
		uint32_t baud;
		usb_profile_id_t profile;
	} services[] = {
		{600, USB_PROFILE_FT232R},
		{1200, USB_PROFILE_PL2303_H},
		{1800, USB_PROFILE_PL2303_HX},
		{2400, USB_PROFILE_PL2303_TA},
		{4800, USB_PROFILE_PL2303_TB},
		{9600, USB_PROFILE_PL2303_HXD},
		{19200, USB_PROFILE_PL2303_HXD_CLONE},
		{38400, USB_PROFILE_PL2303_HXN},
		{7200, USB_PROFILE_CH340},
		{14400, USB_PROFILE_CP2102},
		{28800, USB_PROFILE_CDC_ACM},
		{57600, USB_PROFILE_SIEMENS_SX1},
		{115200, USB_PROFILE_SIEMENS_X65},
		{230400, USB_PROFILE_SIEMENS_X75},
		{460800, USB_PROFILE_SIEMENS_EF81},
		{921600, USB_PROFILE_BENQ_SIEMENS_S81},
	};
	for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); ++i) {
		if (baud_matches(baud, services[i].baud)) {
			*profile = services[i].profile;
			return true;
		}
	}
	return false;
}

void personality_switch_control_update(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits, bool dtr,
									   bool rts, bool break_enabled) {
	bool clone_without_break = runtime_profile_is_pl2303_hxd_clone() && !break_enabled;
	if (data_bits != 7 || parity != 1 || stop_bits != 2 || !rts || (!break_enabled && !clone_without_break)) {
		return;
	}

	if (baud_matches(baud, 300)) {
		if (dtr) {
			return;
		}
		pending_action = SWITCH_ACTION_BOOTSEL;
	} else if (profile_for_service_baud(baud, &pending_profile)) {
		pending_action = SWITCH_ACTION_PROFILE;
		pending_profile_persist = dtr;
	} else {
		return;
	}

	/* Let the triggering control transfer finish before disconnecting. */
	action_deadline_us = time_us_64() + 100000u;
}

void personality_switch_task(void) {
	if (pending_action == SWITCH_ACTION_NONE || time_us_64() < action_deadline_us) {
		return;
	}

	if (pending_action == SWITCH_ACTION_BOOTSEL) {
		reset_usb_boot(0, 0);
	}

	if (pending_action == SWITCH_ACTION_PROFILE) {
		(void) runtime_profile_select(pending_profile, pending_profile_persist);
		pending_action = SWITCH_ACTION_NONE;
	}
}
