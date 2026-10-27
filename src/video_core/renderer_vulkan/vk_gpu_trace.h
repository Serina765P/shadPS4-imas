// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/types.h"

namespace Vulkan::GpuTrace {

/// A lost Vulkan device is only reported by the *next* submit, so the submit (and the draw inside
/// it) that actually faulted the GPU is gone by the time the emulator notices. Keep a small ring
/// buffer of the last submitted operations and dump it on device loss: that turns "device lost
/// during submit" into "these draws were in the last submits".
enum class EntryKind : u8 {
    Draw,
    Submit,
};

struct Entry {
    u64 pipeline_hash{};
    u64 args_address{};
    u32 count{};
    u32 instances{};
    u32 vertex_offset{};
    u32 instance_offset{};
    u32 index_offset{};
    u8 kind{static_cast<u8>(EntryKind::Draw)};
    bool indexed{};
};

constexpr size_t Capacity = 512;

inline std::array<Entry, Capacity> ring{};
inline std::atomic<size_t> cursor{0};

inline void Push(const Entry& entry) {
    const size_t index = cursor.fetch_add(1, std::memory_order_relaxed);
    ring[index % Capacity] = entry;
}

inline void PushDraw(u64 pipeline_hash, bool indexed, u32 count, u32 instances, u32 vertex_offset,
                     u32 instance_offset, u32 index_offset) {
    Entry entry{};
    entry.pipeline_hash = pipeline_hash;
    entry.count = count;
    entry.instances = instances;
    entry.vertex_offset = vertex_offset;
    entry.instance_offset = instance_offset;
    entry.index_offset = index_offset;
    entry.indexed = indexed;
    Push(entry);
}

inline void PushIndirectDraw(u64 pipeline_hash, bool indexed, u32 max_count, u32 stride,
                             u64 args_address) {
    Entry entry{};
    entry.pipeline_hash = pipeline_hash;
    entry.count = max_count;
    entry.args_address = args_address;
    entry.instance_offset = stride;
    entry.indexed = indexed;
    Push(entry);
}

inline void PushSubmit() {
    Entry entry{};
    entry.kind = static_cast<u8>(EntryKind::Submit);
    Push(entry);
}

/// Logs the recorded operations, oldest first. Called only when the device was lost.
inline void Dump(const char* reason) {
    const size_t total = cursor.load(std::memory_order_relaxed);
    const size_t count = std::min(total, Capacity);
    if (count == 0) {
        LOG_CRITICAL(Render_Vulkan, "GPU trace is empty ({})", reason);
        return;
    }

    std::string report = fmt::format("{} operation(s) before the device was lost ({})", count,
                                     reason);
    for (size_t i = total - count; i < total; ++i) {
        const Entry& entry = ring[i % Capacity];
        if (entry.kind == static_cast<u8>(EntryKind::Submit)) {
            report += "\n  ---- submit ----";
            continue;
        }
        if (entry.args_address != 0) {
            report += fmt::format(
                "\n  pipeline {:#x}{} indirect count {} stride {} args {:#x}", entry.pipeline_hash,
                entry.indexed ? " indexed" : "", entry.count, entry.instance_offset,
                entry.args_address);
        } else {
            report += fmt::format(
                "\n  pipeline {:#x}{} count {} instances {} vertex_offset {} instance_offset {} "
                "index_offset {:#x}",
                entry.pipeline_hash, entry.indexed ? " indexed" : "", entry.count, entry.instances,
                entry.vertex_offset, entry.instance_offset, entry.index_offset);
        }
    }
    LOG_CRITICAL(Render_Vulkan, "GPU trace: {}", report);
}

} // namespace Vulkan::GpuTrace
