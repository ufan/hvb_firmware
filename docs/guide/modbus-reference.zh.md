# Modbus 寄存器参考 — Jianwei 高压控制板

本文档是 Jianwei 高压控制板的权威线协议参考：传输层、寄存器地图、编码方式与
枚举定义。面向**自行实现主机端工具的第三方开发者**（不复用
`tools/hvb_modbus_core`）——不假设你熟悉本仓库的 C++ 代码。如果你正在本仓库内
开发，下面的寄存器偏移量是从 `include/reg_store/reg_map.h` 和
`include/reg_store/modbus_view.def` 生成的，它们才是唯一的真实来源；本文档不
应与它们产生偏差。

*(English version: [modbus-reference.md](modbus-reference.md))*

建立连接后可能还需要参考：[`calibration-guide.zh.md`](calibration-guide.zh.md)
（出厂校准流程）、[`operating-mode-guide.md`](operating-mode-guide.md)
（Normal 与 Automatic 模式、保护/恢复语义）、
[`parameter-reference.md`](parameter-reference.md)（逐字段默认值与推导过程）。
本文档不重复描述产品*行为*——只描述与它通信所需的线协议格式。

---

## 1. 协议概览

| | |
|---|---|
| 协议版本 | **3.0**（`Protocol Major`=3，`Protocol Minor`=0——在解读任何其他内容之前先读取并校验它） |
| 传输方式 | Modbus RTU |
| 物理层 | RS-485 半双工（当前各变体均如此） |
| 串口格式 | 8N1 |
| 波特率 | 默认 115200，备选 9600（运行时可切换，见第 5 节） |
| 从站地址 | 默认 1，运行时可设置为 1–247；0 为广播地址 |
| 功能码 | 0x03（读保持寄存器）、0x04（读输入寄存器）、0x06（写单个保持寄存器）——仅此三种；不支持 FC10，也没有线圈/离散输入 |

**主版本号不匹配意味着寄存器不兼容——此时不应继续通信。**协议 3 相对协议 2
是破坏性变更（偏移量不同，多处寄存器被移除或新增）；按本文档实现的客户端，
遇到板卡上报的 `Protocol Major` ≠ 3 时必须拒绝继续通信。次版本号的递增只会是
新增性质的（在此前的保留区间中新增寄存器）；遇到不认识的字段可以安全地忽略。

---

## 2. 传输层帧格式

采用标准 Modbus RTU 帧格式——没有任何板卡专属的特殊之处，这里详细说明是为了
方便此前没有实现过 RTU 客户端的开发者：

- 帧结构：`[从站地址: 1字节] [功能码: 1字节] [数据: N字节] [CRC16: 2字节]`
- CRC16 使用标准 Modbus CRC（多项式 `0xA001`，初始值 `0xFFFF`，帧中低字节
  在前——即 CRC 低字节先发送，高字节后发送）。任何标准 Modbus CRC16 实现都
  适用，与板卡本身无关。
- 寄存器数值在线上采用大端序（高字节在前），是标准 Modbus 约定。
- 帧边界由线路上 ≥3.5 个字符时间的静默间隔判定——这是常规的 RTU 帧间隔。
  如果你使用现成的 RTU 主站库（如 `libmodbus`、`pymodbus`、ModbusLib），这些
  细节已经被库处理好了；你只需要参考下面的寄存器地图。
- **RS-485 半双工**：板卡驱动使能的时序由固件侧处理——除非你的 USB-RS485
  适配器有特殊需求，否则不需要在正常的 RTU 收发间隔之外额外添加延时。
- **广播**（从站地址 0）：只写不读，不会返回响应。发往地址 0 的读请求没有
  意义，不应发送。

以下所有内容描述的都是 **PDU**（数据域）内容——寄存器地址、编码方式与
语义——这部分与具体传输层无关。

---

## 3. 数据约定

- **所有寄存器都是单个 16 位字**（`UINT16` 或 `INT16`，有符号数采用补码）。
  线协议层面不存在超过 16 位的寄存器。
- **32 位数值**（运行时间、固件版本、故障时间戳、原始 ADC 码）拆分到两个
  连续寄存器中，先 `_HI` 后 `_LO`，地址分别为 `N` 和 `N+1`：
  `value = (HI << 16) | LO`。原始 ADC 值是有符号 32 位（合并后需做符号扩展）。
- **电压**：寄存器原始值 × **0.1 V/LSB**（即 ×100 mV）。原始值 5000 = 500.0 V。
  这一比例统一适用于所有电压类寄存器（设定目标值、运行目标值、测量电压、
  斜坡步进）。
- **电流**：寄存器原始值 × **0.1 nA/LSB**。原始值 10000 = 1000.0 nA。适用于
  电流阈值与测量电流寄存器。
- **时间间隔**：原始值 × 0.1 = 秒（即原始值单位为 0.1 秒——下文字段名中的
  "×10" 表示原始值是秒数的十倍）。Delay/window 类字段（自动重试延时、窗口）
  本身就是整数秒，没有 ×10 的换算——请以每个字段的具体说明为准，"interval"
  与 "delay/window" 两类字段的约定并不相同。
- **百分比**：普通 `UINT16`，如 10 = 10%，不做缩放。
- **校准系数**：见第 9 节专门的公式——输出轴与两条测量轴使用不同的除数，
  不是其他地方统一使用的简单 ×0.1 模式。
- **保留寄存器**：始终读回 `0`，写入始终被拒绝并返回异常 `0x02`。不要对
  某个保留偏移量的含义做任何假设，即使它在某个固件版本上读回了非零值——
  那属于固件缺陷，不是可以依赖的既定行为。

---

## 4. 地址空间

```
0      系统块（System）        输入 + 保持，40 个寄存器
40     通道 0 块               输入 + 保持，40 个寄存器
80     通道 1 块               输入 + 保持，40 个寄存器
120    通道 2 块               输入 + 保持，40 个寄存器  （2 通道变体上保留未用）
...
640    通道 15 块              输入 + 保持，40 个寄存器  （协议上限；未实现的通道保留未用）
680    扩展块（Extension）     仅保持寄存器，80 个寄存器
```

```
CH_BASE(ch) = 40 + ch × 40
EXT_BASE    = 40 + 16 × 40 = 680
```

协议始终为 16 个通道预留地址空间，无论某个具体板卡变体实际实现了多少个。
**在假设 0 号以外的通道存在之前，先读取 `Supported Channels` 和
`Active Channel Mask`（第 6 节）**——访问一个不支持的通道块中的任意寄存器，
会对该块内每一个寄存器都返回异常 `0x02`，而不是返回置零的占位数据。

---

## 5. 快速上手——推荐的建连流程

一个最小化的客户端实现应当按以下顺序进行：

1. 以 115200 8N1、从站地址 1 连接（这是通用默认值——只有在确认板卡已被
   重新配置过的情况下才需要覆盖）。
2. **FC04 读取 System Input 偏移 0–1**（`Protocol Major`/`Minor`）。若
   `Major` ≠ 3 则拒绝继续。
3. **FC04 读取 System Input 偏移 2–5**（`Variant ID`、`System Capability
   Flags`、`Supported Channels`、`Active Channel Mask`）。用它们来判断该
   板卡上哪些通道块、哪些系统级特性（Automatic 模式、环境传感器、
   Calibration 模式）是有意义的——不要把"2 个通道"写死，即使目前唯一出货
   的变体确实只有 2 个通道。
4. **对每个活跃通道，FC04 读取 Channel Input 偏移 9**（`Capability Flags`），
   然后再访问该通道上任何受能力位保护的寄存器（第 10 节）。若某寄存器所需
   的能力位缺失，读写都会返回异常 `0x02`——这不是一个"读回零值"的软性回退。
5. 之后即可按需轮询或写入。用每个寄存器标注的**轮询类别**（见第 6/9/10
   节）决定轮询节奏：`REALTIME` 字段持续变化，可以较快轮询（约 ≥100 ms）；
   `CONFIG` 字段只有在你自己或其他客户端写入时才会变化——可以慢速轮询或按需
   读取；`FIXED` 字段启动后不会再变化——只需读取一次；`COMMAND` 寄存器是
   只写触发器——永远不要轮询它们。

### 自清零命令是同步执行的

命令类寄存器（Output Action、Fault Command、Param Action、校准会话相关命令）
在 **FC06 写响应发出之前**就已经执行完毕——板卡的命令分发逻辑会阻塞该次写
操作，直到底层动作完成。写入成功后，命令寄存器会立即读回 `0`；你不需要
轮询等待它清零。如果对命令寄存器的写入返回了 Modbus 异常，说明该命令被
直接拒绝，没有产生任何效果。

---

## 6. System Input 块（FC04，偏移 0–39）

| 偏移 | 寄存器 | 类型 | 轮询类别 | 说明 |
|---:|---|---|---|---|
| 0 | Protocol Major | UINT16 | FIXED | 务必最先检查这个——见第 1 节 |
| 1 | Protocol Minor | UINT16 | FIXED | 同一主版本内只做新增，不做修改 |
| 2 | Variant ID | UINT16 | FIXED | 板卡/产品变体标识（第 14 节） |
| 3 | Capability Flags | UINT16 | FIXED | `0x0001`=Automatic 模式，`0x0002`=环境传感器，`0x0004`=Calibration 模式——见第 12 节 |
| 4 | Supported Channels | UINT16 | FIXED | 该变体寻址的通道数量 |
| 5 | Active Channel Mask | UINT16 | REALTIME | 第 N 位置位 ⇒ 通道 N 当前可寻址 |
| 6 | Board Temperature | INT16 | REALTIME | ×0.1 °C；无环境传感器时为 0 |
| 7 | Board Humidity | UINT16 | REALTIME | ×0.1 %RH；无环境传感器时为 0 |
| 8–9 | Uptime HI/LO | UINT32 | REALTIME | 开机后经过的秒数 |
| 10–11 | FW Version HI/LO | UINT32 | FIXED | 打包成 32 位；当前固件将 HI 设为主版本号、LO 设为次版本号，是两个独立的 16 位数值，不是语义化版本字符串 |
| 12 | Active Operating Mode | UINT16 | REALTIME | 见第 11.1 节 |
| 13 | System Status | UINT16 | REALTIME | 全局状态位掩码（产品预留，目前未定义任何位） |
| 14 | Fault Cause | UINT16 | REALTIME | 全局故障汇总，位布局与通道故障原因相同（第 13 节） |
| 15–39 | Reserved | — | — | 读回 0，拒绝写入 |

---

## 7. System Holding 块（FC03/FC06，偏移 0–39）

| 偏移 | 寄存器 | 访问 | 轮询类别 | 说明 |
|---:|---|---|---|---|
| 0 | Operating Mode | RW | CONFIG | 0=Normal，1=Automatic，2=Calibration——见第 11.1 节。进入 Calibration 前需先完成解锁序列（第 8 节） |
| 1 | Startup Channel Policy | RW | CONFIG | 0=启动时从 NVS 加载通道配置，1=启动时应用出厂默认运行参数 |
| 2 | Slave Address | RW | CONFIG | 1–247。写入本寄存器**不会**立即生效——见下方流程 |
| 3 | Baud Rate Code | RW | CONFIG | 0=115200，1=9600。生效方式与 Slave Address 相同，需复位后生效 |
| 4–38 | Reserved | — | — | 读回 0，拒绝写入 |
| 39 | Param Action | RW | COMMAND | 1=Save，2=Load，3=Factory Reset，255=Software Reset——见第 11.4 节 |

### 修改从站地址或波特率

```
1. FC06 将新值写入偏移 2（Slave Address）和/或偏移 3（Baud Rate Code）
2. FC06 向偏移 39（Param Action）写入 1（Save）
3. 修改会立即持久化，但当前运行中的 Modbus 接口仍然沿用旧的地址/波特率，
   直到下一次复位或重新上电。
4. 复位（Param Action = 255，或断电重启），然后用新的地址/波特率重新连接。
```

只写入偏移 2/3 而不随后执行 Save，修改会在复位后丢失——这两个寄存器本身只是
暂存新值；真正把它持久化到 NVS 的是 `Param Action = Save`。

---

## 8. Extension Holding 块（FC03/FC06，偏移 680–759）

| 偏移 | 绝对地址 | 寄存器 | 访问 | 说明 |
|---:|---:|---|---|---|
| 0 | 680 | Calibration Unlock | RW | 两步解锁——见下方。始终读回 0 |
| 1 | 681 | Calibration Exit | RW | 写入 1 以退出 Calibration 模式，恢复进入前的工作模式 |
| 2–79 | 682–759 | Reserved | R | 读回 0，拒绝写入 |

**Calibration Unlock 解锁序列**（一个易失性的防误触保护，不是加密身份验证——
它的作用只是防止一次误操作的写入把板卡带入 Calibration 模式）：

```
1. FC06 向偏移 680 写入 0xCA1B
2. FC06 向偏移 680 写入 0xA11B
3. FC06 向 System Holding 偏移 0（Operating Mode）写入 2（Calibration）
```

第 1 步或第 2 步写入了错误的值，都会把解锁进度清零——必须从第 1 步重新开始。
成功进入 Calibration 模式（第 3 步）同样会清零解锁进度，因此退出后再次进入
需要重新走一遍解锁流程。Calibration 模式本身是易失的：无论进入校准前板卡
处于哪个模式、校准过程中是否掉电重启，下次开机都不会恢复到 Calibration 模式。

---

## 9. Channel Input 块（FC04，基址 `CH_BASE(ch)`，偏移 0–39）

| 偏移 | 寄存器 | 类型 | 轮询类别 | 能力位保护 | 说明 |
|---:|---|---|---|---|---|
| 0 | Status Bits | UINT16 | REALTIME | — | 见第 13.1 节 |
| 1 | Active Fault Cause | UINT16 | REALTIME | — | 当前正在阻断运行的故障位；见第 13.2 节 |
| 2 | Fault History Cause | UINT16 | REALTIME | — | 自上次清除历史以来观察到的故障位 |
| 3 | Last Protection Output Action | UINT16 | REALTIME | — | 见第 11.2 节 |
| 4 | Auto Retry Count | UINT16 | REALTIME | — | 当前滑动重试窗口内已计数的重试次数 |
| 5 | Auto Cooldown Remaining | UINT16 | REALTIME | — | 距下次允许重试还剩的秒数，未处于冷却期时为 0 |
| 6–7 | Last Fault Timestamp HI/LO | INT32 | REALTIME | — | 记录上一次故障时的板卡运行时间（秒） |
| 8 | Operational Target Voltage | INT16 | REALTIME | — | 实时目标值，向 Configured Target Voltage 斜坡逼近；×0.1 V |
| 9 | Capability Flags | UINT16 | FIXED | — | 访问该通道上任何受保护寄存器之前先读取这个——见第 12.2 节 |
| 10 | Measured Voltage | INT16 | REALTIME | VOLTAGE_MEASUREMENT | 已校准，×0.1 V |
| 11 | Measured Current | INT16 | REALTIME | CURRENT_MEASUREMENT | 已校准，×0.1 nA |
| 12–13 | Raw ADC Voltage HI/LO | INT32 | REALTIME | VOLTAGE_MEASUREMENT | 未校准的 ADC 原始码——仅在 Calibration 模式下有意义 |
| 14–15 | Raw ADC Current HI/LO | INT32 | REALTIME | CURRENT_MEASUREMENT | 未校准的 ADC 原始码——仅在 Calibration 模式下有意义 |
| 16–39 | Reserved | — | — | — | 读回 0，拒绝写入 |

## 10. Channel Holding 块（FC03/FC06，基址 `CH_BASE(ch)`，偏移 0–39）

### 命令（偏移 0–2）

| 偏移 | 寄存器 | 轮询类别 | 说明 |
|---:|---|---|---|
| 0 | Output Action | COMMAND | 合法取值及主机/保护上下文规则见第 11.2 节 |
| 1 | Fault Command | COMMAND | 1=Clear Active Fault Block，2=Clear Fault History——见第 11.3 节 |
| 2 | Param Action | COMMAND | 1=Save，2=Load，3=Factory Reset，仅作用于该通道。**255（Software Reset）在此处非法**——那是仅系统块可用的值；在通道层写入会返回异常 `0x03` |

### 运行配置（偏移 3–16）

| 偏移 | 寄存器 | 能力位保护 | 说明 |
|---:|---|---|---|
| 3 | Configured Target Voltage | RAW_OUTPUT_DRIVE | ×0.1 V。使能后通道向其斜坡逼近的目标值 |
| 4 | Ramp Up Step | RAW_OUTPUT_DRIVE | 每个间隔的电压变化量，×0.1 V |
| 5 | Ramp Up Interval | RAW_OUTPUT_DRIVE | 每步间隔时间，×0.1 秒（0.1 秒单位） |
| 6 | Ramp Down Step | RAW_OUTPUT_DRIVE | 每个间隔的电压变化量，×0.1 V |
| 7 | Ramp Down Interval | RAW_OUTPUT_DRIVE | 每步间隔时间，×0.1 秒 |
| 8 | Recovery Policy Mode | — | 0=ManualLatch，1=AutoRetry，2=AutoDerateRetry，3=NeverRetry——见第 11.5 节 |
| 9 | Auto Retry Delay | — | 秒（无 ×10 换算——就是整数秒），重试前的冷却时间 |
| 10 | Auto Retry Max Count | — | 滑动窗口内允许的最大重试次数，超过后锁存 `RETRY_EXHAUST` |
| 11 | Auto Retry Window | — | 秒（无 ×10 换算），用于统计重试次数的滑动窗口宽度 |
| 12 | Current Safe Band % | — | `UINT16` 百分比。固件本身不对该字段做范围钳位；0–50 是约定/文档化的常规范围（见 `parameter-reference.md`），超过 100 会使清除条件公式（`threshold × (100−pct)/100`）变为负值 |
| 13 | Current Protection Mode | CURRENT_MEASUREMENT | 0=Disabled，1=FlagOnly，2=ApplyOutputAction——见第 11.6 节 |
| 14 | Current Protection Output Action | CURRENT_MEASUREMENT | 与偏移 0 相同的枚举，但在 **Protection** 上下文中取值——见第 11.2 节 |
| 15 | Current Limit Threshold | CURRENT_MEASUREMENT | ×0.1 nA，直接与 Measured Current 比较 |
| 16 | Auto Derate Step | RAW_OUTPUT_DRIVE + VOLTAGE_MEASUREMENT | AutoDerateRetry 策略下，每次重试从目标值中扣减的量，×0.1 V |
| 17–19 | Reserved | — | 读回 0，拒绝写入 |

### 校准系数（偏移 20–25）

任意模式下均可读取。**仅在 Calibration 模式下可写**——在其他模式下写入会
返回异常 `0x03`。

| 偏移 | 寄存器 | 能力位保护 | 说明 |
|---:|---|---|---|
| 20 | Output Cal K | RAW_OUTPUT_DRIVE | `UINT16`，÷10000——见第 9 节公式。默认 32768 |
| 21 | Output Cal B | RAW_OUTPUT_DRIVE | `INT16`，原始偏移量，单位为 DAC 码。默认 0 |
| 22 | Measured Voltage Cal K | VOLTAGE_MEASUREMENT | `UINT16`，÷1000000——见第 9 节公式 |
| 23 | Measured Voltage Cal B | VOLTAGE_MEASUREMENT | `INT16`，原始偏移量，单位为 ×0.1 V |
| 24 | Measured Current Cal K | CURRENT_MEASUREMENT | `UINT16`，÷1000000——见第 9 节公式 |
| 25 | Measured Current Cal B | CURRENT_MEASUREMENT | `INT16`，原始偏移量，单位为 ×0.1 nA |
| 26–29 | Reserved | — | 读回 0，拒绝写入 |

**校准公式：** `calibrated = raw × K / D + B`，其中 `raw` 是未校准的 ADC
原始码（测量轴）或待转换为 DAC 码的目标值（输出轴），`D` 在**输出轴上为
10000**，在**两条测量轴上均为 1000000**。测量轴需要更精细的除数，是因为
它们要把一个很小、经过衰减的原始 ADC 增益缩小到物理量——1.0 倍增益在这两条
轴上无法表示（最大可表示值约为 65535/1000000 ≈ 0.0655）；完整推导见
[`parameter-reference.md`](parameter-reference.md)。完整的校准操作流程见
[`calibration-guide.zh.md`](calibration-guide.zh.md)。

### 校准会话（偏移 30–33）

**仅在 Calibration 模式下可写。**

| 偏移 | 寄存器 | 能力位保护 | 说明 |
|---:|---|---|---|
| 30 | Cal Output Enable | RAW_OUTPUT_DRIVE | 1=开，0=关。固件规定全板同一时刻只能有一个通道使能此项——在另一通道仍处于使能状态时尝试使能会被拒绝 |
| 31 | Cal DAC Code | RAW_OUTPUT_DRIVE | 原始 DAC 码，0–65535（若配置了编译期上限则更低——该上限**不是** Modbus 寄存器，它编译进固件中，无法通过线协议发现） |
| 32 | Cal Sample Command | VOLTAGE_MEASUREMENT 或 CURRENT_MEASUREMENT | 写入 1 以在该通道支持的每条测量通路上触发一次新采样；完成后读回 0（同步执行，见第 5 节） |
| 33 | Cal Commit Command | 任意具备校准能力 | 写入 1 以将该通道的校准系数（偏移 20–25）持久化到 NVS |
| 34–39 | Reserved | — | 读回 0，拒绝写入——**注意：**早期协议草案曾在偏移 34 处安排过一个 "Cal Max Raw DAC Limit" 寄存器，但它从未作为 Modbus 寄存器实际发布（DAC 上限只是固件内部的会话状态） |

---

## 11. 命令与模式枚举

### 11.1 Operating Mode

| 值 | 名称 |
|---:|---|
| 0 | Normal |
| 1 | Automatic |
| 2 | Calibration |

Normal 与 Automatic 的行为差异见
[`operating-mode-guide.md`](operating-mode-guide.md)；Calibration 见
[`calibration-guide.zh.md`](calibration-guide.zh.md)。

### 11.2 Output Action

| 值 | 名称 | 作为主机命令合法（通道偏移 0） | 作为保护动作合法（通道偏移 14） |
|---:|---|:---:|:---:|
| 0 | None | 是 | 是 |
| 1 | Enable | 是 | **否** |
| 2 | Disable Graceful | 是 | 是 |
| 3 | Disable Immediate | 是 | 是 |
| 4 | Force Output Zero | **否** | 是 |

在错误的上下文中写入某个取值（例如在保护动作偏移 14 处写入 `Enable`，或
在偏移 0 处作为主机命令写入 `Force Output Zero`）会返回异常 `0x03`。这一
上下文区分是由固件强制执行的，不只是约定俗成——不要假设两个偏移接受相同的
取值集合。

### 11.3 Channel Fault Command

| 值 | 名称 |
|---:|---|
| 0 | None（执行后的读回值） |
| 1 | Clear Active Fault Block |
| 2 | Clear Fault History |

清除一个仍处于不安全状态的 Active Fault Block（例如电流仍高于安全带）会被
拒绝并返回异常 `0x03`，而不是静默地不做任何事。

### 11.4 Param Action

| 值 | 名称 | 适用范围 |
|---:|---|---|
| 0 | None（执行后的读回值） | 系统 + 通道 |
| 1 | Save | 系统 + 通道 |
| 2 | Load | 系统 + 通道 |
| 3 | Factory Reset | 系统 + 通道 |
| 255 | Software Reset | **仅系统级**——在通道偏移 2 处非法，返回异常 `0x03` |

Software Reset 会先确认该次写入（使 FC06 响应得以发出），然后板卡才重启。

### 11.5 Recovery Policy Mode

| 值 | 名称 |
|---:|---|
| 0 | Manual Latch |
| 1 | Auto Retry |
| 2 | Auto Derate Retry |
| 3 | Never Retry |

仅在 Automatic 工作模式下有意义；见
[`operating-mode-guide.md`](operating-mode-guide.md) 第 4 节。

### 11.6 Protection Mode

| 值 | 名称 |
|---:|---|
| 0 | Disabled |
| 1 | Flag Only（记录故障历史，不影响输出） |
| 2 | Apply Output Action（记录故障历史**并**按偏移 14 的设置关闭输出） |

---

## 12. 能力标志位

### 12.1 System Capability Flags（System Input 偏移 3）

| 位 | 掩码 | 含义 |
|---:|---:|---|
| 0 | 0x0001 | 支持 Automatic 工作模式 |
| 1 | 0x0002 | 存在环境传感器（板载温度/湿度有意义） |
| 2 | 0x0004 | 支持 Calibration 模式 |

### 12.2 Channel Capability Flags（Channel Input 偏移 9）

| 位 | 掩码 | 含义 |
|---:|---:|---|
| 0 | 0x0001 | 具备 Output Enable 控制能力 |
| 1 | 0x0002 | 具备原始输出驱动能力（即上文的 `RAW_OUTPUT_DRIVE` 保护位） |
| 2 | 0x0004 | 具备电压测量能力（`VOLTAGE_MEASUREMENT` 保护位） |
| 3 | 0x0008 | 具备电流测量能力（`CURRENT_MEASUREMENT` 保护位） |
| 4 | 0x0010 | 具备硬件状态/故障证据能力 |

**任何标注了以上能力位保护的寄存器，若通道不具备对应能力位，读写都会返回
异常 `0x02`**——这不是一个"读回零值"的软性回退，应当与访问一个不支持的通道
同等对待。请在连接时对每个通道读取一次能力位并缓存结果，不要在运行时通过
异常码反推能力位。

---

## 13. 状态位与故障位字段

### 13.1 Channel Status Bits（Channel Input 偏移 0）

| 位 | 掩码 | 含义 |
|---:|---:|---|
| 0 | 0x0001 | 输出驱动电平非零 |
| 1 | 0x0002 | 输出使能处于激活状态 |
| 2 | 0x0004 | 正在斜坡过程中 |
| 3 | 0x0008 | 存在 Active Fault Block（输出可能已被强制关闭，取决于保护模式） |
| 4 | 0x0010 | 存在 Fault History |
| 5 | 0x0020 | Automatic 模式的重试冷却期处于激活状态 |
| 6 | 0x0040 | 测量数据陈旧（自上次预期更新以来没有新采样） |

### 13.2 Fault Cause Bits（Channel Input 偏移 1/2，System Input 偏移 14）

| 位 | 掩码 | 含义 |
|---:|---:|---|
| 1 | 0x0002 | 电流超限 |
| 2 | 0x0004 | 测量无效 |
| 3 | 0x0008 | 输出硬件故障 |
| 4 | 0x0010 | 变体安全联锁 |
| 5 | 0x0020 | 自动重试预算耗尽 |
| 6 | 0x0040 | 配置无效，无法自动启动 |
| 7 | 0x0080 | 测量数据陈旧 |

第 0 位（`0x0001`）目前未分配——早期协议草案曾用它表示电压超限故障；当前
出货版本只保留了电流保护。多个位可以同时置位（例如 `0x0022` = 电流故障 +
重试耗尽，这是自动重试策略最终放弃时的典型特征）。

---

## 14. 变体档案 — HVB（id=1）

| 字段 | 取值 |
|---|---|
| Variant ID | 1 |
| 物理通道数 | 2，由硬件固定 |
| 电压量程 | 0.1 V/LSB（100 mV/LSB） |
| 电流量程 | 0.1 nA/LSB |
| 默认工作模式 | Normal |
| 默认恢复策略 | Manual Latch |
| 默认电流安全带 | 10% |
| 默认校准系数 | Output K=32768 B=0；Voltage/Current 的 K/B 因板而异，在出厂校准时设定——见 [`calibration-guide.zh.md`](calibration-guide.zh.md) |

其他 Variant ID（例如未来的多通道 LVB 变体）会使用相同的协议结构，但
`Variant ID`、`Supported Channels` 不同，每个通道的能力标志位也可能不同——
不要把通道数量或能力假设写死在这张表里，始终在运行时通过
`Supported Channels`/`Active Channel Mask`/`Capability Flags`（第 5 节）
动态获取。

---

## 15. 异常码

| 代码 | 名称 | 返回时机 |
|---:|---|---|
| 0x01 | Illegal Function | 功能码不是 0x03/0x04/0x06 |
| 0x02 | Illegal Data Address | 保留寄存器、不支持的通道，或通道不具备对应能力位时访问受保护寄存器 |
| 0x03 | Illegal Data Value | 取值超范围、非法枚举、错误上下文的 Output Action、不安全状态下清除故障、在非 Calibration 模式下写校准寄存器、通道级 Software Reset |
| 0x04 | Slave Device Failure | 内部故障——例如 Save/commit 时的 NVS 写入失败 |

---

## 16. 字节级示例

以下均以从站地址 1 为例。给出的 CRC 字节对应给定的具体帧内容——如果修改了
任意一个字节，需要重新计算 CRC。

**读取前 15 个 System Input 寄存器（协议版本到故障原因），FC04，起始地址 0，
数量 15：**

```
请求  : 01 04 00 00 00 0F B0 0E
响应  : 01 04 1E 00 03 00 00 00 01 00 07 00 02 00 03 00 F5 01 C2
        00 00 02 58 00 00 01 02 00 00 00 00 00 00 2B 7C
```
（字节数 `0x1E` = 30 = 15 个寄存器 × 2 字节。解读其中几项：Protocol
Major=3，Minor=0，Variant ID=1，Capability Flags=0x0007，Supported
Channels=2，Active Channel Mask=0x0003，Board Temp=0x00F5=24.5°C，Board
Humidity=0x01C2=45.0%RH，Uptime=0x00000258=600 秒，FW Version=0x00000102。）

**写入 System Holding 偏移 0（Operating Mode）= 1（Automatic），FC06：**

```
请求  : 01 06 00 00 00 01 48 0A
响应  : 01 06 00 00 00 01 48 0A   （FC06 成功时原样回显请求）
```

**写入通道 0 的 Configured Target Voltage（holding 偏移 3，绝对地址
40+3=43=0x002B）为原始值 5000（= 500.0 V），FC06：**

```
请求  : 01 06 00 2B 13 88 F4 94
响应  : 01 06 00 2B 13 88 F4 94
```

**尝试写入一个保留寄存器（System Holding 偏移 4，绝对地址 4）——被拒绝：**

```
请求  : 01 06 00 04 00 01 09 CB
响应  : 01 86 02 C3 A1
```
（功能码 `0x86` = `0x06 | 0x80` 表示这是一个异常响应；`0x02` 是异常码，
Illegal Data Address。）

---

## 17. 自行实现客户端——检查清单

- [ ] 在信任本文档中任何偏移量之前，先校验 `Protocol Major` = 3。
- [ ] 在假设某个通道存在之前先读取 `Supported Channels`/
      `Active Channel Mask`；在访问某通道上受保护的寄存器之前先读取该
      通道的 `Capability Flags`。
- [ ] 把受保护寄存器上的异常 `0x02` 理解为"此处不支持"，而不是需要重试的
      错误。
- [ ] 不要轮询 `COMMAND` 类别的寄存器——它们是只写触发器，成功写入后会
      立即读回 0（第 5 节）。
- [ ] 对每个字段应用正确的量程：电压 0.1 V/LSB，电流 0.1 nA/LSB，校准 K
      使用第 10 节专属的除数，安全带是普通整数百分比——不要假设存在统一
      的换算比例。
- [ ] 遵守 Output Action 的主机/保护上下文区分（第 11.2 节）——同一个枚举，
      在不同偏移处的合法取值子集并不相同。
- [ ] 记住 Slave Address / Baud Rate 的写入需要显式执行 `Param Action =
      Save` 并复位后才会生效（第 7 节）。
