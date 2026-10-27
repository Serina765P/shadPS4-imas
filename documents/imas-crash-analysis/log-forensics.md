# 地狱特训Pro 设备丢失：日志取证报告

**Log forensics report — `8月.txt` (crash) vs `9月.txt` (clean)**

| | |
|---|---|
| Author | log-forensics (team member) |
| Task | task-1 — pinpoint the device-lost trigger in 8月.txt vs 9月.txt |
| Inputs | `C:\Users\Serina\Downloads\8月.txt` (3319 lines / 458 556 B), `C:\Users\Serina\Downloads\9月.txt` (3186 lines / 437 877 B) |
| Log author | third-party PC (user **Excalibur**), path `C:/Users/Excalibur/...` |
| Build in both logs | shadPS4 **v0.16.1 WIP**, revision `26d27aef048d4fbc2774a42878c7742850603134`, branch `release`, remote `Serina765P/shadPS4` |
| Game | THE IDOLM@STER STELLA STAGE `CUSA09153` (ASIA), App Version **01.08**, fw `0x4700000` |
| Local source cross-check | `src/video_core/renderer_vulkan/vk_scheduler.cpp:195` in this checkout (HEAD `14c39880`) matches the assert site **exactly**, so the local tree is close enough to reason about (only `vk_scheduler.cpp` / `vk_pipeline_cache.cpp` / `vk_pipeline_serialization.cpp` were read; **no source was modified**) |

---

## 0. Executive summary

1. **The device loss is a real GPU-side loss, not an emulator logic assert.** `vk_scheduler.cpp:195` is
   `ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");` — the
   `vkQueueSubmit` call itself returned `VK_ERROR_DEVICE_LOST`. Everything else in the log is upstream of that.

2. **The last thing the GPU thread did was create a brand-new pipeline from a brand-new vertex-shader
   permutation, and the very next queue submit died.** Aug line 3305 compiles a *new* `vs 0xb3b7b928`
   specialization, 3306 raises the recompiler warning for it, 3307 creates graphics pipeline
   `0x59b3f0dd8289f1d6` (a hash that appears **exactly once** in the whole log), and the GPU thread then goes
   silent for 10 filesystem lines before the assert at 3318. In the final 300 lines the GPU thread did only
   10 log-worthy things: **8 new graphics pipelines + 2 new shader modules** — the 8th pipeline is immediately
   followed by the loss.

3. **The crash happens during the *opening* of the lesson, not during sustained gameplay.** Aug dies while the
   5th idol's (`azu`) face/hair assets are still streaming in, and **never requests** `dan_kov_*` (dance
   motion), `cam_*` (camera), `2d_ch/result`, `2d_ch/idolrankup` or `rhythm_*` — all of which the September
   log does load before exiting normally. Aug died ≈ 300 log-lines into a lesson-load sequence that takes
   Sept ≈ 450 lines to complete plus the silent song playthrough.

4. **The August/September pair is NOT a controlled A/B for "Pro vs plain".** It differs in *four* independent
   dimensions at once (see §2 and §4): pipeline cache off/on, a different chart/stage/idol-lineup, a different
   in-game month, *and* Sept progressed to the result + rank-up flow. The single cleanest, most actionable
   difference is `PipelineCacheEnabled: false` → Sept preloaded **391 pipelines** from disk and compiled **0**,
   Aug compiled **124 graphics + 8 compute pipelines and 111 shader modules** in one live render loop.

5. **Nothing in the log supports GPU-memory exhaustion.** No allocation failures, no OOM, no image-creation
   errors; guest-side kernel allocations are *identical* in both logs (31× `sceKernelAllocateDirectMemory`,
   31× `MapNamedDirectMemory` out, 30× `'anon'`, 14× `ReserveVirtualRange`); AJM create/destroy is balanced;
   warning/error *template sets* are essentially identical apart from "shader was compiled at all" symptoms.

**Ranked hypothesis (headline):** the first draw that uses the newly created vertex-shader permutation /
pipeline `0x59b3f0dd8289f1d6` faults or hangs the GPU, and the following `vkQueueSubmit` returns
`VK_ERROR_DEVICE_LOST`. Secondary reading: the device was already lost by the preceding 6-pipeline burst
(3133–3138) and 3305–3307 is merely the last frame recorded before the failing submit. Disambiguating needs
timestamps, `vkValidationSync`, or a RenderDoc capture — see §8.

---

## 1. Method / evidence quality

* No timestamps exist in either log. `Skipped N duplicate messages..` is the only density proxy:
  Aug 19 skip-lines totalling **356** suppressed duplicates; Sept 23 skip-lines totalling **369**.
* Thread interleaving is preserved, so intra-thread ordering is trustworthy. **All shader/pipeline messages
  come from the single thread `shadPS4:GpuCommandProcessor`**, so CompileModule/TranslateProgram/
  GetGraphicsPipeline ordering within one burst is causally ordered (verified in source:
  `vk_pipeline_cache.cpp:606-610` logs, then calls `TranslateProgram`, then `:326` logs the pipeline).
* Normalisation used for template counting: hex→`<HEX>`, digits→`<N>`, thread/driver parens stripped.
* Alignment of the two files: subtracting the one extra `[Config]` line and the extra
  `WarmUp: Preloaded 391 pipelines` line, a `Compare-Object -SyncWindow 60` over the **first 1320 lines of
  each file yields exactly 12 differing records (6 per side)** — i.e. **boot/init is bit-identical** apart from
  the items listed in §2. Everything that diverges does so after the first shader compilation.

---

## 2. Side-by-side configuration

### 2.1 Identical (verified line by line)

| Setting | Value in **both** logs |
|---|---|
| `window_width` / `window_height` override | `1280→1920` / `720→1080` |
| `readbacks_mode` override | `0 → 2` (**Precise**; enum `{Disabled, Relaxed, Precise}` in `emulator_settings.h:33`, imgui options `settings_dialog_imgui.h:119`) |
| `GPU readbacksMode` | 2 |
| `readbackLinearImages`, `directMemoryAccess`, `shouldDumpShaders`, `shouldCopyGPUBuffers` | false |
| `vblankFrequency` | 60 |
| `isNullGpu`, `isNeo`, `isDevKit`, `isConnectedToNetwork` | false |
| `Vulkan gpuId` | 0 |
| `vkValidation` / `vkValidationSync` / `vkValidationGpu` | false / false / false |
| `crashDiagnostics`, `hostMarkers`, `guestMarkers`, `rdocEnable` | false |
| `PipelineCacheArchived` | false |
| GPU | **NVIDIA GeForce RTX 5060 Laptop GPU**, driver `621.1.0.0`, Vulkan `1.4.357`, subgroup 32, 3 physical devices |
| CPU / RAM / OS | i7-13700HX, 16 physical / 24 logical, 16 GB, Windows 11 家庭版 中文版 |
| Guest framebuffer (`sceVideoOutSetBufferAttribute`) | `A8B8G8R8Srgb`, 1920×1080, tilingMode 0, 2 buffers (Aug 912-915, Sept 914-917) |
| Audio | 48 kHz Bgm FloatStereo + Main Float_8CH, SDL default device, gain 2.0 |
| Content mount errors | identical DLC/`asset_dlc` missing-file set |

### 2.2 The only real differences

| # | Item | 8月 (crash) | 9月 (clean) | Evidence |
|---|---|---|---|---|
| D1 | `pipeline_cache_enabled` game override | **absent** (off) | `pipeline_cache_enabled ( 0 -> 1 )` | Aug:3-6 / Sept:3-7 |
| D2 | `Vulkan PipelineCacheEnabled` | `false` | `true` | Aug:36 / Sept:36 |
| D3 | On-disk pipeline DB preload | *no such line* | `[Render] <Info> (shadPS4:Main) vk_pipeline_serialization.cpp:364 WarmUp: Preloaded 391 pipelines` | Sept:122 |
| D4 | Guest VMM region base | `memory.cpp:27 MemoryManager: 0x5560000 - 0x7ffe0000` | `… 0x5440000 - 0x7ffe0000` (Δ = 0x120000) | Aug:49 / Sept:50 |
| D5 | Lesson content (chart / venue / lineup) | `exl_kov_p.jxb`, `bg3d_liv_015`, `mdy_pth_liv_015`, bg2d stages 003/020/001/028, idols **mam, har, yuk, ior, azu (5)** | `exl_kov_r.jxb`, `bg3d_liv_013`, `mdy_pth_liv_013`, bg2d stages 003/021/017/026, idols **mam, ior, rit, har, azu, mak, yuk (7)** | see §4.2 |
| D6 | How far the run got | dies inside the lesson opening; no `dan_kov_*`, no `cam_*`, no `result`/`idolrankup`/`rhythm_*` | reaches `2d_ch/result`, `idolrankup`, `tutorial_memo`, next-lesson UI, then `UpdatePlayTime … 38:14:25` + `cache_storage.cpp:137 Close: Cache dumped` | Aug:3318 / Sept:3150-3186 |

D4 is almost certainly host ASLR / DB-open side effect, not a cause: the guest-visible layout that follows
(`SetupMemoryRegions … flexible 0x1c000000, direct 0x120000000`, first mapping at `phys 0x11e600000`) is
identical, and the whole `Kernel.Vmm` message multiset is identical (§6).

**Source confirmation for D1–D3** (`src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp:297-301`):

```cpp
void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;   // <-- 8月 takes this path: no DB open, nothing preloaded
    }
    Storage::DataBase::Instance().Open();
```

Therefore "Aug compiled 172 things / Sept compiled 0" is **by construction of the setting**, not evidence of a
different workload. Any "the Pro lesson is fine/broken with cache on" conclusion is still an open experiment.

---

## 3. Crash timeline — the last 300 lines of `8月.txt` (lines 3019–3318)

### 3.1 Subsystem census of the final 300 lines

| Tag | lines |
|---|---|
| `Kernel.Fs` (asset streaming, `imas::ResourceJob`) | **240** |
| `Lib.Ajm` (AT9 decode instance churn) | 34 |
| `Render.Vulkan` (pipeline/shader compiles) | **10** (8 graphics pipelines + 2 shader modules) |
| `Kernel.Event` | 8 |
| `Skipped N duplicate messages..` | 5 |
| `Render.Recompiler` warnings | 2 |
| `Debug <Critical>` | 1 |

So the renderer contributed exactly **ten** discrete events in the whole crash window; nothing was "spamming".

### 3.2 The GPU-thread spine (verbatim, line numbers preserved)

```
3069: [Render.Vulkan] <Info> (shadPS4:GpuCommandProcessor) vk_pipeline_cache.cpp:607 CompileModule: Compiling fs shader 0xeeca4b6b (permutation)
3070: [Render.Recompiler] <Warning> (shadPS4:GpuCommandProcessor) recompiler.cpp:36 TranslateProgram: First instruction is not s_mov_b32 vcc_hi, #imm
3071: [Render.Vulkan] <Info> (shadPS4:GpuCommandProcessor) vk_pipeline_cache.cpp:326 GetGraphicsPipeline: Compiling graphics pipeline 0x709f65b9a94ee0bd

3133: [Render.Vulkan] <Info> (shadPS4:GpuCommandProcessor) vk_pipeline_cache.cpp:326 GetGraphicsPipeline: Compiling graphics pipeline 0xa1bb75596165435c
3134: … Compiling graphics pipeline 0x1f707112ae2d2720
3135: … Compiling graphics pipeline 0x37d019937b09cad7
3136: … Compiling graphics pipeline 0xbb52c838702fe846
3137: … Compiling graphics pipeline 0x7d87e6ae8c563def
3138: … Compiling graphics pipeline 0xda4c291f61cf0011
        (six new pipeline objects in one burst, **no** shader compiles -> all modules already existed)

3305: [Render.Vulkan] <Info> (shadPS4:GpuCommandProcessor) vk_pipeline_cache.cpp:607 CompileModule: Compiling vs shader 0xb3b7b928 (permutation)
3306: [Render.Recompiler] <Warning> (shadPS4:GpuCommandProcessor) recompiler.cpp:36 TranslateProgram: First instruction is not s_mov_b32 vcc_hi, #imm
3307: [Render.Vulkan] <Info> (shadPS4:GpuCommandProcessor) vk_pipeline_cache.cpp:326 GetGraphicsPipeline: Compiling graphics pipeline 0x59b3f0dd8289f1d6
      --- GPU thread produces no further line ---
3318: [Debug] <Critical> (shadPS4:GpuCommandProcessor) vk_scheduler.cpp:195 SubmitExecution: Assertion Failed!
3319: Device lost during submit
```

### 3.3 What the other threads were doing in between (3308–3317)

The `imas::ResourceJob` thread continued streaming the 5th idol's face rig, i.e. the process was mid-load:

```
3292: [Kernel.Event] <Error> (Game:Main) event_flag.cpp:227 sceKernelCreateEventFlag: ThreadPriority attr is not supported!
3293: Skipped 6 duplicate messages..
3295: [Kernel.Fs] <Info> … open: path = /app0/asset/sound/song3/kov/ior/song3_kov_ior.nus3bank flags = 0x0 mode = 0777
3296: [Kernel.Fs] <Error> … file_system.cpp:161 open: Opening path /app0/asset/sound/song3/kov/ior/song3_kov_ior.nus3bank failed, file does not exist
3301: … open: path = /app0/asset/character/soul/chr_soul_azu.jxk
3303: … open: path = /app0/asset/character/face/azu/chr_face_azu.jxk
3308/3310/3312/3314: chr_face_azu_eyes.nuanmb / _mouth.nuanmb / meta_face_azu_eye_param_table.jxb / _lip_param_table.jxb
3316: … open: path = /app0/asset/character/hair/priv/001/chr_hair_priv_001_azu.jxk
3317: … close: Closing …/chr_hair_priv_001_azu.jxk
3318: <Critical>
```

### 3.4 Reading of the timeline

* Within a frame the GPU thread **records** (creating pipelines ⇒ logging) and only then **submits**. So the
  sequence "pipeline 3307 created → submit fails" is consistent with either
  (a) the loss occurring on the frame that first drew with pipeline `0x59b3f0dd8289f1d6`, or
  (b) the loss having already occurred on the previous frame(s) that first drew with the 3133–3138 batch, and
  the 3305–3307 recording being merely the last thing logged before the next submit reported it.
* A GPU fault/hang is asynchronous w.r.t. `vkQueueSubmit`; NVIDIA returns `VK_ERROR_DEVICE_LOST` from the first
  *subsequent* submit. So "which draw actually hung" needs a capture, not this log.
* **Duration:** the whole crash window is the lesson's opening. Aug never reaches the dance-motion stage
  (`dan_kov_d01..d05` + `_loop`, which Sept loads at 3079–3097 ≈ 340 lines after the lesson starts), the camera
  set (`cam_cam5_*`, Sept 3125), nor any result/rank-up UI. This is a *start-of-lesson* death, not a
  long-session death — which further weakens memory-growth explanations.

---

## 4. Content analysis — the two sessions are different lessons

### 4.1 Set difference of opened asset paths (598 opens/593 unique in Aug vs 689/666 in Sept)

**26 paths only in Aug** (crash), all content-selection, no new error class:

```
/app0/asset/bg2d/textures/bg2d_stage_{001,020,028}{,_b01,_b02,_b03}.nutexb      (12)
/app0/asset/bg3d/liv_015/{bg3d_liv_015.jxk, bg3d_liv_015_layout.jxk, lit_liv_015.jxb}
/app0/asset/motion/medley_etc/pth/mdy_pth_liv_015.nuanmb
/app0/asset/fumen/game/kov/exl_kov_p.jxb          <-- lesson chart
/app0/asset/character/orna/priv/001/chr_orna_priv_001_yuk.jxk
/app0/asset/character/orna/exte/001/chr_orna_exte_001_har.jxk
/app0/asset/character/body/priv/001/chr_body_priv_001_{a,e}.jxk
/app0/asset/character/body/exte/108/chr_body_exte_108_a.jxk
/app0/asset/motion/commu/com_{har,yuk}.jxk
/app0/asset/gscript/ex2/tca/tca/tca_sea_wbd3.jxk
/app0/asset/bg2d/textures/bg2d_163.nutexb
```

**99 paths only in Sept** (clean) — the whole "the lesson finished" tail plus a different venue/lineup:

```
/app0/asset/2d_ch/hells_title/hells_title.jxk            (地獄特訓 title card)
/app0/asset/2d_ch/result/result.jxk, idolrankup/idolrankup.jxk, tutorial_memo_w02_001/…
/app0/asset/2d_ch/rhythm_note/rhythm_ico.nutexb, rhythm_with_bpm.jxk, rhythm_with_out_bpm.jxk
/app0/asset/effect/rhythm_game/eff_game_star_02.jxk
/app0/asset/effect/chemicallight/chemicallight_model_{high,middle,low}.jxk
/app0/asset/bg3d/liv_013/{bg3d_liv_013.jxk, _layout.jxk, lit_liv_013.jxb}
/app0/asset/motion/medley_etc/pth/mdy_pth_liv_013.nuanmb
/app0/asset/fumen/game/kov/exl_kov_r.jxb                 <-- lesson chart
/app0/asset/motion/dance/kov/dan_kov_d0{1..5}{,_loop}.nuanmb   (10 dance motions)
/app0/asset/motion/camera/cam{1..5}/… (18 camera files)
/app0/asset/character/{soul,face,hair,orna}/… rit, mak       (2 extra idols)
```

Key inference (stated as inference, not fact): the only lesson-**chart** file differs (`exl_kov_p.jxb` vs
`exl_kov_r.jxb`) while both load the same `song3_kov_bgm.nus3bank` and both miss the same
`song3_kov_yell.jxb`. `_p` / `_r` are consistent with *Pro* / *Regular* variants of the same song but the
mapping is not proven from the logs.

### 4.2 Idol lineups (from `chr_soul_*.jxk` order)

* Aug: `mam → har → yuk → ior → azu` (**5**). Died while loading **azu** (the last one).
* Sept: `mam → ior → rit → har → azu → mak → yuk` (**7**), all completed.
  So the Pro lesson drew *fewer* idols than the plain one; "more characters ⇒ more load" is **not** supported.

### 4.3 Missing-file prologue is identical in shape

Both logs miss the exact same per-character song banks, `exl_kov_yell.jxb`, `mdy_pth_liv_*.nuanmb` and the
`bg2d_stage_XXX_b0N` layers. The only *set* differences in missing files are content-specific
(`chr_orna_priv_001_{yuk|rit}.jxk`, `song3_kov_{azu|mak|yuk}.nus3bank`, `cam_*_m_c0N/_m_m0N.jxb`).
**No "missing asset ⇒ crash" story.**

---

## 5. Quantitative comparison

| Metric | 8月 (crash) | 9月 (clean) |
|---|---|---|
| Total log lines | 3319 | 3186 |
| `Render.Vulkan` Info | 355 | 112 |
| `Render.Recompiler` Warning | **102** | **0** |
| `Compiling graphics pipeline` | **124** (all hashes unique) | **0** |
| `Compiling compute pipeline` | **8** | **0** |
| `CompileModule: Compiling <stage> shader` | **111** (unique stage+hash 98; 13 are `(permutation)`) | **0** |
| → fs / vs / cs / es / gs | 70 / 31 / 8 / 1 / 1 | 0 / 0 / 0 / 0 / 0 |
| `GetTilingPipeline` lines (detiler/untiler) | 70 (7 pipeline groups) | 70 (7 pipeline groups) |
| Tiling pipeline names | `Display2DThin_32`, `Depth2DThin64_32`, `Thin2DThin_32`, `Thin1DThin_32`, `Thin2DThin_64`, `Thin1DThin_128`, `Thin1DThin_64` | **identical, same order** |
| `texture_cache.cpp:260 ResolveDepthOverlap: Unimplemented depth overlap copy` | 1 | 1 |
| `CollectImageFormatInfo … not fully supported` | 11 | 11 |
| `emit_spirv.cpp:434 Float32 denorm flushing is not supported by the GPU` | 1 (at line **1305**, right after the 1st shader compile) | 0 (never emits SPIR-V) |
| `sceKernelCreateEventFlag: ThreadPriority attr is not supported!` | 28 | 30 |
| `sceKernelStat: error = 2` | 316 | 326 |
| `Lib.Ajm` create / destroy | 16 / 15 | 20 / 19 |
| `sceAjmBatchWait returned -0x7f6cfff1` | 4 | 4 |
| `Kernel.Vmm` messages | 129 | 129 (**same multiset**: 31 `AllocateDirectMemory`, 31 `MapNamedDirectMemory` out, 30 `'anon'`, 14 `ReserveVirtualRange`, `SceGnmDriver`, `SceLibcHeap`, `SceKernelPrimaryTcbTls`) |
| `Core.Linker` Info/Warning/Error | 231 / 439 / 12 | 231 / 439 / 12 |
| `Lib.AppContent` Info | 220 | 220 |
| Messages from `shadPS4:GpuCommandProcessor` | **418** | **71** (all 70 = tiling defines + 1) |
| Messages from `imas::ResourceJob` | 1489 | 1681 |
| `Skipped N duplicate` lines / Σ N | 19 / 356 | 23 / 369 |
| Last line | `Device lost during submit` | `cache_storage.cpp:137 Close: Cache dumped` |

Shaders compiled per stage in Aug, and how often the recompiler warning fires:

| stage | compiles | with `First instruction is not s_mov_b32 vcc_hi, #imm` |
|---|---|---|
| fs | 70 | 69 |
| vs | 31 | 30 |
| cs | 8 | **1** |
| es | 1 | 1 |
| gs | 1 | 1 |

⇒ the warning is a near-constant of the **graphics** shader compile path (101 of 103 graphics-stage compiles), so it is
**not** by itself the trigger; but it is a genuine translation-fidelity flag and the *last* one in the log
belongs to the exact shader that preceded the loss (§6).

---

## 6. What the logs let us rule out

| Candidate cause | Verdict | Evidence |
|---|---|---|
| **GPU memory exhaustion / OOM** | **ruled out** | zero `vkAllocateMemory`/OOM/image-creation failures in 3319 lines; guest VMM allocation multiset identical (31/31/30/14); Aug had built only 124 graphics + 8 compute pipelines before dying, i.e. it never reached the scale of the 391-pipeline cache that a warm session restores |
| **Detiler / tiling-pipeline bug** | **ruled out** | 7 tiling pipeline groups, identical names, identical `#define` values, in both runs |
| **Missing/corrupt assets, DLC, trophy, fonts** | **ruled out** | same boot errors (`No dumped system fonts`, `Couldn't extract trophy file trophy00`, 316 vs 326 `sceKernelStat error=2`) in both; missing-file *templates* are content-keyed, not new |
| **AJM / audio instance leak** | **ruled out** | Aug 16 create / 15 destroy (the 1 outstanding is the instance alive at kill), Sept 20/19; both show the same 4 `-0x7f6cfff1` batch-wait warnings |
| **Guest kernel / event-flag misuse** | **ruled out** | `ThreadPriority attr is not supported!` present 28/30× in both, benign |
| **Readback mode / config divergence** | **ruled out** | `readbacks_mode=2 (Precise)`, `directMemoryAccess=false`, `readbackLinearImages=false`, 1920×1080 in **both** |
| **Recompiler warning `vcc_hi`** | **not causal by itself**, see §5 table | fires on 101 of 103 graphics-stage compiles; 0 in Sept only because Sept compiled nothing |
| **"Pro lesson is heavier ⇒ needs more GPU work"** | **not supported** | Pro loaded *fewer* idols (5 vs 7); the crash precedes even the dance-motion load |
| **Driver / GPU hardware difference** | **ruled out** | identical GPU model, driver `621.1.0.0`, Vulkan `1.4.357`, same machine |

---

## 7. Ranked hypotheses

### H1 — *New vertex-shader permutation → new pipeline → first draw faults the GPU* — **STRONG**

*Chain:* 3305 creates a **new specialization (permutation) of VS program `0xb3b7b928`** (perm 0 was created at
2704, perm 1 at 2719, so 3305 is a further distinct `StageSpecialization`) → 3306 the recompiler flags that
new translation → 3307 a never-before-seen pipeline `0x59b3f0dd8289f1d6` → the GPU thread never logs again →
3318 the submit returns `VK_ERROR_DEVICE_LOST`.

*Source grounding* (`src/shader_recompiler/specialization.h:81-116`): `StageSpecialization` keys on
`RuntimeInfo` + a resource bitset + **per-vertex-attribute specialisation** (`num_class = GetNumberClass(number
fmt)`, `dst_select`, `divisor`) + buffer/image/fmask/sampler specs + start bindings. So a new VS permutation is
created precisely when a *new vertex attribute number-class combination* is drawn for an already-known program
— exactly the situation when a new character/outfit mesh (here: `chr_orna_*`, `chr_body_*`, `chr_hair_*` for
the 4th/5th idol) enters the frame. A wrong number-class/`dst_select` specialisation, or a fetch-shader
mismatch, yields a shader that reads with the wrong type/stride → GPU page fault or hang → device lost.

*Falsifier:* if no loss occurs when the 391-pipeline cache is warm (same draw uses a *cached* pipeline built in
an earlier session), H1 is wrong.

### H2 — *The loss happened one frame earlier (3133–3138 burst); 3305–3307 is just the last recording* — **MEDIUM**

`vkQueueSubmit` is asynchronous; NVIDIA reports `DEVICE_LOST` on a *later* submit. The six pipelines created at
3133–3138 are the first draw of the lesson's character set, so they are equally exposed.

*Falsifier/confirm:* per-frame timestamps + host markers, or a RenderDoc capture, or `vkValidationSync=true`
(serialises every submit) which would move the assert to the truly-failing submit.

### H3 — *Cold-compile path is the difference; the crash is a "cache off" artefact, not a "Pro" artefact* — **MEDIUM-HIGH as a framing, and the cheapest experiment**

`WarmUp()` returns early when the cache is disabled (`vk_pipeline_serialization.cpp:297-301`), so Aug executed
124 + 8 pipeline creations and 111 shader translations **inside the render loop**, sequentially, on the GPU
thread. Sept executed 0. Nothing in either log shows the *same* lesson run both ways.

*Decisive experiment:* re-run the **Pro** lesson with `pipeline_cache_enabled = 1` (cache already holds 391
pipelines from prior sessions) on the same PC. Outcomes:
- runs → trigger is in the compile/serialization path (H1/H5), and the workaround is trivial;
- still crashes at the same point → the trigger is content/draw related (H2/H4), and the cache is a red herring.

### H4 — *GPU hang / TDR on a pathological draw (huge vertex count, long compute, shader infinite loop)* — **MEDIUM**

The Pro chart may issue a denser note stream / larger batch. A ~2 s TDR would surface exactly like this
(`DEVICE_LOST` from the next submit) and would leave **no** trace in the emulator log.

*Confirm:* Windows Event Viewer → `nvlddmkm` "stopped responding and has recovered" (Event ID 4101) / bugcheck
`0x116`/`0x117` in `C:\Windows\LiveKernelReports` at the crash wall-clock time; also `nvidia-smi -q -d
PAGE_RETIREMENT`.

### H5 — *Pipeline-cache DB corruption / bad restore* — **LOW-MEDIUM**

Sept preloaded 391 pipelines; if any serialized blob is stale relative to the *current* shader recompiler, a
restored pipeline could be inconsistent. This run did **not** crash, so it is not the active trigger, but a
stale blob is a plausible *next* failure once caching is enabled for the Pro lesson.

*Confirm:* delete the pipeline cache DB and re-run with the cache enabled (cold rebuild, then warm hit);
compare `Preloaded N pipelines` counts and look for `Pipeline cache isn't compatible with current system`.
`PipelineCacheArchived=false` in both, so the DB lives in the per-game cache dir.

### H6 — *Device lost from an unrelated subsystem (present thread, readback path)* — **LOW**

`readbacks_mode=Precise` forces GPU→CPU readbacks; a readback racing image/buffer aliasing could fault. But
both runs use Precise, and no readback/`ResolveDepthOverlap` anomalies cluster near the crash (1 each, in both).

---

## 8. Recommended next experiments (cheapest first)

1. **A/B the pipeline cache on the Pro lesson** (H3/H1): `pipeline_cache_enabled=1`, same lesson, same PC.
   Record whether the log reaches `dan_kov_*` and how many `Compiling graphics pipeline` lines appear.
2. **Enable `Vulkan vkValidation=true` + `vkValidationSync=true` + `crashDiagnostics=true`** for one run.
   `vkValidationSync` serialises submits so the assert lands on the *offending* submit; the validation output
   will name the object (pipeline / descriptor / image) that is invalid. This is the single highest-value run.
3. **`shouldDumpShaders=true`** on a crashing run → dump `vs_0x00000000b3b7b928_2.bin/.spv` (and the other
   permutations) — then diff the SPIR-V against perm 0/1 to see what the new vertex specialisation changed.
4. **Timestamped log** (`Log sync: true` already on, but the app prints no time) + `hostMarkers=true`, so
   frame boundaries become visible and H1 vs H2 can be separated.
5. **RenderDoc** capture of the crashing Pro lesson (`rdocEnable=true`); the failing draw will be the last one
   in the capture.
6. **Windows-side evidence** for H4: Event Viewer `nvlddmkm` 4101 / LiveKernelReport at the crash time.
7. **Add a per-compile breadcrumb**: log `pgm_hash + perm_idx + stage` at `GetGraphicsPipeline` (or log the
   recompiler warning *with* `{:#x}`), so the next report can name the failing shader without guesswork.

---

## 9. Appendix — line-number index

| 8月.txt (crash) | 9月.txt (clean) | Item |
|---|---|---|
| 3-6 | 3-7 | game-specific overrides |
| 36 | 36 | `Vulkan PipelineCacheEnabled` false / true |
| 49 | 50 | guest VMM region base `0x5560000` / `0x5440000` |
| — | 122 | `WarmUp: Preloaded 391 pipelines` |
| 1291-1303 | 1293-1305 | first tiling pipeline (`Display2DThin_32`) |
| **1304** | — | **first shader compile (`vs 0x83b353fa`)** — Sept has none |
| 1305 | — | `Float32 denorm flushing is not supported by the GPU` |
| 1306 | — | first graphics pipeline `0xf159a05e3b3aa19d` |
| 2704 / 2719 / **3305** | — | `vs 0xb3b7b928` perm 0 / perm 1 / **last new perm** |
| 3014-3115 | 2739-2838 | lesson `bg2d_stage_*` venue textures |
| 3156 | 2871 | lesson chart `exl_kov_p.jxb` / `exl_kov_r.jxb` |
| 3239 | 2953 | `song3_kov_bgm.nus3bank` |
| 3246 | 2960 | `bg3d_liv_015` / `bg3d_liv_013` |
| 3256 | 2970 | `mdy_pth_liv_015` / `mdy_pth_liv_013` |
| 3272 / 3296 | 2999 / 3011 | `song3_kov_har` / `song3_kov_ior` banks — missing in both |
| — | 3031-3074 | `song3_kov_{azu,mak,yuk}` probes (Sept only) |
| — | 3079-3097 | `dan_kov_d01..d05` dance motions (Aug never gets here) |
| — | 3125 | `cam_cam5_*` camera set |
| 3301-3317 | (~3020) | 5th idol `azu` face/hair streamed in |
| **3318** | — | `vk_scheduler.cpp:195 SubmitExecution: Assertion Failed!` |
| 3319 | — | `Device lost during submit` |
| — | 3150-3177 | `result`, `idolrankup`, `tutorial_memo`, next-lesson UI |
| — | 3185 | `UpdatePlayTime: Playing time for CUSA09153: 38:14:25` |
| — | 3186 | `cache_storage.cpp:137 Close: Cache dumped` |

### Source files consulted (read-only)

* `src/video_core/renderer_vulkan/vk_scheduler.cpp:152-205` — `SubmitExecution`, the failing `ASSERT_MSG`
* `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp:319-347, 603-682` — `GetGraphicsPipeline`, `CompileModule`, permutation selection
* `src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp:297-301` — `WarmUp()` early-out on cache disabled
* `src/shader_recompiler/specialization.h:81-116` — what a "permutation" keys on
* `src/core/emulator_settings.h:33, 393` — `GpuReadbacksMode { Disabled, Relaxed, Precise }`
* `src/video_core/cache_storage.cpp:137` — `Cache dumped` (the clean-exit line)
