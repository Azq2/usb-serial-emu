#pragma once

#include "usbd_core.h"

uint16_t usb_device_ep0_size(void);

#undef USB_CTRL_EP_MPS
#define USB_CTRL_EP_MPS usb_device_ep0_size()
