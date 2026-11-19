// 16550 UART on COM1: the kernel console and the test/bench output channel.
#pragma once

#include <stddef.h>

namespace serial {

void init();
void write(const char* s, size_t n);
bool can_read();
char read_byte();  // only call when can_read()
void enable_rx_interrupt();

}  // namespace serial
