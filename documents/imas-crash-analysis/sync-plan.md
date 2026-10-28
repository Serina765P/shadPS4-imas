<!--
SPDX-FileCopyrightText: 2026 Serina765P
SPDX-License-Identifier: GPL-2.0-or-later
-->
# 同步评估：Nyces/imas 与上游 main（2026-10-28）
本轮问题：`https://github.com/Nyces/shadPS4/tree/imas` 与上游 `shadps4-emu/shadPS4` 主线的更新中，
哪些是 `codex/idolmaster-release-gs-device-lost` 必须同步的。
## 0. 结论
* **Nyces `imas`：没有可同步的内容。** 他的 5 个 commit 是本分支（0.16.1 线）已有补丁在 **0.19.1 上的重移植**，
  逐项核对后功能全部已经在我们分支里（见第 2 节）。
* **上游主线：424 commits，不能直接合并，但有一小组必须落地。** 与本分支同一子系统的 16 个修复里，
  **3 个可以干净 cherry-pick**，**3 个需要手工移植（合计约 40 行）**，其余被 0.20.x 的结构重构挡住（见第 3 节）。
## 1. 基线
| 项 | 值 |
| :--- | :--- |
| 分叉点 | `dc5a33d6` 2026-07-26 `avplayer: improve sync between audio and video (#4761)` |
| 本分支 HEAD | `9e02b4666`（55 commits） |
| 上游 HEAD | `3eecea94` 2026-10-26，版本 **0.20.1**（本分支 0.16.1） |
| 上游独有 | **424** commits / **623** 个文件 |
| 双方都改过的文件 | 50 |
| 直接合并的冲突面 | **87 条冲突记录，涉及 29 个文件**（`git merge-tree --write-tree HEAD upstream/main`） |
| 已 backport 的上游 PR | #4839 #4859 #5001 #5092 #5110 #5196 #5231 #5258 #5317 |
冲突文件里既有本分支的全部热路径：`buffer_cache.cpp/.h`、`vk_rasterizer.cpp/.h`、`vk_pipeline_cache.*`、
`vk_scheduler.cpp`、`vk_instance.cpp`、`vk_graphics_pipeline.cpp`、`vk_pipeline_serialization.cpp`、
`texture_cache/*`、`shader_recompiler/info.h`、`amdgpu/liverpool.cpp`。
## 2. Nyces `imas`：逐条对照
他的分支基于上游 `bf794b3f`（2026-10-02，0.19.1），只有 5 个 commit。
`git diff HEAD nyces/imas` 显示的 351 行 `liverpool.cpp`、140 行 `vk_scheduler.cpp` 差异几乎全是
**0.16.1 → 0.19.1 的上游演进**；判断实际差异必须看 `git diff bf794b3f nyces/imas`。
| commit | 内容 | 我们分支的对应实现 | 结论 |
| :--- | :--- | :--- | :--- |
| `7cae56722` | GS copy shader 常量解析泛化（`ReadConstant`、`S_ADD/S_AND/S_OR/S_LSHR`、`BUFFER_LOAD_DWORDX2/X4`） | `copy_shader.cpp` 已有全部这些（`b1820a654`） | 已具备 |
| `93cec5be2` | AJM 批次生命周期、输出节奏、线程优先级 | `3adad4a75` + `34785dc9d`；`VeryHigh` 优先级见 `audioout.cpp:172`、`ajm_context.cpp:69`；SDL `Output()` 已不再 sleep（`sdl_audio_out.cpp:232`） | 已具备 |
| `765073c73` | PM4 类型 0 寄存器写、未知报文跳过、跨环报文长度、`WaitVoLabel` 不阻塞、500ms 自旋告警、`submit_mutex` 递归、信号量 500ms 分片 | `vk_scheduler.h:426` 已是 `std::recursive_mutex`；`SubmitExecution` 已在 `Refresh()` 前 `lk.unlock()`；`vk_master_semaphore.cpp` 已是 500ms 分片 + `Refresh()/IsFree` 复查 + `eErrorDeviceLost` 处理；`WaitVoLabel` 已改为 `ProcessCommands() + YIELD_GFX()`（`liverpool.cpp:904`）；CE/GFX/ASC 的 type 0 写、未知 type/opcode 跳过、MemSemaphore/Rewind 500ms 告警均在 | 已具备 |
| `5d6c2150e` | 关闭管线优化 `eDisableOptimization` | `vk_graphics_pipeline.cpp:365`、`vk_compute_pipeline.cpp:107` | 已具备 |
| `549b673ff` | GFX PM4 type-3 分支花括号结构 | 修的是他移植代码自身的结构；我们的 `default:` 已在 switch 内 | 不适用 |
**结论：从 `imas` 同步为零。** 他的分支价值在于反证——我们在 0.16.1 上的这些修复至今没有上游等价物。
## 3. 上游同步清单
### 3.1 可干净落地（实测 `git cherry-pick -n` 退出码 0）
| 提交 | PR | 内容 | 规模 | 价值 |
| :--- | :--- | :--- | :--- | :--- |
| `6752d8b19` | #5447 | shader_recompiler 处理 buffer cache 失效指令 | 2 文件 / 12 行 | **高**：与本分支 `b3d75746` 异步失效 + 已 backport 的 #5092 直接配套 |
| `c679ce609` | #5229 | texture_cache 初始化新增 depth-overlap 子资源 | 1 文件 / 5 行 | 中：图像验证健壮性 |
| `fec7684d7` | #5463 | gnmdriver 重映射 compute queue 起始环偏移 | 1 文件 / 1 行 | 低：本作无 compute 场景，顺手带上无害 |
### 3.2 需手工移植（都很小；冲突原因：依赖被重构过的上下文）
| 提交 | PR | 内容 | 规模 | 价值 |
| :--- | :--- | :--- | :--- | :--- |
| `1a0f5be8d` | #5137 | `Sampler::Valid()` 收窄 + 非法 S# 绑默认 sampler | 2 文件 / 14 行 | **高**：与本次根因同属"客户机描述符全零 → 非法驱动状态" |
| `699009c9` | #5444 | `ExpandImage` 不复制陈旧 CPU-dirty 内容 | 1 文件 | 中高：流式纹理槽位复用时的陈旧覆盖 |
| `4f1a70f6` | #5504 | `RefreshImage` 时重置 `gpu_hash` | 1 文件 | **不适用**：0.16.1 没有 `gpu_hash` 字段（`grep -rn gpu_hash src/video_core` 为空），`TouchImage` 也不碰任何 hash，该缺陷的等价实现不存在 |
### 3.3 不取（0.20.x 结构重构，与 0.16.1 不可共存）
`Runtime` 抽象与 sparse arenas（#5047 / #5193 / #5233，`buffer_cache.h` 里出现 `class Runtime`、
`ObtainBufferForImage`）、渲染优化 pt1/pt2（#5112 / #5219）、runtime info tagged union（#5027）、
wave64 lowering（#4995）、garbage collector（#3396）、CMask（#5046）。这 100+ commits 是 87 条冲突的主体；
取其中任何一条都会把整条重构链拖进来。
### 3.4 本分支补丁在合并时的处置
| 补丁 | 处置 |
| :--- | :--- |
| `2c141d516` 非法顶点输入过滤 | **保留**（上游 `vk_graphics_pipeline.cpp:437` 仍在为每个 semantic 无条件下 `VK_FORMAT_UNDEFINED` 属性；建议提 PR） |
| `0cd63fd71` `AddBindings` 描述符数组计数 | **保留**（上游 `info.h:162` 仍是 `bnd.buffer += buffers.size()`；建议提 PR） |
| `ca4a1561e`/`f8bb2b33`/`4681f99`/`a556b0e` 设备丢失诊断 | **保留**（上游 `vk_scheduler.cpp:241` 仍直接 `ASSERT_MSG`） |
| `630a10e7` `PipelineKeyVersion` | 合并后需在新序列化代码上重做 |
| `726272ba` GS `OutputVertices` 钳制 | 合并时丢弃：上游 #5075 / #5027 已按 `runtime_info.hw.gs.output_vertices` 正确写入 |
| `005f24b37` 跳过渲染目标的 `UpdateImage` | **建议撤销**：上游保留 3 处调用，并持续在同一路径修 bug（#5504 / #5444 / #5433） |
| `b3d75746` 异步内存失效 | 保留，与 3.1 的 #5447 配套 |
### 3.5 推荐路径
1. **现在**（0.16.1 线）：land 3.1 的三条 + 手工移植 #5137；#5504 / #5444 与"撤销 `005f24b37`"一起做 A/B。
2. **以后**：只在需要 0.20.x 新功能时做全量合并（29 文件冲突 + 上述两个重构 + 本表 4 个 fork 补丁重新落位）。
   本作的崩溃根因与 GS 假设都已排除，没有被 0.20.x 修掉而我们仍缺的阻塞项。
## 4. 验证
* CI：`Build windows-x64` / `Build linux-x64`（含 CPU 测试）全绿。
* 实机：地狱特训 Pro（8 月、12 月各一遍）+ medley7 `kimi wa relations -mix medley`，各完整跑完。
* 日志判据：`Critical` 行为 0；`First draw with new pipeline …` 的 `attributes dropped` 只出现在带空 V# 的布局上；
  无 `Device lost`、无 `Querying VK_EXT_device_fault`。
## 5. 复现命令
```bash
git fetch upstream --prune
git fetch --filter=blob:none nyces "+refs/heads/imas:refs/remotes/nyces/imas"
git rev-list --left-right --count upstream/main...HEAD          # 424 / 55
git merge-tree --write-tree HEAD upstream/main | grep -c CONFLICT
git diff bf794b3f nyces/imas --stat                             # 他的真实改动
for c in <候选提交>; do git cherry-pick -n $c; git cherry-pick --abort; done
```

## 6. 本轮实际落地（测试构建）
| 方式 | 提交 | 说明 |
| :--- | :--- | :--- |
| cherry-pick `-x` | `6752d8b19` #5447 | `BUFFER_WBINVL1_SC` / `BUFFER_WBINVL1` 译为 `DeviceMemoryBarrier()`；MUBUF 表项参数量归零 |
| cherry-pick `-x` | `c679ce609` #5229 | depth-overlap 新建子资源先 `RefreshImage` 再清 Dirty |
| cherry-pick `-x` | `fec7684d7` #5463 | 重映射 compute queue 的起始环偏移 |
| 手工移植 | #5137 | `Sampler::Valid()` 增加范围校验；`BindTextures` 对非法 S#（或自定义 border 且 `ta_bc_base` 为 0）改用默认 sampler 并告警 |
| 手工移植 | #5444 | 新增 `Image::HasGpuValidData()`；`ExpandImage` 仅在源图带有效 GPU 数据时 `CopyImage` 并继承 `GpuModified` |
| 未落地 | #5504 | 见 3.2：0.16.1 无 `gpu_hash`，无可修的等价缺陷 |
未纳入本轮（留作独立 A/B，避免混淆稳定性结论）：
* 撤销 `005f24b37`（跳过渲染目标的 `UpdateImage`）：行为改动，与渲染通道优化互相影响。
* 丢弃 `726272ba`（GS `OutputVertices` 钳制）：只在全量合并上游时成立。
构建：CI run [40646944820](https://github.com/Serina765P/shadPS4-imas/actions/runs/40646944820)，
`52147580a`，`Build windows-x64` 与 `Build linux-x64`（含 CPU 测试）均 success；
产物 `shadps4-idolmaster-windows-x64-52147580a649a94c72b1c161d684d0eea0b14ba5`。
新增可观测点：`Rejecting invalid S# max_aniso=…, filter_mode=…, mip_filter=…, border_color_type=…,
border_color_base=…`。出现即说明该管线送来了非法 S#，与本轮顶点输入修复同属"客户机描述符未初始化"类。
