// crashsim: systematic crash-consistency testing of ShaleFS (in the spirit of
// CrashMonkey/ALICE).
//
// 1. Run a seeded workload (writes, appends, overwrites, truncates, unlinks,
//    renames, mkdir/rmdir, fsync, sync, and write-tmp/fsync/rename atomic
//    replaces) on a device that records every block write and flush.
// 2. Generate crash states. Writes before the last completed flush are
//    durable; any subset of the writes issued since may be lost or
//    reordered; optionally one of them is torn at 512-byte granularity.
// 3. For each state: mount (journal recovery), unmount, run fsck, mount
//    again and check the durability oracle:
//      * a file fsynced (or synced) and not modified afterwards must have
//        exactly its fsynced contents;
//      * an atomically replaced file must hold one complete version, no
//        older than the last replace that was fsynced.
//
//   crashsim [--seed N] [--ops N] [--states N] [--torn] [--nojournal]
//            [--trust-commits] [--blocks N] [--quiet]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/blockdevs.hpp"
#include "fs/fsck.hpp"
#include "fs/shalefs.hpp"
#include "util/random.hpp"

using shale::fs::FsckReport;
using shale::fs::InodeInfo;
using shale::fs::ShaleFs;

namespace {

struct Config {
  uint64_t seed = 1;
  int ops = 1500;
  int states = 5000;
  bool torn = false;
  bool journaling = true;
  bool trust_commits = false;
  uint32_t blocks = 8192;  // 32 MiB
  bool quiet = false;
};

struct DurablePoint {
  std::string path;
  std::string content;
  size_t flush_index;  // events index of the flush that made it durable
  size_t after;        // events position when the fsync returned
};
struct ModEvent {
  std::string path;
  size_t pos;  // events position when the operation started
};
struct Version {
  std::string content;
  size_t rename_start;
  size_t durable_flush;  // flush index of the fsync after the rename (SIZE_MAX: none)
};

struct Recorded {
  std::vector<DurablePoint> durable;
  std::vector<ModEvent> mods;
  std::map<std::string, std::vector<Version>> replaced;
};

std::string make_content(shale::Random& rng, size_t n, uint64_t tag) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; i++) s[i] = static_cast<char>((rng.next() & 0xFF) ^ (tag & 0xFF));
  if (n >= 8) std::memcpy(&s[0], &tag, 8);  // versions are distinguishable
  return s;
}

size_t last_flush(const host::RecordingDevice& rec) {
  const auto& ev = rec.events();
  for (size_t i = ev.size(); i-- > 0;)
    if (ev[i].flush) return i;
  return SIZE_MAX;
}

int write_whole(ShaleFs& fs, const std::string& path, const std::string& data) {
  uint32_t ino;
  int r = fs.create(path.c_str(), false, &ino);
  if (r < 0) return r;
  if ((r = fs.truncate(ino, 0)) < 0) return r;
  return fs.write(ino, 0, data.data(), data.size()) == static_cast<int64_t>(data.size()) ? 0 : -1;
}

ShaleFs::Options mount_options(const Config& c, uint32_t cache) {
  ShaleFs::Options o;
  o.cache_blocks = cache;
  o.max_txn_blocks = 64;
  o.trust_commit_blocks = c.trust_commits;
  return o;
}

// Runs the workload, filling `rec` with the write log and `out` with the oracle.
void run_workload(const Config& c, host::RecordingDevice* rec, Recorded* out) {
  ShaleFs fs;
  if (fs.mount(rec, mount_options(c, 256)) != 0) {
    std::fprintf(stderr, "crashsim: workload mount failed\n");
    std::exit(2);
  }
  shale::Random rng(c.seed);
  std::map<std::string, std::string> model;  // live file contents
  const char* dirs[] = {"", "/d0", "/d1", "/d2"};
  for (int i = 1; i < 4; i++) fs.mkdir(dirs[i]);
  fs.sync();
  uint64_t version_tag = 1;
  auto random_path = [&]() {
    return std::string(dirs[rng.below(4)]) + "/f" + std::to_string(rng.below(12));
  };
  auto durable_all = [&]() {
    size_t f = last_flush(*rec);
    for (auto& [p, content] : model) out->durable.push_back({p, content, f, rec->position()});
  };
  for (int op = 0; op < c.ops; op++) {
    size_t start = rec->position();
    uint64_t kind = rng.below(100);
    std::string path = random_path();
    if (kind < 30) {  // write: replace, append or overwrite
      uint32_t ino;
      if (fs.create(path.c_str(), false, &ino) != 0) continue;
      out->mods.push_back({path, start});
      std::string& m = model[path];
      size_t n = rng.chance(1, 8) ? 40000 + rng.below(160000) : rng.below(9000);
      std::string data = make_content(rng, n, version_tag++);
      uint64_t off = 0;
      uint64_t mode = rng.below(3);
      if (mode == 0) {
        fs.truncate(ino, 0);
        m.clear();
      } else if (mode == 1) {
        off = m.size();
      } else {
        off = m.empty() ? 0 : rng.below(m.size());
      }
      if (fs.write(ino, off, data.data(), data.size()) == static_cast<int64_t>(data.size())) {
        if (m.size() < off + data.size()) m.resize(off + data.size(), '\0');
        m.replace(off, data.size(), data);
      }
    } else if (kind < 44) {  // fsync
      uint32_t ino;
      if (!model.count(path) || fs.lookup(path.c_str(), &ino) != 0) continue;
      if (fs.fsync(ino) == 0) out->durable.push_back({path, model[path], last_flush(*rec), rec->position()});
    } else if (kind < 52) {  // unlink
      if (fs.unlink(path.c_str()) == 0) {
        out->mods.push_back({path, start});
        model.erase(path);
      }
    } else if (kind < 60) {  // truncate
      uint32_t ino;
      if (!model.count(path) || fs.lookup(path.c_str(), &ino) != 0) continue;
      out->mods.push_back({path, start});
      uint64_t len = rng.below(model[path].size() + 8192);
      if (fs.truncate(ino, len) == 0) model[path].resize(len, '\0');
    } else if (kind < 68) {  // rename (possibly replacing)
      std::string to = random_path();
      if (!model.count(path)) continue;
      out->mods.push_back({path, start});
      out->mods.push_back({to, start});
      if (fs.rename(path.c_str(), to.c_str()) == 0 && to != path) {
        model[to] = model[path];
        model.erase(path);
      }
    } else if (kind < 73) {  // mkdir / rmdir churn
      std::string d = std::string(dirs[1 + rng.below(3)]) + "/s" + std::to_string(rng.below(3));
      if (fs.mkdir(d.c_str()) != 0) fs.unlink(d.c_str());
    } else if (kind < 78) {  // sync: everything durable
      if (fs.sync() == 0) durable_all();
    } else if (kind < 86) {  // atomic replace via tmp + fsync + rename + fsync
      std::string target = "/cfg" + std::to_string(rng.below(3));
      std::string tmp = target + ".tmp";
      std::string data = make_content(rng, 100 + rng.below(20000), version_tag++);
      out->mods.push_back({tmp, start});
      if (write_whole(fs, tmp, data) != 0) continue;
      uint32_t ino;
      fs.lookup(tmp.c_str(), &ino);
      if (fs.fsync(ino) != 0) continue;
      size_t rename_start = rec->position();
      if (fs.rename(tmp.c_str(), target.c_str()) != 0) continue;
      fs.lookup(target.c_str(), &ino);
      size_t durable = fs.fsync(ino) == 0 ? last_flush(*rec) : SIZE_MAX;
      out->replaced[target].push_back({data, rename_start, durable});
    }
    // remaining ~14%: reads (no state change)
  }
  fs.sync();
  durable_all();
}

struct Totals {
  uint64_t states = 0, mount_failures = 0, fsck_failures = 0, durability_failures = 0, replace_failures = 0;
  uint64_t replayed_txns = 0;
  std::vector<std::string> examples;
};

void note(Totals& t, const std::string& s) {
  if (t.examples.size() < 8) t.examples.push_back(s);
}

std::string read_file(ShaleFs& fs, const std::string& path, bool* exists) {
  uint32_t ino;
  *exists = fs.lookup(path.c_str(), &ino) == 0;
  if (!*exists) return {};
  InodeInfo info;
  if (fs.stat(ino, &info) != 0) return {};
  if (info.size > (256ull << 20)) return "<corrupt size>";  // a damaged inode in the ablation modes
  std::string s(info.size, '\0');
  if (info.size && fs.read(ino, 0, &s[0], info.size) != static_cast<int64_t>(info.size)) return "<read error>";
  return s;
}

// Checks one crash state. `crash` = events position of the crash; `flushed`
// = index of the last flush before it (SIZE_MAX: none).
void check_state(const Config& c, host::OverlayDevice* dev, const Recorded& rec, size_t crash, size_t flushed,
                 Totals* t, const std::string& label) {
  t->states++;
  {
    ShaleFs fs;
    if (fs.mount(dev, mount_options(c, 128)) != 0) {
      t->mount_failures++;
      note(*t, label + ": mount failed");
      return;
    }
    t->replayed_txns += fs.stats().replayed_txns;
    fs.unmount();
  }
  FsckReport rep;
  shale::fs::fsck(dev, &rep);
  if (rep.errors) {
    t->fsck_failures++;
    note(*t, label + ": fsck: " + rep.messages[0] + (rep.errors > 1 ? " (+" + std::to_string(rep.errors - 1) + ")" : ""));
  }
  ShaleFs fs;
  if (fs.mount(dev, mount_options(c, 128)) != 0) {
    t->mount_failures++;
    return;
  }
  // Durable files: latest guarantee per path, unless modified since.
  std::map<std::string, const DurablePoint*> latest;
  if (flushed != SIZE_MAX) {
    for (const DurablePoint& d : rec.durable)
      if (d.flush_index != SIZE_MAX && d.flush_index <= flushed && d.after <= crash) latest[d.path] = &d;
  }
  bool failed = false;
  for (auto& [path, d] : latest) {
    bool modified = false;
    for (const ModEvent& m : rec.mods)
      if (m.path == path && m.pos >= d->after && m.pos < crash) modified = true;
    if (modified) continue;
    bool exists;
    std::string got = read_file(fs, path, &exists);
    if (!exists || got != d->content) {
      if (!failed) {
        t->durability_failures++;
        note(*t, label + ": " + path + (exists ? " has wrong contents" : " is missing") + " after fsync");
      }
      failed = true;
    }
  }
  // Atomic replaces: one complete version, not older than the last durable one.
  for (auto& [target, versions] : rec.replaced) {
    size_t floor = 0;
    bool any_durable = false;
    std::vector<const std::string*> allowed;
    for (size_t i = 0; i < versions.size(); i++)
      if (versions[i].durable_flush != SIZE_MAX && flushed != SIZE_MAX && versions[i].durable_flush <= flushed) {
        floor = i;
        any_durable = true;
      }
    for (size_t i = floor; i < versions.size(); i++)
      if (versions[i].rename_start < crash) allowed.push_back(&versions[i].content);
    bool exists;
    std::string got = read_file(fs, target, &exists);
    bool ok = !exists ? !any_durable
                      : std::any_of(allowed.begin(), allowed.end(), [&](const std::string* v) { return *v == got; });
    if (!ok) {
      t->replace_failures++;
      note(*t, label + ": " + target + (exists ? " is not a complete allowed version" : " vanished"));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::strtoull(argv[++i], nullptr, 10) : 0ull; };
    if (a == "--seed") c.seed = next();
    else if (a == "--ops") c.ops = static_cast<int>(next());
    else if (a == "--states") c.states = static_cast<int>(next());
    else if (a == "--blocks") c.blocks = static_cast<uint32_t>(next());
    else if (a == "--torn") c.torn = true;
    else if (a == "--nojournal") c.journaling = false;
    else if (a == "--trust-commits") c.trust_commits = true;
    else if (a == "--quiet") c.quiet = true;
    else {
      std::fprintf(stderr, "usage: crashsim [--seed N] [--ops N] [--states N] [--torn] [--nojournal] "
                           "[--trust-commits] [--blocks N] [--quiet]\n");
      return 2;
    }
  }

  host::MemDevice base(c.blocks);
  ShaleFs::MkfsOptions mo;
  mo.journaling = c.journaling;
  mo.journal_blocks = 512;
  mo.uuid = 0xC0FFEE00ull + c.seed;
  if (ShaleFs::mkfs(&base, mo) != 0) return 2;
  host::MemDevice work(c.blocks);
  work.bytes() = base.bytes();
  host::RecordingDevice rec(&work);
  Recorded oracle;
  run_workload(c, &rec, &oracle);
  const auto& ev = rec.events();
  size_t writes = 0, flushes = 0;
  for (const auto& e : ev) (e.flush ? flushes : writes)++;

  // Epochs: runs of writes between flushes. Spread the state budget over them.
  std::vector<size_t> flush_idx;
  for (size_t i = 0; i < ev.size(); i++)
    if (ev[i].flush) flush_idx.push_back(i);
  flush_idx.push_back(ev.size());  // the tail after the last flush
  size_t epochs = flush_idx.size();
  size_t per_epoch = std::max<size_t>(1, static_cast<size_t>(c.states) / epochs);
  shale::Random rng(c.seed * 7919 + 1);
  Totals t;
  host::MemDevice durable(c.blocks);  // image with every write before the current epoch
  durable.bytes() = base.bytes();
  size_t epoch_start = 0;
  for (size_t e = 0; e < epochs; e++) {
    size_t end = flush_idx[e];  // flush position (or end of log)
    size_t flushed = e == 0 ? SIZE_MAX : flush_idx[e - 1];
    std::vector<size_t> window;  // write events in [epoch_start, end)
    for (size_t i = epoch_start; i < end; i++)
      if (!ev[i].flush) window.push_back(i);
    for (size_t s = 0; s < per_epoch; s++) {
      host::OverlayDevice ov(&durable);
      // Crash position inside the epoch, and which issued writes survived.
      size_t upto = window.empty() ? 0 : (s == 0 ? window.size() : rng.below(window.size() + 1));
      size_t crash = upto == window.size() ? end : window[upto];
      std::string kind;
      for (size_t k = 0; k < upto; k++) {
        bool keep = s == 0 || s == 1 ? (s == 0) : rng.chance(1, 2);  // s0: all issued writes, s1: none
        if (!keep) continue;
        const auto& w = ev[window[k]];
        if (c.torn && s >= 2 && rng.chance(1, 8)) {
          uint32_t first = static_cast<uint32_t>(rng.below(8)), count = 1 + static_cast<uint32_t>(rng.below(8 - first));
          ov.write_sectors(w.block, rec.payload(w.data), first, count);
        } else {
          ov.write(w.block, 1, rec.payload(w.data));
        }
      }
      std::string label = "epoch " + std::to_string(e) + " state " + std::to_string(s);
      check_state(c, &ov, oracle, crash, flushed, &t, label);
      if (!c.quiet && t.states % 2000 == 0)
        std::fprintf(stderr, "  %lu states...\n", static_cast<unsigned long>(t.states));
    }
    for (size_t idx : window) durable.write(ev[idx].block, 1, rec.payload(ev[idx].data));
    epoch_start = end + 1;
  }

  // A clean unmount of the full workload must also check out.
  {
    host::OverlayDevice full(&durable);
    ShaleFs fs;
    bool ok = fs.mount(&full, mount_options(c, 128)) == 0 && fs.unmount() == 0;
    FsckReport rep;
    shale::fs::fsck(&full, &rep);
    if (!ok || rep.errors) note(t, std::string("final image: ") + (ok ? rep.messages[0] : "mount failed"));
  }

  std::printf("crashsim seed=%lu mode=%s%s%s ops=%d: %zu writes, %zu flushes, %zu durable points, %zu replaces\n",
              static_cast<unsigned long>(c.seed), c.journaling ? "journal" : "no-journal",
              c.trust_commits ? "+trust-commits" : "", c.torn ? "+torn" : "", c.ops, writes, flushes,
              oracle.durable.size(), oracle.replaced.size());
  uint64_t bad_states = t.mount_failures + t.fsck_failures + t.durability_failures + t.replace_failures;
  std::printf("crashsim: %lu crash states, %lu transactions replayed; mount failures %lu, fsck failures %lu, "
              "durability failures %lu, atomic-replace failures %lu\n",
              static_cast<unsigned long>(t.states), static_cast<unsigned long>(t.replayed_txns),
              static_cast<unsigned long>(t.mount_failures), static_cast<unsigned long>(t.fsck_failures),
              static_cast<unsigned long>(t.durability_failures), static_cast<unsigned long>(t.replace_failures));
  for (const std::string& s : t.examples) std::printf("  %s\n", s.c_str());
  std::printf("CRASHSIM states=%lu failed=%lu\n", static_cast<unsigned long>(t.states),
              static_cast<unsigned long>(bad_states));
  return bad_states ? 1 : 0;
}
