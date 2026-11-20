// QEMU fw_cfg (I/O port interface). Used to pass the kernel command line from
// the test runner without rebuilding the ESP image:
//   qemu ... -fw_cfg name=opt/os/cmdline,string="test=all"
#pragma once

#include <stddef.h>

namespace fw_cfg {

bool present();
// Copies file `name` into buf (NUL-terminated, truncated). Returns its size or -1.
long read_file(const char* name, char* buf, size_t size);

}  // namespace fw_cfg
