#include "runtime_profile.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "pico/platform.h"

enum {
	PROFILE_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE,
	PROFILE_VERSION = 1,
	PROFILE_SCRATCH_INDEX = 1,
};

static const uint32_t profile_magic = 0x55454d55u;
static const uint32_t profile_check_xor = 0xa53cc35au;
static const uint32_t profile_scratch_magic = 0x55534250u;

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t profile;
	uint32_t check;
} profile_record_t;

typedef struct {
	uint8_t page[FLASH_PAGE_SIZE];
} profile_page_t;

static const usb_profile_t profiles[USB_PROFILE_COUNT] = {
	[USB_PROFILE_FT232R] =
		{
			.chip = USB_CHIP_FT232,
			.vid = 0x0403,
			.pid = 0x6001,
			.bcd_usb = 0x0200,
			.bcd_device = 0x0600,
			.ep0_size = 8,
			.manufacturer = "FTDI",
			.product = "FT232R USB UART",
			.serial = "FTRP2040",
		},
	[USB_PROFILE_PL2303_H] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x067b,
			.pid = 0x2303,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0100,
			.ep0_size = 8,
			.manufacturer = "Prolific Technology Inc.",
			.product = "USB-Serial Controller",
			.serial = NULL,
		},
	[USB_PROFILE_PL2303_HX] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x067b,
			.pid = 0x2303,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0300,
			.ep0_size = 64,
			.manufacturer = "Prolific Technology Inc.",
			.product = "USB-Serial Controller",
			.serial = NULL,
		},
	[USB_PROFILE_PL2303_TA] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x067b,
			.pid = 0x2303,
			.bcd_usb = 0x0200,
			.bcd_device = 0x0300,
			.ep0_size = 64,
			.manufacturer = "Prolific Technology Inc.",
			.product = "USB-Serial Controller",
			.serial = NULL,
		},
	[USB_PROFILE_PL2303_TB] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x067b,
			.pid = 0x2303,
			.bcd_usb = 0x0200,
			.bcd_device = 0x0500,
			.ep0_size = 64,
			.manufacturer = "Prolific Technology Inc.",
			.product = "USB-Serial Controller",
			.serial = NULL,
		},
	[USB_PROFILE_PL2303_HXD] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x067b,
			.pid = 0x2303,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0400,
			.ep0_size = 64,
			.manufacturer = "Prolific Technology Inc.",
			.product = "USB-Serial Controller",
			.serial = NULL,
		},
	[USB_PROFILE_PL2303_HXD_CLONE] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x067b,
			.pid = 0x2303,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0400,
			.ep0_size = 64,
			.manufacturer = "Prolific Technology Inc.",
			.product = "USB-Serial Controller",
			.serial = NULL,
		},
	[USB_PROFILE_PL2303_HXN] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x067b,
			.pid = 0x2303,
			.bcd_usb = 0x0200,
			.bcd_device = 0x0100,
			.ep0_size = 64,
			.manufacturer = "Prolific Technology Inc.",
			.product = "USB-Serial Controller",
			.serial = NULL,
		},
	[USB_PROFILE_CH340] =
		{
			.chip = USB_CHIP_CH341,
			.vid = 0x1a86,
			.pid = 0x7523,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0264,
			.ep0_size = 8,
			.manufacturer = "QinHeng Electronics",
			.product = "USB Serial",
			.serial = NULL,
		},
	[USB_PROFILE_CP2102] =
		{
			.chip = USB_CHIP_CP210X,
			.vid = 0x10c4,
			.pid = 0xea60,
			.bcd_usb = 0x0200,
			.bcd_device = 0x0100,
			.ep0_size = 64,
			.manufacturer = "Silicon Labs",
			.product = "CP2102 USB to UART Bridge Controller",
			.serial = "CP2102RP2040",
		},
	[USB_PROFILE_CDC_ACM] =
		{
			.chip = USB_CHIP_CDC_ACM,
			.vid = 0x2e8a,
			.pid = 0x000a,
			.bcd_usb = 0x0200,
			.bcd_device = 0x0100,
			.ep0_size = 64,
			.device_class = 0xef,
			.device_subclass = 0x02,
			.device_protocol = 0x01,
			.manufacturer = "Raspberry Pi",
			.product = "CDC ACM USB Serial",
			.serial = "ACMRP2040",
		},
	[USB_PROFILE_SIEMENS_SX1] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x11f5,
			.pid = 0x0001,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0300,
			.ep0_size = 64,
			.manufacturer = "Siemens AG",
			.product = "SX1",
			.serial = NULL,
		},
	[USB_PROFILE_SIEMENS_X65] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x11f5,
			.pid = 0x0003,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0300,
			.ep0_size = 64,
			.manufacturer = "Siemens AG",
			.product = "Mobile phone USB cable",
			.serial = NULL,
		},
	[USB_PROFILE_SIEMENS_X75] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x11f5,
			.pid = 0x0004,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0300,
			.ep0_size = 64,
			.manufacturer = "Siemens AG",
			.product = "X75",
			.serial = NULL,
		},
	[USB_PROFILE_SIEMENS_EF81] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x11f5,
			.pid = 0x0005,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0300,
			.ep0_size = 64,
			.manufacturer = "Siemens AG",
			.product = "SXG75/EF81",
			.serial = NULL,
		},
	[USB_PROFILE_BENQ_SIEMENS_S81] =
		{
			.chip = USB_CHIP_PL2303,
			.vid = 0x04a5,
			.pid = 0x4027,
			.bcd_usb = 0x0110,
			.bcd_device = 0x0300,
			.ep0_size = 64,
			.manufacturer = "BenQ",
			.product = "BenQ-Siemens S81",
			.serial = NULL,
		},
};

_Static_assert(sizeof(profiles) / sizeof(profiles[0]) == USB_PROFILE_COUNT,
			   "profile table must cover every profile ID");

static usb_profile_id_t active_profile = USB_PROFILE_FT232R;

static uint32_t record_check(const profile_record_t *record) {
	return record->magic ^ record->version ^ record->profile ^ profile_check_xor;
}

static uint32_t scratch_profile_value(usb_profile_id_t profile) {
	uint32_t value = (uint32_t) profile;
	return value | ((~value & 0xffffu) << 16);
}

static bool scratch_profile_read(usb_profile_id_t *profile) {
	uint32_t value = watchdog_hw->scratch[PROFILE_SCRATCH_INDEX];
	uint32_t id = value & 0xffffu;
	if (watchdog_hw->scratch[0] != profile_scratch_magic || ((value >> 16) & 0xffffu) != (~id & 0xffffu) ||
		id >= USB_PROFILE_COUNT) {
		return false;
	}
	*profile = (usb_profile_id_t) id;
	return true;
}

static void scratch_profile_write(usb_profile_id_t profile) {
	watchdog_hw->scratch[PROFILE_SCRATCH_INDEX] = scratch_profile_value(profile);
	watchdog_hw->scratch[0] = profile_scratch_magic;
}

static void scratch_profile_clear(void) {
	watchdog_hw->scratch[0] = 0;
	watchdog_hw->scratch[PROFILE_SCRATCH_INDEX] = 0;
}

void runtime_profile_init(void) {
	if (scratch_profile_read(&active_profile)) {
		return;
	}

	const profile_record_t *record = (const profile_record_t *) (XIP_BASE + PROFILE_FLASH_OFFSET);
	if (record->magic == profile_magic && record->version == PROFILE_VERSION && record->profile < USB_PROFILE_COUNT &&
		record->check == record_check(record)) {
		active_profile = (usb_profile_id_t) record->profile;
	}
}

const usb_profile_t *runtime_profile_get(void) {
	return &profiles[active_profile];
}

bool runtime_profile_is_pl2303(void) {
	return profiles[active_profile].chip == USB_CHIP_PL2303;
}

bool runtime_profile_is_ft232(void) {
	return profiles[active_profile].chip == USB_CHIP_FT232;
}

bool runtime_profile_is_ch341(void) {
	return profiles[active_profile].chip == USB_CHIP_CH341;
}

bool runtime_profile_is_cp210x(void) {
	return profiles[active_profile].chip == USB_CHIP_CP210X;
}

bool runtime_profile_is_cdc_acm(void) {
	return profiles[active_profile].chip == USB_CHIP_CDC_ACM;
}

bool runtime_profile_is_pl2303_hxn(void) {
	return active_profile == USB_PROFILE_PL2303_HXN;
}

bool runtime_profile_is_pl2303_hxd_clone(void) {
	return active_profile == USB_PROFILE_PL2303_HXD_CLONE;
}

bool runtime_profile_supports_hx_status(void) {
	return active_profile == USB_PROFILE_PL2303_TA || active_profile == USB_PROFILE_PL2303_TB;
}

bool runtime_profile_pl2303_status_at_index_zero(void) {
	return active_profile == USB_PROFILE_SIEMENS_SX1 || active_profile == USB_PROFILE_SIEMENS_X65 ||
		   active_profile == USB_PROFILE_SIEMENS_X75;
}

bool runtime_profile_pl2303_split_interfaces(void) {
	return active_profile == USB_PROFILE_SIEMENS_EF81;
}

static void __not_in_flash_func(write_profile_page)(void *parameter) {
	const profile_page_t *page = parameter;
	flash_range_erase(PROFILE_FLASH_OFFSET, FLASH_SECTOR_SIZE);
	flash_range_program(PROFILE_FLASH_OFFSET, page->page, sizeof(page->page));
}

bool runtime_profile_select(usb_profile_id_t profile, bool persist) {
	if (profile >= USB_PROFILE_COUNT) {
		return false;
	}

	if (!persist) {
		scratch_profile_write(profile);
		watchdog_reboot(0, 0, 10);
		return true;
	}

	profile_page_t page;
	memset(&page, 0xff, sizeof(page));
	profile_record_t *record = (profile_record_t *) page.page;
	record->magic = profile_magic;
	record->version = PROFILE_VERSION;
	record->profile = profile;
	record->check = record_check(record);

	if (flash_safe_execute(write_profile_page, &page, 1000) != PICO_OK) {
		return false;
	}

	scratch_profile_clear();
	watchdog_reboot(0, 0, 10);
	return true;
}
