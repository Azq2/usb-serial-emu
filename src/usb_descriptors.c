#include <stddef.h>
#include <stdint.h>

#include "runtime_profile.h"
#include "usb_util.h"
#include "usb_cdc.h"
#include "usb_def.h"

static struct usb_device_descriptor device_descriptor;

const uint8_t *usb_descriptor_device(void) {
	const usb_profile_t *profile = runtime_profile_get();
	device_descriptor = (struct usb_device_descriptor){
		.bLength = sizeof(struct usb_device_descriptor),
		.bDescriptorType = USB_DESCRIPTOR_TYPE_DEVICE,
		.bcdUSB = profile->bcd_usb,
		.bDeviceClass = profile->device_class,
		.bDeviceSubClass = profile->device_subclass,
		.bDeviceProtocol = profile->device_protocol,
		.bMaxPacketSize0 = profile->ep0_size,
		.idVendor = profile->vid,
		.idProduct = profile->pid,
		.bcdDevice = profile->bcd_device,
		.iManufacturer = 1,
		.iProduct = 2,
		.iSerialNumber = profile->serial == NULL ? 0 : 3,
		.bNumConfigurations = 1,
	};
	return (const uint8_t *) &device_descriptor;
}

static const uint8_t ft232_configuration[] = {
	9,
	USB_DESCRIPTOR_TYPE_CONFIGURATION,
	WBVAL(32),
	1,
	1,
	0,
	0xa0,
	45,
	9,
	USB_DESCRIPTOR_TYPE_INTERFACE,
	0,
	0,
	2,
	USB_DEVICE_CLASS_VEND_SPECIFIC,
	0xff,
	0xff,
	2,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x81,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(64),
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x02,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(64),
	0,
};

static const uint8_t pl2303_configuration[] = {
	9,
	USB_DESCRIPTOR_TYPE_CONFIGURATION,
	WBVAL(39),
	1,
	1,
	0,
	0x80,
	50,
	9,
	USB_DESCRIPTOR_TYPE_INTERFACE,
	0,
	0,
	3,
	USB_DEVICE_CLASS_VEND_SPECIFIC,
	0,
	0,
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x81,
	USB_ENDPOINT_TYPE_INTERRUPT,
	WBVAL(10),
	1,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x02,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(64),
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x83,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(64),
	0,
};

static const uint8_t pl2303_split_configuration[] = {
	9,
	USB_DESCRIPTOR_TYPE_CONFIGURATION,
	WBVAL(48),
	2,
	1,
	0,
	0x80,
	50,
	9,
	USB_DESCRIPTOR_TYPE_INTERFACE,
	0,
	0,
	1,
	USB_DEVICE_CLASS_VEND_SPECIFIC,
	0,
	0,
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x81,
	USB_ENDPOINT_TYPE_INTERRUPT,
	WBVAL(10),
	1,
	9,
	USB_DESCRIPTOR_TYPE_INTERFACE,
	1,
	0,
	2,
	USB_DEVICE_CLASS_VEND_SPECIFIC,
	0,
	0,
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x02,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(64),
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x83,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(64),
	0,
};

static const uint8_t ch341_configuration[] = {
	9,
	USB_DESCRIPTOR_TYPE_CONFIGURATION,
	WBVAL(39),
	1,
	1,
	0,
	0x80,
	49,
	9,
	USB_DESCRIPTOR_TYPE_INTERFACE,
	0,
	0,
	3,
	USB_DEVICE_CLASS_VEND_SPECIFIC,
	0x01,
	0x02,
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x82,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(32),
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x02,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(32),
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x81,
	USB_ENDPOINT_TYPE_INTERRUPT,
	WBVAL(8),
	1,
};

static const uint8_t cp210x_configuration[] = {
	9,
	USB_DESCRIPTOR_TYPE_CONFIGURATION,
	WBVAL(32),
	1,
	1,
	0,
	0x80,
	50,
	9,
	USB_DESCRIPTOR_TYPE_INTERFACE,
	0,
	0,
	2,
	USB_DEVICE_CLASS_VEND_SPECIFIC,
	0,
	0,
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x81,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(64),
	0,
	7,
	USB_DESCRIPTOR_TYPE_ENDPOINT,
	0x01,
	USB_ENDPOINT_TYPE_BULK,
	WBVAL(64),
	0,
};

static uint8_t cdc_acm_configuration[] = {
	USB_CONFIG_DESCRIPTOR_INIT(9 + CDC_ACM_DESCRIPTOR_LEN, 2, 1, USB_CONFIG_BUS_POWERED, 100),
	CDC_ACM_DESCRIPTOR_INIT(0, 0x81, 0x02, 0x82, 64, 0),
};

_Static_assert(sizeof(ft232_configuration) == 32, "invalid FT232 descriptor");
_Static_assert(sizeof(pl2303_configuration) == 39, "invalid PL2303 descriptor");
_Static_assert(sizeof(pl2303_split_configuration) == 48, "invalid split PL2303 descriptor");
_Static_assert(sizeof(ch341_configuration) == 39, "invalid CH341 descriptor");
_Static_assert(sizeof(cp210x_configuration) == 32, "invalid CP210x descriptor");
_Static_assert(sizeof(cdc_acm_configuration) == 9 + CDC_ACM_DESCRIPTOR_LEN, "invalid CDC ACM descriptor");

const uint8_t *usb_descriptor_configuration(void) {
	if (runtime_profile_is_ft232()) {
		return ft232_configuration;
	}
	if (runtime_profile_is_ch341()) {
		return ch341_configuration;
	}
	if (runtime_profile_is_cp210x()) {
		return cp210x_configuration;
	}
	if (runtime_profile_is_cdc_acm()) {
		/* Advertise SEND_BREAK in addition to line coding and modem control. */
		cdc_acm_configuration[39] = 0x06;
		return cdc_acm_configuration;
	}
	if (runtime_profile_pl2303_split_interfaces()) {
		return pl2303_split_configuration;
	}
	return pl2303_configuration;
}

const char *usb_descriptor_string(uint8_t index) {
	static const char langid[] = {0x09, 0x04, 0x00};

	if (index == 0) {
		return langid;
	}

	const usb_profile_t *profile = runtime_profile_get();
	const char *ascii = NULL;
	if (index == 1) {
		ascii = profile->manufacturer;
	} else if (index == 2) {
		ascii = profile->product;
	} else if (index == 3) {
		ascii = profile->serial;
	}
	return ascii;
}
