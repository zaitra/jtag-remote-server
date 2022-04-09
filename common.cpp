#include "common.h"
#include <assert.h>
#include <stdarg.h>

JtagState next_state(JtagState cur, int bit) {
  switch (cur) {
  case TestLogicReset:
    return bit ? TestLogicReset : RunTestIdle;
  case RunTestIdle:
    return bit ? SelectDRScan : RunTestIdle;
  case SelectDRScan:
    return bit ? SelectIRScan : CaptureDR;
  case CaptureDR:
    return bit ? Exit1DR : ShiftDR;
  case ShiftDR:
    return bit ? Exit1DR : ShiftDR;
  case Exit1DR:
    return bit ? UpdateDR : PauseDR;
  case PauseDR:
    return bit ? Exit2DR : PauseDR;
  case Exit2DR:
    return bit ? UpdateDR : ShiftDR;
  case UpdateDR:
    return bit ? SelectDRScan : RunTestIdle;
  case SelectIRScan:
    return bit ? TestLogicReset : CaptureIR;
  case CaptureIR:
    return bit ? Exit1IR : ShiftIR;
  case ShiftIR:
    return bit ? Exit1IR : ShiftIR;
  case Exit1IR:
    return bit ? UpdateIR : PauseIR;
  case PauseIR:
    return bit ? Exit2IR : PauseIR;
  case Exit2IR:
    return bit ? UpdateIR : ShiftIR;
  case UpdateIR:
    return bit ? SelectDRScan : RunTestIdle;
  default:
    assert(false);
  }
  return TestLogicReset;
}

const char *state_to_string(JtagState state) {
  switch (state) {
  case TestLogicReset:
    return "TestLogicReset";
  case RunTestIdle:
    return "RunTestIdle";
  case SelectDRScan:
    return "SelectDRScan";
  case CaptureDR:
    return "CaptureDR";
  case ShiftDR:
    return "ShiftDR";
  case Exit1DR:
    return "Exit1DR";
  case PauseDR:
    return "PauseDR";
  case Exit2DR:
    return "Exit2DR";
  case UpdateDR:
    return "UpdateDR";
  case SelectIRScan:
    return "SelectIRScan";
  case CaptureIR:
    return "CaptureIR";
  case ShiftIR:
    return "ShiftIR";
  case Exit1IR:
    return "Exit1IR";
  case PauseIR:
    return "PauseIR";
  case Exit2IR:
    return "Exit2IR";
  case UpdateIR:
    return "UpdateIR";
  default:
    assert(false);
  }
}

bool mpsse_init() {
  // reset mpsse and enable
  ftdi_set_bitmode(ftdi, 0, 0);
  ftdi_set_bitmode(ftdi, 0, BITMODE_MPSSE);

  uint8_t setup[256] = {SET_BITS_LOW,  0x88, 0x8b, TCK_DIVISOR,   0x01, 0x00,
                        SET_BITS_HIGH, 0,    0,    SEND_IMMEDIATE};
  if (ftdi_write_data(ftdi, setup, 10) != 10) {
    printf("error: %s\n", ftdi_get_error_string(ftdi));
    return false;
  }

  return true;
}

bool jtag_fsm_reset() {
  // 11111: Goto Test-Logic-Reset
  uint8_t tms[] = {0x1F};
  return jtag_tms_seq(tms, 5);
}

bool jtag_tms_seq(const uint8_t *data, size_t num_bits) {
  dprintf("Sending TMS Seq ");
  print_bitvec(data, num_bits);
  dprintf("\n");

  // compute state transition
  JtagState new_state = state;
  for (size_t i = 0; i < num_bits; i++) {
    uint8_t bit = (data[i / 8] >> (i % 8)) & 1;
    new_state = next_state(new_state, bit);
  }
  dprintf("JTAG state: %s -> %s\n", state_to_string(state),
          state_to_string(new_state));
  state = new_state;

  for (size_t i = 0; i < (num_bits + 7) / 8; i++) {
    uint8_t cur_bits = std::min((size_t)8, num_bits - i * 8);

    // Clock Data to TMS pin (no read)
    uint8_t idle[256] = {MPSSE_WRITE_TMS | MPSSE_LSB | MPSSE_BITMODE |
                             MPSSE_WRITE_NEG,
                         // length in bits -1
                         (uint8_t)(cur_bits - 1),
                         // data
                         data[i]};
    if (ftdi_write_data(ftdi, idle, 3) != 3) {
      printf("error: %s\n", ftdi_get_error_string(ftdi));
      return false;
    }
  }

  return true;
}

void print_bitvec(const uint8_t *data, size_t bits) {
  for (size_t i = 0; i < bits; i++) {
    int off = i % 8;
    int bit = ((data[i / 8]) >> off) & 1;
    dprintf("%c", bit ? '1' : '0');
  }
  dprintf("(0x");
  int bytes = (bits + 7) / 8;
  for (int i = bytes - 1; i >= 0; i--) {
    dprintf("%02X", data[i]);
  }
  dprintf(")");
}

bool jtag_scan_chain(const uint8_t *data, uint8_t *recv, size_t num_bits,
                     bool flip_tms) {
  dprintf("Write TDI%s %d bits: ", flip_tms ? "+TMS" : "", num_bits);
  print_bitvec(data, num_bits);
  dprintf("\n");

  size_t bulk_bits = num_bits;
  if (flip_tms) {
    // last bit should be sent along TMS 0->1
    bulk_bits -= 1;
  }

  // send whole bytes first
  size_t length_in_bytes = bulk_bits / 8;
  if (length_in_bytes) {
    std::vector<uint8_t> buffer;
    buffer.push_back(MPSSE_DO_READ | MPSSE_DO_WRITE | MPSSE_LSB |
                     MPSSE_WRITE_NEG);
    // length in bytes -1 lo
    buffer.push_back((length_in_bytes - 1) & 0xff);
    // length in bytes -1 hi
    buffer.push_back((length_in_bytes - 1) >> 8);
    // data
    buffer.insert(buffer.end(), &data[0], &data[length_in_bytes]);
    assert(buffer.size() == 3 + length_in_bytes);

    if (ftdi_write_data(ftdi, buffer.data(), buffer.size()) != buffer.size()) {
      printf("error: %s\n", ftdi_get_error_string(ftdi));
      return false;
    }
  }

  // sent rest bits
  if (bulk_bits % 8) {
    uint8_t buf[256] = {
        MPSSE_DO_READ | MPSSE_DO_WRITE | MPSSE_LSB | MPSSE_WRITE_NEG |
            MPSSE_BITMODE,
        // length in bits -1
        (uint8_t)((bulk_bits % 8) - 1),
        // data
        data[length_in_bytes],
    };
    if (ftdi_write_data(ftdi, buf, 3) != 3) {
      printf("error: %s\n", ftdi_get_error_string(ftdi));
      return false;
    }
  }

  if (flip_tms) {
    // send last bit along TMS=1
    JtagState new_state = next_state(state, 1);
    dprintf("JTAG state: %s -> %s\n", state_to_string(state),
            state_to_string(new_state));
    state = new_state;

    uint8_t bit = (data[num_bits / 8] >> ((num_bits - 1) % 8)) & 1;
    uint8_t buf[3] = {MPSSE_DO_READ | MPSSE_WRITE_TMS | MPSSE_LSB |
                          MPSSE_BITMODE | MPSSE_WRITE_NEG,
                      // length in bits -1
                      0x00,
                      // data
                      // 7-th bit: last bit
                      // TMS=1
                      (uint8_t)(0x01 | (bit << 7))};
    if (ftdi_write_data(ftdi, buf, 3) != 3) {
      printf("error: %s\n", ftdi_get_error_string(ftdi));
      return false;
    }
  }

  // read bulk
  size_t len = (bulk_bits + 7) / 8;
  size_t offset = 0;
  while (len > offset) {
    int read = ftdi_read_data(ftdi, &recv[offset], len - offset);
    if (read < 0) {
      printf("error: %s\n", ftdi_get_error_string(ftdi));
      return false;
    }
    offset += read;
  }

  // handle last bit when TMS=1
  if (flip_tms) {
    uint8_t last_bit;
    while (ftdi_read_data(ftdi, &last_bit, 1) != 1)
      ;

    recv[num_bits / 8] |= last_bit << ((num_bits - 1) % 8);
  }

  dprintf("Read TDO: ");
  print_bitvec(recv, num_bits);
  dprintf("\n");

  return true;
}

bool write_full(int fd, const uint8_t *data, size_t count) {
  size_t num_sent = 0;
  while (num_sent < count) {
    ssize_t res = write(fd, &data[num_sent], count - num_sent);
    if (res > 0) {
      num_sent += res;
    } else if (count < 0) {
      return false;
    }
  }

  return true;
}

void dprintf(const char *fmt, ...) {
  va_list va_args;
  va_start(va_args, fmt);
  if (debug) {
    vprintf(fmt, va_args);
  }
  va_end(va_args);
}