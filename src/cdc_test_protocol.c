#include "cdc_test_protocol.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdc_test_device.h"
#include "pico/bootrom.h"
#include "pico/time.h"

enum {
	LINE_CAPACITY = 1024,
	OUTPUT_RESERVE = 256,
	TRANSFER_BUFFER_SIZE = 256,
	MAX_TIMEOUT_MS = 600000,
	DEFAULT_SCRIPT_TIMEOUT_MS = 5000,
	MAX_SCRIPT_OPS = 64,
	SCRIPT_DATA_CAPACITY = 4096,
	CDC_STATE_DCD = 1u << 0,
	CDC_STATE_DSR = 1u << 1,
	CDC_STATE_BREAK = 1u << 2,
	CDC_STATE_RING = 1u << 3,
	CDC_STATE_FRAMING = 1u << 4,
	CDC_STATE_PARITY = 1u << 5,
	CDC_STATE_OVERRUN = 1u << 6,
};

typedef enum {
	MODE_COMMAND,
	MODE_SCRIPT_UPLOAD,
	MODE_SCRIPT_RUN,
	MODE_DELAY,
	MODE_TX_PRNG,
	MODE_RX_PRNG,
	MODE_TX_LITERAL,
	MODE_RX_LITERAL,
	MODE_LOOPBACK,
} protocol_mode_t;

typedef enum {
	SCRIPT_DELAY,
	SCRIPT_SIGNAL,
	SCRIPT_PULSE,
	SCRIPT_TX_PRNG,
	SCRIPT_RX_PRNG,
	SCRIPT_SEND,
	SCRIPT_RECV,
	SCRIPT_LOOPBACK,
} script_opcode_t;

typedef struct {
	script_opcode_t opcode;
	uint16_t data_offset;
	uint16_t data_size;
	uint32_t arg0;
	uint32_t arg1;
	uint32_t arg2;
} script_op_t;

typedef struct {
	uint32_t state;
	uint32_t word;
	uint8_t bytes_left;
} prng_t;

typedef struct {
	protocol_mode_t mode;
	char line[LINE_CAPACITY];
	size_t line_size;
	bool line_overflow;
	uint32_t command_id;
	uint32_t remaining;
	uint32_t transferred;
	uint32_t timeout_ms;
	uint64_t deadline_us;
	prng_t prng;
	bool mismatch;
	uint32_t mismatch_offset;
	uint8_t mismatch_expected;
	uint8_t mismatch_actual;
	uint16_t literal_offset;
	bool script_running;
	uint32_t script_id;
	uint32_t script_line;
	uint32_t script_error_line;
	uint32_t script_timeout_ms;
	size_t script_op_count;
	size_t script_op_index;
	size_t script_data_size;
	script_op_t script_ops[MAX_SCRIPT_OPS];
	uint8_t script_data[SCRIPT_DATA_CAPACITY];
	uint16_t steady_serial_state;
	uint16_t pulse_serial_state;
	uint64_t pulse_deadlines[7];
	bool event_pending;
	cdc_test_event_t event;
	uint32_t baud;
	uint16_t host_break;
	uint8_t data_bits;
	uint8_t parity;
	uint8_t stop_bits;
	bool dtr;
	bool rts;
	bool bootsel_pending;
	uint64_t bootsel_deadline_us;
	bool personality_pending;
	bool personality_persist;
	uint32_t personality_id;
	uint64_t personality_deadline_us;
	uint32_t reset_generation;
} protocol_state_t;

static protocol_state_t state;

static uint64_t deadline_after_ms(uint32_t timeout_ms) {
	return time_us_64() + (uint64_t) timeout_ms * 1000u;
}

static void prng_init(prng_t *prng, uint32_t seed) {
	prng->state = seed;
	prng->word = 0;
	prng->bytes_left = 0;
}

static uint8_t prng_next(prng_t *prng) {
	if (prng->bytes_left == 0) {
		uint32_t value = prng->state;
		value ^= value << 13;
		value ^= value >> 17;
		value ^= value << 5;
		prng->state = value;
		prng->word = value;
		prng->bytes_left = 4;
	}
	uint8_t result = (uint8_t) prng->word;
	prng->word >>= 8;
	--prng->bytes_left;
	return result;
}

static bool send_line(const char *format, ...) {
	char output[OUTPUT_RESERVE];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(output, sizeof(output), format, args);
	va_end(args);
	if (length < 0 || (size_t) length >= sizeof(output) || cdc_test_device_write_free() < (size_t) length) {
		return false;
	}
	return cdc_test_device_write(output, (size_t) length) == (size_t) length;
}

static bool parse_u32(const char *text, uint32_t *value) {
	if (text == NULL || *text == '\0' || *text == '-') {
		return false;
	}
	char *end;
	errno = 0;
	unsigned long parsed = strtoul(text, &end, 0);
	if (errno == ERANGE || *end != '\0' || parsed > UINT32_MAX) {
		return false;
	}
	*value = (uint32_t) parsed;
	return true;
}

static const char *next_token(char **context) {
	return strtok_r(NULL, " \t", context);
}

static bool no_more_tokens(char **context) {
	return next_token(context) == NULL;
}

static void set_serial_state(void) {
	cdc_test_device_set_serial_state(state.steady_serial_state | state.pulse_serial_state);
}

static uint16_t signal_bit(const char *name) {
	if (strcmp(name, "DCD") == 0) {
		return CDC_STATE_DCD;
	}
	if (strcmp(name, "DSR") == 0) {
		return CDC_STATE_DSR;
	}
	if (strcmp(name, "RING") == 0) {
		return CDC_STATE_RING;
	}
	if (strcmp(name, "BREAK") == 0) {
		return CDC_STATE_BREAK;
	}
	if (strcmp(name, "FRAMING") == 0) {
		return CDC_STATE_FRAMING;
	}
	if (strcmp(name, "PARITY") == 0) {
		return CDC_STATE_PARITY;
	}
	if (strcmp(name, "OVERRUN") == 0) {
		return CDC_STATE_OVERRUN;
	}
	return 0;
}

static void reset_protocol(void) {
	state.mode = MODE_COMMAND;
	state.line_size = 0;
	state.line_overflow = false;
	state.remaining = 0;
	state.transferred = 0;
	state.mismatch = false;
	state.script_running = false;
	state.script_line = 0;
	state.script_error_line = 0;
	state.script_timeout_ms = DEFAULT_SCRIPT_TIMEOUT_MS;
	state.script_op_count = 0;
	state.script_op_index = 0;
	state.script_data_size = 0;
	state.steady_serial_state = 0;
	state.pulse_serial_state = 0;
	memset(state.pulse_deadlines, 0, sizeof(state.pulse_deadlines));
	state.bootsel_pending = false;
	state.personality_pending = false;
	set_serial_state();
}

static void reset_host_state(void) {
	state.baud = 115200;
	state.data_bits = 8;
	state.parity = 0;
	state.stop_bits = 0;
	state.dtr = false;
	state.rts = false;
	state.host_break = 0;
}

static void command_error(uint32_t id, const char *code) {
	(void) send_line("%lu ERR %s\n", (unsigned long) id, code);
}

static bool parse_transfer(char **context, uint32_t *seed, uint32_t *size, uint32_t *timeout_ms, bool with_timeout) {
	const char *seed_text = next_token(context);
	const char *size_text = next_token(context);
	const char *timeout_text = with_timeout ? next_token(context) : NULL;
	if (!parse_u32(seed_text, seed) || *seed == 0 || !parse_u32(size_text, size) ||
		(with_timeout && (!parse_u32(timeout_text, timeout_ms) || *timeout_ms == 0 || *timeout_ms > MAX_TIMEOUT_MS)) ||
		!no_more_tokens(context)) {
		return false;
	}
	return true;
}

static void start_tx(uint32_t id, uint32_t seed, uint32_t size) {
	state.command_id = id;
	state.remaining = size;
	state.transferred = 0;
	prng_init(&state.prng, seed);
	state.mode = MODE_TX_PRNG;
	(void) send_line("%lu DATA %lu\n", (unsigned long) id, (unsigned long) size);
}

static void start_rx(uint32_t id, uint32_t seed, uint32_t size, uint32_t timeout_ms) {
	state.command_id = id;
	state.remaining = size;
	state.transferred = 0;
	state.deadline_us = deadline_after_ms(timeout_ms);
	state.timeout_ms = timeout_ms;
	state.mismatch = false;
	prng_init(&state.prng, seed);
	state.mode = MODE_RX_PRNG;
	(void) send_line("%lu READY %lu\n", (unsigned long) id, (unsigned long) size);
}

static void start_loopback(uint32_t id, uint32_t size, uint32_t timeout_ms) {
	state.command_id = id;
	state.remaining = size;
	state.transferred = 0;
	state.deadline_us = deadline_after_ms(timeout_ms);
	state.timeout_ms = timeout_ms;
	state.mode = MODE_LOOPBACK;
	(void) send_line("%lu READY %lu\n", (unsigned long) id, (unsigned long) size);
}

static char *skip_spaces(char *text) {
	while (*text == ' ' || *text == '\t') {
		++text;
	}
	return text;
}

static char *take_word(char **cursor) {
	char *word = skip_spaces(*cursor);
	if (*word == '\0') {
		*cursor = word;
		return NULL;
	}
	char *end = word;
	while (*end != '\0' && *end != ' ' && *end != '\t') {
		++end;
	}
	if (*end != '\0') {
		*end++ = '\0';
	}
	*cursor = end;
	return word;
}

static bool cursor_empty(char *cursor) {
	return *skip_spaces(cursor) == '\0';
}

static int hex_value(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static bool parse_literal(char *text, uint32_t expected_size, uint16_t *offset) {
	if (expected_size > SCRIPT_DATA_CAPACITY - state.script_data_size) {
		return false;
	}
	text = skip_spaces(text);
	size_t start = state.script_data_size;
	size_t count = 0;
	if (*text == '"') {
		++text;
		while (*text != '\0' && *text != '"') {
			uint8_t value;
			if (*text == '\\') {
				++text;
				switch (*text) {
					case '"':
					case '\\':
						value = (uint8_t) *text++;
						break;
					case '0':
						value = 0;
						++text;
						break;
					case 'n':
						value = '\n';
						++text;
						break;
					case 'r':
						value = '\r';
						++text;
						break;
					case 't':
						value = '\t';
						++text;
						break;
					case 'x': {
						if (text[1] == '\0' || text[2] == '\0') {
							return false;
						}
						int high = hex_value(text[1]);
						int low = hex_value(text[2]);
						if (high < 0 || low < 0) {
							return false;
						}
						value = (uint8_t) ((high << 4) | low);
						text += 3;
						break;
					}
					default:
						return false;
				}
			} else {
				unsigned char value_char = (unsigned char) *text++;
				if (value_char < 0x20 || value_char > 0x7e) {
					return false;
				}
				value = value_char;
			}
			if (count >= expected_size) {
				return false;
			}
			state.script_data[start + count++] = value;
		}
		if (*text != '"' || !cursor_empty(text + 1)) {
			return false;
		}
	} else {
		char *end = text + strlen(text);
		while (end > text && (end[-1] == ' ' || end[-1] == '\t')) {
			--end;
		}
		if ((size_t) (end - text) != (size_t) expected_size * 2u) {
			return false;
		}
		for (uint32_t i = 0; i < expected_size; ++i) {
			int high = hex_value(text[i * 2u]);
			int low = hex_value(text[i * 2u + 1u]);
			if (high < 0 || low < 0) {
				return false;
			}
			state.script_data[start + count++] = (uint8_t) ((high << 4) | low);
		}
	}
	if (count != expected_size) {
		return false;
	}
	*offset = (uint16_t) start;
	state.script_data_size += count;
	return true;
}

static bool add_script_op(const script_op_t *op) {
	if (state.script_op_count == MAX_SCRIPT_OPS) {
		return false;
	}
	state.script_ops[state.script_op_count++] = *op;
	return true;
}

static void script_parse_error(void) {
	if (state.script_error_line == 0) {
		state.script_error_line = state.script_line;
	}
}

static void process_script_line(void) {
	state.line[state.line_size] = '\0';
	++state.script_line;
	char *line = skip_spaces(state.line);
	char *end = line + strlen(line);
	while (end > line && (end[-1] == ' ' || end[-1] == '\t')) {
		*--end = '\0';
	}
	if (strcmp(line, "END") == 0) {
		if (state.line_overflow && state.script_error_line == 0) {
			state.script_error_line = state.script_line;
		}
		if (state.script_error_line != 0) {
			(void) send_line("%lu ERR BAD_SCRIPT line=%lu\n", (unsigned long) state.script_id,
							 (unsigned long) state.script_error_line);
			state.mode = MODE_COMMAND;
			state.script_op_count = 0;
			state.script_data_size = 0;
			return;
		}
		(void) send_line("%lu RUN\n", (unsigned long) state.script_id);
		state.script_running = true;
		state.script_op_index = 0;
		state.mode = MODE_SCRIPT_RUN;
		return;
	}
	if (state.script_error_line != 0) {
		return;
	}
	if (state.line_overflow) {
		script_parse_error();
		return;
	}
	if (*line == '\0') {
		return;
	}
	if (state.script_op_count == MAX_SCRIPT_OPS) {
		script_parse_error();
		return;
	}

	char *cursor = line;
	const char *command = take_word(&cursor);
	script_op_t op = {0};
	if (strcmp(command, "TIMEOUT") == 0) {
		uint32_t timeout_ms;
		if (!parse_u32(take_word(&cursor), &timeout_ms) || timeout_ms == 0 || timeout_ms > MAX_TIMEOUT_MS ||
			!cursor_empty(cursor)) {
			script_parse_error();
			return;
		}
		state.script_timeout_ms = timeout_ms;
		return;
	}
	if (strcmp(command, "DELAY") == 0) {
		op.opcode = SCRIPT_DELAY;
		if (!parse_u32(take_word(&cursor), &op.arg0) || op.arg0 > MAX_TIMEOUT_MS || !cursor_empty(cursor)) {
			script_parse_error();
			return;
		}
	} else if (strcmp(command, "SIGNAL") == 0 || strcmp(command, "PULSE") == 0) {
		const char *name = take_word(&cursor);
		op.arg0 = name == NULL ? 0 : signal_bit(name);
		op.opcode = command[0] == 'S' ? SCRIPT_SIGNAL : SCRIPT_PULSE;
		if (op.arg0 == 0 || !parse_u32(take_word(&cursor), &op.arg1) || (op.opcode == SCRIPT_SIGNAL && op.arg1 > 1) ||
			(op.opcode == SCRIPT_PULSE && (op.arg1 == 0 || op.arg1 > MAX_TIMEOUT_MS)) || !cursor_empty(cursor)) {
			script_parse_error();
			return;
		}
	} else if (strcmp(command, "TX") == 0 || strcmp(command, "RX") == 0) {
		op.opcode = command[0] == 'T' ? SCRIPT_TX_PRNG : SCRIPT_RX_PRNG;
		if (!parse_u32(take_word(&cursor), &op.arg0) || op.arg0 == 0 || !parse_u32(take_word(&cursor), &op.arg1) ||
			!cursor_empty(cursor)) {
			script_parse_error();
			return;
		}
		op.arg2 = state.script_timeout_ms;
	} else if (strcmp(command, "LOOPBACK") == 0) {
		op.opcode = SCRIPT_LOOPBACK;
		if (!parse_u32(take_word(&cursor), &op.arg0) || !cursor_empty(cursor)) {
			script_parse_error();
			return;
		}
		op.arg1 = state.script_timeout_ms;
	} else if (strcmp(command, "SEND") == 0 || strcmp(command, "RECV") == 0) {
		uint32_t size;
		op.opcode = command[0] == 'S' ? SCRIPT_SEND : SCRIPT_RECV;
		if (!parse_u32(take_word(&cursor), &size) || size > UINT16_MAX ||
			!parse_literal(cursor, size, &op.data_offset)) {
			script_parse_error();
			return;
		}
		op.data_size = (uint16_t) size;
		op.arg0 = state.script_timeout_ms;
	} else {
		script_parse_error();
		return;
	}
	if (!add_script_op(&op)) {
		script_parse_error();
	}
}

static void process_command(void) {
	state.line[state.line_size] = '\0';
	if (state.line_overflow) {
		command_error(0, "LINE_TOO_LONG");
		return;
	}

	char *context;
	const char *id_text = strtok_r(state.line, " \t", &context);
	const char *command = next_token(&context);
	uint32_t id;
	if (!parse_u32(id_text, &id) || command == NULL) {
		command_error(0, "SYNTAX");
		return;
	}

	if (strcmp(command, "INFO") == 0) {
		if (!no_more_tokens(&context)) {
			command_error(id, "SYNTAX");
			return;
		}
		(void) send_line("%lu OK protocol=cdc-test version=2 prng=xorshift32\n", (unsigned long) id);
		return;
	}
	if (strcmp(command, "BEGIN") == 0) {
		if (!no_more_tokens(&context)) {
			command_error(id, "SYNTAX");
			return;
		}
		state.script_id = id;
		state.script_line = 0;
		state.script_error_line = 0;
		state.script_timeout_ms = DEFAULT_SCRIPT_TIMEOUT_MS;
		state.script_op_count = 0;
		state.script_op_index = 0;
		state.script_data_size = 0;
		state.mode = MODE_SCRIPT_UPLOAD;
		(void) send_line("%lu READY\n", (unsigned long) id);
		return;
	}
	if (strcmp(command, "PERSONALITIES") == 0) {
		if (!no_more_tokens(&context)) {
			command_error(id, "SYNTAX");
			return;
		}
		(void) send_line("%lu OK %s\n", (unsigned long) id, cdc_test_device_personality_names());
		return;
	}
	if (strcmp(command, "STATE") == 0) {
		if (!no_more_tokens(&context)) {
			command_error(id, "SYNTAX");
			return;
		}
		(void) send_line("%lu OK baud=%lu data=%u parity=%u stop=%u dtr=%u rts=%u serial_state=0x%02x host_break=%u\n",
						 (unsigned long) id, (unsigned long) state.baud, state.data_bits, state.parity, state.stop_bits,
						 state.dtr, state.rts, state.steady_serial_state, state.host_break);
		return;
	}
	if (strcmp(command, "SIGNAL") == 0) {
		const char *name = next_token(&context);
		uint32_t enabled;
		uint16_t bit = name == NULL ? 0 : signal_bit(name);
		if (bit == 0 || !parse_u32(next_token(&context), &enabled) || enabled > 1 || !no_more_tokens(&context)) {
			command_error(id, "BAD_ARGUMENT");
			return;
		}
		if (enabled) {
			state.steady_serial_state |= bit;
		} else {
			state.steady_serial_state &= (uint16_t) ~bit;
		}
		set_serial_state();
		(void) send_line("%lu OK\n", (unsigned long) id);
		return;
	}
	if (strcmp(command, "PULSE") == 0) {
		const char *name = next_token(&context);
		uint32_t duration_ms;
		uint16_t bit = name == NULL ? 0 : signal_bit(name);
		if (bit == 0 || !parse_u32(next_token(&context), &duration_ms) || duration_ms == 0 ||
			duration_ms > MAX_TIMEOUT_MS || !no_more_tokens(&context)) {
			command_error(id, "BAD_ARGUMENT");
			return;
		}
		state.pulse_serial_state |= bit;
		for (unsigned index = 0; index < 7; ++index) {
			if (bit == (1u << index)) {
				state.pulse_deadlines[index] = deadline_after_ms(duration_ms);
				break;
			}
		}
		set_serial_state();
		(void) send_line("%lu OK\n", (unsigned long) id);
		return;
	}
	if (strcmp(command, "TX") == 0) {
		uint32_t seed;
		uint32_t size;
		uint32_t unused = 0;
		if (!parse_transfer(&context, &seed, &size, &unused, false)) {
			command_error(id, "BAD_ARGUMENT");
			return;
		}
		start_tx(id, seed, size);
		return;
	}
	if (strcmp(command, "RX") == 0) {
		uint32_t seed;
		uint32_t size;
		uint32_t timeout_ms;
		if (!parse_transfer(&context, &seed, &size, &timeout_ms, true)) {
			command_error(id, "BAD_ARGUMENT");
			return;
		}
		start_rx(id, seed, size, timeout_ms);
		return;
	}
	if (strcmp(command, "LOOPBACK") == 0) {
		uint32_t size;
		uint32_t timeout_ms;
		if (!parse_u32(next_token(&context), &size) || !parse_u32(next_token(&context), &timeout_ms) ||
			timeout_ms == 0 || timeout_ms > MAX_TIMEOUT_MS || !no_more_tokens(&context)) {
			command_error(id, "BAD_ARGUMENT");
			return;
		}
		start_loopback(id, size, timeout_ms);
		return;
	}
	if (strcmp(command, "RESET") == 0) {
		if (!no_more_tokens(&context)) {
			command_error(id, "SYNTAX");
			return;
		}
		reset_protocol();
		(void) send_line("%lu OK\n", (unsigned long) id);
		return;
	}
	if (strcmp(command, "PERSONALITY") == 0) {
		const char *name = next_token(&context);
		const char *storage = next_token(&context);
		uint32_t personality_id;
		bool persist = false;
		if (name == NULL || !cdc_test_device_personality_id(name, &personality_id)) {
			command_error(id, "BAD_ARGUMENT");
			return;
		}
		if (storage != NULL) {
			if (strcmp(storage, "PERSIST") == 0) {
				persist = true;
			} else if (strcmp(storage, "VOLATILE") != 0) {
				command_error(id, "BAD_ARGUMENT");
				return;
			}
		}
		if (!no_more_tokens(&context)) {
			command_error(id, "SYNTAX");
			return;
		}
		(void) send_line("%lu OK\n", (unsigned long) id);
		state.personality_pending = true;
		state.personality_persist = persist;
		state.personality_id = personality_id;
		state.personality_deadline_us = deadline_after_ms(100);
		return;
	}
	if (strcmp(command, "BOOTSEL") == 0) {
		if (!no_more_tokens(&context)) {
			command_error(id, "SYNTAX");
			return;
		}
		(void) send_line("%lu OK\n", (unsigned long) id);
		state.bootsel_pending = true;
		state.bootsel_deadline_us = deadline_after_ms(100);
		return;
	}
	command_error(id, "UNKNOWN_COMMAND");
}

static void command_task(void) {
	if (cdc_test_device_write_free() < OUTPUT_RESERVE) {
		return;
	}
	uint8_t byte;
	while (cdc_test_device_read(&byte, 1) == 1) {
		if (byte == '\n') {
			if (state.mode == MODE_SCRIPT_UPLOAD) {
				process_script_line();
			} else {
				process_command();
			}
			state.line_size = 0;
			state.line_overflow = false;
			return;
		}
		if (byte == '\r') {
			continue;
		}
		if (state.line_size + 1 < sizeof(state.line)) {
			state.line[state.line_size++] = (char) byte;
		} else {
			state.line_overflow = true;
		}
	}
}

static void operation_complete(void) {
	state.mode = state.script_running ? MODE_SCRIPT_RUN : MODE_COMMAND;
}

static void script_run_task(void) {
	while (state.script_op_index < state.script_op_count) {
		const script_op_t *op = &state.script_ops[state.script_op_index];
		switch (op->opcode) {
			case SCRIPT_DELAY:
				++state.script_op_index;
				if (op->arg0 != 0) {
					state.deadline_us = deadline_after_ms(op->arg0);
					state.mode = MODE_DELAY;
					return;
				}
				break;
			case SCRIPT_SIGNAL:
				if (op->arg1 != 0) {
					state.steady_serial_state |= (uint16_t) op->arg0;
				} else {
					state.steady_serial_state &= (uint16_t) ~op->arg0;
				}
				set_serial_state();
				++state.script_op_index;
				break;
			case SCRIPT_PULSE:
				state.pulse_serial_state |= (uint16_t) op->arg0;
				for (unsigned index = 0; index < 7; ++index) {
					if (op->arg0 == (1u << index)) {
						state.pulse_deadlines[index] = deadline_after_ms(op->arg1);
						break;
					}
				}
				set_serial_state();
				++state.script_op_index;
				break;
			case SCRIPT_TX_PRNG:
				if (cdc_test_device_write_free() < OUTPUT_RESERVE) {
					return;
				}
				++state.script_op_index;
				start_tx(state.script_id, op->arg0, op->arg1);
				return;
			case SCRIPT_RX_PRNG:
				if (cdc_test_device_write_free() < OUTPUT_RESERVE) {
					return;
				}
				++state.script_op_index;
				start_rx(state.script_id, op->arg0, op->arg1, op->arg2);
				return;
			case SCRIPT_SEND:
				if (cdc_test_device_write_free() < OUTPUT_RESERVE) {
					return;
				}
				state.command_id = state.script_id;
				state.literal_offset = op->data_offset;
				state.remaining = op->data_size;
				state.transferred = 0;
				++state.script_op_index;
				state.mode = MODE_TX_LITERAL;
				(void) send_line("%lu DATA %u\n", (unsigned long) state.script_id, op->data_size);
				return;
			case SCRIPT_RECV:
				if (cdc_test_device_write_free() < OUTPUT_RESERVE) {
					return;
				}
				state.command_id = state.script_id;
				state.literal_offset = op->data_offset;
				state.remaining = op->data_size;
				state.transferred = 0;
				state.timeout_ms = op->arg0;
				state.deadline_us = deadline_after_ms(op->arg0);
				state.mismatch = false;
				++state.script_op_index;
				state.mode = MODE_RX_LITERAL;
				(void) send_line("%lu READY %u\n", (unsigned long) state.script_id, op->data_size);
				return;
			case SCRIPT_LOOPBACK:
				if (cdc_test_device_write_free() < OUTPUT_RESERVE) {
					return;
				}
				++state.script_op_index;
				start_loopback(state.script_id, op->arg0, op->arg1);
				return;
		}
	}
	if (cdc_test_device_write_free() >= OUTPUT_RESERVE && send_line("%lu OK\n", (unsigned long) state.script_id)) {
		state.script_running = false;
		state.mode = MODE_COMMAND;
	}
}

static void delay_task(void) {
	if (time_us_64() >= state.deadline_us) {
		state.mode = MODE_SCRIPT_RUN;
	}
}

static void tx_task(void) {
	uint8_t buffer[TRANSFER_BUFFER_SIZE];
	if (state.remaining == 0) {
		if (state.script_running) {
			operation_complete();
		} else if (cdc_test_device_write_free() >= OUTPUT_RESERVE &&
				   send_line("%lu OK\n", (unsigned long) state.command_id)) {
			operation_complete();
		}
		return;
	}
	size_t count = state.remaining < sizeof(buffer) ? state.remaining : sizeof(buffer);
	size_t free = cdc_test_device_write_free();
	if (count > free) {
		count = free;
	}
	for (size_t i = 0; i < count; ++i) {
		buffer[i] = prng_next(&state.prng);
	}
	count = cdc_test_device_write(buffer, count);
	state.remaining -= (uint32_t) count;
	state.transferred += (uint32_t) count;
}

static void tx_literal_task(void) {
	if (state.remaining == 0) {
		operation_complete();
		return;
	}
	size_t count = state.remaining < TRANSFER_BUFFER_SIZE ? state.remaining : TRANSFER_BUFFER_SIZE;
	size_t free = cdc_test_device_write_free();
	if (count > free) {
		count = free;
	}
	count = cdc_test_device_write(&state.script_data[state.literal_offset + state.transferred], count);
	state.remaining -= (uint32_t) count;
	state.transferred += (uint32_t) count;
}

static void finish_rx(void) {
	if (cdc_test_device_write_free() < OUTPUT_RESERVE) {
		return;
	}
	if (state.mismatch) {
		if (state.script_running) {
			(void) send_line("%lu ERR MISMATCH step=%lu offset=%lu expected=%u actual=%u received=%lu\n",
							 (unsigned long) state.command_id, (unsigned long) state.script_op_index,
							 (unsigned long) state.mismatch_offset, state.mismatch_expected, state.mismatch_actual,
							 (unsigned long) state.transferred);
		} else {
			(void) send_line("%lu ERR MISMATCH offset=%lu expected=%u actual=%u received=%lu\n",
							 (unsigned long) state.command_id, (unsigned long) state.mismatch_offset,
							 state.mismatch_expected, state.mismatch_actual, (unsigned long) state.transferred);
		}
	} else if (!state.script_running) {
		(void) send_line("%lu OK\n", (unsigned long) state.command_id);
	}
	if (state.mismatch) {
		state.script_running = false;
		state.mode = MODE_COMMAND;
	} else {
		operation_complete();
	}
}

static void rx_task(void) {
	uint8_t buffer[TRANSFER_BUFFER_SIZE];
	if (state.remaining == 0) {
		finish_rx();
		return;
	}
	size_t count = state.remaining < sizeof(buffer) ? state.remaining : sizeof(buffer);
	count = cdc_test_device_read(buffer, count);
	if (count != 0) {
		for (size_t i = 0; i < count; ++i) {
			uint8_t expected = prng_next(&state.prng);
			if (!state.mismatch && buffer[i] != expected) {
				state.mismatch = true;
				state.mismatch_offset = state.transferred + (uint32_t) i;
				state.mismatch_expected = expected;
				state.mismatch_actual = buffer[i];
			}
		}
		state.remaining -= (uint32_t) count;
		state.transferred += (uint32_t) count;
		state.deadline_us = deadline_after_ms(state.timeout_ms);
		return;
	}
	if (time_us_64() >= state.deadline_us && cdc_test_device_write_free() >= OUTPUT_RESERVE) {
		if (state.script_running) {
			(void) send_line("%lu ERR TIMEOUT step=%lu received=%lu\n", (unsigned long) state.command_id,
							 (unsigned long) state.script_op_index, (unsigned long) state.transferred);
			state.script_running = false;
		} else {
			(void) send_line("%lu ERR TIMEOUT received=%lu\n", (unsigned long) state.command_id,
							 (unsigned long) state.transferred);
		}
		state.mode = MODE_COMMAND;
	}
}

static void rx_literal_task(void) {
	uint8_t buffer[TRANSFER_BUFFER_SIZE];
	if (state.remaining == 0) {
		finish_rx();
		return;
	}
	size_t count = state.remaining < sizeof(buffer) ? state.remaining : sizeof(buffer);
	count = cdc_test_device_read(buffer, count);
	if (count != 0) {
		for (size_t i = 0; i < count; ++i) {
			uint8_t expected = state.script_data[state.literal_offset + state.transferred + i];
			if (!state.mismatch && buffer[i] != expected) {
				state.mismatch = true;
				state.mismatch_offset = state.transferred + (uint32_t) i;
				state.mismatch_expected = expected;
				state.mismatch_actual = buffer[i];
			}
		}
		state.remaining -= (uint32_t) count;
		state.transferred += (uint32_t) count;
		state.deadline_us = deadline_after_ms(state.timeout_ms);
		return;
	}
	if (time_us_64() >= state.deadline_us && cdc_test_device_write_free() >= OUTPUT_RESERVE) {
		(void) send_line("%lu ERR TIMEOUT step=%lu received=%lu\n", (unsigned long) state.command_id,
						 (unsigned long) state.script_op_index, (unsigned long) state.transferred);
		state.script_running = false;
		state.mode = MODE_COMMAND;
	}
}

static void loopback_task(void) {
	uint8_t buffer[TRANSFER_BUFFER_SIZE];
	if (state.remaining == 0) {
		if (state.script_running) {
			operation_complete();
		} else if (cdc_test_device_write_free() >= OUTPUT_RESERVE &&
				   send_line("%lu OK\n", (unsigned long) state.command_id)) {
			operation_complete();
		}
		return;
	}
	size_t count = state.remaining < sizeof(buffer) ? state.remaining : sizeof(buffer);
	size_t free = cdc_test_device_write_free();
	if (count > free) {
		count = free;
	}
	count = cdc_test_device_read(buffer, count);
	if (count != 0) {
		(void) cdc_test_device_write(buffer, count);
		state.remaining -= (uint32_t) count;
		state.transferred += (uint32_t) count;
		state.deadline_us = deadline_after_ms(state.timeout_ms);
		return;
	}
	if (time_us_64() >= state.deadline_us && free >= OUTPUT_RESERVE) {
		if (state.script_running) {
			(void) send_line("%lu ERR TIMEOUT step=%lu received=%lu\n", (unsigned long) state.command_id,
							 (unsigned long) state.script_op_index, (unsigned long) state.transferred);
			state.script_running = false;
		} else {
			(void) send_line("%lu ERR TIMEOUT received=%lu\n", (unsigned long) state.command_id,
							 (unsigned long) state.transferred);
		}
		state.mode = MODE_COMMAND;
	}
}

static const char *usb_event_name(cdc_test_event_type_t type) {
	switch (type) {
		case CDC_TEST_EVENT_USB_RESET:
			return "RESET";
		case CDC_TEST_EVENT_USB_CONFIGURED:
			return "CONFIGURED";
		case CDC_TEST_EVENT_USB_SUSPEND:
			return "SUSPEND";
		case CDC_TEST_EVENT_USB_RESUME:
			return "RESUME";
		default:
			return "UNKNOWN";
	}
}

static void update_event_state(const cdc_test_event_t *event) {
	if (event->type == CDC_TEST_EVENT_LINE_CODING) {
		state.baud = event->baud;
		state.data_bits = event->data_bits;
		state.parity = event->parity;
		state.stop_bits = event->stop_bits;
	} else if (event->type == CDC_TEST_EVENT_CONTROL_LINES) {
		state.dtr = event->dtr;
		state.rts = event->rts;
	} else if (event->type == CDC_TEST_EVENT_HOST_BREAK) {
		state.host_break = event->value;
	}
}

static void event_task(void) {
	if (!state.event_pending && cdc_test_device_take_event(&state.event)) {
		state.event_pending = true;
		update_event_state(&state.event);
		if (state.event.type == CDC_TEST_EVENT_USB_RESET) {
			reset_protocol();
			state.event_pending = true;
		}
	}
	if (!state.event_pending || state.mode != MODE_COMMAND || cdc_test_device_write_free() < OUTPUT_RESERVE) {
		return;
	}

	bool sent;
	switch (state.event.type) {
		case CDC_TEST_EVENT_LINE_CODING:
			sent = send_line("EVENT LINE baud=%lu data=%u parity=%u stop=%u\n", (unsigned long) state.event.baud,
							 state.event.data_bits, state.event.parity, state.event.stop_bits);
			break;
		case CDC_TEST_EVENT_CONTROL_LINES:
			sent = send_line("EVENT CONTROL dtr=%u rts=%u\n", state.event.dtr, state.event.rts);
			break;
		case CDC_TEST_EVENT_HOST_BREAK:
			sent = send_line("EVENT BREAK value=%u\n", state.event.value);
			break;
		case CDC_TEST_EVENT_OVERFLOW:
			sent = send_line("EVENT OVERFLOW\n");
			break;
		default:
			sent = send_line("EVENT USB state=%s\n", usb_event_name(state.event.type));
			break;
	}
	if (sent) {
		state.event_pending = false;
	}
}

void cdc_test_protocol_init(void) {
	memset(&state, 0, sizeof(state));
	reset_host_state();
	reset_protocol();
}

void cdc_test_protocol_task(void) {
	uint32_t reset_generation = cdc_test_device_reset_generation();
	if (reset_generation != state.reset_generation) {
		state.reset_generation = reset_generation;
		state.event_pending = false;
		reset_host_state();
		reset_protocol();
	}
	uint64_t now = time_us_64();
	uint16_t expired_pulses = 0;
	for (unsigned index = 0; index < 7; ++index) {
		if ((state.pulse_serial_state & (1u << index)) != 0 && now >= state.pulse_deadlines[index]) {
			expired_pulses |= 1u << index;
		}
	}
	if (expired_pulses != 0) {
		state.pulse_serial_state &= (uint16_t) ~expired_pulses;
		set_serial_state();
	}
	if (state.bootsel_pending && now >= state.bootsel_deadline_us && cdc_test_device_tx_idle()) {
		reset_usb_boot(0, 0);
	}
	if (state.personality_pending && now >= state.personality_deadline_us && cdc_test_device_tx_idle()) {
		uint32_t personality_id = state.personality_id;
		bool persist = state.personality_persist;
		state.personality_pending = false;
		cdc_test_device_select_personality(personality_id, persist);
	}

	event_task();
	switch (state.mode) {
		case MODE_COMMAND:
			if (!state.event_pending && !state.bootsel_pending && !state.personality_pending) {
				command_task();
			}
			break;
		case MODE_SCRIPT_UPLOAD:
			command_task();
			break;
		case MODE_SCRIPT_RUN:
			script_run_task();
			break;
		case MODE_DELAY:
			delay_task();
			break;
		case MODE_TX_PRNG:
			tx_task();
			break;
		case MODE_RX_PRNG:
			rx_task();
			break;
		case MODE_TX_LITERAL:
			tx_literal_task();
			break;
		case MODE_RX_LITERAL:
			rx_literal_task();
			break;
		case MODE_LOOPBACK:
			loopback_task();
			break;
	}
}
