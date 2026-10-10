#include "usb_device.h"

#include <stddef.h>
#include <string.h>

#include "runtime_profile.h"
#include "usb_descriptors.h"
#include "usb_serial_dispatch.h"

static struct usbd_interface interfaces[2];
static struct usbd_endpoint endpoints[3];

static const uint8_t *device_descriptor_callback(uint8_t speed) {
	(void) speed;
	return usb_descriptor_device();
}

static const uint8_t *configuration_descriptor_callback(uint8_t speed) {
	(void) speed;
	return usb_descriptor_configuration();
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index) {
	(void) speed;
	return usb_descriptor_string(index);
}

static const struct usb_descriptor descriptors = {
	.device_descriptor_callback = device_descriptor_callback,
	.config_descriptor_callback = configuration_descriptor_callback,
	.string_descriptor_callback = string_descriptor_callback,
};

static int class_request(uint8_t bus, struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	(void) bus;
	return usb_serial_class_request(request, data, length);
}

static int vendor_request(uint8_t bus, struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	(void) bus;
	return usb_serial_vendor_request(request, data, length);
}

static void endpoint_complete(uint8_t bus, uint8_t endpoint, uint32_t transferred) {
	(void) bus;
	usb_serial_transfer_complete(endpoint, transferred);
}

static void device_event(uint8_t bus, uint8_t event) {
	(void) bus;
	if (event == USBD_EVENT_RESET) {
		usb_serial_reset();
	} else if (event == USBD_EVENT_CONFIGURED) {
		usb_serial_configured();
	}
}

static void add_endpoint(unsigned index, uint8_t address) {
	endpoints[index].ep_addr = address;
	endpoints[index].ep_cb = endpoint_complete;
	usbd_add_endpoint(0, &endpoints[index]);
}

bool usb_device_transfer(uint8_t endpoint, void *buffer, uint32_t size) {
	if (endpoint & USB_EP_DIR_IN) {
		return usbd_ep_start_write(0, endpoint, buffer, size) == 0;
	}
	return usbd_ep_start_read(0, endpoint, buffer, size) == 0;
}

uint16_t usb_device_ep0_size(void) {
	return runtime_profile_get()->ep0_size;
}

void usb_device_init(void) {
	memset(interfaces, 0, sizeof(interfaces));
	memset(endpoints, 0, sizeof(endpoints));

	usbd_desc_register(0, &descriptors);
	interfaces[0].class_interface_handler = class_request;
	interfaces[0].vendor_handler = vendor_request;
	usbd_add_interface(0, &interfaces[0]);

	if (runtime_profile_pl2303_split_interfaces() || runtime_profile_is_cdc_acm()) {
		interfaces[1].class_interface_handler = class_request;
		usbd_add_interface(0, &interfaces[1]);
	}

	if (runtime_profile_is_ft232()) {
		add_endpoint(0, 0x81);
		add_endpoint(1, 0x02);
	} else if (runtime_profile_is_ch341()) {
		add_endpoint(0, 0x82);
		add_endpoint(1, 0x02);
		add_endpoint(2, 0x81);
	} else if (runtime_profile_is_cp210x()) {
		add_endpoint(0, 0x81);
		add_endpoint(1, 0x01);
	} else if (runtime_profile_is_cdc_acm()) {
		add_endpoint(0, 0x81);
		add_endpoint(1, 0x02);
		add_endpoint(2, 0x82);
	} else {
		add_endpoint(0, 0x81);
		add_endpoint(1, 0x02);
		add_endpoint(2, 0x83);
	}

	usb_serial_reset();
	(void) usbd_initialize(0, 0, device_event);
}
