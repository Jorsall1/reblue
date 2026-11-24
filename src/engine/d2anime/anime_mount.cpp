/**
 * @file    engine/d2anime/anime_mount.cpp
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */
#include "engine/d2anime/anime_mount.h"

#include "engine/d2anime/anime_layout.h"
#include "vfs/vfs.h"

#if defined(__SWITCH2__)
#include <chrono>

#include "core/logging.h"
#endif

namespace bd::engine {

namespace {

std::vector<u8> Bytes(const std::string &s) { return {s.begin(), s.end()}; }

} // namespace

LayoutMount::LayoutMount(std::string dir)
    : dir_(std::move(dir)), mount_(std::make_shared<vfs::GeneratedMount>()) {}

LayoutMount::~LayoutMount() = default;

LayoutMount &LayoutMount::Add(const char *name, AnimeLayout *layout) {
  return Add(name, [layout] { return layout->ToCSV(); });
}

LayoutMount &LayoutMount::Add(const char *name,
                              std::function<std::string()> csv) {
#if defined(__SWITCH2__)
  // Session 013: every Settings open costs a 126-176 ms frame (log 009) with
  // nothing read, created or compiled. A generated file that takes long to
  // build names itself here, so the next log says whether it is these.
  return AddRaw(dir_ + name, [csv = std::move(csv), key = dir_ + name] {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<u8> out = Bytes(csv());
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    if (ms >= 5.0)
      BD_INFO("[menu-csv] {} built in {:.1f} ms ({} bytes)", key, ms, out.size());
    return out;
  });
#else
  return AddRaw(dir_ + name,
                [csv = std::move(csv)] { return Bytes(csv()); });
#endif
}

LayoutMount &LayoutMount::AddRaw(std::string key,
                                 std::function<std::vector<u8>()> bytes) {
  mount_->Set(vfs::Key::FromRelative(key), std::move(bytes));
  return *this;
}

void LayoutMount::Publish(const char *mountName) {
  vfs::VFS::Get().Files().Add(mountName, vfs::kPriorityGenerated, mount_);
}

void LayoutMount::Remove(const char *mountName) {
  vfs::VFS::Get().Files().Remove(mountName);
}

} // namespace bd::engine
