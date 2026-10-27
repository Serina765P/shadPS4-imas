<!--
SPDX-FileCopyrightText: 2026 Serina765P
SPDX-License-Identifier: GPL-2.0-or-later
-->

# 第二轮诊断：GS 假设被推翻，改用"故障自证"构建

上一轮结论（GS `OutputVertices` 超限）**已被实际遥测推翻**：新日志里 `max_vert_out` 只有 8 和 20，
截断逻辑从未触发，而地狱特训 Pro 依然闪退。本轮据此重新定位，并把模拟器改造成"崩溃时自证"。

## 1. 两个新崩溃的共同签名

| 场景 | 崩溃前最后动作 | 丢失时的线程 / 显存 |
| :--- | :--- | :--- |
| 地狱特训 Pro（8月/12月，`liv_015` + `kov`） | `vs 0xb3b7b928 (permutation)` → 新管线 `0x59b3f0dd8289f1d6` → 5/5 名偶像加载完、`dan_kov_d01/d02` 舞蹈动作刚开始 | PresentThread，tick 2254，1566 MiB / 6239 MiB |
| 三角光露营区 + medley7（`arn_004` + `m03`，relations 前奏结束） | `vs 0xb3b7b928 (permutation)` → 新管线 `0xfa36d21f2da4ef4f`；此前 1611 行日志里**没有任何新资源加载** | GpuCommandProcessor，tick 8980，1900 MiB / 6239 MiB |

* `0xb3b7b928` 在每份崩溃日志里都恰好被编译 **3 次**：基础版 → 排列 #1（三次崩溃都对应同一条管线
  `0xe54f252e3309840d`，没崩）→ 排列 #2 → 新管线（每次只出现一次）→ 设备丢失。
* 地狱特训 Pro 的崩溃管线和旧版本**完全一致**（`0x59b3f0dd8289f1d6`），说明这是稳定可复现的签名，
  不是随机时序问题。
* 关键修正：设备丢失是在 `vkQueueSubmit` **返回** `VK_ERROR_DEVICE_LOST` 时报出来的，也就是说
  **真正出错的是更早的一次提交**；"新管线"是嫌疑人而不是已证实的凶手。

## 2. 之前对比被混淆的地方

9 月那份"正常"日志是**热缓存**运行：`PipelineCacheEnabled: true` + `WarmUp: Preloaded 391 pipelines`，
整个会话 **0 次着色器编译、0 条管线创建**。而三次崩溃（8月、两份新日志）全部是 `false` 冷编译
（分别编译 166 / 118 / 109 个着色器模块）。因此"某 shader 在正常日志里没出现"不构成证据，
而"冷编译路径"本身成为可疑点（廉价验证见第 4 节）。

其余已排除项：显存耗尽（1.5–1.9 GiB / 6.2 GiB，无分配失败）、GS 寄存器异常（8/20，mode 3）、
tiling/detiler 差异（三份日志一致）、着色器/资源告警（与启动时集合一致，无新增）、音频泄漏。

## 3. 本轮的"自证"构建（分支 `codex/idolmaster-release-gs-device-lost`）

CI 产物对应提交 `f8bb2b33`：

1. **`VK_EXT_device_fault` 故障报告** —— 设备丢失时调用 `vkGetDeviceFaultInfoEXT`，打印驱动描述、
   故障地址 + 地址类型（读/写/执行/指令指针）、NVIDIA 厂商故障码。这一步直接区分
   **非法显存访问** 与 **GPU 挂起/TDR**，是当前最缺的信息。
2. **新管线 key 日志** —— 每条新建图形管线打印各阶段 shader hash、MRT 掩码、各颜色附件采样数、
   深度采样数、图元类型、patch 控制点，用于识别崩溃管线是"角色/特效/后处理"中的哪一类。
3. **新管线首绘日志** —— 记录第一次使用该管线的绘制调用（是否 indexed、count、instances、
   vertex/instance/index 偏移、间接参数地址）。
4. **GPU 飞行记录器（`vk_gpu_trace.h`）** —— 环形缓冲记录最近 512 次绘制 + 每次提交的分隔标记；
   设备丢失时整段打印。这样就能看到"出问题的那次提交里到底画了什么"，把驱动故障地址和具体绘制对上。
5. 顺带修掉上一版引入的日志噪声：GS 信息从每帧 `BuildRuntimeInfo` 移到管线创建处
   （原来一份日志刷了 1380 行）。

## 4. 请用新构建做的三次实验（按性价比排序）

新构建：`630a10e7`（CI run [40514050350](https://github.com/Serina765P/shadPS4-imas/actions/runs/40514050350)，
Windows/Linux 均 success）。请先只跑实验 A，它的日志就能决定后续方向。

**A. 直接复现（必做）**：地狱特训 Pro，保持 `pipeline_cache_enabled = 0`（和崩溃时一致）。
新日志中关注四行：
* `Reusing {} stage with {} extra image descriptor(s)` → 本轮修复的那条路径是否真的被走到
  （出现即说明"复用阶段带多绑定图像"，正是根因所在）；
* `Driver device fault details: ...` → 有地址 = 非法访问（读/写/执行），无细节 = 驱动复位/TDR；
* `GPU trace: ...` → 最近 512 次绘制与提交边界，最后一段就是可疑提交；
* `New graphics pipeline {:#x}: ...` / `First draw with new pipeline {:#x}: ...`。

**B. 热缓存对照（一次运行即可证伪"冷编译"相关假设）**：把 `pipeline_cache_enabled` 改成 **1**
（首次运行会生成缓存在磁盘，之后同一课程再跑一次），再进地狱特训 Pro。

**C. 最强定位（可选，较慢）**：`vkValidation=true` + `vkValidationSync=true`。同步校验会把提交串行化，
断言就会落在真正出错的那次提交上，校验层会直接指出非法对象/参数。

## 5. 假设排序（本轮）

| # | 假设 | 依据 | 如何验证 |
| :--- | :--- | :--- | :--- |
| H1 | **新建管线/排列的首绘触发 GPU 故障或挂起** | 三次崩溃均紧随"新排列 → 新管线"；同一条管线 hash 跨版本复现；medley 崩溃处无任何新资源 | 故障报告（有无地址）+ 飞行记录器最后一帧的绘制 |
| H2 | **冷编译 / 运行时创建管线路径** | 唯一正常日志是热缓存（0 编译），全部崩溃日志是冷编译 | 实验 B |
| H3 | **GPU 挂起（TDR）** | 挂起不会留下故障地址，且 Windows 事件日志会有 `nvlddmkm` 4101 | 故障报告无细节 + 事件查看器 |
| H4 | 资源生命周期（上传/回读悬垂） | 上一轮审计在 `b3d75746` 的异步失效路径发现真实悬垂引用，但本次场景未播放视频 | 若故障地址落在已释放缓冲区范围内则可确认 |
| H5 | 前一轮 fork 性能补丁（`8d98d6c0`） | 尚未做过 A/B | 需要时可用 `git revert --no-edit 8d98d6c0` 单独构建对照 |

## 6. 本轮同时落地的根因修复：置换复用时的绑定计数错误

审计（`renderer-audit-2.md`）发现并已被源码确认的**真实缺陷**：

* 图形管线按 **FS → VS** 的顺序编译/编号描述符（`LogicalStage` 枚举里 `Fragment` 在 `Vertex` 之前，
  `RefreshGraphicsStages()` 也先绑 FS，`vk_pipeline_cache.cpp:531`）。
* 描述符编号在三个地方必须完全一致：SPIR-V 发射器（`spirv_emit_context.cpp:961-970`）、
  布局构建（`vk_graphics_pipeline.cpp:457-467`）、绘制时绑定（`vk_rasterizer.cpp:817-832`），
  它们对图像都使用 `ImageResource::NumBindings()`——NVIDIA 没有 AMD 的图像 LOD 扩展，
  所以"带 LOD 的 image store"会退化成 **按 mip 层级展开的多绑定**（`resource_tracking_pass.cpp:552,593`），
  `NumBindings()` 可能 > 1。
* 而缓存命中时推进编号的 `Info::AddBindings()`（`info.h:189`）却按 `images.size()` 只算 **1** 个绑定。

后果：当 FS 命中缓存（`AddBindings`）而 VS 需要新编译（新排列）时，VS 的起始绑定号比管线布局少
`Σ(NumBindings-1)`，于是 VS 在 SPIR-V 里读的绑定号与布局/写入的绑定号错位 → 绘制时读到伪造的描述符
→ GPU 访问非法地址 → 下一次提交 `DEVICE_LOST`。这与三份崩溃日志完全吻合：**崩溃帧里只有 VS 被编译，
FS 是复用**；也解释了为什么只在冷编译会话里出现，以及为什么同一管线 hash 能跨版本稳定复现。

修复（提交见分支）：

* `Info::AddBindings()` 改为按 `NumBindings()` 累加，与其余三处一致；
* 复用分支打印多出来的描述符数量与当前编号游标（下次日志可直接确认是否命中该路径）；
* `ShaderBinaryVersion` 3 → 4，作废用旧编号编译的缓存 SPIR-V。

这个缺陷同样存在于上游主线（`git show upstream/main:src/shader_recompiler/info.h` 里 `AddBindings`
是同样的写法），确认后可反馈上游。

## 7. 下一步（如果 A 的报告指向具体绘制）

* 若故障地址落在**顶点/索引缓冲**区间：检查该模型 V#（`NumRecords`/stride/format）与 `setVertexInputEXT`
  生成的绑定是否一致（`buffer_cache.cpp:177` `BindVertexBuffers`、`pipeline.GetVertexInputs`）。
* 若故障地址落在**纹理/常量缓冲**：检查该管线首绘时的描述符写入（`pipeline->BindResources`）与该排列
  声明的资源数量是否匹配。
* 若无故障地址（TDR）：按 H3 处理——检查是否有超长提交（precise readback 的 detile/download 批量），
  并考虑把大帧拆成多次提交。
* 若 B 实验表明"热缓存不崩"：问题在运行时管线创建路径，可对比 WarmUp 预载与运行时创建两条路径的差异
  （`vk_pipeline_cache.cpp` `WarmUp` / `GetGraphicsPipeline` / `GetProgram`）。
