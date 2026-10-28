// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "common/io_file.h"
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

/// State of the draw that was recorded last. The device loss is reported one submit late, so the
/// index buffer and the vertex bindings that produced it are logged next to the trace.
struct DrawState {
    u64 pipeline_hash{};
    VAddr index_base_address{};
    u32 index_offset{};
    u32 num_indices{};
    u32 num_instances{};
    u32 num_vertex_buffers{};
    u32 dropped_attributes{};
    bool indexed{};
    bool index_16bit{};
};

inline DrawState last_draw{};

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

/// Records the state of the draw about to be recorded. Called for every draw, so it must stay a
/// handful of stores.
inline void PushDrawState(u64 pipeline_hash, bool indexed, VAddr index_base_address,
                          u32 index_offset, u32 num_indices, u32 num_instances, bool index_16bit) {
    last_draw.pipeline_hash = pipeline_hash;
    last_draw.indexed = indexed;
    last_draw.index_base_address = index_base_address;
    last_draw.index_offset = index_offset;
    last_draw.num_indices = num_indices;
    last_draw.num_instances = num_instances;
    last_draw.index_16bit = index_16bit;
}

inline void PushVertexState(u32 num_vertex_buffers, u32 dropped_attributes) {
    last_draw.num_vertex_buffers = num_vertex_buffers;
    last_draw.dropped_attributes = dropped_attributes;
}

/// The guest shader code and the SPIR-V of the most recently compiled stages. The last compile
/// before a device loss identifies the offending permutation, and the SPIR-V is what has to be
/// inspected offline - the guest never leaves the failing draw anywhere else.
struct ShaderStageSnapshot {
    std::string stage;
    u64 hash{};
    size_t perm{};
    std::vector<u32> code;
    std::vector<u32> spv;
};

constexpr size_t MaxShaderSnapshots = 8;

inline std::mutex shader_mutex;
inline std::deque<ShaderStageSnapshot> shader_snapshots;
inline std::atomic<u64> shader_snapshot_bytes{0};

/// Keeps a copy of a compiled stage. Called once per shader compile, never from a hot path.
inline void PushShaderStage(std::string stage, u64 hash, size_t perm, std::span<const u32> code,
                            std::span<const u32> spv) {
    const u64 new_bytes = u64(code.size_bytes()) + spv.size_bytes();
    constexpr u64 MaxShaderSnapshotBytes = 8_MB;
    if (new_bytes > MaxShaderSnapshotBytes) {
        return;
    }
    std::scoped_lock lock{shader_mutex};
    while (!shader_snapshots.empty() &&
           (shader_snapshots.size() >= MaxShaderSnapshots ||
            shader_snapshot_bytes.load(std::memory_order_relaxed) + new_bytes >
                MaxShaderSnapshotBytes)) {
        const auto& oldest = shader_snapshots.front();
        shader_snapshot_bytes.fetch_sub(oldest.code.size_bytes() + oldest.spv.size_bytes());
        shader_snapshots.pop_front();
    }
    ShaderStageSnapshot snapshot{};
    snapshot.stage = std::move(stage);
    snapshot.hash = hash;
    snapshot.perm = perm;
    snapshot.code.assign(code.begin(), code.end());
    snapshot.spv.assign(spv.begin(), spv.end());
    shader_snapshots.emplace_back(std::move(snapshot));
    shader_snapshot_bytes.fetch_add(new_bytes);
}

/// Writes the retained stages into the shader dump directory. Called only when the device is lost.
inline void DumpShaderStages(const std::filesystem::path& dir) {
    std::scoped_lock lock{shader_mutex};
    if (shader_snapshots.empty()) {
        LOG_CRITICAL(Render_Vulkan, "No shader compile recorded before the device was lost");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (const auto& snapshot : shader_snapshots) {
        const auto base =
            fmt::format("{}_{:#018x}_{}", snapshot.stage, snapshot.hash, snapshot.perm);
        const auto bin_path = dir / (base + ".bin");
        const auto spv_path = dir / (base + ".spv");
        IOFile{bin_path, FileAccessMode::Create}.WriteSpan(std::span<const u32>{snapshot.code});
        IOFile{spv_path, FileAccessMode::Create}.WriteSpan(std::span<const u32>{snapshot.spv});
        LOG_CRITICAL(Render_Vulkan, "Dumped {} ({} guest dwords, {} SPIR-V dwords)", base,
                     snapshot.code.size(), snapshot.spv.size());
    }
}

/// Logs the recorded operations, oldest first. Called only when the device was lost.
inline void Dump(const char* reason) {
    const auto& draw = last_draw;
    LOG_CRITICAL(Render_Vulkan,
                 "Last draw: pipeline {:#x}, indexed = {}, index buffer {:#x} + {} ({} x {} bit), "
                 "indices = {}, instances = {}, vertex buffers = {}, attributes dropped = {}",
                 draw.pipeline_hash, draw.indexed, draw.index_base_address, draw.index_offset,
                 draw.num_indices, draw.index_16bit ? 16 : 32, draw.num_indices, draw.num_instances,
                 draw.num_vertex_buffers, draw.dropped_attributes);

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
