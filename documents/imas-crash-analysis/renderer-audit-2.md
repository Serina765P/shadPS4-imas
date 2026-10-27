<!--
SPDX-FileCopyrightText: Copyright 2026 Serina765P
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Renderer audit v2 — “first draw with a new VS permutation” faults the GPU

**Task** task-5 · **Tree** `C:\Users\Serina\Documents\ChatGPT\shadps4` · **Branch**
`codex/idolmaster-release-gs-device-lost` · **HEAD at time of writing** `f8bb2b33`
(“diag(vulkan): keep a flight recorder …”). The three `diag(vulkan)` commits
(`4681f99f`, `a556b0eb`, `f8bb2b33`) landed *while* this audit was running; they are taken into
account and **line numbers below are for `f8bb2b33`**.

This audit is read-only: no source file was modified, no branch created, nothing pushed. All
claims carry a `file:line` anchor; log claims carry `file:line` of the log in `~/Downloads`
(`8月.txt`, `9月.txt`, `地狱特训闪退.txt`, `medley7闪退.txt`).

---

## 0. TL;DR — ranked

| # | Finding | Confidence | Faults the GPU? |
|---|---|---|---|
| **F1** | **The inter-stage descriptor-binding chain under-counts image descriptor arrays.** `Info::AddBindings()` (`src/shader_recompiler/info.h:189`) advances `unified` by `images.size()`, while the SPIR-V emitter, `BuildDescSetLayout()` and the draw-time binder all advance by `ImageResource::NumBindings()`, which can be **> 1**. A permutation compiled right after a reused previous stage therefore bakes *lower* binding numbers than the pipeline layout declares → the stage reads the *previous* stage's descriptors → illegal GPU access. | **High** as a code defect (verified inconsistency between the four counter updates); **Medium** as *this* crash’s cause — it needs the precondition in §3.1, *Mechanism* steps 1–2, which §5 measures directly | **Yes** |
| **F2** | `StageSpecialization::operator==` short-circuits with `bitset.none() && other.bitset.none() → true` *before* comparing `start` (`src/shader_recompiler/specialization.h:222`). A resource-less stage therefore reuses a module whose `start` (descriptor base **and push-constant UD base**) differs. | Medium | Rarely (UD mis-read → wrong constants); would mis-bind if the stage ever gains a descriptor |
| **F3** | A new permutation is compiled from a **fresh `Shader::Info`** (`vk_pipeline_cache.cpp:699`) while the descriptor layout and all per-draw binding numbers are derived from `Program::info` (`vk_pipeline_cache.cpp:710`, `vk_graphics_pipeline.cpp:436`). Any divergence in the two resource lists shifts every binding of that stage. | Medium (latent unless counts differ) | Yes |
| **F4** | Fork-only: `InvalidateMemoryFromGPU()` widened from `is_written && is_formatted` to every `is_written` buffer (`vk_rasterizer.cpp:687`), executed **after** `PrepareRenderState()` registered the render targets. | Low–medium | Possible (image teardown mid-draw) |
| **F5** | Fork-only: render-target `UpdateImage()` removed (`vk_rasterizer.cpp:874`) — loses `TrackImage`/`TouchImage` bookkeeping; the file itself documents that this path can call `EndRendering()`. | Low | Indirect |
| **F6** | `RegisterShaderMeta(info, …)` for a **new** permutation serialises the *old* `Info` (`vk_pipeline_cache.cpp:702`) next to the new SPIR-V; `LoadPipelineStage()` then stores a `spec` whose `info` pointer dangles (`vk_pipeline_serialization.cpp:252-289`). | Fact (correctness of the shader DB) | No (yet) |
| **F7** | Fork-only GS fallback: `FillMissingGsCopyData()`/`BuildFallbackGsCopyData()` can change `dwords_per_vertex`, which changes `comp_ofs`/`vc_read_ofs` (`ring_access_elimination.cpp:147-162, 219-229`). | Fact | No — GS only, see §4.1 |
| **F8** | `BuildRuntimeInfo()` logged + `DumpShader()`-ed on **every draw** for `Stage::Geometry` (≈1380–1500 lines/session). Already fixed by `4681f99f`. | Fact | No (noise) |

**Recommendation (single):** land **F1’s three-line fix plus the binding-fingerprint check** from
§5 — that check turns the next crash into *“VS baked binding 7, layout has a sampled image at 7”*
instead of “device lost”. Everything else in this list is secondary.

---

## 1. Evidence re-read — the crashing pipeline has **no geometry stage**

The whole previous round (and the fork’s `OutputVertices` patch) assumed a GS pipeline. The three
crash logs say otherwise, and the proof does not depend on any theory.

### 1.1 The label of the last compile is load-bearing

`CompileModule()` logs `info.stage` (`vk_pipeline_cache.cpp:636`), formatted by
`fmt::formatter<Shader::Stage>` as `{"fs","vs","gs","es","hs","ls","cs"}`
(`src/shader_recompiler/runtime_info.h:288-295`). `bind_stage()` maps
(guest stage → logical stage) as follows (`vk_pipeline_cache.cpp:502-604`):

| `stage_enable` arm | `Stage` passed to `GetProgram` (→ log label) |
|---|---|
| `Vs` (line 599) | `Vertex` → **“vs”** |
| `EsGs` | `Fragment`, `Export`→**“es”**, `Geometry`→**“gs”** |
| `LsHs` | `Hull`→**“hs”**, `Vertex`→**“vs”** (as *domain* shader), `Local`→**“ls”** |
| `LsHsEsGs` | `hs`, `es`(as DS), `ls`, `gs` |

So `Compiling vs shader …` is either the plain-VS arm **or** a tessellation-evaluation shader. The
tie-breaker is the stage census of each session:

| log | `fs` | `vs` | `gs` | `es` | `hs` | `ls` | `cs` | cache |
|---|---|---|---|---|---|---|---|---|
| `8月.txt` | 70 | 31 | 1 | 1 | **0** | **0** | 8 | cold (no `Preloaded`) |
| `地狱特训闪退.txt` | 76 | 32 | 1 | 1 | **0** | **0** | 8 | cold |
| `medley7闪退.txt` | 107 | 45 | 2 | 2 | **0** | **0** | 10 | cold |
| `9月.txt` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | `Preloaded 391 pipelines` (line 122) |

A tessellated pipeline cannot exist without first compiling an `hs` module
(`bind_stage(Stage::Hull, …)` → `GetProgram(Stage::Hull, …)` → `CompileModule`) and, for `LsHs`, an
`ls` module. In all three crash sessions the caches were cold and **`hs`/`ls` never appear at all**
→ **no tessellation pipeline was ever created** → `Compiling vs shader 0xb3b7b928 (permutation)`
can only be the plain-VS arm.

**⇒ The crashing pipeline is `{Vertex, Fragment}` only.** `a556b0eb`’s new
`New graphics pipeline …: vs = …, fs = …, gs = …` line will print `gs = 0x0` on the next run and
confirm this in one line.

### 1.2 The permutation sequence is identical in all three logs

```
8月.txt              地狱特训闪退.txt       medley7闪退.txt
2704 vs 0xb3b7b928 (perm 0)     2605        4343
2719 vs 0xb3b7b928 (perm 1)     2620        4380
3069 fs 0xeeca4b6b (permutation) 3186       5157 / 5320 …
3305 vs 0xb3b7b928 (perm 2)  →  3458    →   6009     ← the last compile before the loss
3307 pipeline 0x59b3f0dd8289f1d6 3460       6011 pipeline 0xfa36d21f2da4ef4f
3319 device lost                 3530       6014 device lost
```

Two consequences:

* Perms 0/1 of this VS are compiled **early**, perm 2 **late**, after a run of `fs` permutations
  (8 / 8 / 36 `fs` permutation compiles before the crash). The new VS permutation is created by a
  *changed FS footprint*, exactly what F1/F2/F3 predict (the VS’s `start` depends on the FS).
* `0x59b3f0dd8289f1d6` is identical in two unrelated builds ⇒ the sequence of specialisations, and
  therefore the permutation indices, are deterministic. This is a content-deterministic bug, not a
  race and not VRAM (`1566 MiB / 6239 MiB`, `1900 MiB / 6239 MiB`).

### 1.3 `9月.txt` is *not* a cache A/B

`9月.txt` prints `Preloaded 391 pipelines` and **never compiles `0xb3b7b928` at all** (0 hits).
The clean session used a different lesson/venue (`liv_013` vs `liv_015`). The “cold compile ⇒ crash”
framing in `diagnosis.md` §2.2 is therefore a confound: the control run never reaches the offending
pipeline. Keep `pipeline_cache_enabled` out of the hypothesis list.

---

## 2. The invariant that has to hold (and where it is computed three times)

Descriptor binding numbers for a graphics pipeline are produced in **three** independent places and
must agree exactly:

| stage of the pipeline | code | buffer | image | sampler | push-constant UD |
|---|---|---|---|---|---|
| **baked into SPIR-V** (per module) | `spirv_emit_context.cpp:780,845` / `:961-970` / `:997` | `unified++` | `unified += NumBindings()` | `unified++` | `start.user_data + ud_mask.Index()` |
| **descriptor set layout** | `vk_graphics_pipeline.cpp:436-477` | `binding++` | `binding += NumBindings()` | `binding++` | n/a |
| **draw-time writes** | `vk_rasterizer.cpp:694` / `:831` / `:846` | `unified++` | `unified += array_size` (= `NumBindings`) | `unified++` | `PushUd()` `info.h:179` |
| **inter-stage chain in `GetProgram`** | `info.h:189-193` (`AddBindings`) | `buffer += buffers.size()` | **`unified += images.size()`** ✗ | `+= samplers.size()` | `+= ud_mask.NumRegs()` |

Row 2 and row 3 agree with row 1. **Row 4 does not**: `AddBindings()` is the only place that does
not account for `ImageResource::NumBindings() > 1` (`resource.h:110-115`).

`AddBindings()` is called from exactly one place — the *reuse* branch of `GetProgram`
(`vk_pipeline_cache.cpp:705`) — and its output is the `start` of the **next** stage, which is then
frozen into any permutation compiled in that same draw. That is precisely the “new permutation”
event in all three logs.

---

## 3. Findings

### 3.1 F1 — `Info::AddBindings()` under-counts image descriptor arrays (highest confidence)

**Code.**

```cpp
// src/shader_recompiler/info.h:189
void AddBindings(Backend::Bindings& bnd) const {
    bnd.buffer += buffers.size();
    bnd.unified += buffers.size() + images.size() + samplers.size();   // <-- no NumBindings()
    bnd.user_data += ud_mask.NumRegs();
}
```

versus the emitter it is supposed to mirror:

```cpp
// src/shader_recompiler/backend/spirv/spirv_emit_context.cpp:961
const u32 num_bindings = image_desc.NumBindings(info);
...
// :969
Decorate(id, spv::Decoration::Binding, binding.unified);
binding.unified += num_bindings;                                      // <-- array of descriptors
```

and the layout, which also uses `NumBindings()` (`vk_graphics_pipeline.cpp:458-466`).

`NumBindings()` is `> 1` only for `MipStorageFallbackMode::DynamicIndex`
(`resource.h:110-115`), which is set when a shader **stores to / atomically accesses an image at a
dynamic LOD** and the driver lacks image-load/store LOD
(`resource_tracking_pass.cpp:551-552, 589-594`). `supports_image_load_store_lod` is wired to
`VK_AMD_shader_image_load_store_lod` (`vk_instance.cpp:320`), i.e. **always false on the NVIDIA RTX
5060** these logs come from. So on this machine *any* dynamic-LOD image store into a multi-mip
image produces `NumBindings = last_level - base_level + 1 > 1`.

**Mechanism (VS+FS pipeline, the exact configuration of all three crashes).**

1. Draw *N*: the FS’s permutation is **reused** (`it != end`, `vk_pipeline_cache.cpp:704`), so
   `info.AddBindings(binding)` runs and under-advances `binding.unified` by
   `k = Σ (NumBindings(imageᵢ) − 1)`.
2. The VS is specialised with `start.unified = chain − k`
   (`vk_pipeline_cache.cpp:690`, `start` is compared in
   `specialization.h:226-228`, so **a new permutation is created** — the log line we see).
3. `CompileModule(new_info, …)` bakes the VS’s buffers/images/samplers at
   `chain − k, chain − k + 1, …` (`spirv_emit_context.cpp:780, 845, 969, 997`).
4. `BuildDescSetLayout()` — using the *correct* count — places the VS’s descriptors at
   `chain, chain + 1, …`. The pipeline is created without error (all baked indices are still below
   the layout’s binding count).
5. First draw: `BindBuffers()`/`BindTextures()` write the FS’s image descriptors at exactly the
   slots the VS now reads as a **UBO/SSBO**. With `robustBufferAccess2` the access is clamped to the
   range taken from `VkDescriptorBufferInfo` — which is a reinterpreted
   `{sampler, imageView, layout}` triple, i.e. a garbage device address/range → **GPU fault →
   `VK_ERROR_DEVICE_LOST` on the next submit**. (`robustness2`/`nullDescriptor` are required, so a
   *plain* out-of-bounds access cannot fault; a *forged descriptor* can.)

**Log-side check that the reuse branch really ran in the crash frame.** `AddBindings()` only runs
when a stage’s permutation is *found*, i.e. when that stage compiles nothing in this draw. In all
three logs the draw that creates the crashing pipeline compiles **only** the VS:

| log | last compile before the crash pipeline | crash pipeline | anything compiled in between? |
|---|---|---|---|
| `8月.txt` | 3069 `fs 0xeeca4b6b (permutation)` → 3071 pipeline (then 3133-3138 six pipelines, no compiles) | 3307 `0x59b3f0dd8289f1d6` | no |
| `地狱特训闪退.txt` | 3186 `fs 0xeeca4b6b (permutation)` | 3460 `0x59b3f0dd8289f1d6` | no |
| `medley7闪退.txt` | 5320 `fs 0x591d9b6f` + 5322 `vs 0x994c1eba` | 6011 `0xfa36d21f2da4ef4f` | no (6009 is the VS permutation itself) |

⇒ the FS was served by the reuse branch, so its `AddBindings()` output **is** the VS’s
`start`. (In the `medley7` log the record then continues for two more GS draws, 6012-6013, before
the submit fails — consistent with a fault that is detected, not with a host-side abort.)

**Why it explains the observations.** New permutation immediately before the loss ✔ (step 2);
loss on the first use of the new pipeline ✔ (the module is cached, the mismatch is permanent);
identical pipeline hash across builds ✔ (deterministic permutation indices); no host `ASSERT` fired
✔ (`AddBindings` has no bound check, unlike `PushUd`’s `bnd.user_data < 16`); GS theory dead ✔
(no GS stage, §1.1); stage×song specificity ✔ — the offending content is any course that (a) draws
VS `0xb3b7b928` *with descriptors*, (b) has an FS whose image store uses a dynamic LOD on a
multi-mip image, and (c) asks for a new VS vertex layout afterwards: the 5th idol’s mesh
(`azu` in August, `tak` in Hell Training) and the medley stage `三角光露营区` + `m03`.

**Prediction (falsifiable, and the reason the instrumentation in §5 prints `NumBindings`):**
the first FS permutation whose spec contains an image with `NumBindings > 1` should be the FS
permutation compiled *before* the crashing VS permutation (August: 3069; Hell Training: 3186;
medley: 5157 or 5320). If no FS in these sessions ever has `NumBindings > 1`, F1 is dead and
§3.4/§3.2 become the leading candidates.

### 3.2 F2 — `bitset.none()` short-circuit ignores `start`

```cpp
// src/shader_recompiler/specialization.h:197
bool operator==(const StageSpecialization& other) const {
    ...
    // For VS which only generates geometry and doesn't have any inputs, its start
    // bindings still may change as they depend on previously processed FS. ...
    if (bitset.none() && other.bitset.none()) {      // :222
        return true;                                 // :223
    }
    if (start != other.start) { return false; }       // :226
```

`bitset` is only set for resources whose sharp is valid:
`ForEachSharp(binding, buffers, …)` (`specialization.h:180-191`) sets it only when
`desc.GetSharp(*info)` is truthy. A stage whose only resources are *special* buffers with a null
base, or that has no resources at all, takes the short-circuit and then **reuses a module compiled
with a different `start`** — different `user_data` base. The module’s push-constant indices
(`emit_spirv.cpp:645`, `emit_spirv_context_get_set.cpp:49`) then read *another stage’s* UD dwords.
That yields wrong constants rather than an address, so by itself it is not a fault; it matters
because the same code path can also silently mix `unified` bases if such a stage ever gains a
descriptor later. Cheap hardening: delete the early return and compare `start` unconditionally
(the comment’s “redundant permutations” cost is one module).

### 3.3 F3 — new permutation compiled from a fresh `Info`, laid out from `Program::info`

```cpp
// vk_pipeline_cache.cpp:698
if (it == program->modules.end()) {
    auto new_info = Shader::Info(stage, l_stage, params);      // :699  empty resource lists
    module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);  // :700
    RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);     // :702  OLD info
    program->AddPermut(module, std::move(spec));                                     // :703
}
```

`TranslateProgram()` fills the passed `Info` (`IR::Program` holds `Info& info`,
`ir/program.h:15-21`), i.e. the module’s resource lists come from `new_info`, while
`infos[stage] = &program->info` (`vk_pipeline_cache.cpp:710`) is what
`BuildDescSetLayout()`/`BindResources()` use. The two translations normally agree because the
resource lists are code-determined, but they *can* diverge (special-buffer injection in
`shader_info_collection_pass.cpp:129-183`, `shared_memory_to_storage_pass.cpp:117-118`, and any
pass whose behaviour depends on the current sharps). Cheap and permanent guard: after
`CompileModule()`, `ASSERT_MSG(new_info.{buffers,images,samplers}.size() == info.{…}.size())`.

### 3.4 F4/F5 — fork-only changes around first-use state

* `vk_rasterizer.cpp:687-688` — `InvalidateMemoryFromGPU()` now runs for **every** written buffer
  (upstream: only `is_written && is_formatted`). It runs inside `BindBuffers()`, i.e. **after**
  `PrepareRenderState()` (`vk_rasterizer.cpp:110`) has registered the colour/depth targets in
  `bound_images`. A newly-bound storage buffer whose range overlaps a render target can therefore
  invalidate/replace an image *between* `PrepareRenderState()` and `bindDescriptorSets()`.
* `vk_rasterizer.cpp:874-877` — `texture_cache.UpdateImage(image_id)` removed for render targets;
  the fork’s own comment says the removed path “can trigger RefreshImage which calls
  EndRendering()”. Whatever it was protecting (dirty upload **and** `TrackImage`/`TouchImage`
  bookkeeping) is now skipped. Both are worth reverting in a control build, but neither is
  specific to “new permutation”, so they rank below F1–F3.

### 3.5 F6 — shader-DB metadata for new permutations is written from the wrong `Info`

`RegisterShaderMeta(info, …)` (`vk_pipeline_cache.cpp:702`) pairs the **perm-2 SPIR-V** (written at
`:700` via `RegisterShaderBinary(spv, pgm_hash, perm_idx)`) with `program->info` (perm-0 resource
lists, plus this frame’s `pgm_base`/`user_data`), and `LoadPipelineStage()` then does
`spec.info = &program->info` on a temporary `Program` that is destroyed in the reuse branch
(`vk_pipeline_serialization.cpp:252-289`) — a dangling `spec.info`. Consequences: a warm-cache run
reproduces F3’s mismatch by construction, and the dangling pointer is latent UB. Fix: serialise
`new_info`, and keep the temporary `Program` alive (or re-point `spec.info` after
`InsertPermut`).

### 3.6 F7/F8 — GS code (kept for completeness, cannot explain these crashes)

* `BuildFallbackGsCopyData()`/`FillMissingGsCopyData()` (`ring_access_elimination.cpp:19-54`) and
  the new multi-dword counting in `ParseCopyShader()` (`copy_shader.cpp:1-190`) can change
  `num_comps` → `dwords_per_vertex` (`ring_access_elimination.cpp:147-153`) → `comp_ofs` /
  `vc_read_ofs` (`:219-222`). Effect is a wrong attribute mapping in **GS** pipelines (visual
  corruption, plus the new “Missing GS copy mapping” warnings). No GPU fault: the replaced
  `ASSERT` used to abort, and every address used remains a validated attribute export.
* `BuildRuntimeInfo(Stage::Geometry)` used to `LOG_INFO` + `DumpShader()` on **every draw** —
  1380–1500 lines/session of `Geometry program: max_vert_out = …` (e.g. `medley7闪退.txt:5953-6013`).
  Fixed by `4681f99f`; note that with `dumpShaders = true` that path also wrote one file per draw.

---

## 4. Explicitly ruled out

**4.1 GS ring / copy shader / `OutputVertices`** — `RingAccessElimination()` and the whole
`esgs`/`gsvs` path (`ring_access_elimination.cpp:56-249`, `BuildRuntimeInfo` Geometry arm) only run
for `Stage::Geometry` programs. The crashing pipeline has no geometry stage (§1.1), so the clamp
(`:172-184`), `max_vert_out = 8/20`, the ring arithmetic at `:219-222` and
`ASSERT(offset < es_info.vertex_data_size*4)` at `:111` are all irrelevant to this crash. (Also, the
observed configs — `vert_itemsize 12 / esgs 8` and `28 / 4` — keep `dwords_per_vertex * 4` ≥
`esgs_ring_itemsize`, so no ring index can exceed the declared vertex stride.)

**4.2 Vertex input (area B).** On this device `VK_EXT_vertex_input_dynamic_state` is present, so
`GetVertexInputs()` is evaluated per draw (`buffer_cache.cpp:185-192`) with the *current* user data;
formats come from `LiverpoolToVK::SurfaceFormat()` (`liverpool_to_vk.cpp:772-779`, `ASSERT_MSG` on unknown
formats — never fired) and the shader’s input *type* is specialised on `NumberClass`
(`specialization.h:100-116`, `spirv_emit_context.cpp:316-337`). Attribute location = binding =
`attrib.semantic` (`vk_graphics_pipeline.cpp:394-421`) assumes dense semantics — true for
`ParseFetchShader()` output. A mismatch degrades to zeros because `nullDescriptor` is mandatory
(`vk_instance.cpp:273-278`) rather than faulting. Not the crash.

**4.3 Tessellation/pipeline stage ordering.** Suspicious at first: `RefreshGraphicsStages()` binds
FS → TCS → TES → Vertex → Geometry for `LsHs`, while the layout/`BindResources()` iterate
`GetStages()`. It is correct — `LogicalStage` is deliberately ordered `Fragment,
TessellationControl, TessellationEval, Vertex, Geometry` (`runtime_info.h:28-35`, comment: “Vertex
intentionally comes after TCS/TES due to order of compilation”) and the draw-time chain
(`vk_rasterizer.cpp:439-449`) uses the same order. (Moot anyway: no tessellation in these sessions.)

**4.4 Draw-time descriptor writes, `uses_push_descriptors`, `desc_heap`.** `BindBuffers()`/
`BindTextures()` advance exactly like `BuildDescSetLayout()` (table §2), and layout size vs
`MaxPushDescriptors()` (`vk_graphics_pipeline.cpp:477`) is per-pipeline-consistent.

**4.5 VRAM / OOM.** 1566 MiB and 1900 MiB of a 6239 MiB budget; no allocation failures in any log.

**4.6 “Cold pipeline cache causes it”.** See §1.3 — `9月.txt` never compiles the offending shader.

---

## 5. Recommendation for the next build (single most informative change)

The team has already made the crash *locatable* (`4681f99f` fault report, `a556b0eb` pipeline key,
`f8bb2b33` draw flight recorder). What is still missing is the one thing that converts a fault
address into a cause: **the descriptor-binding fingerprint of each stage, checked against the
layout at pipeline creation.** It is ~35 lines, read-only in behaviour, and it either proves F1/F2/F3
or eliminates the entire “wrong binding numbers” family in one run.

**(a) Fix the chain (3 lines, `src/shader_recompiler/info.h:189`).**

```cpp
void AddBindings(Backend::Bindings& bnd) const {
    bnd.buffer += buffers.size();
    u32 image_bindings = 0;
    for (const auto& image : images) {
        image_bindings += image.NumBindings(*this);   // mirror spirv_emit_context.cpp:961-970
    }
    bnd.unified += buffers.size() + image_bindings + samplers.size();
    bnd.user_data += ud_mask.NumRegs();
}
```

**(b) Record what each module was compiled with, and assert it equals the layout.**
In `PipelineCache` add `std::array<Shader::Backend::Bindings, MaxShaderStages> stage_starts{};`,
set it from the *used* module in `GetProgram()`
(`program->modules[perm_idx].spec.start`, both branches, and in `LoadPipelineStage()` after
`Deserialize`), then in `GetGraphicsPipeline()` right after `RefreshGraphicsKey()`:

```cpp
Shader::Backend::Bindings chain{};
for (u32 i = 0; i < MaxShaderStages - 1; ++i) {          // logical-stage order, skip Compute
    const auto* info = infos[i];
    if (!info) continue;
    u32 image_bindings = 0;
    for (const auto& img : info->images) image_bindings += img.NumBindings(*info);
    const auto& baked = stage_starts[i];
    if (baked != chain) {
        LOG_CRITICAL(Render_Vulkan,
                     "binding chain mismatch: {} baked unified/buffer/ud = {}/{}/{} layout = {}/{}/{} "
                     "(buffers {}, image_bindings {}, image_resources {}, samplers {}, ud {})",
                     info->l_stage, baked.unified, baked.buffer, baked.user_data, chain.unified,
                     chain.buffer, chain.user_data, info->buffers.size(), image_bindings,
                     info->images.size(), info->samplers.size(), info->ud_mask.NumRegs());
    }
    chain.buffer += info->buffers.size();
    chain.unified += info->buffers.size() + image_bindings + info->samplers.size();
    chain.user_data += info->ud_mask.NumRegs();
}
```

**How to read the next log.**

* `baked.unified < layout.unified` by `k > 0` **and** `image_bindings > image_resources` → **F1
  confirmed**, and `k` tells you which descriptor the VS stole. Fix (a) alone then fixes the crash.
* `baked != layout` with `image_bindings == image_resources` → **F2/F3**, look at which stage and
  which counter differs (`user_data` → F2; `unified` with equal counts → F3).
* No mismatch printed and the device still dies → the descriptor family is excluded; the fault
  report from `4681f99f` then discriminates the two remaining classes: an *illegal address* means
  stale/destroyed handles (go to F4/F5 and `InvalidateMemoryAsync`’s by-reference capture), and a
  *hang* (`nvlddmkm` 4101, no fault address) means a mis-specialised shader driven loop → dump
  `vs_0x00000000b3b7b928_2.spv` with `dumpShaders = true` and diff it against `_1.spv`.

**Free companions, no code:** run the same course with `dumpShaders = true` (gives
`vs_0x…b3b7b928_{0,1,2}.{bin,spv}` + `copy.bin`) and with `vkValidation = true` +
`vkValidationSync = true`; VVL reports the descriptor/layout mismatch class of F1/F3 at the
offending draw, and `vkValidationSync` makes the failing submit the offending one.

---

## 6. If the fingerprint comes back clean (order of next steps)

1. Turn F2’s short-circuit into a real comparison and re-run (one line, no risk).
2. Revert the two fork-only rasterizer hunks in a control build (`vk_rasterizer.cpp:687`,
   `:874`) — this is the only A/B that isolates the fork from upstream, and it is now cheap because
   the flight recorder keeps the crash attributable.
3. Fix the async-download dangling capture (`buffer_cache.cpp:125-175`, `[&]` closure outliving its
   frame) because a destroyed `VkBuffer` still referenced by a descriptor is the *other* way to
   fault a GPU that has `robustness2` and `nullDescriptor`.

---

## 7. Appendix — log evidence used

| log | line | event |
|---|---|---|
| `8月.txt` | 2704 / 2719 | `Compiling vs shader 0xb3b7b928` (perm 0 / perm 1) |
| `8月.txt` | 3069, 3186 | `Compiling fs shader … (permutation)` — the FS specialisations between the VS perms |
| `8月.txt` | 3305 / 3307 / 3319 | VS perm 2 → pipeline `0x59b3f0dd8289f1d6` → device lost |
| `地狱特训闪退.txt` | 2605 / 2620 / 2700 / 2703 / 3458 / 3460 / 3530 | same pattern, `es 0x2b5ab32b` + `gs 0xf3bd17fb` created at 2700-2703, loss after pipeline `0x59b3f0dd8289f1d6` |
| `medley7闪退.txt` | 4343 / 4380 / 6009 / 6011 / 6014 | VS perms 0/1, perm 2 → pipeline `0xfa36d21f2da4ef4f` → loss; 2 further GS draws (6012-6013) recorded before the failing submit |
| `medley7闪退.txt` | 5953-6013 | the per-draw `Geometry program:` spam (F8) |
| `9月.txt` | 7 / 122 | `pipeline_cache_enabled (0 -> 1)`, `Preloaded 391 pipelines`; zero occurrences of `0xb3b7b928` |

**Source files consulted (read-only):** `vk_pipeline_cache.{h,cpp}`, `vk_graphics_pipeline.{h,cpp}`,
`vk_pipeline_common.{h,cpp}`, `vk_pipeline_serialization.{h,cpp}`, `vk_rasterizer.{h,cpp}`,
`vk_instance.{h,cpp}`, `liverpool_to_vk.cpp`, `buffer_cache.{h,cpp}`, `shader_recompiler/info.h`,
`specialization.h`, `resource.h`, `params.h`, `runtime_info.h`, `backend/bindings.h`,
`backend/spirv/{emit_spirv,spirv_emit_context}.cpp`, `frontend/{fetch_shader.h,copy_shader.cpp}`,
`ir/passes/{ring_access_elimination,resource_tracking_pass,shader_info_collection_pass}.cpp`,
`ir/program.h`, `recompiler.cpp`, plus `git diff upstream/main...HEAD` and
`git log upstream/main..HEAD` for the fork-only surface.
