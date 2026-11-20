// File-system on-disk format (version 1). All integers little endian, 4 KiB blocks.
//
//   block 0, 1        superblock slots A and B (written alternately; the valid
//                     one with the higher sb_seq wins at mount)
//   journal           circular log of metadata transactions
//   inode bitmap      1 bit per inode
//   block bitmap      1 bit per block (bits for metadata regions are set)
//   inode table       128-byte inodes, 32 per block; inode 0 unused, 1 = root
//   data              file data, directory blocks, indirect blocks
//
// Journal transaction = one or more [descriptor + the metadata blocks it
// lists] groups, then a commit block whose CRC32C covers every descriptor
// and data block of the transaction. Recovery starts at the superblock's
// journal tail and replays consecutive transactions (seq, seq+1, ...) until
// one is missing or fails its checksum.
#pragma once

#include <stdint.h>

namespace os::fs {

constexpr uint32_t kBlockSize = 4096;
constexpr uint64_t kMagic = 0x313053464c4e4a21ull;  // "!JNLFS01"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kRootInode = 1;
constexpr uint32_t kInodeSize = 128;
constexpr uint32_t kInodesPerBlock = kBlockSize / kInodeSize;
constexpr uint32_t kDirectBlocks = 12;
constexpr uint32_t kPointersPerBlock = kBlockSize / 4;
constexpr uint32_t kNameMax = 57;  // plus NUL fits the 58-byte field
constexpr uint64_t kMaxFileBlocks =
    kDirectBlocks + kPointersPerBlock + static_cast<uint64_t>(kPointersPerBlock) * kPointersPerBlock;

struct Superblock {
  uint64_t magic;
  uint32_t version;
  uint32_t block_size;
  uint64_t sb_seq;  // bumps on every superblock write
  uint64_t uuid;    // ties journal blocks to this file system instance
  uint32_t total_blocks;
  uint32_t inode_count;
  uint32_t journal_start, journal_blocks;
  uint32_t inode_bitmap_start, inode_bitmap_blocks;
  uint32_t block_bitmap_start, block_bitmap_blocks;
  uint32_t inode_table_start, inode_table_blocks;
  uint32_t data_start;
  uint32_t root_inode;
  uint64_t journal_tail_seq;    // first transaction recovery must consider
  uint32_t journal_tail_block;  // its position, relative to journal_start
  uint32_t clean;               // 1 if unmounted cleanly
  uint32_t journaling;          // 0 = journal disabled (the crash-test control)
  uint32_t crc;                 // CRC32C of the struct with crc = 0
};
static_assert(sizeof(Superblock) <= kBlockSize, "superblock fits a block");

enum class InodeType : uint16_t { kFree = 0, kFile = 1, kDir = 2 };

struct Inode {
  InodeType type;
  uint16_t links;
  uint32_t parent;  // directories: parent directory inode
  uint64_t size;
  uint64_t mtime_ns;
  uint64_t ctime_ns;
  uint32_t blocks;  // allocated data + indirect blocks
  uint32_t generation;
  uint32_t direct[kDirectBlocks];
  uint32_t indirect;
  uint32_t dindirect;
  uint8_t reserved[32];
};
static_assert(sizeof(Inode) == kInodeSize, "inode layout");

struct DirEnt {
  uint32_t inode;  // 0 = free slot
  uint8_t type;    // InodeType
  uint8_t name_len;
  char name[58];
};
static_assert(sizeof(DirEnt) == 64, "dirent layout");
constexpr uint32_t kDirEntsPerBlock = kBlockSize / sizeof(DirEnt);

constexpr uint32_t kJournalMagic = 0x4c4e524a;  // "JRNL"
enum class JournalBlockType : uint32_t { kDescriptor = 1, kCommit = 2 };

struct JournalHeader {
  uint32_t magic;
  JournalBlockType type;
  uint64_t seq;
  uint64_t uuid;
};

struct JournalDescriptor {
  JournalHeader h;
  uint32_t count;  // metadata blocks following this descriptor
  uint32_t targets[(kBlockSize - sizeof(JournalHeader) - 4) / 4];
};
static_assert(sizeof(JournalDescriptor) <= kBlockSize, "descriptor fits a block");
constexpr uint32_t kTargetsPerDescriptor = (kBlockSize - sizeof(JournalHeader) - 4) / 4;

struct JournalCommit {
  JournalHeader h;
  uint32_t total_blocks;  // descriptors + metadata blocks in this transaction
  uint32_t data_crc;      // CRC32C over those blocks in log order
  uint32_t crc;           // CRC32C of this struct with crc = 0
};

}  // namespace os::fs
