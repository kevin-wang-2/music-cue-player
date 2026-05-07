# Music Context — 设计规格

> 本文档整合了原始需求及设计问答，作为实现阶段的参考依据。

---

## 概念定义

**Music Context** = tempo map + time signature map 的统称，附加在 cue 上，使 cue 的 timeline 从线性时基（秒）切换到音乐时基（小节/拍）显示。

---

## 哪些 cue 可以附加 Music Context

所有有 duration 的 cue（Audio、Timeline Group、Sync Group）均可附加。

**Inspector 中的 Music Context Tab：**
- 始终显示，紧跟 Basic Tab 之后
- 若当前 cue **未附加** MC：Tab 内只显示一个 checkbox "附加 Music Context"
- 若当前 cue **已附加** MC：显示完整编辑器；checkbox 仍存在，取消勾选即移除 MC（及所有 points）

---

## MC 面板布局

MC Tab 内部分为三个区域（从上到下）：

### 顶部全局设置栏
- "第一个点之前：应用第一个点的值 / 不应用 MC"（单选，默认前者）
  - 控制弱起小节之前的区域的 tempo/拍号如何处理

### 中部时间轴编辑器（可拖拽调整高度，有最大高度限制）

**横轴**：默认 bar/beat（音乐时基），Ruler 格式为 `1|1`（小节|拍）
- 细节缩放后只显示 tick（不显示数字）
- 最细粒度：当前 edit quantization（none 时为 1/32 音符）
- 不完全小节的小节背景色不同（警告色），但合法可存在

**两条独立轨道：**
1. **Tempo 轨**（上）：BPM 曲线
   - 水平线：下一个点为 jump（跳变）
   - 斜线：下一个点为 ramp（渐变，BPM 在两点的 musical position 之间线性插值）
2. **Time Signature 轨**（下）：拍号块（每段显示对应拍号，如 `4/4`）

**Edit Quantization**（右键菜单，针对该面板）：
- 选项：`1/1`（bar）、`1/2`、`1/4`、`1/8`、`1/16`、`1/32`、`adaptive`、`none`
- MC 面板默认：`1/1`
- `adaptive`：取当前 ruler 自适应算法最接近的粒度再 ÷4（如 ruler 自适应到 1/1，则 adaptive = 1/4）
- Shift + 拖动：无视 quantization，自由拖动

### 底部固定属性区（选中点后显示）
- 类型：Jump / Ramp
- BPM 值（整数或小数）
- 拍号：分子 / 分母
- 时间位置：bar|beat（音乐时基）

---

## Points（控制点）

### 类型
一个点可以**同时**携带速度变化和拍号变化。

- **Jump**（跳变）：到达该点时 BPM/拍号瞬间切换
- **Ramp**（渐变）：从前一个点的 BPM 线性渐变到该点的 BPM（在 musical position 之间做线性插值）；拍号不渐变（拍号变化始终为 jump）

### 第一个点（特殊规则）
- 附加 MC 时**自动创建**，默认值：4/4、120 BPM、Jump
- **不可删除**，只能拖动
- 第一个点的位置定义 bar 1 beat 1 在 cue 内的位置（弱起小节支持）
- **不可在第一个点之前添加任何点**

### 添加 / 删除点
- 点击两点之间的连线可添加新点；默认值继承该线段在该位置的当前值（线性插值）
- 右键 → Delete 删除（第一个点除外）

### 位置约束
- 点均以**音乐时基**（bar/beat）定位
- 拖动一个点改变其 bar/beat 位置和对应的实际 sample 时间；该点**之后**所有点的 bar/beat 位置不变，但实际 sample 时间联动变化
- 不可交错拖动（不能越过相邻点，顺序固定）

### 不完全小节
- 当拍号变化点不落在小节线上时，该小节为不完全小节
- 合法，但该小节在 ruler 上用警告色背景标注

---

## Music Context 的时间轴影响

附加 MC 后，以下 cue 的 timeline ruler 从秒切换到 bar/beat：
- **Audio cue**：WaveformView 的 ruler
- **Timeline Group**：TimelineGroupView 的 ruler
- **Sync Group**：SyncGroupView 的 ruler

Marker、子 cue 开始时间、duration 的显示值均换算为 bar/beat（**仍使用线性时基计算实际位置，不是音乐时基**）。

**MC 不感知 loop**：即使音频有循环，MC 时间轴视其为不存在，连续计算 bar/beat。

---

## 子 cue 的 MC 继承

- 子 cue **未设置独立 MC**：继承父 cue 的 MC，起点从子 cue 的 `timelineOffset` 对应的父 MC bar/beat 位置开始（ruler 继续显示父 MC 的 bar/beat 值，而非从 1|1 重新开始）
- 子 cue **设置了独立 MC**：完全覆盖父 MC，从子 cue 自己的 bar 1 beat 1 开始

---

## Global Music Context

- **自动设定**：当一个带有 MC 的 cue 实际触发（prewait + quantization 都完成后）时，其 MC 成为 global MC
- **只有最外层**：若 group cue 和其子 cue 都有 MC，只有 group 的 MC 成为 global
- **替代规则**：新 cue 触发时直接替代 global MC，不停止之前 cue 的播放
- **无 global MC 时**：带有 cue quantization 的 cue 立即执行（不等待）

---

## Cue Quantization（per-cue，在 Basic Tab）

控制 go() 何时实际触发该 cue：

| 选项 | 行为 |
|------|------|
| `none` | 立即执行 |
| `1/1`~`1/32` | 等 global MC 运行到下一个对应边界时执行 |

**执行顺序**：prewait → quantization 等待 → 实际触发（MC 在实际触发时成为 global）

**弱起小节 + quantization 的特殊处理**：保证第一个完整小节的起点落在 quantization 边界上，而不是 cue 的时间起点对齐到边界。

---

## Edit Quantization（针对编辑操作，非播放）

| 场景 | 默认值 |
|------|--------|
| MC 面板 | `1/1` |
| Timeline / SyncGroup 面板（已有 MC）| `none` |

所有拖动操作（块拖动、trim 拖动、marker 拖动）及添加点/marker 均自动 quantize。
Shift + 拖动 = 自由拖动，不 quantize。

---

## Ruler 显示规格

| 层级 | 格式 |
|------|------|
| 大刻度 | `1\|1`（小节\|拍） |
| 细化后 | 只显示 tick，不显示数字 |
| 最细粒度 | 当前 edit quantization；none 时为 1/32 音符 |
| 不完全小节 | ruler 该小节区域警告色背景 |

---

## 实现分层建议

```
engine/
  MusicContext          数据结构：points 列表（bar/beat 时基）
                        转换函数：bar/beat ↔ sample position
                        （不涉及播放，纯计算层）

engine / ShowFile       MC 序列化：附加在 CueData 内

qt_ui/
  MusicContextView      MC Tab 内的时间轴编辑器（两轨 + ruler）
  InspectorWidget       新增 MC Tab 逻辑，cue quantization spinbox in Basic
  TimelineGroupView     当 cue 有 MC 时切换 ruler 模式
  SyncGroupView         同上
  WaveformView          同上
  CueList / go()        cue quantization 等待逻辑
```
