<!--
SPDX-FileCopyrightText: 2026 Serina765P
SPDX-License-Identifier: GPL-2.0-or-later
-->

# 第三轮：设备丢失的自证链路被自己打断，顶点输入状态是非法的 Vulkan 状态

输入：`新建文件夹 (11)/medley.txt`（三角光露营区 + medley7，relations 前奏结束）与
`新建文件夹 (11)/shad_log.txt`（地狱特训 Pro，`dan_kov_d01/d02` 开始处），两份均来自
`630a10e7`（第二轮"自证"构建）。两台机器、同一份日志。

## 1. 新日志给出的三条硬事实

### 1.1 设备丢失后，进程死在驱动查询里，飞行记录器一个字都没输出

第二轮构建在 `Device lost on queue submit` 之后应当输出 `Driver device fault details:` / `GPU trace:`，
但两份日志都在第二条 CRITICAL 之后直接 End：

```
[Render.Vulkan] <Critical> (shadPS4:PresentThread) vk_scheduler.cpp:212 SubmitExecution: This is a GPU-side failure…
[Debug] <Critical> (shadPS4:PresentThread) signals.cpp:70 SignalHandler: Unhandled Exception code 0xc0000005 at 0x7ffd68f5b01c
```

对照上一版（`d7e66f5e`）的 `medley7闪退.txt` / `地狱特训闪退.txt`：同一位置是
`Assertion Failed! Device lost during submit`，**没有** `0xc0000005`。两版之间唯一新增的执行路径就是
`Instance::GetDeviceFaultReport()` → 这次访问违例发生在 `vkGetDeviceFaultInfoEXT` 内部（地址落在系统 DLL 区）。
即：**在该 NVIDIA 驱动上，设备丢失后查询故障信息会让宿主进程崩溃**；它顺带吞掉了本轮唯一要看的证据。
这是本 fork 自己引入的缺陷，必须先修。

### 1.2 崩溃管线没有几何着色器

两次崩溃的新管线都是 `gs = 0x0`（`GraphicsPipelineKey::stage_hashes[Geometry]` 为 0）：

```
New graphics pipeline 0x59b3f0dd8289f1d6: vs = 0xb3c96b2cc65, fs = 0x1fd6f7b7218, gs = 0x0, mrt_mask = 0x1, color_samples = 4/0/0/0/0, depth_samples = 4, prim = 4
New graphics pipeline 0x3bb608255dbb5213: vs = 0xb3c96b2cc65, fs = 0x298f09185d9, gs = 0x0, mrt_mask = 0x1, color_samples = 4/0/0/0/0, depth_samples = 4, prim = 4
```

**⇒ 对 Nyces 提交的评估**：`9e05f553`（及其完整版 `56d57cba`）修的是 GS **copy shader** 解析时
`vcc_lo/vcc_hi/m0/exec` 这类特殊 SGPR 没被当成常量源，导致 buffer load 被静默丢弃 → `attr_map` 错 →
`output_vertices` 错。那需要管线里存在几何着色器（copy shader 只在 GS 输出展开时使用）。
本次两个崩溃的管线 `gs = 0x0`，没有 copy shader，**因此该提交与这两个崩溃无关**。
（他的另一个提交 `b1820a65` 本 fork 已包含，就是现在的 copy_shader 加固版。）
真正和这次相关的是他 `fix` 分支里的 `7fc805eed`（`eDisableOptimization`，绕过驱动在
`vkCreateGraphicsPipelines` 里卡死）——但本日志里管线创建是成功返回的（"New graphics pipeline" 之后才发生丢失），
所以也不是它。

### 1.3 两次崩溃的 VS 是同一排列，FS 不同；同一 FS 的上一排列工作正常

地狱特训日志里，同一个 FS（`0x1fd6f7b7218`）先后与 VS 的第 2 个排列、第 3 个排列组成管线：

| 管线 | VS 排列 | FS | 首绘 | 结果 |
|---|---|---|---|---|
| `0x1f707112ae2d2720` | `0xb3c96b2cc63`（perm 2） | `0x1fd6f7b7218` | indexed, count 12 | 正常 |
| `0x59b3f0dd8289f1d6` | `0xb3c96b2cc65`（perm 3） | `0x1fd6f7b7218` | indexed, count 36 | **DEVICE_LOST** |

两次崩溃的 VS 都是 `0xb3c96b2cc65`；medley 那次的 FS 是另一个（`0x298f09185d9`）。
⇒ 差异不在 FS、不在采样数、不在图元类型、不在 MRT —— **只在 VS 面向的顶点取数布局**
（`StageSpecialization::fetch_shader_data` / `vs_attribs`），也就是第 5 名偶像/该曲目专属模型的
fetch shader 属性表。这与"只在 三角光露营区 + relations、只在 5 人地狱特训 Pro 复现"完全一致。

AddBindings 复用例在本次日志中依然没有触发（无 `Reusing … extra image descriptor(s)`），
所以第二轮的绑定游标缺陷不是这两个崩溃的成因（缺陷本身是真的，修复保留）。

## 2. 顶点输入状态：为什么一个"新布局的首绘"能直接打死 GPU

`GraphicsPipeline::GetVertexInputs()`（`vk_graphics_pipeline.cpp:385`）把 fetch shader 的每条属性翻译成
一条 Vulkan 顶点输入，并把 `attrib.semantic` **同时**当作 `location` 与 `binding`：

* 属性来自 `attrib.GetSharp(vs_info)`，即跑在该 VS 用户数据里的一条 V#。
* 当这条 V# 是 0/空（游戏把未使用的流清零；或该模型带有一条未使用的属性），
  `GetDataFmt()` 是 `FormatInvalid` → `SurfaceFormat()` 返回 `VK_FORMAT_UNDEFINED`，`GetSize()` 为 0。
* `BufferCache::BindVertexBuffers()` 对 `base_address == 0 || GetSize() == 0` 的属性写入
  `VK_NULL_HANDLE`，但 `vkCmdSetVertexInputEXT` 仍然为它声明了 binding（stride 为 0）。

于是首绘时驱动拿到的是"被 attribute 引用、却从未绑定的 vertex binding + `VK_FORMAT_UNDEFINED`"。
这在 Vulkan 里不是"读回未定义值"，而是**非法的管线/动态状态**：实现可以忽略，也可以把取数地址算成野地址。
NVIDIA 的表现与前几次日志一致——首绘当即非法访问，丢失在**下一次** `vkQueueSubmit` 才报出来。

另一个同类问题：`bindVertexBuffers(0, N, host_buffers…)` 假设"属性在列表中的序号 == 它的 semantic"。
一旦列表里出现缺失/被丢弃的属性，缓冲就会被绑到错误 binding 上，用**这条流的 stride** 去读**另一条流**。

## 3. 本轮改动（`codex/idolmaster-release-gs-device-lost`）

| # | 文件 | 改动 | 目的 |
|---|---|---|---|
| 1 | `vk_graphics_pipeline.cpp/.h` | `GetVertexInputs()` 丢弃无法翻译成合法顶点输入的属性（空 V#、0 大小、`eUndefined` 格式），返回丢弃数；管线创建时打印一次告警 | 不再向驱动提交非法顶点输入状态；被丢弃的 location 只读回未定义值（等价于空流），不会产生野地址 |
| 2 | `buffer_cache.cpp` | 顶点缓冲**按各自 binding 号**逐个绑定；索引缓冲按客户机映射长度截断，null/未映射时绑定 16 字节零缓冲（上游 #5327 / #5358 的同类修复） | 绑定号不再依赖列表位置；索引取数永不越出客户机内存 |
| 3 | `vk_scheduler.cpp` | 设备丢失时**先**输出飞行记录器、转储最近编译的着色器，**最后**才查询驱动故障信息 | 故障查询崩掉进程时证据已经落盘 |
| 4 | `vk_gpu_trace.h` | 记录最后一次绘制的索引基址/位宽/索引数/vertex buffer 数/被丢弃属性数；环形缓存最近 8 个编译阶段的 guest 代码 + SPIR-V，丢失时写入 `shader/dumps/device_lost/` | 下一次日志能直接指向具体绘制与具体排列，且拿到可离线检查的 SPIR-V |
| 5 | `vk_pipeline_serialization.cpp` | `PipelineKeyVersion` 2 → 3 | 作废用非法顶点输入状态序列化的旧管线 |

## 4. 下一份日志怎么读（判定标准）

1. `Last draw: pipeline …, index buffer 0x… + …, vertex buffers = N, attributes dropped = M`
   * `attributes dropped > 0` ⇒ 命中的正是本轮修复的非法状态（也就是根因）；
   * `index buffer 0x0` ⇒ 命中空索引基址；
   * `pipeline` 应与 `First draw with new pipeline` 的 hash 一致。
2. `GPU trace:` 最后一段（`---- submit ----` 之后）里的 pipeline hash ⇒ 故障提交里到底画了什么。
3. `Graphics pipeline …: N vertex attribute(s) have an empty guest descriptor and are not bound`
   ⇒ 该管线就是攻击面，且 N 会写明有几条。
4. `shader/dumps/device_lost/*.spv`
   ⇒ 失败排列的 SPIR-V；用 `spirv-val` 校验、或直接比对 `vs_0xb3b7b928_*`。
5. 不再崩溃 ⇒ 根因即为第 2 节所述的非法顶点输入状态。

## 5. 验证结果：根因确认（`9fddbff14`，地狱特训 Pro 全程 + medley7）

同一台机器、同样冷编译（103 条新管线 / 36 次 vs、74 次 fs 编译）跑完地狱特训 Pro，日志
`C:\Users\Serina\AppData\Roaming\shadPS4\log\shad_log.txt` 关键三行：

```
3491  Compiling vs shader 0xb3b7b928 (permutation)          ← 三次崩溃前的同一个排列
3494  New graphics pipeline 0x59b3f0dd8289f1d6: vs = 0xb3c96b2cc65, fs = 0x1fd6f7b7218, gs = 0x0
3505  First draw with new pipeline 0x59b3f0dd8289f1d6: indexed = true, count = 36, …, vertex buffers = 3, attributes dropped = 1
3735  First draw with new pipeline 0xfa36d21f2da4ef4f: … attributes dropped = 1      ← 第一轮 medley 崩溃的那条管线
3705  First draw with new pipeline 0xa273d9b1233ebd4b: … count = 0, attributes dropped = 3
```

* `0x59b3f0dd8289f1d6` 就是三次崩溃里"新管线 → 首绘 → DEVICE_LOST"的那条，现在被创建并在后续帧里反复使用；
  整个会话 `Critical` 行为 0，日志以 `UpdatePlayTime` 正常结束。
* 它的首绘明确报出 **`attributes dropped = 1`**：该 fetch 布局里恰有一条属性的客户机 V# 是空的，
  即第 2 节描述的非法顶点输入状态。换言之**根因就是它**，不是绑定游标、不是 GS、不是显存、不是冷编译本身。
* 同一会话里第一轮 medley 崩溃的管线 `0xfa36d21f2da4ef4f` 也带着 `attributes dropped = 1` 正常绘制，
  两条崩溃路径在同一次运行里都被覆盖。
* 其余 96 条首绘全部 `attributes dropped = 0`，说明丢弃只发生在真正带空 V# 的那几种布局上，没有普适性副作用。

## 6. 仍需注意

* 被丢弃的输入 location 只读回未定义值；对空流（base 0 / size 0）而言客户机原本也只会拿到地址 0 的内容，
  实测画面正常。若后续在其他游戏看到某个属性明显错位，说明该 V# 是"看起来空、实际被 shader 使用"，
  届时改成绑定零缓冲 + 匹配格式而不是丢弃。
* `GetVertexInputs()` 在建管线路径上的告警（`N vertex attribute(s) have an empty guest descriptor`）
  只在**没有** `VK_EXT_vertex_input_dynamic_state` 时打印；NVIDIA 走动态顶点输入，所以丢弃数出现在
  `First draw with new pipeline …` 那一行里，这是正常的。
* 设备丢失的报告总是晚一次提交，这一点没有变；`vkGetDeviceFaultInfoEXT` 在该驱动上仍会让进程崩溃，
  但它已经是丢失处理的最后一步，证据不再受影响。
* 上游主线比本 fork 多 424 个提交，其中 `c09fa46a`（vertex/index 缓冲跟踪）、`e9f2c54c` + `23c41333`
  （null 索引缓冲）都是同类真修复，已被本轮按最小改动移植；其余不做整体合并。
