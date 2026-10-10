#include "usb_serial_dispatch.h"

#include "cdc_acm_device.h"
#include "ch341_device.h"
#include "cp210x_device.h"
#include "ft232_device.h"
#include "pl2303_device.h"
#include "runtime_profile.h"

int usb_serial_vendor_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	if (runtime_profile_is_ft232()) {
		return ft232_vendor_request(request, data, length);
	}
	if (runtime_profile_is_ch341()) {
		return ch341_vendor_request(request, data, length);
	}
	if (runtime_profile_is_cp210x()) {
		return cp210x_vendor_request(request, data, length);
	}
	if (runtime_profile_is_pl2303()) {
		return pl2303_vendor_request(request, data, length);
	}
	return -1;
}

int usb_serial_class_request(struct usb_setup_packet *request, uint8_t **data, uint32_t *length) {
	if (runtime_profile_is_pl2303()) {
		return pl2303_class_request(request, data, length);
	}
	if (runtime_profile_is_cdc_acm()) {
		return cdc_acm_class_request(request, data, length);
	}
	return -1;
}

void usb_serial_reset(void) {
	if (runtime_profile_is_ft232()) {
		ft232_reset();
	} else if (runtime_profile_is_pl2303()) {
		pl2303_reset();
	} else if (runtime_profile_is_ch341()) {
		ch341_reset();
	} else if (runtime_profile_is_cp210x()) {
		cp210x_reset();
	} else {
		cdc_acm_reset();
	}
}

void usb_serial_configured(void) {
	if (runtime_profile_is_ft232()) {
		ft232_configured();
	} else if (runtime_profile_is_pl2303()) {
		pl2303_configured();
	} else if (runtime_profile_is_ch341()) {
		ch341_configured();
	} else if (runtime_profile_is_cp210x()) {
		cp210x_configured();
	} else {
		cdc_acm_configured();
	}
}

void usb_serial_transfer_complete(uint8_t endpoint, uint32_t transferred) {
	if (runtime_profile_is_ft232()) {
		ft232_transfer_complete(endpoint, transferred);
	} else if (runtime_profile_is_pl2303()) {
		pl2303_transfer_complete(endpoint, transferred);
	} else if (runtime_profile_is_ch341()) {
		ch341_transfer_complete(endpoint, transferred);
	} else if (runtime_profile_is_cp210x()) {
		cp210x_transfer_complete(endpoint, transferred);
	} else {
		cdc_acm_transfer_complete(endpoint, transferred);
	}
}

void usb_serial_task(void) {
	if (runtime_profile_is_ft232()) {
		ft232_task();
	} else if (runtime_profile_is_pl2303()) {
		pl2303_task();
	} else if (runtime_profile_is_ch341()) {
		ch341_task();
	} else if (runtime_profile_is_cp210x()) {
		cp210x_task();
	} else {
		cdc_acm_task();
	}
}

size_t usb_serial_read(void *buffer, size_t size) {
	if (runtime_profile_is_ft232()) {
		return ft232_read(buffer, size);
	}
	if (runtime_profile_is_pl2303()) {
		return pl2303_read(buffer, size);
	}
	if (runtime_profile_is_ch341()) {
		return ch341_read(buffer, size);
	}
	if (runtime_profile_is_cp210x()) {
		return cp210x_read(buffer, size);
	}
	return cdc_acm_read(buffer, size);
}

size_t usb_serial_write(const void *buffer, size_t size) {
	if (runtime_profile_is_ft232()) {
		return ft232_write(buffer, size);
	}
	if (runtime_profile_is_pl2303()) {
		return pl2303_write(buffer, size);
	}
	if (runtime_profile_is_ch341()) {
		return ch341_write(buffer, size);
	}
	if (runtime_profile_is_cp210x()) {
		return cp210x_write(buffer, size);
	}
	return cdc_acm_write(buffer, size);
}
