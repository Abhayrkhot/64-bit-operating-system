// The console device behind fds 0-2: writes go to the serial port, reads
// block on a ring buffer filled by the UART receive interrupt (IRQ 4).
#pragma once

#include "fs/file.hpp"

namespace console {

void init();
File* open();

}  // namespace console
