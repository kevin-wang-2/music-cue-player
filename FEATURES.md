# Music Cue Player — Feature Reference

> 开发参考文档。记录已实现的功能、数据结构和行为约定，供后续开发查阅。

---

## 目录

1. [架构概览](#架构概览)
2. [引擎层 (engine)](#引擎层-engine)
3. [Cue 类型](#cue-类型)
4. [Group Cue 系统](#group-cue-系统)
5. [Show File 格式](#show-file-格式)
6. [Qt UI 层](#qt-ui-层)
7. [Inspector 面板](#inspector-面板)
8. [Timeline / SyncGroup 编辑器](#timeline--syncgroup-编辑器)
9. [行为约定与边界条件](#行为约定与边界条件)

---

## 架构概览

```
engine/                     纯 C++ 引擎，无 UI 依赖
  AudioEngine               PortAudio 输出，64-voice slot 混音池
  StreamReader              单文件背景流式读取，SPSC 无锁 ring buffer
  CueList                   高层 cue 序列器，管理所有 cue 类型和播放逻辑
  Scheduler                 定时事件调度（prewait、slice end 回调等）
  ShowFile                  JSON 序列化 / 反序列化

qt_ui/
  MainWindow                顶层窗口，菜单、工具栏、全局快捷键
  CueTableView              cue 列表表格，支持拖拽排序、分组折叠
  InspectorWidget           右侧属性面板，多 Tab 编辑所选 cue
  WaveformView              音频波形 + 标记点编辑（音频 cue）
  TimelineGroupView         Timeline Group 子 cue 时间轴编辑器
  SyncGroupView             Sync Group 子 cue 编辑器 + slice/loop 可视化
  FaderWidget               音量推子控件
  AppModel                  Qt 对象，持有 ShowFile + CueList，连接 UI 与引擎
```

---

## 引擎层 (engine)

### AudioEngine

- PortAudio 输出，默认 48 kHz / 自动检测声道数
- 最多 **64 个同时 voice**（`kMaxVoices = 64`）
- 每个 voice 有 tag（= cue flat index），支持按 tag 批量停止
- 维护 `enginePlayheadFrames` 全局时钟，所有调度以此为基准
- `scheduleStreamingVoice()` 接受 `StreamReader*`，非阻塞启动
- `softPanic()` 线性淡出所有 voice，再停止

### StreamReader

- 打开音频文件（libsndfile 优先，失败后 FFmpeg），后台 I/O 线程填充 ring buffer
- Ring buffer：`kRingFrames = 65536`（约 1.36 s @ 48 kHz）
- Arm 阈值：`kArmFrames = 8192`（约 170 ms）
- 支持**多段 (LoopSegment)**：每段有 startSecs / endSecs / loops（0 = 无限循环）
- **Devamp**：`devamp(stopAfter, preVamp)` 在当前 loop iteration 结束后切换段或停止
  - `stopAfter=false`：advance to next segment
  - `stopAfter=true`：stop after current iteration
  - `preVamp=true`：跳过随后的 looping segments（loops != 1）
- `wasDevampFired()` / `clearDevampFired()`：主线程轮询 devamp 是否已触发
- **SegMarker**：I/O 线程在每个 loop iteration 开始时写入 ring write position，主线程用此计算精确播放进度

### Scheduler

- 基于 `enginePlayheadFrames` 调度回调
- `scheduleFromFrame(originFrame, delaySecs, callback)`：在 origin + delay 处触发
- `cancelAll()` / `isPending(eventId)` 用于 prewait 管理

---

## Cue 类型

### Audio

| 字段 | 说明 |
|------|------|
| `path` | 音频文件路径 |
| `startTime` | 文件内起始位置（秒） |
| `duration` | 播放时长（0 = 到文件末尾） |
| `level` | 增益（dB，0 = 单位增益） |
| `trim` | Trim 增益（dB） |
| `routing` | 声道路由（见下文） |
| `markers` | 时间标记点（绝对文件时间，升序排列） |
| `sliceLoops` | 每个 slice 的循环次数（0 = 无限） |
| `armedStream` | 预缓冲流（Arm 后填充，go() 时取用） |

**声道路由**（`Routing` struct）：
- `outLevelDb[o]`：每个输出声道的电平（dB）
- `xpoint[srcCh][outCh]`：交叉点矩阵，`nullopt` = 断开，`float` = dB 增益
- 默认：对角线 0 dB，其余断开

**Markers / Slices**：
- N 个 marker 将文件分成 N+1 个 slice
- `sliceLoops[i]` 对应第 i 个 slice 的循环次数
- StreamReader 将 markers 转换为 LoopSegment 数组

### Start / Stop

- `targetIndex`：目标 cue 的 flat index
- Start：触发目标 cue 的音频（不改变 selectedIndex）
- Stop：立即停止目标 cue 的所有 voice

### Fade

- `fadeData`：目标 cue index、曲线类型（Linear / EqualPower）、stopWhenDone
- 支持：master level、per-output-channel level、crosspoint cell
- 通过 `StreamReader::setOutLevelGain()` / `setXpointGain()` 实时更新，无锁

### Arm

- `targetIndex`：目标音频 cue
- `armStartTime`：预加载起始位置（0 = 从 startTime 开始）
- 触发后目标 cue 的 `armedStream` 被填充，下次 go() 零延迟启动
- `isArmed(index)` 返回 true 的条件：
  - 音频 cue：`armedStream` 非空且 `isArmed()` 返回 true
  - Timeline/Sync Group cue：`timelineArmSec > 0`
  - Timeline/Sync Group 的子 cue：父 group 已 arm 且该子 cue 在 arm 位置内

### Devamp

- `targetIndex`：目标 cue（音频 cue 或 Sync Group cue）
- `devampMode`：
  - `0`：Next Slice — 当前 loop iteration 结束后进入下一个 slice
  - `1`：Go + Stop Current — 停止目标 cue，触发全局 go()
  - `2`：Go + Keep Current — 进入下一 slice，同时触发全局 go()
- `devampPreVamp`：见下文 [Devamp PreVamp 语义](#devamp-prevamp-语义)
- 对 Sync Group 的 devamp 由 `devampSyncGroup()` 单独处理（不使用 StreamReader devamp）

---

## Group Cue 系统

### 数据结构

Group cue 在 flat list 中连续存放：

```
[groupIdx]          GroupData，childCount = 所有后代数量
[groupIdx+1]        第一个子 cue（或嵌套 group）
...
[groupIdx+childCount]  最后一个后代
```

- `parentIndex`：直接父 group 的 flat index（-1 = 顶层）
- `childCount`：所有后代总数（不只是直接子节点）
- `timelineOffset`：在父 Timeline group 中的时间偏移（秒）

### 执行模式

#### Timeline 模式

- 子 cue 按 `timelineOffset` 调度，所有子 cue 同时（差值）启动
- `timelineArmSec > 0`：从 timeline 中指定位置开始，过去的子 cue 从中间开始（seek），未来的子 cue 延迟启动
- Inspector：**Timeline Tab** 显示 `TimelineGroupView` 编辑器

#### Playlist 模式

- 子 cue 顺序播放（前一个结束后触发下一个）
- `groupRandom = true`：随机排列子 cue 顺序

#### StartFirst & Enter 模式

- go() 进入 group 后选中第一个子 cue，后续 go() 顺序执行子 cue
- 子 cue 最后一个之后，selectedIndex 跳出 group

#### Sync 模式

- 子 cue 按 `timelineOffset` 排列（与 Timeline 相同的 timeline 结构）
- Group 自身的 `markers` 定义 **slice 边界**，`sliceLoops` 定义每个 slice 的循环次数
- 每次 go() 触发当前 arm 位置对应的 slice，完成后自动进入下一 slice（或按 sliceLoops 循环）
- Inspector：**Time & Loop Tab** 显示 `SyncGroupView` 编辑器

### Devamp PreVamp 语义

给定 `A(single) → B(loop) → C`：

| 当前 slice | PreVamp | 行为 |
|---|---|---|
| A（非循环）| false | A 结束 → B |
| A（非循环）| true | A 结束 → 检查 B 是否 loop；若是 → 跳过 B → C |
| B（循环）| true | 与普通 devamp 相同（等当前 loop iteration 结束 → C） |
| B（循环）| false | 当前 loop iteration 结束 → C |

- **所有 devamp 均等待当前 slice iteration 完成**（`syncLoopsLeft` 被截断为 1）
- PreVamp skip 判断依据是 `sliceLoops[nextSlice] != 1`（配置值，非运行时剩余次数）
- PreVamp 仅在触发 devamp 时当前 slice 为非循环状态才生效（`!isLooping` 时存储标志）

### isCuePlaying 对 Group 的处理

`isCuePlaying(groupIdx)` 会遍历 `[groupIdx+1, groupIdx+childCount]` 检查所有后代的 voice，因为 Group cue 自身没有 voice。这样：
- cue 列表中 Group cue 在子 cue 播放期间显示 "playing" 状态
- `go()` 的重复触发保护（`isCuePlaying || isCuePending`）对 Group 有效

---

## Show File 格式

JSON 文件，顶层结构：

```json
{
  "mcp_version": "1.0",
  "show": { "title": "..." },
  "engine": { "sampleRate": 48000, "channels": 2, "deviceName": "" },
  "cueLists": [{ "id": "main", "name": "Main", "cues": [...] }]
}
```

Cue 序列化要点：
- Group cue 的子 cue 嵌套在 `"children"` 数组中（保存时），加载后展平为 flat list
- 未知字段静默忽略（向前兼容）；缺失字段取默认值（向后兼容）
- Group `timelineOffset` 存在子 cue 的 `"timelineOffset"` 字段

---

## Qt UI 层

### MainWindow

- 快捷键：`Space` / `Go` 按钮触发 `cues.go()`；`Esc` 清除 timeline arm
- Undo/Redo：`AppModel::pushUndo()` 保存 cueLists 快照，最多 50 步
- 文件：新建 / 打开 / 保存 / 另存为（`.mcp` JSON）
- 音频设备：DeviceDialog 选择输出设备，重新初始化 AudioEngine
- 周期性 `QTimer`（~30 fps）调用 `cues.update()` + `table->refreshStatus()`

### CueTableView

列：`#`（cue number）、Type、Name、Target、Duration、Status

**Status 颜色**：
- `playing`：绿色
- `armed`：黄色
- `pending`：橙色
- `fading`：蓝色
- `idle`：灰色

**Group 显示**：
- 缩进前缀按深度（每级 4 空格）
- Group cue 有 `> ` / `∨ ` 折叠 toggle
- 折叠时隐藏所有后代行

**拖拽**：行拖拽重排序；拖放到 Start/Stop/Fade/Arm/Devamp 的 Target 列设置目标

**操作**：
- 右键菜单：Add Audio / Start / Stop / Fade / Arm / Devamp / Group，Delete，Wrap in Group
- 选中多行后 → Group 按钮：`createGroupFromSelection()`，默认创建 Sync Group

### AppModel

- 持有 `ShowFile sf`、`CueList cues`、undo/redo 栈
- `dirty`：文件是否有未保存修改
- 信号：`dirtyChanged`、`engineRestarted`

### ShowHelpers

- `rebuildCueList(model)`：从 `sf.cueLists[0].cues`（嵌套）展平，重建 `cues`（flat list）
- `syncSfFromCues(model)`：从 flat `cues` 重建嵌套 `sf` 结构，用于每次模型修改后同步

---

## Inspector 面板

Tab 顺序（按 cue 类型显示/隐藏）：

| Tab | 可见条件 |
|-----|---------|
| Basic | 始终 |
| Levels | 音频 / Fade |
| Trim | 音频 |
| Curve | Fade |
| Mode | Group |
| Time & Loop | 音频 / Sync Group |
| Timeline | Timeline Group |

### Basic Tab

- Cue Number、Name、PreWait、AutoContinue、AutoFollow

### Levels Tab

- Master 增益推子（dB）
- Per-output-channel 推子
- 交叉点矩阵（每个源声道 → 每个输出声道的增益）

### Trim Tab

- Trim 增益推子

### Curve Tab（Fade）

- 目标 cue 选择
- 曲线类型（Linear / EqualPower）
- Stop When Done

### Mode Tab（Group）

- 执行模式：Timeline / Playlist / StartFirst / Sync
- Random 选项（Playlist 专用）

### Time & Loop Tab

**音频 cue**：
- Start / Duration spinbox
- WaveformView：波形显示 + 标记点（右键添加/删除，拖拽移动）
- 每个 slice 的循环次数 inline 编辑（双击 loop strip）
- 标记点属性面板：时间、名称

**Sync Group cue**：
- SyncGroupView（见下文）

---

## Timeline / SyncGroup 编辑器

### 公共特性（两个 view 均有）

- **横轴**：时间（秒），可缩放（鼠标滚轮）
- **子 cue 方块**：
  - 显示波形（如有音频文件）
  - **Sticky label**：cue 名称固定显示在可视区域左侧，方块很窄或起点在屏幕外时不消失
  - Sub-label 显示 prewait（timelineOffset）时间
  - 选中时高亮（蓝色边框）
- **拖拽交互**：
  - 方块中间拖拽：移动（调整 `timelineOffset`）
  - 左边框拖拽：左 trim（同时调整 `timelineOffset` + `startTime`，保持右端固定）
  - 右边框拖拽：右 trim（调整 `duration`）
  - 边框拖拽区域：`kHandleW = 6px`
- **点击即选中**（不需要拖拽）
- **缩放/位置记忆**：切换选中 cue 后重新打开同一 group 不会重置视图

### TimelineGroupView（Timeline Group 专用）

- Ruler 点击：设置 arm 位置（绿色三角光标）
- `rulerClicked(double timeSec)` 信号 → InspectorWidget → `setCueTimelineArmSec()`

### SyncGroupView（Sync Group 专用）

- **Markers**（group 级别的 slice 分隔线）：
  - 右键菜单：Add Marker / Delete Marker
  - 左键拖拽：移动 marker
  - 点击 marker：同时选中该 marker 并将 arm cursor 移至该 marker 时间点
  - Marker 面板（InspectorWidget）：时间、名称编辑
- **Loop Strip**（底部 18px 区域）：
  - 每个 slice 显示循环次数（`∞` = 0，数字 = N 次）
  - 双击：inline `QLineEdit` 编辑，非数字输入自动转为 `∞`
- **Arm cursor**：绿色三角 + 竖线贯穿 lane 区域
- **`clearArmCursor()`**：重置绿标（Esc 时由 MainWindow 调用）

---

## 行为约定与边界条件

### Arm 状态传播

```
ruler/marker 点击
  → setCueTimelineArmSec(groupIdx, timeSec)
  → emit cueEdited()
  → MainWindow::onCueListModified()
  → CueTableView::refresh()
  → setRowStatus() → isArmed() → 显示 "armed"（黄色）
```

`isArmed()` 检查链：
1. 音频 cue：`armedStream` 有效且 `isArmed()`
2. Timeline/Sync Group：`timelineArmSec > 0`
3. Timeline/Sync Group 的子 cue：父 group `timelineArmSec > 0` 且子 cue 未被跳过

### Esc 清除 Arm

`MainWindow::Esc` → `clearTimelineArm()` → 同时清除 `TimelineGroupView` 和 `SyncGroupView` 的绿标。

### 默认新建 Group

`createGroupFromSelection()` 默认创建 **Sync Group**（`groupMode = "sync"`）。

### Stop cue 对 Sync Group 子 cue 的保护

Stop cue 不允许停止 Sync Group 的直接子 cue（由 Sync Group 自行管理 voice 生命周期）。

### AutoFollow vs Devamp

- `autoFollow`：cue 播放完毕后自动触发全局 `go()`
- Devamp Mode 1/2：在 devamp 时机触发全局 `go()`（可与 autoFollow 叠加）
- Sync Group 最后一个 slice 结束时：`autoFollow` 触发 `go()`，或 devamp mode 1 直接 `go()`
