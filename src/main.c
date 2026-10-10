#include "personality_switch.h"
#include "runtime_profile.h"
#include "usb_device.h"
#include "usb_serial_dispatch.h"

#include "hardware/sync.h"

int main(void) {
	uint8_t buffer[64];
	size_t pending_offset = 0;
	size_t pending_size = 0;

	runtime_profile_init();
	usb_device_init();

	while (true) {
		uint32_t irq_state = save_and_disable_interrupts();
		usb_serial_task();

		if (pending_offset == pending_size) {
			pending_size = usb_serial_read(buffer, sizeof(buffer));
			pending_offset = 0;
		}
		if (pending_offset != pending_size) {
			pending_offset += usb_serial_write(&buffer[pending_offset], pending_size - pending_offset);
		}
		restore_interrupts(irq_state);

		personality_switch_task();
	}
}
