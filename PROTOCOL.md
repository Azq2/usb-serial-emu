# CDC test protocol

## Format

- ASCII, `\n` or `\r\n` line endings, case-sensitive names.
- Command: `<id> <command> [arguments]`; `id` is a client-assigned `uint32`.
- Response: `<id> OK ...`, `<id> ERR <code> ...`, `<id> DATA ...`, or `<id> READY ...`.
- Event: `EVENT <type> ...`.
- Numbers: decimal or `0x`-prefixed hexadecimal.
- Maximum command length: 191 bytes without the line ending.
- Unknown `name=value` fields must be ignored.

## Commands

| Command | Result |
|---|---|
| `<id> INFO` | Version and PRNG. |
| `<id> PERSONALITIES` | Personality list. |
| `<id> PERSONALITY <name> [VOLATILE\|PERSIST]` | Select and reboot. Default: `VOLATILE`. |
| `<id> STATE` | Line coding, DTR/RTS, serial state, host BREAK. |
| `<id> SIGNAL <signal> <0\|1>` | Set a serial-state bit. |
| `<id> PULSE <signal> <ms>` | Pulse a serial-state bit. |
| `<id> TX <seed> <size>` | Device sends PRNG data. |
| `<id> RX <seed> <size> <timeout_ms>` | Device verifies PRNG data. |
| `<id> LOOPBACK <size> <timeout_ms>` | Echo raw data. |
| `<id> RESET` | Clear generated signals. |
| `<id> BOOTSEL` | Enter the RP2040 bootloader. |

Signals: `DCD`, `DSR`, `BREAK`, `RING`, `FRAMING`, `PARITY`, `OVERRUN`. CDC ACM cannot generate CTS; Linux reports it as asserted.

`seed` must be nonzero. `timeout_ms`: 1–600000; restarted after each received block. `PERSIST` writes the selection to flash. Other personalities run loopback.

## Raw transfers

Use byte counts, not USB packets or WebSerial chunks.

```text
7 TX 0x12345678 1048576
7 DATA 1048576
<1048576 bytes from device>
7 OK

8 RX 0x12345678 1048576 5000
8 READY 1048576
<1048576 bytes to device>
8 OK

9 LOOPBACK 1048576 5000
9 READY 1048576
<write and read 1048576 bytes concurrently>
9 OK
```

RX finishes the block before reporting a mismatch:

```text
8 ERR MISMATCH offset=123 expected=45 actual=67 received=1048576
```

Events are held until transfer completion. USB reset aborts the transfer. After a timeout, the next byte starts a command; reset USB if the host may still send the old block.

## PRNG

xorshift32 updates the state, then emits the word little-endian:

```js
function* xorshift32(seed) {
  let state = seed >>> 0;
  if (state === 0) throw new RangeError("seed must be nonzero");

  while (true) {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    state >>>= 0;
    yield state & 0xff;
    yield (state >>> 8) & 0xff;
    yield (state >>> 16) & 0xff;
    yield state >>> 24;
  }
}
```

| Seed | First 16 bytes |
|---|---|
| `0x00000001` | `2120040001060804c5a8cc9d4f995512` |
| `0x12345678` | `a55a9887a3245b15c4f4204898acb381` |

## Events

| Event | Fields |
|---|---|
| `EVENT LINE` | `baud`, `data`, `parity`, `stop` |
| `EVENT CONTROL` | `dtr`, `rts` |
| `EVENT BREAK` | Host `SEND_BREAK` value |
| `EVENT USB` | `state=RESET\|CONFIGURED\|SUSPEND\|RESUME` |
| `EVENT OVERFLOW` | Queue overflow; use `STATE` to resync. |

Parity: 0 none, 1 odd, 2 even, 3 mark, 4 space. Stop bits: 0 one, 1 one-and-a-half, 2 two. BREAK: 0 off, 65535 indefinite, otherwise milliseconds.

## Errors

`SYNTAX`, `BAD_ARGUMENT`, `UNKNOWN_COMMAND`, `LINE_TOO_LONG`, `TIMEOUT`, `MISMATCH`.
