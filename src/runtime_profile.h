#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	USB_PROFILE_FT232R = 0,
	USB_PROFILE_PL2303_H,
	USB_PROFILE_PL2303_HX,
	USB_PROFILE_PL2303_TA,
	USB_PROFILE_PL2303_TB,
	USB_PROFILE_PL2303_HXD,
	USB_PROFILE_PL2303_HXD_CLONE,
	USB_PROFILE_PL2303_HXN,
	USB_PROFILE_CH340,
	USB_PROFILE_CP2102,
	USB_PROFILE_CDC_ACM,
	USB_PROFILE_SIEMENS_SX1,
	USB_PROFILE_SIEMENS_X65,
	USB_PROFILE_SIEMENS_X75,
	USB_PROFILE_SIEMENS_EF81,
	USB_PROFILE_BENQ_SIEMENS_S81,
	USB_PROFILE_COUNT,
} usb_profile_id_t;

typedef enum {
	USB_CHIP_FT232,
	USB_CHIP_PL2303,
	USB_CHIP_CH341,
	USB_CHIP_CP210X,
	USB_CHIP_CDC_ACM,
} usb_chip_t;

typedef struct {
	usb_chip_t chip;
	uint16_t vid;
	uint16_t pid;
	uint16_t bcd_usb;
	uint16_t bcd_device;
	uint8_t ep0_size;
	uint8_t device_class;
	uint8_t device_subclass;
	uint8_t device_protocol;
	const char *manufacturer;
	const char *product;
	const char *serial;
} usb_profile_t;

void runtime_profile_init(void);
const usb_profile_t *runtime_profile_get(void);
bool runtime_profile_is_pl2303(void);
bool runtime_profile_is_ft232(void);
bool runtime_profile_is_ch341(void);
bool runtime_profile_is_cp210x(void);
bool runtime_profile_is_cdc_acm(void);

bool runtime_profile_is_pl2303_hxn(void);
bool runtime_profile_is_pl2303_hxd_clone(void);
bool runtime_profile_supports_hx_status(void);
bool runtime_profile_pl2303_status_at_index_zero(void);
bool runtime_profile_pl2303_split_interfaces(void);

/* Select a profile through watchdog scratch and reboot.  With persist=true,
   also update the flash fallback used after power loss. */
bool runtime_profile_select(usb_profile_id_t profile, bool persist);
