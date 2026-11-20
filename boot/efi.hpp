// Minimal UEFI 2.x definitions for the loader. Written from the UEFI
// specification; only the tables, protocols and fields the loader touches are
// typed, everything else is a placeholder pointer so offsets stay correct.
//
// The loader is compiled for x86_64-unknown-windows, so every function pointer
// here already uses the Microsoft x64 calling convention UEFI requires.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace efi {

using UINTN = uint64_t;
using Status = UINTN;
using Handle = void*;
using Char16 = char16_t;
using PhysAddr = uint64_t;

constexpr Status kSuccess = 0;
constexpr Status kErrorBit = 1ull << 63;
constexpr Status kInvalidParameter = kErrorBit | 2;
constexpr Status kBufferTooSmall = kErrorBit | 5;
constexpr Status kNotFound = kErrorBit | 14;

struct Guid {
  uint32_t d1;
  uint16_t d2;
  uint16_t d3;
  uint8_t d4[8];
};

inline bool operator==(const Guid& a, const Guid& b) {
  if (a.d1 != b.d1 || a.d2 != b.d2 || a.d3 != b.d3) return false;
  for (int i = 0; i < 8; i++)
    if (a.d4[i] != b.d4[i]) return false;
  return true;
}

struct TableHeader {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;
  uint32_t crc32;
  uint32_t reserved;
};

enum MemoryType : uint32_t {
  kReservedMemoryType = 0,
  kLoaderCode = 1,
  kLoaderData = 2,
  kBootServicesCode = 3,
  kBootServicesData = 4,
  kRuntimeServicesCode = 5,
  kRuntimeServicesData = 6,
  kConventionalMemory = 7,
  kUnusableMemory = 8,
  kAcpiReclaimMemory = 9,
  kAcpiMemoryNvs = 10,
  kMemoryMappedIo = 11,
  kMemoryMappedIoPortSpace = 12,
  kPalCode = 13,
  kPersistentMemory = 14,
};

enum AllocateType : uint32_t { kAllocateAnyPages = 0, kAllocateMaxAddress = 1, kAllocateAddress = 2 };

struct MemoryDescriptor {
  uint32_t type;
  uint32_t pad;
  PhysAddr physical_start;
  uint64_t virtual_start;
  uint64_t number_of_pages;
  uint64_t attribute;
};

struct SimpleTextOutput {
  Status (*reset)(SimpleTextOutput*, uint8_t extended);
  Status (*output_string)(SimpleTextOutput*, const Char16*);
  void* test_string;
  void* query_mode;
  void* set_mode;
  void* set_attribute;
  Status (*clear_screen)(SimpleTextOutput*);
  void* set_cursor_position;
  void* enable_cursor;
  void* mode;
};

struct BootServices {
  TableHeader hdr;
  void* raise_tpl;
  void* restore_tpl;
  Status (*allocate_pages)(uint32_t type, uint32_t memory_type, UINTN pages, PhysAddr* memory);
  Status (*free_pages)(PhysAddr memory, UINTN pages);
  Status (*get_memory_map)(UINTN* size, MemoryDescriptor* map, UINTN* map_key, UINTN* descriptor_size,
                           uint32_t* descriptor_version);
  Status (*allocate_pool)(uint32_t pool_type, UINTN size, void** buffer);
  Status (*free_pool)(void* buffer);
  void* create_event;
  void* set_timer;
  void* wait_for_event;
  void* signal_event;
  void* close_event;
  void* check_event;
  void* install_protocol_interface;
  void* reinstall_protocol_interface;
  void* uninstall_protocol_interface;
  Status (*handle_protocol)(Handle handle, const Guid* protocol, void** interface);
  void* reserved;
  void* register_protocol_notify;
  void* locate_handle;
  void* locate_device_path;
  void* install_configuration_table;
  void* load_image;
  void* start_image;
  void* exit;
  void* unload_image;
  Status (*exit_boot_services)(Handle image, UINTN map_key);
  void* get_next_monotonic_count;
  Status (*stall)(UINTN microseconds);
  Status (*set_watchdog_timer)(UINTN timeout, uint64_t code, UINTN data_size, Char16* data);
  void* connect_controller;
  void* disconnect_controller;
  void* open_protocol;
  void* close_protocol;
  void* open_protocol_information;
  void* protocols_per_handle;
  void* locate_handle_buffer;
  Status (*locate_protocol)(const Guid* protocol, void* registration, void** interface);
  void* install_multiple_protocol_interfaces;
  void* uninstall_multiple_protocol_interfaces;
  void* calculate_crc32;
  void* copy_mem;
  void* set_mem;
  void* create_event_ex;
};

struct ConfigurationTable {
  Guid vendor_guid;
  void* vendor_table;
};

struct SystemTable {
  TableHeader hdr;
  Char16* firmware_vendor;
  uint32_t firmware_revision;
  Handle console_in_handle;
  void* con_in;
  Handle console_out_handle;
  SimpleTextOutput* con_out;
  Handle standard_error_handle;
  SimpleTextOutput* std_err;
  void* runtime_services;
  BootServices* boot_services;
  UINTN number_of_table_entries;
  ConfigurationTable* configuration_table;
};

struct LoadedImage {
  uint32_t revision;
  Handle parent_handle;
  SystemTable* system_table;
  Handle device_handle;
  void* file_path;
  void* reserved;
  uint32_t load_options_size;
  void* load_options;
  void* image_base;
  uint64_t image_size;
  uint32_t image_code_type;
  uint32_t image_data_type;
  void* unload;
};

struct File {
  uint64_t revision;
  Status (*open)(File* self, File** new_handle, const Char16* file_name, uint64_t open_mode,
                 uint64_t attributes);
  Status (*close)(File* self);
  void* del;
  Status (*read)(File* self, UINTN* buffer_size, void* buffer);
  void* write;
  Status (*get_position)(File* self, uint64_t* position);
  Status (*set_position)(File* self, uint64_t position);
  void* get_info;
  void* set_info;
  void* flush;
};

constexpr uint64_t kFileModeRead = 1;

struct SimpleFileSystem {
  uint64_t revision;
  Status (*open_volume)(SimpleFileSystem* self, File** root);
};

struct PixelBitmask {
  uint32_t red, green, blue, reserved;
};

enum GopPixelFormat : uint32_t { kPixelRgbx8 = 0, kPixelBgrx8 = 1, kPixelBitMask = 2, kPixelBltOnly = 3 };

struct GopModeInfo {
  uint32_t version;
  uint32_t horizontal_resolution;
  uint32_t vertical_resolution;
  uint32_t pixel_format;
  PixelBitmask pixel_information;
  uint32_t pixels_per_scan_line;
};

struct GopMode {
  uint32_t max_mode;
  uint32_t mode;
  GopModeInfo* info;
  UINTN size_of_info;
  PhysAddr frame_buffer_base;
  UINTN frame_buffer_size;
};

struct GraphicsOutput {
  void* query_mode;
  void* set_mode;
  void* blt;
  GopMode* mode;
};

constexpr Guid kLoadedImageGuid = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
constexpr Guid kSimpleFileSystemGuid = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
constexpr Guid kGraphicsOutputGuid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
constexpr Guid kAcpi20TableGuid = {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
constexpr Guid kAcpi10TableGuid = {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

}  // namespace efi
