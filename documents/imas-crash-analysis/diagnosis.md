<!--
SPDX-FileCopyrightText: 2026 Serina765P
SPDX-License-Identifier: GPL-2.0-or-later
-->

# 《偶像大师 星光舞台》地狱特训 Pro 闪退诊断与修复计划

对象：`THE iDOLM@STER STELLA STAGE [CUSA09153][ASIA]`（5 人 Live 场景）
日志：`8月.txt`（闪退）、`9月.txt`（正常），同一台 PC（RTX 5060 Laptop / i7-13700HX / 16 GB / 1080p 窗口 / `readbacks_mode = 2` Precision）
版本：shadPS4 v0.16.1 WIP，`26d27ae` + 本 fork 的性能补丁

## 1. 结论摘要

* 闪退**不是游戏逻辑问题，也不是模拟器主动的 bug 检查**：`vkQueueSubmit` 返回了 `VK_ERROR_DEVICE_LOST`，
  即 **GPU 本身已失效**（显卡驱动把设备复位了）。`vk_scheduler.cpp:195` 的断言只是把这个既成事实打印出来。
* "闪退"观感来自断言实现：`src/common/assert.cpp:16` 的 `assert_fail_impl()` 先 `Emulator::Shutdown()` 再
  `int 3` 触发断点异常，进程直接消失，没有弹窗、没有 dump。
* 崩溃点高度集中：8 月日志在**课程开场阶段**（尚未进入演奏，从未请求 `dan_kov_*` / `cam_*` / `result` 资源），
  在 GPU 线程刚编译出**一条全新的图形管线** `0x59b3f0dd8289f1d6`（第 3307 行）之后的下一次提交就 DEVICE_LOST。
* 本次运行里**存在且仅存在一条几何着色器（GS）管线**：`es 0x2b5ab32b` + `gs 0xf3bd17fb` +
  `pipeline 0xb3c99c2279216186`（第 2841–2845 行）。GS 管线在首绘时若 `OutputVertices` 声明超限，正是
  "首次绘制 → GPU fault/TDR → 下一次 submit 报 DEVICE_LOST" 的经典形态。
* 因此本分支优先落地**上游仍在评审的同型修复**（shadps4-emu/shadPS4#5523：GS `OutputVertices` 截断），
  并补齐设备丢失的诊断信息，让下一次复现能直接判定是"非法显存访问"还是"GPU 挂起"。

## 2. 证据链

### 2.1 两份日志的差异是"内容"而不是"配置"

前 1320 行逐行同步对比只有 12 条差异记录：

| 项目 | 8月（崩溃） | 9月（正常） |
| :--- | :--- | :--- |
| GPU / 驱动 / Vulkan | RTX 5060 Laptop / 621.1.0.0 / 1.4.357 | 完全相同 |
| 分辨率 / readbacks | 1920×1080 / Precision(2) | 完全相同 |
| 客户机 VMM 分配 | 31/31/30/14 | 完全相同 |
| `pipeline_cache_enabled` | false（冷编译，172 个着色器） | true（`Preloaded 391 pipelines`，0 个编译） |
| 课程谱面 / 场地 | `exl_kov_p`（Pro）/ `bg3d/liv_015` | `exl_kov_r`（Regular）/ `bg3d/liv_013` |

即：**缓存开关、谱面难度、场地、编成人数**同时不同，A/B 无法只归因于缓存。9 月那条日志跑到了
`result.luac` / `idolrankup` / `tutorial_memo`，8 月那条连 `dan_kov_*`（舞蹈）都没请求过 —— 崩溃发生在开场。

### 2.2 崩溃时序（GPU 线程单线程，顺序即因果）

```
3069/3071  fs 0xeeca4b6b (permutation) + 新管线 0x709f65b9a94ee0bd
3133-3138  一次突发创建 6 条新图形管线（无着色器编译，模块已存在）
3305       vs 0xb3b7b928 (permutation)   ← 第三次为该 VS 生成排列（新顶点布局/第 5 名偶像）
3306       [Render.Recompiler] First instruction is not s_mov_b32 vcc_hi, #imm
3307       新图形管线 0x59b3f0dd8289f1d6（全日志仅此一处）
3308-3317  GPU 线程静默，ResourceJob 继续加载第 5 名偶像（azu）的脸/发资源
3318       [Debug] <Critical> ... vk_scheduler.cpp:195 SubmitExecution: Assertion Failed! / Device lost during submit
```

`First instruction is not ...` 在本日志出现 102 次（101/103 个图形阶段编译都会打印），**不能单独作为线索**；
但最后一次（3306）确实紧贴崩溃点。

### 2.3 已排除

* **显存耗尽**：两份日志都没有任何分配失败；客户机 VMM 分配集合完全一致；崩溃前只建了 124 条图形管线。
* **detiler / 平铺管线**：两份日志的 7 组 tiling 管线及 `#define` 完全一致。
* **资源缺失 / 音频泄漏 / 内核接口**：警告-错误模板集合除"有着色器被编译"这一症状外一致（AJM 16/15 vs 20/19）。
* **AvPlayer 异步失效路径**：两份日志中 `libSceAvPlayer` 只被加载，**从未播放任何视频**，`InvalidateMemoryAsync`
  不可能在本次崩溃中被调用。

## 3. 根因假设排序

| # | 假设 | 依据 | 状态 |
| :--- | :--- | :--- | :--- |
| F1 | **GS 管线的 `OutputVertices` 声明超过设备上限**，首绘即非法 → GPU fault/TDR → 下一次 submit DEVICE_LOST | `regs.h` 中 `vgt_gs_max_vert_out` 为 11 bit（≤2047），`vk_pipeline_cache.cpp:155` 原样写入 `gs_info.output_vertices`，`emit_spirv.cpp` 原样声明；上游 #5523 对**同一句断言**给出同一根因（LBP3）；本次运行确实存在唯一一条 GS 管线 | **高**，本次修复目标 |
| F2 | `b3d75746` 引入的异步失效路径：`DownloadBufferMemory<true>` 的延迟回调 `write_data` 以 `[&]` 捕获局部量（`copies` / `download` / `buffer`），在 `PopPendingOperations` 时才执行 → 悬垂引用 | `buffer_cache.cpp:159-174`；该路径在此之前是死代码，`b3d75746` 才让它 60 Hz 可达 | 真 bug，但**本次崩溃无关**（无视频播放），另开 issue 修 |
| F3 | `005f24b3` 移除渲染目标的 `UpdateImage` 调用（同时丢掉了 `TrackImage`/`TouchImage` 的 LRU 追踪） | `texture_cache.h:115-121`、`texture_cache.cpp:635-701`、`vk_rasterizer.cpp:850-853` | **中**，观感/贴图正确性问题；不构成 layout 错乱（`vk_rasterizer.cpp:885,930` 在 `Transit` 后重新推导 layout） |
| F4 | `SubmitExecution` 先 `NextTick()` 再取 `submit_mutex`，且锁改为 `recursive_mutex`、提前解锁 | `vk_scheduler.cpp:152-155`、`199-204` | 低（仅空帧路径可跨线程），非本次原因 |
| F5 | 完全没有设备故障诊断：`VK_EXT_device_fault` 被启用却从未调用 `vkGetDeviceFaultInfoEXT`；`crashDiagnostics` 只是加载 NVIDIA 的崩溃诊断层 | 全仓库无调用点 | 事实，见第 4 节 |
| F6 | 提交只对 `DEVICE_LOST` 断言；其它错误会让时间线永不推进，`MasterSemaphore::Wait` 无限自旋（崩溃变卡死） | `vk_scheduler.cpp:194`、`vk_master_semaphore.cpp:64-70` | 事实，见第 4 节 |

## 4. 本分支的改动

分支：`codex/idolmaster-release-gs-device-lost`（同时满足 CI 触发规则 `codex/idolmaster-release-*`）

### 4.1 `shader_recompiler: bound geometry shader OutputVertices to a portable count`

* `src/shader_recompiler/info.h`：新增 `Info::gs_output_vertices`。
* `src/shader_recompiler/ir/passes/ring_access_elimination.cpp`：GS 分支计算要声明的顶点数，
  超过 256（Vulkan 规定所有实现都必须支持的下限）时截断并 `LOG_WARNING`，日志同时打印 copy shader 认为的
  顶点数。**不使用** copy shader 的启发式计数作为声明值（低估会截断几何体）。
* `src/shader_recompiler/backend/spirv/emit_spirv.cpp`：优先使用 `info.gs_output_vertices`；
  为 0 时保持旧行为（用寄存器原值）。
* `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`：构建 GS 管线时打印
  `max_vert_out / vert_itemsize / esgs_ring_itemsize / instance_cnt / mode / copy shader`，
  下次日志可**直接证实或证伪 F1**。
* `src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp`：`ShaderBinaryVersion` 2 → 3，
  作废旧着色器缓存（否则管线缓存里的旧 SPIR-V 仍带超大声明）。

### 4.2 `video_core: report Vulkan device loss instead of dying silently`

* `vk_scheduler.cpp`：`DEVICE_LOST` 时先打印 signal tick、等待/信号量数量、显存占用/预算、线程名，
  并提示复现所需设置（`vkValidation` / `vkValidationSync` / `shouldDumpShaders`，以及 Windows 事件日志
  `nvlddmkm` 4101 = TDR），再断言。
* `vk_master_semaphore.cpp`：`VK_ERROR_DEVICE_LOST` 不再被当作"超时"反复重试（设备丢失后时间线永不推进，
  原逻辑会死循环），改为打印 tick 后断言。

## 5. 如何验证

1. CI（`idolmaster-release.yml`，push 到 `codex/idolmaster-release-*` 或 `release` 触发）产出
   `shadps4-idolmaster-windows-x64-<sha>`，解压得到 `shadPS4.exe`。
2. **保持与崩溃时相同的配置**：`pipeline_cache_enabled = 0`（8 月就是冷编译状态复现的），
   `readbacks_mode = 2`，1080p。
3. 进入 8 月 / 12 月的**地狱特训 Pro**，跑完开场并进入演奏。
4. 判定：
   * **修复成功**：日志出现 `Geometry program: max_vert_out = ...`（记录原值）与/或
     `Geometry shader ... declaring 256 output vertices`（说明确实触发了截断），课程可以进入 `dan_kov_*`
     舞蹈段并正常结算。
   * **未修复但可定位**：新的 `Device lost on queue submit: ...` 行会带显存/线程信息；若日志显示
     `max_vert_out` 本来就 ≤ 256（未触发截断），则 F1 被证伪，请按第 6 节 A/B。

## 6. 如果未修复：下一步 A/B（按性价比排序）

1. **确认 F1 是否被触发**：看新日志里 `Geometry program: max_vert_out` 的数值。
   ≤256 说明 GS 不是元凶。
2. **开 `vkValidation=true` + `vkValidationSync=true`**：`vkValidationSync` 会把提交串行化，
   断言就会落在**真正出错的那次提交**上，VVL 会指出非法对象/参数。这是拿到确定答案的最快路径。
3. **`pipeline_cache_enabled` 1 ↔ 0 对照**：8 月是 0（冷编译）。若 1 时不再崩溃，说明问题与
   "新建管线/新排列"的路径相关（F1 或管线状态），与缓存数据本身无关。
4. **A/B 掉性能补丁**：`git revert --no-edit 8d98d6c0`（同时撤销 F2/F3 两处改动）再跑同一课程。
   这是唯一能一次性排除 fork 自身改动的实验；若此步不崩、而带补丁必崩，则根因在 8d98d6c0 内。
5. **看 Windows 事件查看器**：`nvlddmkm` 事件 4101 / LiveKernelReport 可区分
   "GPU 挂起（TDR）"与"非法显存访问"。

## 7. 需要回报的信息

* 新版本日志（完整，勿裁剪），尤其是 `Geometry program:` / `Geometry shader ...` / `Device lost on queue submit:` 三行；
* 是否出现 `nvlddmkm` 4101；
* 同一课程在 `pipeline_cache_enabled=1` 时是否仍崩溃。

## 8. 附：相关上游工作（均为 **open** PR，尚未合并）

* shadps4-emu/shadPS4#5523 —— `OutputVertices` 声明超 `maxGeometryTotalOutputComponents` → 设备丢失（LBP3 同款断言），本分支据此移植。
* shadps4-emu/shadPS4#5014 —— 泛化 GS copy shader 常量解析；提交说明明确提到
  "crash during GS pipeline compilation (e.g. THE IDOLM@STER STELLA STAGE)"；本 fork 已有自己的 copy shader
  加固（`b1820a65`），若后续仍出现 copy shader 解析断言可参考。
* shadps4-emu/shadPS4#5453 —— 截断 guest mip 层级，避免非法 `VkImageCreateInfo` 引发设备丢失（备选）。
