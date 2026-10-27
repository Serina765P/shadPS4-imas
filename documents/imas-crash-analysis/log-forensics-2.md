# 日志取证 v2：medley7 与 地狱特训 Pro 崩溃日志（build d7e66f5e）

**Scope.** Read-only forensic analysis of the two new crash logs produced by build
`d7e66f5e0ecb91b4e962313cd3a47d2a969b5879` (branch `codex/idolmaster-release-gs-device-lost`, the build that
first logs GS registers and device-loss context), cross-checked against the two reference logs from build
`26d27aef048d4fbc2774a42878c7742850603134` (branch `release`). No source files were modified.

| # | File | Build | Size | Lines | Outcome |
|---|------|-------|------|-------|---------|
| A | `C:\Users\Serina\Downloads\medley7闪退.txt` | d7e66f5e | 952,668 B | 6,017 | `VK_ERROR_DEVICE_LOST` @6014 |
| B | `C:\Users\Serina\Downloads\地狱特训闪退.txt` | d7e66f5e | 489,862 B | 3,533 | `VK_ERROR_DEVICE_LOST` @3530 |
| C | `C:\Users\Serina\Downloads\8月.txt` | 26d27ae | 458,556 B | 3,319 | `VK_ERROR_DEVICE_LOST` @3318 |
| D | `C:\Users\Serina\Downloads\9月.txt` | 26d27ae | 437,877 B | 3,186 | **clean exit** @3186 (`cache_storage.cpp:137 Close: Cache dumped`) |

All four are single sessions from emulator start to exit/abort; there are no timestamps anywhere.

---

## 0. Executive summary

1. **Both new crashes reproduce the same shape as the August crash, and it is a three-step sequence, not a
   one-off.** In *every* crashing log the vertex shader `0xb3b7b928` is compiled three times: the base program,
   then one permutation that runs fine for hundreds/thousands of lines, then a **second permutation that is
   followed within 2–3 log lines by a new graphics pipeline and then by the device loss**:

   | Log | base `0xb3b7b928` | permutation #1 (survives) | permutation #2 → pipeline → loss |
   |-----|-------------------|--------------------------|----------------------------------|
   | A medley7 | 4343 → `0x3cd7c4a2bee1c71a` | 4380 → `0xe54f252e3309840d` | **6009 → `0xfa36d21f2da4ef4f` @6011** |
   | B 地狱特训 | 2605 → `0x3cd7c4a2bee1c71a` | 2620 → `0xe54f252e3309840d` | **3458 → `0x59b3f0dd8289f1d6` @3460** |
   | C 8月 | 2704 → `0x3cd7c4a2bee1c71a` | 2719 → `0xe54f252e3309840d` | **3305 → `0x59b3f0dd8289f1d6` @3307** |
   | D 9月 | *(none — 0 shader compiles, cache warm)* | — | — |

   The first two pipelines are **bit-identical across all three crashing sessions**, including across two
   different builds. The third pipeline differs between the medley case and the other two, but each of the two
   third-stage hashes occurs **exactly once in its log** — it is created and never re-used before the crash.

2. **The crash is detected one submit late, not at the moment of the fault.** `vk_scheduler.cpp:194` logs the
   line only when `vkQueueSubmit` itself returns `vkResult::eErrorDeviceLost`; a GPU fault that happened in an
   already-submitted command buffer surfaces on the *next* submit. Therefore the log cannot attribute the loss
   to the pipeline created 2 lines earlier — only to "the last few submissions before this one". This is the
   single most important methodological constraint in this whole analysis (§1.3).

3. **The medley crash is NOT at the song/section boundary.** `stage_scene.luac` @4304 (the only scene-boundary
   marker in the log) is followed by the last asset request of the whole session at **4402** — after which the
   session performs **1,611 more log lines with exactly zero file opens** while drawing continuously (1,377
   per-draw GS log lines ≈ 690 frame-pairs, i.e. the order of ten seconds at 60 fps). The loss happens
   mid-performance, well after the boundary, on the first frame that needs a brand-new pipeline.

4. **The medley's transition to the next section loads no assets at all.** The five songs' dance motions
   (`dan_m03_d01…d05` + `_loop`) were all pre-loaded at 4234–4257, before the boundary. The only new state at
   the crash is a **shader permutation + graphics pipeline** (6009–6011). No costume, effect, camera or audio
   asset is requested anywhere near the loss.

5. **The Hell Training crash IS at a load boundary — of the live's dance-motion stream.** The fifth and last
   unit member finishes streaming at 3522, then `motion/dance/kov/dan_kov_d01.nuanmb` (3524), `…d01_loop`
   (3526) and `…d02` (3528) are opened and closed, and the loss is reported 2 lines later. The clean reference
   session loads **ten** such files (`d01…d05` + `_loop`, D log 3079–3097) followed by the camera set
   (3099–3146) and the rhythm-game UI (3148–3168). B dies after **three of ten**.

6. **The fix did move the crash later, but only within the same load phase.** C (August) dies while the **third**
   of five unit members is still streaming (at `chr_hair_priv_001_azu.jxk` @3316); B (fixed build) gets through
   **all five** members (yuk, ami, har, mam, tak) and into the dance-motion stream. Roughly 28 more asset
   requests, 94 more log lines. Same stage (`bg3d_liv_015`), same song (`kov`), same crash pipeline
   `0x59b3f0dd8289f1d6`, same warning-free tail.

7. **The one confound nobody has priced in yet: every crashing session ran with `PipelineCacheEnabled: false`,
   and the only surviving session ran with it `true`.** D preloaded **391 pipelines** from disk
   (`vk_pipeline_serialization.cpp:364 WarmUp: Preloaded 391 pipelines` @122) and compiled **0** shaders and
   **0** pipelines; A/B/C compiled 132/130/124 graphics pipelines and 166/118/109 shader modules at runtime.
   D therefore cannot be used as a control for "this content works" — it never compiled the pipeline at all.
   See §2 and H2.

8. **No warning or error precedes either loss.** In both new logs the complete set of renderer warnings is
   *identical* to the set emitted at boot (19 `vk_instance.cpp` capability/format warnings + 1
   `emit_spirv.cpp:439` denorm warning + 1 `texture_cache.cpp:260` `ResolveDepthOverlap` warning at 2689/2602).
   No unhandled opcode,
   no unimplemented texture format, no `sceKernel` failure, no allocation failure anywhere near the crash. The
   only warning adjacent to the loss is `recompiler.cpp:36 First instruction is not s_mov_b32 vcc_hi, #imm`,
   which fires for **every** shader (155× in A, 109× in B) and is not a discriminator.

9. **GPU memory is irrelevant.** 1,900 MiB / 6,239 MiB (A) and 1,566 MiB / 6,239 MiB (B). No OOM, no
   `vkAllocateMemory` failure, no image-creation error in either log.

10. **Ranked reading:** a *newly created pipeline whose first draws fault or hang the GPU* (H1, STRONG), with the
    caveat of #2; the cold-compile/cache-off path as the real discriminator (H2, MEDIUM-HIGH and the cheapest
    experiment to falsify); everything else (GS OutputVertices — already falsified by the Lead; TDR on a
    pathological draw; resource lifetime) is MEDIUM or below. §9 and §10.

---

## 1. Method and how to read these logs

### 1.1 Counts per log (the numbers every later claim rests on)

| Metric | A medley7 | B 地狱特训 | C 8月 | D 9月 |
|---|---|---|---|---|
| Total lines | 6,017 | 3,533 | 3,319 | 3,186 |
| `Kernel.Fs` lines (file open/close/stat) | 2,540 | 1,624 | — | — |
| Asset `open:` requests / unique paths | 1,109 / 855 | 649 / 620 | 594 / 589 | 685 / 662 |
| `BuildRuntimeInfo: Geometry program:` lines | **1,380** | **47** | 0 (logging absent) | 0 (logging absent) |
| `Render.Vulkan` lines total | 1,688 | 303 | — | — |
| Graphics pipelines created | 132 | 130 | 124 | **0** |
| Compute pipelines created | 10 | 8 | 8 | **0** |
| Shader modules compiled (vs/fs/gs/es/cs) | 45/107/2/2/10 | 32/76/1/1/8 | 31/70/1/1/8 | **0/0/0/0/0** |
| `recompiler.cpp:36` warnings | 155 | 109 | — | — |
| `Skipped N duplicate` lines / total suppressed / max N | 40 / 633 / 299 | 41 / 683 / 299 | 19 / 356 / 299 | 23 / 369 / 299 |
| `sceKernelStat: error = 2` probes | 322 | 319 | — | — |

Arithmetic checks that catch transcription errors: A: 1,688 − 1,380 = 308 = 132 + 10 + 166. B: 303 − 47 = 256 =
130 + 8 + 118. Filtering out `Kernel.Fs` + `Geometry program:` leaves 2,097 signal lines in A and 1,862 in B.

### 1.2 `Geometry program:` is a per-draw line, not a pipeline-creation line

Confirmed in source: `PipelineCache::GetProgram` calls `BuildRuntimeInfo(stage, l_stage)` unconditionally at
entry (`vk_pipeline_cache.cpp:654`), and in build d7e66f5e `BuildRuntimeInfo`'s Geometry branch emitted the
`LOG_INFO(... "Geometry program: max_vert_out = {} …")`. `GetProgram` is called once per enabled stage from
`RefreshGraphicsStages()`, which runs on every draw whose state changed — i.e. **one `Geometry program:` line per
draw that has a geometry shader enabled**. `GetGraphicsPipeline()` only runs when the key changed, hence
`Compiling graphics pipeline …` really is once per creation.

(The Lead's working tree has already moved this line from `BuildRuntimeInfo` (per-fetch) to
`GetGraphicsPipeline` (per-creation, tagged with the pipeline hash) — that change makes the next round of logs
much easier to read; it is *not* in these logs.)

Two useful consequences:

* The two distinct GS programs give a **frame/draw counter**: A has exactly two alternating GS pipelines,
  `max_vert_out = 8, vert_itemsize = 12, esgs_ring_itemsize = 8, mode = 3, copy shader 0xf3bd17fb` (**688**
  fetches) and `max_vert_out = 20, vert_itemsize = 28, esgs_ring_itemsize = 4, mode = 3, copy shader
  0x395cf4e2` (**692** fetches). They alternate 1:1 → an ordinary two-pass frame (≈690 frame-pairs of drawing).
  B has only the first one (**47** fetches) — its main pass does not use a GS pipeline.
* Continuous GS fetches right up to the last line (**no gap > 72 lines** in the whole 1,377-line run, median gap
  1) prove the game was **not** stalled/loading when A died.

### 1.3 The loss is reported one submit after it happened (source-verified)

```cpp
// src/video_core/renderer_vulkan/vk_scheduler.cpp:194-220 (working tree; d7e66f5e is identical minus :211-218)
auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
if (submit_result == vk::Result::eErrorDeviceLost) { LOG_CRITICAL(... "Device lost on queue submit: signal tick = {} …"); }
ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
```

`vkQueueSubmit` returns `VK_ERROR_DEVICE_LOST` **only when the device is already lost**; a fault caused by the
commands inside the submit being issued is reported on a later submit. So:

* the *reporting* line marks the first submit after the fault;
* the offending GPU work is contained in the submissions **before** it — in A that is at most the preceding
  ~88 geometry fetches (the last 100 raw lines are 88% GS draws), in B it is the work submitted while the last
  ~69 lines of asset streaming were running;
* the newly created pipeline *is* a good suspect (its first draws are exactly in that window) but the log alone
  cannot prove it. `signal tick` is the master-semaphore value, i.e. how many submits had already been made:
  **8,980** in A, **2,254** in B — the session had been submitting happily for a long time.

`VK_EXT_device_fault` **is** available on this machine (it is in the enumerated extension list, A/B line 63), so
the Lead's new `instance.GetDeviceFaultReport()` branch (`vk_scheduler.cpp:211-218`) should print the actual
faulting address/instruction on the next run. That is the cheapest way to settle H1 vs H4.

### 1.4 `Skipped N duplicate messages..`

Means "the message on the preceding line repeated N more times consecutively". Useful examples:

* `4391` GS-fetch `max_vert_out = 20 … 0x395cf4e2`, then `4392 Skipped 182 duplicate messages..`,
  `4393` an `event_flag.cpp` warning, `4394 Skipped 59 …`, `4395` the same GS fetch, `4396` the same warning,
  `4397 Skipped 37 …` → **281 consecutive draws of the single-pass GS pipeline** while the other pass was idle.
  This is the medley's stage-scene set-up, right before the `chemicallight` models at 4398–4402.
* `2432 Skipped 299 duplicate messages..` appears in all four logs at the same position (right after
  `sceAppContentGetEntitlementKey`, before `save_data.png`) — boot-time noise, ignore.

---

## 2. The four logs side by side

Taken from the `[Config]` block each log prints at start-up (A/B lines 20–51, C/D lines 13–58).

| Setting | A medley7 | B 地狱特训 | C 8月 | D 9月 |
|---|---|---|---|---|
| Revision | **d7e66f5e** | **d7e66f5e** | 26d27ae | 26d27ae |
| `Vulkan PipelineCacheEnabled` | **false** | **false** | **false** | **true** |
| `Vulkan PipelineCacheArchived` | false | false | false | false |
| `GPU readbacksMode` | 2 | 2 | 2 | 2 |
| window override (`PrintChangedSummary`) | **none** | **none** | 1280×720 → 1920×1080 | 1280×720 → 1920×1080 |
| `vkValidation` / `Sync` / `Gpu` / `crashDiagnostics` | false/false/false/false | false/false/false/false | false/false/false/false | false/false/false/false |
| `shouldDumpShaders` | false | false | false | false |
| GPU / driver | RTX 5060 Laptop, NVIDIA 621.1.0.0, Vulkan 1.4.357 | same | same | same |

Identical: GPU, driver, CPU (i7-13700HX), RAM, OS, game (`CUSA09153`, app 01.08), warnings from
`vk_instance.cpp` (17 capability/format warnings), `emit_spirv.cpp:439`, `texture_cache.cpp:260`.

**Differences that matter:**

* `PipelineCacheEnabled` — the crashing logs recompile everything; the clean log compiled nothing and preloaded
  391 pipelines (`vk_pipeline_serialization.cpp:364` @D:122). D's log has **zero** `CompileModule`,
  **zero** `GetGraphicsPipeline` and **zero** `GetComputePipeline` lines, which is why `0xb3b7b928` does not
  appear in it *at all* — not because the session avoided the shader.
* A/B are one build apart from C/D, so any A/B↔C/D difference is *code + cache + content* at once.
* A logs `Vulkan vkValidationCore: true` at line 23 (all four do) — validation core is on but there is no
  validation output, which is consistent with a device loss rather than an API misuse.

> **Conclusion for the team:** D (9月) is *not* a "this content works" control; it is a "warm pipeline cache"
> datapoint. Any hypothesis that says "content X is broken" must explain why a cache-warm run of comparable
> content never even builds X's pipeline. Conversely, H2 below is one config flip away from being testable.

---

## 3. Crash A — medley7 (`kimi wa relations -mix medley`, song id `m03`, stage `arn_004`)

### 3.1 Phase map (line numbers, from the asset stream)

| Lines | Phase | Key evidence |
|---|---|---|
| 1–~1440 | boot, linker, sysmodules, tiling/compute pipeline warm-up | `Loader … Run: Revision d7e66f5e…` @16, 438 `linker.cpp` records |
| 1446–1500 | title/loading UI | `effect/stage/eff_stage_mirror02.jxk` @1456, `lua/ui/loading.luac` @1480, `pause.luac` @1490 |
| 2465–2560 | title → S4U mode | `lua/ui/title_main.luac` @2465, `lua/ui/s4u.luac` @2538, `2d_ch/s4u/s4u.jxk` @2540, `bgm_s4u.nus3bank` @2546 |
| 2578–3340 | commu / skill-catalog / popup loop (four rounds) | `motion/event/sg/sg05_cucs_00{1..4}_n0*` @2578, 2895, 3090, 3332 |
| 2812–2822 | **medley unit set-up UI** | `lua/ui/unit_setup_medley.luac` @2816, `unit_setup_normal/allstar/common` @2814/2818/2820/2822 |
| 3023 | stage select | `lua/ui/stage_select.luac` @3023 |
| 3211–3214 | song select | `lua/ui/song_select.luac` @3211, `song_select_common.luac` @3214 |
| 3470–3560 | **live set-up begins**: appeal cut-ins, timing sheet | `cutin_sheet/appeal/apl_u01..u05_*` @3470–3529, `effect/appeal/eff_app_cute_u01/02/03/05` @3531–3537, `timing_sheet/tsv_m03.jxk` @3551, `tsp_m03.jxk` @3555 |
| 3573–3581 | medley-specific cut-ins | `cutin_sheet/song/mdy_m03_07{a,b,c}_00*.jxk` @3573–3581 |
| 3585–3829 | 245 appeal motions (`dance_etc/ap/ap01..ap05_{comn,cucs}_00x_n0x`) | first @3585, last @3829 |
| 3951–4014 | shared dance motions | `dance_etc/bd/bd05_001_n01..n07` @3951–3965, `gd/gd01_001_<chara>` @3967–3979, `gr/gr01_001_<chara>` @3981–3993, `bs/bs01_har.jxk` @4010, `bs03_005.jxk` @4012, `bs05_003.jxk` @4014 |
| 4034–4047 | **stage `arn_004`** | `bg3d/arn_004/bg3d_arn_004.jxk` @4034, `_layout` @4037, `lit_arn_004.jxb` @4039, `motion/medley_etc/pth/mdy_pth_arn_004.nuanmb` @4045, `meta_medley_direction_param_table.jxb` @4047 |
| 4049–4059 | medley choreography | `motion/medley_etc/mdy_m03_07{a,b,c}.jxk` @4049/4051/4053, `motion/camera/mdy/cam_mdy_m03_07{a,b,c}.jxk` @4055/4057/4059 |
| 4077–4226 | **7 unit members' costume meshes** | har, chi, yuk, yay, rit, azu, ior — each `chr_orna_cute_<chara>.jxk` + `chr_body_cute_112_a.jxk` + `acc3_head_033/acc3_hand_122/acc3_foot_028` |
| 4234–4257 | **all five medley songs' dance motions** | `motion/dance/m03/dan_m03_d01…d05` + `_loop` (10 files, one pair per song) |
| **4304–4310** | **scene boundary: stage scene UI** | `files/lua/ui/stage_scene.luac` @4304, `2d_ch/stagescene/stagescene.jxk` @4306, `popup.jxk` @4308, `popup.luac` @4310 |
| 4312–4316 | previous section's audio torn down | 5× `sceAjmInstanceDestroy` (instances 16386, 16387, 16389, 16390, 16391) |
| 4317–4444 | 28 new pipelines for the new scene (incl. both GS pipelines) | `cs 0x4160fb9f` @4317, `gs 0x395cf4e2` @4325 → pipeline `0x9b6a6320af73a729` @4327, `vs 0xb3b7b928` @4343 → `0x3cd7c4a2bee1c71a` @4345, `vs 0xb3b7b928 (permutation)` @4380 → `0xe54f252e3309840d` @4382, `gs 0xf3bd17fb` @4409 → `0xb3c99c2279216186` @4411 |
| 4398–4402 | **last asset request of the session** | `effect/chemicallight/chemicallight_model_high.jxk` @4398, `_middle.jxk` @4400, `_low.jxk` @4402 |
| 4551–4592 | next section's audio starts | 8× `sceAjmInstanceCreate` (flags `0x11` ×2, `0x9` ×6) |
| 4673–4743 | **2D transition: 21 new FS permutations, 0 GS draws** | 21 `Compiling fs shader … (permutation)` + `GetGraphicsPipeline` pairs; this is the longest GS-free stretch of the session (gap 4672→4744 = 72 lines) |
| 4743–6008 | continuous rendering + audio-instance churn | 1,377 GS fetches total after 4402; ~30 more pipelines; ~60 AJM create/destroy |
| **6009–6011** | **new VS permutation + new pipeline** | `Compiling vs shader 0xb3b7b928 (permutation)` @6009, `recompiler.cpp:36` warning @6010, `Compiling graphics pipeline 0xfa36d21f2da4ef4f` @6011 |
| 6012–6013 | two GS fetches of the frame's two passes | `max_vert_out = 8 … 0xf3bd17fb` @6012, `max_vert_out = 20 … 0x395cf4e2` @6013 |
| **6014** | **device lost on submit** | `vk_scheduler.cpp:205 … signal tick = 8980, waiting on 0 semaphore(s), signaling 1 semaphore(s), device memory 1900 MiB / 6239 MiB, thread = 'shadPS4:GpuCommandProcessor'` |

### 3.2 The last 400 lines (5614–6013) — subsystem census

| Category | Count |
|---|---|
| GS draw lines (`Geometry program:`) | **377** (94% of the window) |
| `Lib.Ajm` instance create | 10 |
| `Lib.Ajm` instance destroy | 4 |
| `Skipped N duplicate` | 6 |
| `CompileModule` | 1 (`vs 0xb3b7b928 (permutation)` @6009) |
| `GetGraphicsPipeline` | 1 (`0xfa36d21f2da4ef4f` @6011) |
| `Kernel.Fs` | **0** |
| Warnings / errors | 1 (`recompiler.cpp:36` @6010 — fires per shader, not a signal) |

**Reading.** The last 400 lines contain *no* asset loads, *no* resource churn beyond AJM voice instances, and
94% drawing. The GPU thread had been issuing a steady two-pass frame loop for ~690 frames and then, on one
frame, needed a pipeline it had never built. Two lines later `vkQueueSubmit` came back `E_DEVICE_LOST`.

### 3.3 Answer to "what marks *relations intro* ending?"

There are exactly three candidate markers, in this order:

1. `files/lua/ui/stage_scene.luac` @**4304** + `2d_ch/stagescene/stagescene.jxk` @4306 — the stage-scene UI
   module. This is the only load in the log that looks like a scene controller. It is immediately preceded by
   the five-song dance-motion preload (4234–4257) and immediately followed by the teardown of the previous
   section's five AJM instances (4312–4316).
2. The AJM teardown/create pair 4312–4316 → 4551–4592.
3. `effect/chemicallight/*` @4398–4402 — the pen-light effect models for the stage scene.

**However**, the loss is **1,611 lines / ≈690 rendered frames later** (6009), and the "next song" never loads a
single asset of its own in this log. So: the *boundary* is at 4304; the *crash* is not. Whatever changes at the
crash is a **render-state change only** (new shader permutation → new pipeline), i.e. the first draw that needs a
mesh/material/state combination not yet seen in this session. That fits "a member who has not been on camera yet
becomes visible" or "a costume variant appears for the first time" far better than "a new song starts".

---

## 4. Crash B — 地狱特训 Pro (song `kov`, stage `bg3d_liv_015`)

### 4.1 Phase map

| Lines | Phase | Key evidence |
|---|---|---|
| 1–~1440 | boot (identical to A except one memory-manager address) | first 42 lines byte-identical |
| 1456–1500 | title/loading | `eff_stage_mirror02.jxk` @1456, `loading.luac` @1480 |
| 2465–2538 | title → office, first commu | `title_main.luac` @2465, `gscript/ex2/tak/tak_mon_02a.jxk` @2510, `bg3d/ofc_001/*` @2530–2538 |
| 2542–2592 | commu #1: tak, hib | `chr_soul_tak` @2542 … `chr_face_hib` @2574, `com_tak` @2564, `com_hib` @2592 |
| 2851–2960 | skill catalog, mail, office #2 | `gscript/ex2/tca/…/tca_brm_001.jxk` @2851, `bg3d/ofc_002/*` @2949–2960, `mail*.luac` @2898–2906 |
| 3118–3154 | schedule / live-select UI | `gscript/ex2/shk/*` @3118–3126, `bg2d_stage_003` @3130, `live_select.luac` @3146, `schedule*.luac` @3152–3154 |
| 3191–3233 | live-select thumbnails | `bg2d_stage_015` @3191, `bg2d_stage_001` @3204, `bg2d_stage_028` @3224 |
| 3248–3263 | back to office | `gscript/ex2/all/all_gol_459.jxk` @3248, `bg3d/ofc_001/*` @3251–3263 |
| 3302–3378 | **live set-up begins** | `se_rhythm.nus3bank` @3302, `fumen/game/kov/exl_kov_p.jxb` @3304 (the **chart**), appeal cut-ins `apl_u01_jktk_001/002` @3308/3310, `effect/appeal/eff_app_jktk_u01/02/03/05` @3312–3320, `cutin_sheet/otefuri/ote_459` @3322, `cutin_sheet/burst/exb_u01_004_liv` @3324, `exb_u01_yuk_liv` @3326, `timing_sheet/tsv_kov` @3331, `tsp_kov` @3336, `dance_etc/ap/ap01_jktk_*` @3340–3348, `dance_etc/jk/jk05_001_n01..n05` @3361–3369, `dance_etc/bs/bs01_004.jxk` @3376, `bs01_yuk.jxk` @3378 |
| 3382–3392 | camera + song audio | `motion/camera_etc/bs/cam_bs01_yuk_liv.nuanmb` @3382, `cam_bs01_004_liv` @3380, `song3_kov_bgm.nus3bank` @3389, `fumen/game/kov/exl_kov_yell.jxb` @3392 |
| 3394–3411 | **stage `liv_015`** | `bg2d_163.nutexb` @3394, `bg3d/liv_015/bg3d_liv_015.jxk` @3397, `_layout` @3399, `lit_liv_015.jxb` @3401, `motion/medley_etc/pth/mdy_pth_liv_015.nuanmb` @3407, `effect/burst/eff_bst_keep_002/001.jxk` @3409/3411 |
| 3413–3440 | unit member 1/5 — **yuk** | `chr_soul_yuk` @3413, face/eyes/mouth/params @3415–3423, `chr_hair_priv_001_yuk` @3425, `chr_body_exte_108_a.jxk` @3427, `acc3_head_135` @3431/3433, `song3_kov_yuk` (absent) @3436, `voice_stg_{com,mdl}_yuk` @3438/3440 |
| 3442–3479 | members 2/5 (**ami**) and 3/5 (**har**) | ami: soul @3442 … hair @3456, **`Compiling vs shader 0xb3b7b928 (permutation)` @3458 → pipeline `0x59b3f0dd8289f1d6` @3460**, orna @3462, song bank absent @3465, voices @3467/3469; har: orna @3471, song bank absent @3474, voices @3476/3478 |
| 3480–3501 | member 4/5 — **mam** | soul @3480 … hair @3492, orna @3494, song bank absent @3497, voices @3499/3501 |
| 3503–3522 | member 5/5 — **tak** | soul @3503 … hair @3515, song bank absent @3518, voices @3520/3522 |
| **3524–3528** | **dance-motion stream starts** | `motion/dance/kov/dan_kov_d01.nuanmb` @3524 (+close @3525), `dan_kov_d01_loop` @3526 (+close @3527), `dan_kov_d02` @3528 (+close @3529) |
| **3530** | **device lost on submit** | `vk_scheduler.cpp:205 … signal tick = 2254, waiting on 2 semaphore(s), signaling 2 semaphore(s), device memory 1566 MiB / 6239 MiB, thread = 'shadPS4:PresentThread'` |

### 4.2 The last 400 lines (3130–3529) — subsystem census

| Category | Count |
|---|---|
| `Kernel.Fs` open/close | 259 |
| `Kernel.Fs` errors (`sceKernelStat: error = 2` / "file does not exist") | 52 |
| `Lib.Ajm` create / destroy | 26 / 14 |
| GS draw lines | 16 |
| `Skipped N duplicate` | 16 |
| `Kernel.Event` `ThreadPriority attr is not supported!` warnings | 8 |
| `CompileModule` | 2 (`fs 0xeeca4b6b (permutation)` @3186, `vs 0xb3b7b928 (permutation)` @3458) |
| `GetGraphicsPipeline` | 3 (`0x709f65b9a94ee0bd` @3188, `0x1f707112ae2d2720` @3267, `0x59b3f0dd8289f1d6` @3460) |

**Reading.** B's last 400 lines are dominated by *streaming*: 259 file open/close pairs, i.e. the live is
loading its unit and its dance motions while the PresentThread keeps presenting. The last GS fetch is at 3461,
69 lines before the loss — i.e. unlike A, B's final ~69 lines contain **no GPU draw log at all**, only asset
I/O and AJM churn. The loss is reported on the PresentThread, which is consistent with "the device died at some
point during those 69 lines of streaming and the next present-submit discovered it".

The 52 missing-file probes in this window are all **expected optional-file probes** (e.g.
`motion/dance_etc/ap/ap01_jktk_001_n02.nuanmb`, `sound/song3/kov/{yuk,maW,har,mam,tak}/song3_kov_*.nus3bank`,
`character/lipsync/kov/lipsync_kov.nuanmb`, `motion/camera_etc/bs/cam_bs01_004_liv_step.jxb`) and they are
identical in the *clean* reference log (see §8.2). They are not failures; the game probes, gets `ENOENT`, and
uses a fallback. The clean log emits the same probes and survives.

### 4.3 Where B dies relative to the working reference

D (clean) loads, in this order, after the same live set-up prefix:

* `motion/dance/kov/dan_kov_d01 … d05` **and** their `_loop` variants — **10 files**, D 3079–3097
* `motion/camera/cam1/cam_cam1.jxb`, `cam2/*`, `cam3/cam_cam3_kov_u05{,_step}`, `cam4/cam_cam4_m_001..006`,
  `cam5/cam_cam5_m_{c,m}01..05` — D 3099–3146
* `files/lua/ui/result.luac` @3148, `2d_ch/result/result.jxk` @3150, `idolrankup.luac` @3152,
  `tutorial_memo.luac` @3158, `2d_ch/hells_title/hells_title.jxk` @3160, `2d_ch/rhythm_note/rhythm_ico.nutexb`
  @3162, `rhythm_with_bpm.jxk` @3164, `rhythm_with_out_bpm.jxk` @3166, `effect/rhythm_game/eff_game_star_02.jxk`
  @3168
* `effect/chemicallight/chemicallight_model_{high,middle,low}.jxk` @3176–3181
* **`Cache dumped` @3186 — clean exit**

B gets **3 of the 10 dance-motion files** and none of the camera or rhythm-game assets. So B dies *inside* the
live-asset streaming phase, before the lesson/rhythm UI is ever reached.

---

## 5. Unique-asset analysis

### 5.1 Set arithmetic

| Pair | Only in the first | Only in the second | Common |
|---|---|---|---|
| A (855 uniq) vs B (620) | 253 | 18 | 602 |
| C (589) vs D (662) | 25 | 98 | 564 |

Paths that appear in **exactly one** of the four logs: **397** (unique to A) and **48** (unique to B).

### 5.2 Assets unique to the medley case (A) — the medley feature set

44 of the 397 are the medley/stage-session feature assets; everything else is character/costume/motion data
shared with other live content.

| Line | Path | What it is |
|---|---|---|
| 2816 | `files/lua/ui/unit_setup_medley.luac` | medley unit-set-up UI |
| 3023 | `files/lua/ui/stage_select.luac` | stage select |
| 3211 | `files/lua/ui/song_select.luac` | song select |
| 3214 | `files/lua/ui/song_select_common.luac` | song select common |
| 4034 | `bg3d/arn_004/bg3d_arn_004.jxk` | **stage `arn_004`** ("三角光露营区") |
| 4037 | `bg3d/arn_004/bg3d_arn_004_layout.jxk` | stage layout |
| 4039 | `bg3d/arn_004/lit_arn_004.jxb` | stage lighting |
| 4045 | `motion/medley_etc/pth/mdy_pth_arn_004.nuanmb` | per-stage medley path |
| 4047 | `meta_data/meta_medley_direction_param_table.jxb` | **medley direction table** |
| 4049/4051/4053 | `motion/medley_etc/mdy_m03_07{a,b,c}.jxk` | medley choreography |
| 4055/4057/4059 | `motion/camera/mdy/cam_mdy_m03_07{a,b,c}.jxk` | medley camera work |
| 4282–4302 | `motion/camera/cam5/cam_cam5_arn_004_{m,c}01..05.jxb` | 10 stage-specific camera banks (all but `c05`/`m05` exist) |
| 3551/3555 | `timing_sheet/ts{v,p}_m03.jxk` | medley timing sheets |
| 3531–3537 | `effect/appeal/eff_app_cute_u0{1,2,3,5}.jxk` | **appeal effect, `cute` set** |
| 3573–3581 | `cutin_sheet/song/mdy_m03_07{a,b,c}_00{1,2}.jxk` | medley cut-ins |
| 4304 | `files/lua/ui/stage_scene.luac` | stage-scene controller (the boundary marker) |
| 4306/4308/4310 | `2d_ch/stagescene/stagescene.jxk`, `2d_ch/popup/popup.jxk`, `lua/ui/popup.luac` | stage-scene 2D UI |

Not unique (and therefore *not* smoking guns): `effect/chemicallight/chemicallight_model_*.jxk` (also D 3176–3181),
`motion/chemical_light/chemicallight_motion*.nuanmb` (all four logs, at boot), `cutin_sheet/otefuri` and
`cutin_sheet/burst` (all live logs).

### 5.3 Assets unique to the Hell Training case (B) — all 48

`bg2d_stage_015*` (4, live-select row), `bg3d/ofc_002*` (5, office #2), `chr_soul/face/hair/orna/putitem` for
**tak** and **hib** (office commu), `chr_body_priv_001_{h,i}.jxk`, `chr_orna_priv_901_mam.jxk` @3494,
`chr_hair_priv_001_tak.jxk` @3515, `chr_handitem_013_a.jxk` @2964, `cutin_sheet/burst/exb_u01_yuk_liv.jxk` @3326,
`gscript/ex2/tak/tak_mon_02a.jxk` @2510, `gscript/ex2/tca/tca/tca_brm_001.jxk` @2851,
`motion/camera_etc/bs/cam_bs01_yuk_liv{,_step}` @3382/3387, `motion/dance_etc/bs/bs01_yuk.jxk` @3378,
`song3_kov_{maW,mam,tak}.nus3bank` (all absent), `voice_{com,mdl}_{ami,mam,tak}.nus3bank`, `com_{hib,tak}.jxk`.

**The unit identity is the only structural difference between B and C** (both stage `liv_015`, song `kov`,
chart `exl_kov_p`):

| Log | Unit (in load order) | Reached |
|---|---|---|
| B 地狱特训 | **yuk, ami, har, mam, tak** (5) | all 5 + `dan_kov_d01/d01_loop/d02` |
| C 8月 | **har, ior, azu, …** (crashes at #3) | 3 + no dance motion |
| D 9月 (clean) | **har, ior, azu, mak, yuk** (5) | all 5 + 10 dance files + cameras + rhythm UI + clean exit |

Both B and C are 5-member units with two members in common (`har`), the same stage and the same crash pipeline
hash. D differs in unit *and* has the pipeline cache warm. There is therefore **no pair of logs in this corpus
that isolates "unit" from "cache state"** — hence §10.

### 5.4 Verdict on candidate game features (question B)

| Candidate | Verdict | Evidence |
|---|---|---|
| Stage venue effect (`arn_004`) | **not sufficient** — `arn_004` only appears in A; B/C crash on `liv_015` which A never uses | A 4034–4047 vs B 3397–3401 |
| Song-specific effect (`m03` / `kov`) | **not sufficient** — A uses `m03`, B/C/D use `kov`; D is clean on `kov` | A 3531–3581 vs D 2875–2917 |
| Costume / accessory | **weak** — A uses `cute`+`chr_body_cute_112_a`+`acc3_head_033/hand_122/foot_028`; B uses `priv/901`+`chr_body_exte_108_a`+`acc3_head_135`; C uses base `priv/001`; D uses base `priv/001` and `exte/001` | A 4077–4226, B 3413–3522, C 3262–3316, D 2992–3056 |
| Camera work | **weak** — B's camera assets (`cam_cam5_*`) had **not** been loaded when it crashed; A loaded all 10 `cam_cam5_arn_004_*` at 4282–4302 long before the loss | B has no `cam_cam5` open at all |
| Live 特效 / appeal cut-ins | **weak** — loaded in every live session including the clean one, and 1,500+ lines before A's loss | A 3470–3537, D 2875–2886 |
| **New graphics pipeline built from a new shader permutation at the moment of the loss** | **strong — the only feature common to all three crashes and absent from the clean run** | A 6009–6011, B 3458–3460, C 3305–3307; D has 0 compiles |

---

## 6. Shader and pipeline forensics

### 6.1 `vs 0xb3b7b928` permutation history (question C)

| Log | Occurrence | Resulting pipeline | Outcome |
|---|---|---|---|
| A | 4343 base | `0x3cd7c4a2bee1c71a` @4345 | fine |
| A | 4380 **(permutation)** | `0xe54f252e3309840d` @4382 | fine — used for 1,627 lines / ~690 frames |
| A | **6009 (permutation)** | **`0xfa36d21f2da4ef4f` @6011** | **device lost 3 lines later** |
| B | 2605 base | `0x3cd7c4a2bee1c71a` @2607 | fine |
| B | 2620 **(permutation)** | `0xe54f252e3309840d` @2622 | fine — 836 lines |
| B | **3458 (permutation)** | **`0x59b3f0dd8289f1d6` @3460** | **device lost 70 lines later** |
| C | 2704 base | `0x3cd7c4a2bee1c71a` @2706 | fine |
| C | 2719 **(permutation)** | `0xe54f252e3309840d` @2721 | fine — 584 lines |
| C | **3305 (permutation)** | **`0x59b3f0dd8289f1d6` @3307** | **device lost 11 lines later** |
| D | — | — | shader never compiled (cache warm) |

Three observations:

1. **`0x3cd7c4a2bee1c71a` and `0xe54f252e3309840d` are produced identically by all three crashing sessions**, in
   two different builds, in the middle of the *office/commu* scene (`fs 0x29838425` then `fs 0x5b3f4b24`
   immediately precede them in all three: A 4341/4378, B 2603/2618, C 2702/2717). They render happily.
2. The **third** occurrence is always a *new permutation* — i.e. a new `StageSpecialization` (per
   `src/shader_recompiler/specialization.h:197`): a different vertex-attribute number-class/dst-select/divisor
   set (`vs_attribs`), a different `RuntimeInfo` (`vs_info` step rates / num outputs, or the GS/FS runtime info),
   a different fetch-shader layout, or a different buffer/image/sampler sharp. **A new permutation means the
   guest started drawing this shader with a vertex-fetch layout or resource binding it had not used before.**
3. The three crash pipelines (`0xfa36d21f2da4ef4f`, `0x59b3f0dd8289f1d6`) each occur **exactly once** in their
   log — created, first used, crash. `0x59b3f0dd8289f1d6` is *identical* in B (d7e66f5e) and C (26d27ae), which
   means the pipeline key — and therefore the whole `GraphicsPipelineKey` (stage hashes, vertex formats,
   `patch_control_points`, MRT mask, colour buffers/blend/write masks, `cb_shader_mask`, logic op, sample
   counts, depth/stencil format, `depth_clamp_enable`, prim type, polygon mode, clip space, provoking vertex,
   `depth_clip_enable`) — is reproducible across builds. Whatever state produces it is deterministic game state,
   not a race.

### 6.2 The crash pipelines are geometry-shader pipelines

In the new logs every `GetGraphicsPipeline` line is followed by `Geometry program:` lines when the drawn state has
a GS stage enabled, and both crash sites show it:

* A: pipeline `0xfa36d21f2da4ef4f` @6011 → `max_vert_out = 8 … 0xf3bd17fb` @6012 and
  `max_vert_out = 20 … 0x395cf4e2` @6013 (both passes of the frame).
* B: pipeline `0x59b3f0dd8289f1d6` @3460 → `max_vert_out = 8 … 0xf3bd17fb` @3461.

So the newly built pipeline is an ES+GS+VS+FS pipeline, and the frame it is first used in also draws the two
"normal" GS pipelines. The Lead's earlier finding stands: `vgt_gs_max_vert_out` is 8 and 20 and
`vgt_gs_vert_itemsize` 12/28, `vgt_esgs_ring_itemsize` 8/4, `num_invocations` 1, `mode` 3 — nowhere near the
256-vertex cap, and no clamp warning fired. **GS OutputVertices is not the cause.** It remains worth noting that
the *only* pipelines ever created with a GS in these sessions are the two "frame" pipelines plus the crash
pipeline — so the crash pipeline is the third GS pipeline in the session and the second one built from scratch
during the live.

### 6.3 What changed at the crash frame — quantified

In A, per-draw GS logging lets us count exactly how much rendering happened between the last new state and the
loss:

* new pipeline @6011, then **2** GS fetches (6012, 6013), then the failing submit @6014.
* before that: 1,377 GS fetches with no new pipeline for 1,607 lines → the previous pipeline set was stable and
  productive for ≈690 frames.
* the longest burst of *identical* draws in the whole session is **281 draws of the single-pass
  `max_vert_out = 20` pipeline** (`4392 Skipped 182`, `4394 Skipped 59`, `4397 Skipped 37`, interrupted only by
  two `event_flag.cpp` warnings) at the stage-scene set-up 4391–4397 — i.e. the session does have "special"
  frames with 281 back-to-back single-pass draws, and they are **not** the crashing one.

---

## 7. Warning / error scan (question D)

### 7.1 Complete renderer warning set (identical in A and B)

| Line(s) A | Line(s) B | Message | Assessment |
|---|---|---|---|
| 82–92 | 82–92 | 7× `vk_instance.cpp:236 Extension VK_AMD_* unavailable` | device capability, boot only |
| 103–114 | 103–114 | 11× `vk_instance.cpp:711 CollectImageFormatInfo: … not fully supported` + `vk_instance.cpp:721 Depth format z_format=1, stencil_format=1 … D16UnormS8Uint` | device capability, boot only |
| 1299 | 1299 | `emit_spirv.cpp:439 Float32 denorm flushing is not supported by the GPU` | once, at first compute pipeline |
| 2689 | 2602 | `texture_cache.cpp:260 ResolveDepthOverlap: Unimplemented depth overlap copy` | **once**, mid-session, in both; also present in D @2488 |
| 6010 | 3459 | `recompiler.cpp:36 First instruction is not s_mov_b32 vcc_hi, #imm` | fires for 155 (A) / 109 (B) shaders — noise |
| 6014–6015 | 3530–3531 | `vk_scheduler.cpp:205/210` device lost | the event itself |

There is **no** `Render.Recompiler` error other than the ubiquitous `:36` one, **no** unhandled/unimplemented
opcode report, **no** texture-format conversion failure, **no** `sceKernel*` error other than the boot-time
`ThreadPriority attr is not supported!` and the `sceKernelStat error = 2` probes, and **no** allocation failure.

### 7.2 Long GPU stall / TDR indicators

| Probe | A | B | Reading |
|---|---|---|---|
| Largest `Skipped N duplicate` near the crash | 2 (@5932) | 11 (@3293) | no evidence of a multi-second identical-message burst at the crash |
| Largest GS-fetch gap in the last 400 lines | 72 lines (4672→4744, the 2D transition; this gap lies *before* the window) | 50 lines (3396→3446); last fetch @3461, then 69 lines of asset I/O | A never stops drawing; B's GS drawing stops 69 lines before the loss, but those lines are file I/O, not silence |
| AJM create/destroy balance in the last 400 lines | 10 create / 4 destroy | 26 create / 14 destroy | churn, not leak; `sceAjmBatchWait returned -0x7f6cfff1` appears 3× (A) / 4× (B) spread over the session, never adjacent to the loss |
| Device memory at loss | 1,900 / 6,239 MiB | 1,566 / 6,239 MiB | no pressure |
| `signal tick` at loss | 8,980 | 2,254 | the queue had processed thousands of submits |

**Conclusion:** nothing in the log distinguishes "hung GPU / TDR" from "illegal access". Both remain possible and
only `VK_EXT_device_fault` (already added in the working tree) or a Windows Event Log check
(`nvlddmkm` event 4101) can separate them. The `vk_scheduler.cpp:210` message already tells the user to look
there.

### 7.3 The 322 / 319 `sceKernelStat: error = 2`

These are `file_system.cpp:720` stat failures emitted *before* the corresponding `open:` of an optional file. They
occur in the clean log too, they cluster in the boot/DLC-metadata scan (A 1519…4301, B 1519…3517), and the game
always recovers by opening a variant that does exist (e.g. B 3385 opens `cam_bs01_004_liv_step.jxb` → fails →
3387 opens `cam_bs01_yuk_liv_step.jxb` → succeeds). **Benign**, but note the two counts are within 1% of each
other (322 vs 319), which is another sign that the two sessions are doing the same amount of optional-file
probing.

---

## 8. Did the fix move the crash? (question E)

### 8.1 Same live, two builds

B and C are the same live (`kov`, stage `bg3d_liv_015`, chart `exl_kov_p`, both reach `mdy_pth_liv_015`,
`eff_bst_keep_001/002`, `bg2d_163`) but with different units, so the comparison has to be by *phase position*,
not by line number:

| Milestone | C 8月 (26d27ae) | B 地狱特训 (d7e66f5e) | D 9月 (clean, other unit) |
|---|---|---|---|
| Live set-up prefix (`se_rhythm` → dance_etc) | 3152–3237 | 3302–3392 | 2869–2951 |
| Stage `bg3d_liv_015` loaded | 3246 | 3397 | *(liv_013 @2960)* |
| `mdy_pth_liv_015.nuanmb` | 3256 | 3407 | *(liv_013 @2970)* |
| unit members fully loaded | **3 of 5** (har 3262, ior 3278, azu 3301–3316) | **5 of 5** (yuk 3413, ami 3442, har 3471, mam 3480, tak 3503) | 5 of 5 |
| `dan_kov_d0*` dance motion | **never** | **`d01`, `d01_loop`, `d02`** (3524–3528) | **all 10** (`d01…d05` + `_loop`) 3079–3097 |
| camera `cam_cam5_*` | never | **never** | 3122–3146 |
| `result.luac` / `hells_title` / `rhythm_*` | never | **never** | 3148–3168 |
| crash pipeline | `0x59b3f0dd8289f1d6` @3307 | `0x59b3f0dd8289f1d6` @3460 | — |
| loss | @3318 | @3530 | clean exit @3186 |

**Yes, the crash moved later — by two unit members and into the dance-motion stream (28 more asset requests, 94
more log lines after `bg2d_163`): 32 asset opens between `bg2d_163` and the loss in C, 60 in B.** It did *not*
move to a new subsystem, a new stage, or a new song, and it did not change the crash pipeline hash for this
content. That is exactly what you would expect if the fix (the `RingAccessElimination` OutputVertices clamp) removed
one way to lose the device, while the underlying "new pipeline at live start" issue is untouched — B now dies
slightly further along the same sequence.

This also **falsifies "Hell Training now reaches dance motion loading, so the crash is a different bug"**: it
reaches it, but it dies inside the same phase, still before the camera banks and the rhythm-game UI.

### 8.2 Are B's missing-file probes new? No.

Every `failed, file does not exist` path in B's last 400 lines also fails in D (the *clean* log):
`motion/dance_etc/ap/ap01_jktk_001_n02.nuanmb` (B 3344 / D 2909),
`sound/song3/kov/har/song3_kov_har.nus3bank` (B 3475 / D 3000), `character/lipsync/kov/lipsync_kov.nuanmb`
(B 3405, absent in D), `motion/medley_etc/pth/mdy_pth_liv_015.nuanmb` (B 3408; D probes the liv_013 variant).
They are optional-asset probes, not regressions.

### 8.3 What the August build could not tell us and this one can

C's build logged only `vk_scheduler.cpp:195 Assertion Failed!` — no memory figure, no thread, no tick. The new
build's extra context (`vk_scheduler.cpp:199-205` plus the pending `VK_EXT_device_fault` report) is what makes
A and B comparable at all, and it is what will settle H1 vs H4 in the next run.

---

## 9. Ranked hypotheses

### H1 — A newly-created pipeline, first used in the frame(s) before the loss, faults or hangs the GPU — **STRONG**

For:
* In all three crash logs the last *new state* before the loss is a **brand-new shader permutation of
  `0xb3b7b928` plus a brand-new graphics pipeline** (A 6009/6011, B 3458/3460, C 3305/3307). Each crash pipeline
  hash occurs exactly once in its log.
* The clean log is the only one that created **no** pipeline at all (zero compiles, 391 preloaded).
* The crash pipelines are ES+GS+VS+FS pipelines; the newly compiled module is the **vertex** stage, so the
  recompiler is emitting a new vertex-fetch/attribute layout — precisely the code path where an out-of-bounds
  vertex fetch would come from.
* In A the loss is 2 lines after the pipeline, in a session that had been drawing 690 identical frames
  uninterrupted (median GS-fetch gap 1, max 72).
* The pipeline hash `0x59b3f0dd8289f1d6` is stable across two builds → deterministic guest state, reproducible.

Against / caveats:
* The loss is detected on `vkQueueSubmit`, i.e. at least one submit *after* the fault (§1.3). The new pipeline's
  first draws are inside that window, but so is everything else submitted since the last successful submit.
* Three crash sites, three different third-`0xb3b7b928` permutations — if the *permutation itself* were at fault
  we would expect the same key each time. Only the *pattern* is stable, not the key. This points at "some
  property of the new pipeline state that the driver/recompiler mishandles" rather than "this one shader is bad".

### H2 — The discriminator is the cold-compile path: every crash happened with `PipelineCacheEnabled: false` — **MEDIUM-HIGH**

For:
* A, B, C all: `Vulkan PipelineCacheEnabled: false` (A:30, B:30, C:36). D: `true` (D:37) and
  `WarmUp: Preloaded 391 pipelines` (D:122) with **0** runtime compiles.
* A/B compiled 166/118 shader modules and 132/130 graphics pipelines during a single live; D compiled none.
* Every 0xb3b7b928 / 0x59b3f0dd8289f1d6 / 0xfa36d21f2da4ef4f / 0x3cd7c4a2bee1c71a occurrence is a **compile**
  line, so with the cache on we simply cannot see whether the same pipelines are used — the previous forensics
  report already flagged this (`log-forensics.md` §2, H3).

Against:
* It is not a "cache enabled = works" story either: the pipeline cache is a *serialized* pipeline cache, and a
  preloaded pipeline that was serialized from a run that survived is not evidence that a fresh creation would
  survive.
* D also differs in unit, stage and chart (see §2), so this is a 4-way confound, not an A/B.

Why it is ranked so high anyway: **it is the cheapest thing to test** (§10, experiment 1) and, if it holds, it
re-frames the whole bug as "the recompiler/creation path produces a bad pipeline", which is far more actionable
than "this costume is cursed".

### H3 — The loss is a GPU hang/TDR from a pathological draw, not an illegal access — **MEDIUM**

For:
* The driver is NVIDIA 621.1.0.0 on a laptop RTX 5060; laptop parts TDR aggressively.
* No API-level error precedes it; `VK_EXT_device_fault` was available but the *reporting code was not in
  d7e66f5e*, so the logs cannot distinguish hang from fault.
* The GS pipelines are the only unusual pipeline shape in the session (ES+GS+VS+FS, `mode = 3`, `instance_cnt
  = 1`, `esgs_ring_itemsize` 4 and 8).

Against:
* No stall signal: A's GS-fetch cadence never breaks before the last line; nothing looks like a multi-second
  freeze; memory is at 25–30% of budget.
* If it were a plain GPU hang under load, the clean session (which rendered the same stage and the same rhythm
  game content) would be expected to die too.

Discriminator: Windows Event Log `nvlddmkm` event 4101, or the new `GetDeviceFaultReport()` output.

### H4 — Resource lifetime bug: a buffer/image the new pipeline references is freed or aliased — **MEDIUM**

For:
* The medley tears down the previous section's five AJM instances at 4312–4316 and the stream keeps recycling
  AJM instances (IDs 16385–16397) for the rest of the session; B does the same (26 create / 14 destroy in its
  last 400 lines).
* A new pipeline is created → descriptor sets / render targets may be re-created → a use-after-free would fault
  exactly once, on first use, with no preceding API warning.
* `texture_cache.cpp:260 ResolveDepthOverlap: Unimplemented depth overlap copy` fires exactly once in both new
  logs (A 2689, B 2602) — an unimplemented path that copies depth between overlapping images. It is 3,300 lines
  before A's loss and 900 before B's, so it is not temporally adjacent, but it is the only "we don't implement
  this" in the renderer at all.

Against: no `vkDestroyBuffer`/`vkFreeMemory` evidence in the log (not logged), so this is unfalsifiable from logs
alone; needs validation with `vkValidationSync=true`.

### H5 — GS `OutputVertices` over the device limit — **FALSIFIED** (Lead's finding, re-confirmed)

`max_vert_out` = 8 and 20, `num_invocations` = 1, `mode` = 3, `esgs_ring_itemsize` = 8 and 4 in every GS line of
both logs; the clamp added in `726272ba` never fired. This report treats the GS lines purely as a draw counter.

### H6 — Guest-side bug (the game does something illegal) — **LOW**

`:210` in the emulator says it itself: "This is a GPU-side failure, not a guest game fault." Every guest-visible
API call in the window succeeds; the emulator has to reach the driver to fail. That said, the *guest* is what
chooses the vertex format that produces the new permutation, so "guest content selects a state the emulator
mis-translates" is still H1/H2 territory.

---

## 10. Cheapest next experiments (ordered by cost)

1. **Re-run 地狱特训 Pro with the pipeline cache warm.** Keep everything identical to B but set
   `pipeline_cache_enabled = true` and run once so the cache is populated (or reuse the cache D built). If it
   survives → H2 confirmed, and the bug is in creation/translation, not in the content. If it still dies at the
   same place → H2 dead, H1/H4 remain. *Cost: one run.* This is the single highest-value experiment.
2. **Re-run the medley with the cache warm** (same as (1), for A). Two independent content sets surviving on the
   cold path only would be a very strong statement.
3. **Repeat B with the pending build** (`GetDeviceFaultReport()` in `vk_scheduler.cpp:211-218`). The fault
   address + faulting pipeline stage will discriminate H1 (fault in a draw) from H3 (hang: no fault recorded) in
   one run. *Cost: one rebuild, one run.*
4. **Check `Get-WinEvent` for `nvlddmkm` event 4101** for the timestamps of A and B. Present → TDR (H3);
   absent → illegal access (H1/H4).
5. **`vkValidationSync=true` + `shouldDumpShaders=true` for one run of the smallest reproducer.** This will name
   the exact submit and dump the offending SPIR-V. Expect a large log; the run only needs to reach the crash.
6. **Bisect by drawing state, not by content:** with `pipeline_cache_enabled=false`, replay B but force
   `StageSpecialization` to reuse permutation #1 for the third case (temporary hack) — if the crash disappears,
   H1 is confirmed at the cost of one build.
7. **Only after (1)–(3):** instrument `GetGraphicsPipeline()` to dump the full `GraphicsPipelineKey`
   (vertex formats, MRT mask, depth format, prim type, clip space) for every created pipeline. The Lead's
   working tree already logs the GS part of the key; extending it to the whole key makes A's
   `0xfa36d21f2da4ef4f` and B/C's `0x59b3f0dd8289f1d6` directly comparable against the *working* pipeline
   `0xe54f252e3309840d` that the same shader produced earlier in the same session.

---

## 11. Appendix — verbatim evidence

### A.1 Medley, last 9 lines (6017-line file)

```
6009: [Render.Vulkan] <Info> (shadPS4:GpuCommandProcessor) vk_pipeline_cache.cpp:616 CompileModule: Compiling vs shader 0xb3b7b928 (permutation)
6010: [Render.Recompiler] <Warning> (shadPS4:GpuCommandProcessor) recompiler.cpp:36 TranslateProgram: First instruction is not s_mov_b32 vcc_hi, #imm
6011: [Render.Vulkan] <Info> (shadPS4:GpuCommandProcessor) vk_pipeline_cache.cpp:335 GetGraphicsPipeline: Compiling graphics pipeline 0xfa36d21f2da4ef4f
6012: [Render.Vulkan] <Info> (shadPS4:GpuCommandProcessor) vk_pipeline_cache.cpp:193 BuildRuntimeInfo: Geometry program: max_vert_out = 8, vert_itemsize = 12, esgs_ring_itemsize = 8, instance_cnt = 1, mode = 3, copy shader 0xf3bd17fb
6013: [Render.Vulkan] <Info> (shadPS4:GpuCommandProcessor) vk_pipeline_cache.cpp:193 BuildRuntimeInfo: Geometry program: max_vert_out = 20, vert_itemsize = 28, esgs_ring_itemsize = 4, instance_cnt = 1, mode = 3, copy shader 0x395cf4e2
6014: [Render.Vulkan] <Critical> (shadPS4:GpuCommandProcessor) vk_scheduler.cpp:205 SubmitExecution: Device lost on queue submit: signal tick = 8980, waiting on 0 semaphore(s), signaling 1 semaphore(s), device memory 1900 MiB / 6239 MiB, thread = 'shadPS4:GpuCommandProcessor'
6015: [Render.Vulkan] <Critical> (shadPS4:GpuCommandProcessor) vk_scheduler.cpp:210 SubmitExecution: This is a GPU-side failure, not a guest game fault. …
6016: [Debug] <Critical> (shadPS4:GpuCommandProcessor) vk_scheduler.cpp:212 SubmitExecution: Assertion Failed!
6017: Device lost during submit
```

### A.2 Hell Training, last 12 lines (3533-line file)

```
3524: [Kernel.Fs] <Info> (imas::ResourceJob) file_system.cpp:78 open: path = /app0/asset/motion/dance/kov/dan_kov_d01.nuanmb flags = 0x0 mode = 0777
3525: [Kernel.Fs] <Info> (imas::ResourceJob) file_system.cpp:296 close: Closing /app0/asset/motion/dance/kov/dan_kov_d01.nuanmb
3526: [Kernel.Fs] <Info> (imas::ResourceJob) file_system.cpp:78 open: path = /app0/asset/motion/dance/kov/dan_kov_d01_loop.nuanmb flags = 0x0 mode = 0777
3527: [Kernel.Fs] <Info> (imas::ResourceJob) file_system.cpp:296 close: Closing /app0/asset/motion/dance/kov/dan_kov_d01_loop.nuanmb
3528: [Kernel.Fs] <Info> (imas::ResourceJob) file_system.cpp:78 open: path = /app0/asset/motion/dance/kov/dan_kov_d02.nuanmb flags = 0x0 mode = 0777
3529: [Kernel.Fs] <Info> (imas::ResourceJob) file_system.cpp:296 close: Closing /app0/asset/motion/dance/kov/dan_kov_d02.nuanmb
3530: [Render.Vulkan] <Critical> (shadPS4:PresentThread) vk_scheduler.cpp:205 SubmitExecution: Device lost on queue submit: signal tick = 2254, waiting on 2 semaphore(s), signaling 2 semaphore(s), device memory 1566 MiB / 6239 MiB, thread = 'shadPS4:PresentThread'
3531: [Render.Vulkan] <Critical> (shadPS4:PresentThread) vk_scheduler.cpp:210 SubmitExecution: This is a GPU-side failure, not a guest game fault. …
3532: [Debug] <Critical> (shadPS4:PresentThread) vk_scheduler.cpp:212 SubmitExecution: Assertion Failed!
3533: Device lost during submit
```

### A.3 The three `0xb3b7b928` compiles in the medley (context)

```
4343: CompileModule: Compiling vs shader 0xb3b7b928            → 4345 GetGraphicsPipeline: 0x3cd7c4a2bee1c71a   (fs 0x29838425 @4341)
4380: CompileModule: Compiling vs shader 0xb3b7b928 (permutation) → 4382 GetGraphicsPipeline: 0xe54f252e3309840d   (fs 0x5b3f4b24 @4378)
6009: CompileModule: Compiling vs shader 0xb3b7b928 (permutation) → 6011 GetGraphicsPipeline: 0xfa36d21f2da4ef4f   ← loss
```

### A.4 Source references consulted (read-only, no edits)

| File | What it establishes |
|---|---|
| `src/video_core/renderer_vulkan/vk_scheduler.cpp:152-220` | the loss is `vkQueueSubmit == eErrorDeviceLost`; new `GetDeviceFaultReport()` branch is in the working tree but not in d7e66f5e |
| `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:651-697` | `GetProgram` compiles a new permutation only when `StageSpecialization` differs; `perm_idx != 0` is what prints `(permutation)` |
| `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:481-527` | `RefreshGraphicsStages` calls `GetProgram` per enabled stage per draw → one GS log line per draw |
| `src/shader_recompiler/specialization.h:81-210` | what a permutation actually keys on: `vs_attribs` (number class / dst select / divisor), `runtime_info`, `fetch_shader_data`, buffer/image/sampler sharps |
| `src/video_core/renderer_vulkan/vk_graphics_pipeline.h:32-69` | the full `GraphicsPipelineKey` (hashed with `std::hash`) — everything that can make a "new pipeline" |
| `src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp:364` | `WarmUp: Preloaded 391 pipelines` — the D-only line that proves the cache was hot |

### A.5 Line-number quick index

| Thing | A medley7 | B 地狱特训 | C 8月 | D 9月 |
|---|---|---|---|---|
| Boot revision line | 17 | 17 | 8 | 9 |
| `PipelineCacheEnabled` | 30 | 30 | 36 | 37 |
| First `0xb3b7b928` | 4343 | 2605 | 2704 | — |
| Pipeline `0x3cd7c4a2bee1c71a` | 4345 | 2607 | 2706 | — |
| Pipeline `0xe54f252e3309840d` | 4382 | 2622 | 2721 | — |
| Crash permutation | 6009 | 3458 | 3305 | — |
| Crash pipeline | `0xfa36d21f2da4ef4f` @6011 | `0x59b3f0dd8289f1d6` @3460 | `0x59b3f0dd8289f1d6` @3307 | — |
| Stage 3D | `arn_004` 4034 | `liv_015` 3397 | `liv_015` 3246 | `liv_013` 2960 |
| Chart `fumen` | `m03` timings 3551 | `exl_kov_p` 3304 | `exl_kov_p` 3156 | `exl_kov_r` 2871 |
| Dance motion loaded | `dan_m03_d01..d05` 4234–4257 | `dan_kov_d01,d01_loop,d02` 3524–3528 | none | `dan_kov_d01..d05` +loops 3079–3097 |
| Last asset request | 4402 (`chemicallight_model_low.jxk`) | 3528 (`dan_kov_d02.nuanmb`) | 3316 (`chr_hair_priv_001_azu.jxk`) | 3181 (`chemicallight_model_low.jxk`) |
| Loss / exit | 6014 | 3530 | 3318 | 3186 (clean) |
