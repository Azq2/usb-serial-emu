#pragma once

#include <stdbool.h>
#include <stdint.h>

void personality_switch_control_update(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits, bool dtr,
									   bool rts, bool break_enabled);
void personality_switch_task(void);
