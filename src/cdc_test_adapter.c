#include "cdc_test_device.h"

#include <string.h>

#include "cdc_acm_device.h"
#include "runtime_profile.h"

typedef struct {
	const char *name;
	usb_profile_id_t profile;
} personality_t;

static const personality_t personalities[] = {
	{"ft232r", USB_PROFILE_FT232R},
	{"pl2303-h", USB_PROFILE_PL2303_H},
	{"pl2303-hx", USB_PROFILE_PL2303_HX},
	{"pl2303-ta", USB_PROFILE_PL2303_TA},
	{"pl2303-tb", USB_PROFILE_PL2303_TB},
	{"pl2303-hxd", USB_PROFILE_PL2303_HXD},
	{"pl2303-hxd-clone", USB_PROFILE_PL2303_HXD_CLONE},
	{"pl2303-hxn", USB_PROFILE_PL2303_HXN},
	{"ch340", USB_PROFILE_CH340},
	{"cp2102", USB_PROFILE_CP2102},
	{"cdc-acm", USB_PROFILE_CDC_ACM},
	{"siemens-sx1", USB_PROFILE_SIEMENS_SX1},
	{"siemens-x65", USB_PROFILE_SIEMENS_X65},
	{"siemens-x75", USB_PROFILE_SIEMENS_X75},
	{"siemens-ef81", USB_PROFILE_SIEMENS_EF81},
	{"benq-siemens-s81", USB_PROFILE_BENQ_SIEMENS_S81},
	{"cdc-test", USB_PROFILE_CDC_TEST},
};

_Static_assert(sizeof(personalities) / sizeof(personalities[0]) == USB_PROFILE_COUNT,
			   "personality table must cover every profile ID");

size_t cdc_test_device_read(void *buffer, size_t size) {
	return cdc_acm_read(buffer, size);
}

size_t cdc_test_device_write(const void *buffer, size_t size) {
	return cdc_acm_write(buffer, size);
}

size_t cdc_test_device_write_free(void) {
	return cdc_acm_write_free();
}

bool cdc_test_device_take_event(cdc_test_event_t *event) {
	return cdc_acm_take_event(event);
}

void cdc_test_device_set_serial_state(uint16_t serial_state) {
	cdc_acm_set_serial_state(serial_state);
}

bool cdc_test_device_tx_idle(void) {
	return cdc_acm_tx_idle();
}

uint32_t cdc_test_device_reset_generation(void) {
	return cdc_acm_reset_generation();
}

bool cdc_test_device_personality_id(const char *name, uint32_t *id) {
	for (size_t i = 0; i < sizeof(personalities) / sizeof(personalities[0]); ++i) {
		if (strcmp(name, personalities[i].name) == 0) {
			*id = personalities[i].profile;
			return true;
		}
	}
	return false;
}

const char *cdc_test_device_personality_names(void) {
	return "ft232r pl2303-h pl2303-hx pl2303-ta pl2303-tb pl2303-hxd pl2303-hxd-clone pl2303-hxn ch340 "
		   "cp2102 cdc-acm siemens-sx1 siemens-x65 siemens-x75 siemens-ef81 benq-siemens-s81 cdc-test";
}

void cdc_test_device_select_personality(uint32_t id, bool persist) {
	(void) runtime_profile_select((usb_profile_id_t) id, persist);
}
