# Renderer / Vulkan audit — `VK_ERROR_DEVICE_LOST` on `vkQueueSubmit`

**Task** task-2 — device-lost hazards in the fork's perf commits + submit path
**Tree** `C:\Users\Serina\Documents\ChatGPT\shadps4`, branch `release`, HEAD `14c39880`
(reviewed together with the in-progress branch `codex/idolmaster-release-gs-device-lost`, whose working tree
already carries the GS `OutputVertices` clamp, the `MasterSemaphore::Wait` device-lost branch and the
`SubmitExecution` diagnostics — those are called out as *WIP* below, not as shipped code).

**Crash under audit** (`8月.txt`, user Excalibur, RTX 5060 Laptop, driver 621.1.0.0, Vulkan 1.4.357,
1080p, `readbacks_mode=2`, `vkValidation=false`, `crashDiagnostics=false`):
`vk_scheduler.cpp:195 SubmitExecution: Assertion Failed! / Device lost during submit`, immediately after
`Compiling vs shader 0xb3b7b928 (permutation)` (3305) and `Compiling graphics pipeline 0x59b3f0dd8289f1d6`
(3307). Exactly one geometry-shader pipeline exists in the run (`stage_enable=EsGs`,
`es 0x2b5ab32b + gs 0xf3bd17fb`, pipeline `0xb3c99c2279216186`, log 2841-2845).

Read-only audit: **no source file was modified** by this task; the only artifact is this document.

---

## 0. TL;DR — ranked

| # | Finding | Where | Verdict class | Confidence |
|---|---|---|---|---|
| **F1** | GS `OutputVertices` declared straight from the 11-bit guest register `VGT_GS_MAX_VERT_OUT` (can be up to 2047) — exceeds NVIDIA `maxGeometryOutputVertices`=1024 / `maxGeometryTotalOutputComponents`=1024 → spec-invalid pipeline, undefined at draw | `vk_pipeline_cache.cpp:155`, `emit_spirv.cpp:417-419`, `regs.h:136` | invalid pipeline / GPU fault or hang (TDR) | **high** (best fit to vendor + timing) |
| **F2** | `DownloadBufferMemory<true>` defers a lambda that captures **locals by reference** (`[&]`) → `copies` / `download` / `buffer` dangle when it runs; newly *reachable* only because `InvalidateMemoryAsync` (`b3d75746`) made this previously-dead async path live | `buffer_cache.cpp:124-175` (esp. 159-168), `77-103` | host-heap / guest-RAM corruption → downstream GPU fault, hung timeline | **high** as a bug; **unproven** for this crash (no avplayer activity in the log) |
| **F3** | `UpdateImage` removed for render targets: images are no longer `TrackImage`'d or `RefreshImage`'d → stale `CpuDirty` bookkeeping, render-target-as-texture aliasing uploads stale guest bytes over GPU content | `texture_cache.cpp:635-701`, `vk_rasterizer.cpp:850-853`, `texture_cache.h:115-121` | visual corruption; possible fault via detiler reading unmapped guest memory | medium |
| **F4** | `NextTick()` moved **before** `submit_mutex`, mutex made `recursive`, command-buffer re-allocation + `PopPendingOperations` after unlock — timeline values can be issued out of submit order if two threads share a `Scheduler` (`DrawBlankFrame` on `PresentThread` uses `draw_scheduler`) | `vk_scheduler.cpp:152-155`, `199-204`; `driver.cpp:280-282, 339-411`; `vk_presenter.cpp:791` | invalid submit → device lost; cmd-pool reuse while in flight | medium-low |
| **F5** | Device loss is **undiagnosable**: no `vkGetDeviceFaultInfoEXT` anywhere; `crashDiagnostics` only loads the obsolete `VK_LAYER_LUNARG_crash_diagnostic` | `vk_platform.cpp:33, 238-241`; grep | n/a (diagnosis gap) | high (fact) |
| **F6** | `SubmitExecution` asserts **only** on `eErrorDeviceLost`; any other result (OOM …) is ignored → command buffer never executes → timeline never signals → `MasterSemaphore::Wait` spins. Buffer GC lambda is dead code (never invoked) → no LRU buffer eviction | `vk_scheduler.cpp:194-195`; `vk_master_semaphore.cpp:46-72`; `buffer_cache.cpp:885-909` | silent hang / memory growth | medium |
| **F7** | Detiler/tiler dispatch + mip selection: `GetMipLevel` can index `info.mips[num_mips]` (`pitch==0` → `% 0`, tiled offset from garbage) and the tiler writes `tiled_data[offset]` from layout math → OOB SSBO | `tiling.comp:412-421, 443-447`; `tile_manager.cpp:169-247` | GPU OOB (write) → VM fault | low-medium |
| **F8** | Fork replaced GS copy-shader asserts with warnings + sequential fallback mapping; a wrong `attr_map` yields wrong per-vertex stride/attribute routing (silent) | `ring_access_elimination.cpp:19-51, 155-162`; `copy_shader.cpp` | wrong geometry; feeds F1 (bad copy-shader vertex count) | low-medium |

---

## 1. `InvalidateMemoryAsync` / `ReadMemoryAsync` — is it async, is it safe? (Q1)

### 1.1 The chain

```
MemoryManager::InvalidateMemoryAsync   src/core/memory.cpp:1383-1387   -> rasterizer->InvalidateMemoryAsync
Rasterizer::InvalidateMemoryAsync      vk_rasterizer.cpp:1057-1065     -> buffer_cache.InvalidateMemoryAsync
                                                                          + texture_cache.InvalidateMemory
BufferCache::InvalidateMemoryAsync     buffer_cache.cpp:77-83         -> memory_tracker->InvalidateRegion(cb)
BufferCache::ReadMemoryAsync           buffer_cache.cpp:85-103        -> liverpool->SendCommand<true>(...)
DownloadBufferMemory<true>            buffer_cache.cpp:124-175       -> scheduler.DeferOperation(write_data)
Only caller: avplayer                  avplayer_source.cpp:650-656
```

### 1.2 It is *not* fully asynchronous — and the part that is async is unsound

`SendCommand<true>` is the **`wait_done`** specialisation (`amdgpu/liverpool.h:98-120`): it queues the
lambda onto the GPU thread and blocks the **caller** (the guest AvPlayer thread) on a
`std::binary_semaphore` until that lambda has run. So the only thing that became "async" is the *GPU→CPU
copy*: the `scheduler.Finish()` (full pipeline drain) inside `DownloadBufferMemory<false>` was replaced by
`scheduler.DeferOperation(write_data)`.

```cpp
// buffer_cache.cpp:159-174
const auto write_data = [&]() {                       // <-- captures copies/download/offset by REFERENCE
    auto* memory = Core::Memory::Instance();
    for (const auto& copy : copies) {
        const VAddr copy_device_addr = buffer.CpuAddr() + copy.srcOffset;
        const u64 dst_offset = copy.dstOffset - offset;
        memory->TryWriteBacking(std::bit_cast<u8*>(copy_device_addr), download + dst_offset,
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
};
if constexpr (async) {
    scheduler.DeferOperation(write_data);   // runs later, after this frame is gone
} else {
    scheduler.Finish();
    write_data();
}
```

`DeferOperation` stores the closure and runs it from `PopPendingOperations()`
(`vk_scheduler.cpp:122-129`, called at the end of every `SubmitExecution`, or from a later
`Flush`/`Finish`). By then `copies` (a `small_vector<vk::BufferCopy,1>` **local**), `download`
(`u8*` local from `download_buffer.Map()`), `offset` and the `Buffer&` slot are all out of scope.
The closure holds references to dead stack slots: on a reused stack frame, `copies.size()`, each
`copy.srcOffset/size` and `download` are whatever the *current* call chain left there.

Consequences, in increasing severity:
1. stale/garbage bytes written into **guest RAM** by `TryWriteBacking` (see `memory.cpp:155`:
   `ASSERT_MSG(IsValidMapping(virtual_addr, size), ...)` — with a garbage `size` this asserts, or, with
   asserts compiled out, `memcpy`s a wild length into the guest backing);
2. `download` may already have been recycled by the `StreamBuffer` allocator → the *next* upload sees
   video-frame garbage;
3. if the corrupted guest range is a command/constant buffer, the GPU thread can consume a garbage PM4
   packet or a garbage index count → GPU fault/hang on the *next* frame (i.e. exactly a "device lost
   during a later submit"), or an emulator abort in an unrelated place.

**Important scoping fact:** in upstream this code path was effectively **dead** — the only other
`DownloadBufferMemory<true>` call site (`buffer_cache.cpp:905`, inside `RunGarbageCollector`) sits in a
`clean_up` lambda that is **never invoked** (see F6). `b3d75746` is the first commit that makes the async
branch live, and it makes it live on the **60 Hz video path**.

### 1.3 Does anything guarantee the GPU is done before the guest rewrites?

No. `InvalidateMemoryAsync` returns as soon as the GPU thread has *recorded* the `vkCmdCopyBuffer`
(`buffer_cache.cpp:156-158`); the copy itself executes later. AvPlayer then hands the decoded frame
buffer back to the game, which writes it. With the sync path the write was ordered after a full drain;
now the guest write can land **before** the GPU read of the old frame and before the deferred write-back,
which then re-writes the *old* content over the new frame.

Bounded? No. `pending_ops` is an unbounded `std::queue` (`vk_scheduler.h:446`) drained only in
`PopPendingOperations`, gated on `master_semaphore.IsFree(op.gpu_tick)`. There is no "priority" drain for
these (that is `DeferPriorityOperation`/`PriorityPendingOpsThread`, used only by
`texture_cache.cpp:108`). On shutdown, pending ops are simply dropped (`Scheduler::~Scheduler` only frees
the tracy scope) — but the `Scheduler` is destroyed after the device in some paths, so a dropped
write-back is the benign case; a *dangling* write-back is not.

Guest free: `TextureCache::UnmapMemory`/`BufferCache` frees do **not** cancel a queued `write_data`; a
write-back into an unmapped range hits the `TryWriteBacking` assert.

### 1.4 Can a host-visible mapping / BDA descriptor still be referenced after unmap?

The emulator does not map guest RAM into the GPU address space: `Rasterizer::IsMapped`
(`vk_rasterizer.cpp:1080-1097`) is only an interval-set lookup over guest VMAs that are shadowed by
buffers/images; every GPU read/write is an explicit copy to/from a VMA allocation (`StreamBuffer` for
uploads, `download_buffer` for read-backs, `Buffer` objects for cached ranges). So there is no
`vkMapMemory`-alias that outlives the guest range. The real hazard is therefore **not** a stale BDA
descriptor but (a) the deferred closure above and (b) the loss of the "GPU idle" ordering that the guest
write relies on.

**Minimal fix (F2):** make the deferred closure own its data *and* be pinned to the copy's tick:

```cpp
// capture by value / by move, and pin the region until the copy has been consumed
scheduler.DeferOperation([this, copies = std::move(copies), download, offset,
                          device_addr, size, buffer_id = buffer.id] {
    // re-validate: the buffer may have been deleted in the meantime
    ...
});
```
Short-term safety valve (one line, no lifetime work): revert `avplayer_source.cpp:650-656` to
`InvalidateMemory(...)` until the closure is fixed. That restores the exact pre-`b3d75746` behaviour
(33 ms stalls) and removes a memory-corruption class from the shipping build.

---

## 2. Skipping `UpdateImage` for render targets (Q2)

`UpdateImage` is not just "CPU→GPU upload"; it is three things (`texture_cache.h:115-121`):

```cpp
void UpdateImage(ImageId image_id) {
    std::scoped_lock lock{mutex};
    Image& image = slot_images[image_id];
    TrackImage(image_id);     // (re)install guest page protection so CPU writes mark CpuDirty
    TouchImage(image);        // LRU
    RefreshImage(image);      // upload / clear Dirty / set mip_hashes / EndRendering / detile
}
```

`005f24b3` removed it from `FindRenderTarget` (`texture_cache.cpp:635-660`), `FindDepthTarget`
(`662-701`) and from `BeginRendering`'s colour-attachment loop (`vk_rasterizer.cpp:850-853`), on the
premise "render targets are GPU-written, never need CPU→GPU upload".

What that breaks:
* **Page tracking.** A render target that is only ever bound as a target is never `TrackImage`'d, so
  guest CPU writes to its backing pages are not observed as `CpuDirty`/`MaybeCpuDirty`. Any later CPU
  write (or a `memset`/DMA by the game) is invisible; conversely `RefreshImage`'s
  `ImageFlagBits::Dirty`/`mip_hashes` protection (`texture_cache.cpp:703-752`) is never refreshed.
* **Render target aliased as a texture.** `FindTexture` still calls `UpdateImage`
  (`texture_cache.cpp:621-633`), so binding the same guest address as a sampled image will run
  `RefreshImage`; if the image is still `Dirty` (e.g. it was created/expanded but the target path never
  cleared the flag) it will `DetileImage` + `copyBufferToImage` from **guest memory** straight into the
  image the GPU just rendered (`texture_cache.cpp:776-795`, `image.cpp:357-401`). Result: the frame's
  render-target contents are replaced by stale CPU bytes.
* **Layout.** This is *not* where the layout breaks: `BeginRendering` re-derives
  `attachment.image_layout = image->backing->state.layout` (`vk_rasterizer.cpp:885, 930`) immediately
  after `image->Transit(...)` (`865-875, 919-922`), and `Transit` updates the tracked state and records
  the barrier together (`image.cpp:223-355`). Skipping `UpdateImage` does not desynchronise the tracker.
* **Detiler / unmapped guest memory.** `RefreshImage` → `ObtainBufferForImage(image.info.guest_address,
  image.info.guest_size)` (`texture_cache.cpp:778-779`) will happily create a buffer for a guest range the
  game has since freed if the image's bookkeeping is stale, and the detiler then reads it
  (`tile_manager.cpp:169-247`). This is the only path from F3 to a GPU-side fault, and it is indirect.

Net: F3 is a **correctness/visual** bug with a plausible but indirect fault path — not a convincing
single cause for a device loss on the first draw of a new pipeline. It also cannot explain a crash whose
log shows no validation errors.

**Minimal fix:** restore `texture_cache.UpdateImage(image_id)` in `FindRenderTarget` and `FindDepthTarget`;
in `BeginRendering` (where the comment's "breaks render passes" concern is real) call the cheap half only:

```cpp
texture_cache.TrackImage(image_id);   // keep page protection, no EndRendering/RefreshImage
texture_cache.TouchImage(*image);
```
i.e. split `UpdateImage` into `UpdateImageTracking()` + `RefreshImage()` and use the former for render
targets. That keeps the render pass intact *and* keeps invalidation correct.

---

## 3. Submit path lifetime (Q3)

* **Ticks.** `MasterSemaphore` (`vk_master_semaphore.h:23-37`): `current_tick` starts at 1 and
  `NextTick()` returns the pre-increment value, so the first submit signals 1 and `CurrentTick()` is the
  value the *next* submit will signal. `DeferOperation` stamps that value, so a deferred op runs only
  after the command buffer containing its work has completed — the protocol is correct **provided** the
  tick is taken under `submit_mutex` and the command buffer is the one that will be submitted next.
* **Fork change (`3de9b251`)** moved `NextTick()` *before* `std::unique_lock lk{submit_mutex}`
  (`vk_scheduler.cpp:152-155`) and made the mutex `std::recursive_mutex` (`:14`, `:426`). Two threads on
  one `Scheduler` can now receive tick *n* and *n+1* and submit them in the opposite order → a timeline
  value that is not greater than the one already signalled: invalid submit, and on RADV/amdgpu a CS
  rejection surfaces as `VK_ERROR_DEVICE_LOST`; on NVIDIA it is undefined behaviour. The same window
  lets both threads record into the same `current_cmdbuf` and lets `AllocateWorkerCommandBuffers()`
  (`:131-150` → `CommandPool::Commit` → `ResourcePool::CommitResource`, `vk_resource_pool.cpp:16-52`)
  reuse a command buffer whose tick is still in flight.
  Reachable? `draw_scheduler` is driven by `shadPS4:GpuCommandProcessor` (draws, `PrepareFrame` via
  `driver.cpp:312-326` for non-EOP flips) **and** by `shadPS4:PresentThread` through
  `DrawBlankFrame()` → `PrepareBlankFrame(false)` → `draw_scheduler` (`driver.cpp:280-282`,
  `vk_presenter.cpp:787-791`). EOP flips (`driver.cpp:288-313`) execute `SubmitFlipInternal` on the
  caller's thread. So the overlap exists, but only on the "port not open"/EOP paths — not while a lesson
  is rendering. **This is why F4 is ranked below F1.**
* **Resource lifetime.** Image/buffer destruction is tick-deferred: `TextureCache::DeleteImage`
  (`texture_cache.cpp:1097-1105`) and `BufferCache::DeleteBuffer` (`buffer_cache.cpp:915-920`) both use
  `scheduler.DeferOperation`. Detiler scratch buffers are deferred too (`tile_manager.cpp:195-197`,
  `280-282`). Descriptor sets come from a heap whose pools are also tick-guarded
  (`vk_resource_pool.h:67-90`). I found **no** path that frees a `VkImage`/`VkBuffer`/`VkImageView`
  synchronously while a recorded command buffer can still reference it — with the single exception of the
  dangling `write_data` closure (F2), which reads a freed buffer *slot* and a recycled staging range.
* **Priority pending ops thread** (`vk_scheduler.cpp:207-231`): blocks in
  `master_semaphore.Wait(op.gpu_tick)` forever on a lost device; the fork's `500 ms` timeout loop
  (`vk_master_semaphore.cpp:46-72`) makes it spin instead of waiting, and the WIP branch correctly adds
  the `eErrorDeviceLost` exit.

**Minimal fix:** move `NextTick()` back under the lock (it is one line) and keep `recursive_mutex` only
if the deferred-callback re-entry is real; alternatively assert
`std::this_thread::get_id() == owner_thread` in `SubmitExecution`/`AllocateWorkerCommandBuffers` to catch
cross-thread scheduler use in debug builds.

---

## 4. Is device loss diagnosable in this build? (Q4)

No.
* `VK_EXT_device_fault` is brought up by the driver, but the emulator never calls
  `vkGetDeviceFaultInfoEXT` — there is **no** occurrence of `DeviceFault`/`device_fault` in `src/`.
* The setting that sounds like it would help, `crashDiagnostics`, only appends the legacy
  `VK_LAYER_LUNARG_crash_diagnostic` layer to the instance (`vk_platform.cpp:33, 238-241`); it is printed
  at startup (`emulator.cpp:342`) and does nothing for a GPU-side loss.
* `vkValidation` gives API-misuse errors, **not** a GPU hang. `vkValidationSync` (sync validation,
  `vk_platform.cpp:305-307`) catches barrier/layout/queue-order bugs; `vkValidationGpu` (GPU-AV) is the
  only mode that can attribute an out-of-bounds descriptor access to a draw, and it perturbs execution.
* The WIP branch's `LOG_CRITICAL` at `vk_scheduler.cpp:194` is the right idea; see §7 for the exact
  accessors that exist.

**Minimal fix:** after `eErrorDeviceLost`, call `vkGetDeviceFaultInfoEXT`
(`VK_EXT_device_fault`, `VkDeviceFaultCountsEXT`/`VkDeviceFaultInfoEXT`) when
`instance.IsDeviceFaultSupported()` (to be added) and log `addressInfos`/`vendorBinarySize`; this turns
"device lost" into "fault address + fault type" for the user's next run. Cheap and CI-safe.

---

## 5. OOM vs hang vs invalid access (Q5)

* **OOM does not surface as device lost here.** `vk_scheduler.cpp:194-195` asserts only on
  `eErrorDeviceLost`; `VK_ERROR_OUT_OF_DEVICE_MEMORY` (or any other result) falls through, the command
  buffer is never executed, the timeline never advances, and the next `MasterSemaphore::Wait`
  (`vk_master_semaphore.cpp:46-72`) waits in 500 ms slices forever → the emulator *hangs*. Also
  `BufferCache::RunGarbageCollector` (`buffer_cache.cpp:885-909`) never invokes its `clean_up` lambda
  (compare `GarbageCollectImages`, `texture_cache.cpp:1008-1016`, which does) → cached buffers are never
  evicted by LRU, so long/heavy sessions accumulate `VkDeviceMemory` until the allocator fails.
  The crash log contains no OOM signature, so this is not the current trigger, but it is a live hazard.
* **A GPU hang (TDR) is consistent with the log.** On Windows the user sees the loss 2 s after the GPU
  stops making progress; during those 2 s the host keeps streaming assets — which is exactly the shape of
  the log (GS pipeline at 2841, ~470 mostly `Kernel.Fs` lines, then a new VS permutation, then the
  failing submit). A hang leaves no emulator-side trace, so the absence of warnings is not evidence
  against it.
* **Shader-side hang candidates.** The `(permutation)` compile is the pipeline cache creating a new
  specialisation of the same program (`vk_pipeline_cache.cpp:603-612`); the `First instruction is not
  s_mov_b32 vcc_hi, #imm` line is a pre-existing upstream warning (`recompiler.cpp:34-37`). Neither is
  itself a hang. The bounded-count risk lies in the GS vertex/output declaration (F1) and in the
  copy-shader-derived counts (F8), not in the permutation machinery.

---

## 6. Answers to the Lead's questions (A/B/C)

### A. Does the declared SPIR-V `OutputVertices` drive data flow?

**Yes — it is a hard, validated bound on a real geometry stage, not a hint.**

* GS pipelines with `stage_enable == EsGs` bind *two* real stages
  (`vk_pipeline_cache.cpp:513-529`): `Stage::Export` as `LogicalStage::Vertex` and `Stage::Geometry` as
  `LogicalStage::Geometry`.
* The backend emits `ExecutionModel::Geometry` **plus** `OpExecutionMode OutputVertices` and
  `Invocations` (`emit_spirv.cpp:408-422`).
* The GS really emits vertices: `S_SENDMSG` `GsOp::Emit` → `ir.EmitVertex()` → `OpEmitVertex`, and
  `GsOp::Cut` → `EmitPrimitive` → `OpEndPrimitive` (`frontend/translate/scalar_flow.cpp:54-70`,
  `backend/spirv/emit_spirv_special.cpp:92-105`). Outputs are written with `ExportPosition`/`SetAttribute`
  (`ring_access_elimination.cpp:210-215`).
* The declared value is the raw guest register: 11 bits, max 2047 (`amdgpu/regs.h:136`), assigned
  verbatim at `vk_pipeline_cache.cpp:155`. NVIDIA's limits are `maxGeometryOutputVertices = 1024` and
  `maxGeometryTotalOutputComponents = 1024`; the tree already hard-codes the guaranteed 256/1024 pair for
  its own GLSL compiler (`vk_shader_util.cpp:63-64`). `OutputVertices` in (1024, 2047] is a
  pipeline-creation violation; a large value combined with ≥1 output component per vertex can also exceed
  `maxGeometryTotalOutputComponents`.

**Is clamping safe?** Clamping *down* is safe for the device (declaring fewer vertices never violates a
limit), but it changes emitted geometry: emitting more vertices than declared is UB, so the clamp must be
≥ the number of vertices the GS actually emits per invocation. Practical guidance:

1. `min(VGT_GS_MAX_VERT_OUT, 256)` is always limit-safe (256 is the Vulkan minimum guarantee).
2. Additionally clamping to `gs_copy_data.output_vertices` is what upstream #5523 does and is *usually*
   the true per-invocation count, **but** in this fork that number comes from a heuristic parse
   (`copy_shader.cpp:180-183`: `comp_stride / 64`) **or** from the sequential fallback
   (`ring_access_elimination.cpp:19-51`). A mis-parse can make it *smaller* than reality → dropped
   vertices (visual) or, on strict drivers, more UB. If it is used, keep the `LOG_WARNING` and prefer
   `max(copy_count, some floor)` over a pure `min`.
3. Because `info.gs_output_vertices` is part of the emitted SPIR-V, bumping
   `Serialization::ShaderBinaryVersion` (already done in the WIP tree) is **required**, otherwise stale
   cached SPIR-V keeps the old declaration.
4. Watch the pre-existing assert at `ring_access_elimination.cpp:138-145`: it fires whenever
   `max_vert_out <= copy_count` and the scenario is not `ScenarioG`. Any change that makes the copy
   count larger than the register converts a limit bug into an emulator abort.

### B. Ranked verdict — what to land first

**(i) GS `OutputVertices` clamp first**, then **(iv) diagnostics that make it falsifiable**, then F2's
lifetime fix; do **not** start with (ii) or (iii).

* *Failure mechanism I believe in:* the GPU lost the device at the first draw that used a GS pipeline
  whose `OutputVertices` exceeded the implementation limits — i.e. UB at draw time, reported by the
  driver as a fault/TDR and returned as `VK_ERROR_DEVICE_LOST` on the next submit. It is the only
  candidate in the fork delta that (a) is a *spec-level* violation, (b) matches the vendor (NVIDIA's
  1024-component budget), and (c) matches the timing (one GS pipeline created a short time before the
  failing submit; TDR latency ~2 s of "host keeps running" explains the ~470 mostly-I/O log lines in
  between, and explains why the failing submit follows an *unrelated* new pipeline).
* (ii) `UpdateImage` restoration is a visual-correctness fix with no device-loss mechanism, and
  re-introduces render-pass breaks → contradicts the perf intent.
* (iii) The async-invalidation revert removes a real memory-corruption class (F2) but **cannot** be this
  run's trigger: the log has no AvPlayer activity at all (only `Loading libSceAvPlayer` at line 928), so
  `InvalidateMemoryAsync` was never called in the crash run. Reverting it should be done as a *safety*
  step (or by fixing the closure), not as the root-cause fix.
* Anything in the fork's delta that could fault on the **first** draw with a newly created pipeline, in
  order of plausibility: (1) GS `OutputVertices` above limits (F1); (2) a GS copy-shader mapping
  mismatch after the fork's assert→warning hardening (F8) producing ring-buffer offsets that address
  outside the ring allocation; (3) unbound/stale descriptor for a newly specialised shader (no evidence
  in this tree — descriptors are written before every draw in `BindResources`); (4) `eDisableOptimization`
  on every pipeline (`vk_graphics_pipeline.cpp`, `vk_compute_pipeline.cpp`, fork commit `3de9b251`) —
  ignored by NVIDIA, only affects AMD codegen, so it cannot be a fault source on the RTX 5060.

### C. Cheap, compile-safe diagnostics at `vk_scheduler.cpp:195`

Reachable directly from `Scheduler::SubmitExecution` (member `instance`, member `master_semaphore`,
local `signal_value`, `info`, `current_cmdbuf`):

| What | Exact accessor that exists in this tree |
|---|---|
| timeline state | `master_semaphore.CurrentTick()`, `.KnownGpuTick()`, `.Refresh()`, `.Handle()` (`vk_master_semaphore.h:23-41`) |
| semaphores in this submit | `info.num_wait_semas`, `info.num_signal_semas`, `signal_value` |
| device | `instance.GetDevice()`, `instance.GetPhysicalDevice()`, `instance.GetAllocator()` (`vk_instance.h:42-53`) |
| memory (only meaningful with `instance.CanReportMemoryUsage()` == `supports_memory_budget`, `vk_instance.h:443-445`) | `instance.GetDeviceMemoryUsage()` (MiB; `vk_instance.cpp:745-757`), `instance.GetTotalMemoryBudget()` (`vk_instance.h:451-453`) |
| thread | `Common::GetCurrentThreadName()` (`common/thread.h:50`, `common/thread.cpp:252`) — `vk_scheduler.cpp` already includes `common/thread.h` |
| literals / logging | `_MB` is available (`common/types.h:34`), `LOG_CRITICAL` from `common/logging/log.h` |

Not reachable from `Scheduler`: the last pipeline hash, `PipelineCache`, `TextureCache`/`BufferCache`
counts, VMA block counts (there is no `VideoCore` singleton; `Rasterizer` is owned by the frontend).
Cheapest way to get them is **where they are already logged**:
* `vk_pipeline_cache.cpp:326` already logs every created graphics pipeline hash — so the log *does* name
  the last pipeline; no plumbing needed.
* Add one `LOG_INFO(Render_Vulkan, ...)` at GS pipeline creation (`vk_pipeline_cache.cpp:151-186`) printing
  `regs.vgt_gs_max_vert_out`, `regs.vgt_gs_vert_itemsize[0]`, `regs.vgt_esgs_ring_itemsize`,
  `regs.vgt_gs_mode.mode`, `regs.vgt_gs_instance_cnt.count`, `info.gs_copy_data.output_vertices`,
  `info.gs_copy_data.num_comps`, plus `instance.GetPhysicalDevice().getProperties().limits.
  maxGeometryOutputVertices / maxGeometryTotalOutputComponents`. **This single line confirms or refutes
  F1 from a user log** without a debugger.
* VMA statistics, if wanted, are `vmaCalculateStatistics`/`vmaGetHeapBudgets` on `instance.GetAllocator()`
  (add `#include "video_core/renderer_vulkan/vk_memory.h"`); do it behind
  `instance.CanReportMemoryUsage()` to avoid a meaningless `0 MiB / 0 MiB`.
* A `static std::atomic<u64> last_graphics_pipeline` in `PipelineCache` + a static getter is the low-risk
  way to expose the hash if you want it in the assert message.

Note on the WIP diagnostics: `GetDeviceMemoryUsage()` issues `vkGetPhysicalDeviceMemoryProperties2` with
`VkPhysicalDeviceMemoryBudgetPropertiesEXT` in `pNext` **unconditionally** (`vk_instance.cpp:745-757`);
that is legal on a lost device but returns zeros without `VK_EXT_memory_budget`, so gate it on
`CanReportMemoryUsage()` and print `n/a` otherwise.

---

## 7. Which fix first, and how to validate without a local build

1. **Land the GS `OutputVertices` clamp + the GS-register log line** (2 files: `emit_spirv.cpp`,
   `ring_access_elimination.cpp` / `info.h`, plus `Serialization::ShaderBinaryVersion` bump — already in
   the WIP tree — and one `LOG_INFO` in `vk_pipeline_cache.cpp`). Keep it a standalone commit so it can be
   reverted independently.
2. **Also land the two zero-risk safety nets** as separate commits so the next user report is
   actionable: (a) `avplayer_source.cpp` back to `InvalidateMemory(...)` (or fix the closure capture);
   (b) `NextTick()` back under `submit_mutex`; (c) the `eErrorDeviceLost` exit in
   `MasterSemaphore::Wait` (already WIP) so a lost device reports instead of spinning.
3. **Validate without a local build**
   * CI: push the branch, let `.github/workflows/idolmaster-release.yml` produce the artifact (see
     `documents/imas-crash-analysis/upstream-and-ci.md` for triggers and artifact URLs), download the
     `Release` build.
   * User repro: same PC/user (Excalibur), same content, same settings as `8月.txt`
     (`pipeline_cache_enabled` **off** — that is what makes the run compile shaders live and reproduced
     the crash), then the *same* 地狱特训Pro lesson, with `vkValidation=true` in the first run only.
   * Expected confirmation in the new log, in this order:
     a. new `Geometry shader ... declares N output vertices, clamping to 256` / GS register line with
        `max_vert_out > 256` → F1 confirmed and clamped;
     b. the lesson gets past the previous death point (past `dan_kov_*` / `cam_*` loading, which the
        crash run never reached);
     c. if it still dies, the new `Device lost on queue submit` line gives tick/thread/memory, and
        `maxGeometry*` from (a) rules F1 in or out in one look.
   * Falsification test for F1 that needs no code change: run the same lesson with
     `pipeline_cache_enabled=1` after warming the cache with a *different* GS-using scene — if the crash
     disappears when the GS pipeline comes from disk, it is the pipeline creation/declaration, not the
     draw path.

## 8. What I could not verify

* No GPU, no Windows driver stack and no build in this session: every mechanism above is a code argument,
  not an observed fault. F1's register value for this game is **not** in the log (no `MAX_VERT_OUT`
  warning was printed, which means `max_vert_out <= copy_count` *or* `==`), so F1 is "best fit", not
  proof. The new GS log line in §7.3 is what turns it into proof.
* The detiler/host-shader side (F7) was reviewed only at the dispatch/bounds level; the `micro_tiled_mips`
  and 96-bit-tile changes are backports from newer upstream PRs and deserve a separate pass.
* `liverpool.cpp`'s PM4 parser changes (`3de9b251`: unknown opcode → warning + "safe advance") are outside
  this task's scope but are a plausible *command-stream desync* source; the log shows no such warning,
  which is weak evidence against it for this crash.
