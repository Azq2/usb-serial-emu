#pragma once

#include <stddef.h>
#include <stdint.h>

enum { BYTE_FIFO_CAPACITY = 1024 };

typedef struct {
	uint8_t data[BYTE_FIFO_CAPACITY];
	size_t head;
	size_t tail;
	size_t count;
} byte_fifo_t;

static inline void fifo_clear(byte_fifo_t *fifo) {
	fifo->head = 0;
	fifo->tail = 0;
	fifo->count = 0;
}

static inline size_t fifo_free(const byte_fifo_t *fifo) {
	return BYTE_FIFO_CAPACITY - fifo->count;
}

static inline size_t fifo_write(byte_fifo_t *fifo, const uint8_t *data, size_t size) {
	size_t count = 0;
	while (count < size && fifo->count < BYTE_FIFO_CAPACITY) {
		fifo->data[fifo->head] = data[count++];
		fifo->head = (fifo->head + 1) % BYTE_FIFO_CAPACITY;
		++fifo->count;
	}
	return count;
}

static inline size_t fifo_read(byte_fifo_t *fifo, uint8_t *data, size_t size) {
	size_t count = 0;
	while (count < size && fifo->count != 0) {
		data[count++] = fifo->data[fifo->tail];
		fifo->tail = (fifo->tail + 1) % BYTE_FIFO_CAPACITY;
		--fifo->count;
	}
	return count;
}

static inline size_t fifo_peek(const byte_fifo_t *fifo, uint8_t *data, size_t size) {
	size_t count = 0;
	size_t offset = fifo->tail;
	while (count < size && count < fifo->count) {
		data[count++] = fifo->data[offset];
		offset = (offset + 1) % BYTE_FIFO_CAPACITY;
	}
	return count;
}

static inline void fifo_drop(byte_fifo_t *fifo, size_t size) {
	if (size > fifo->count) {
		size = fifo->count;
	}
	fifo->tail = (fifo->tail + size) % BYTE_FIFO_CAPACITY;
	fifo->count -= size;
}
