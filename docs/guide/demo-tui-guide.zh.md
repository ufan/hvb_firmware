# PSB Demo TUI 用户指南

`psb_demo_tui` 是一个交互式终端面板，用于通过 Modbus RTU 对 Jianwei 高压控制板进行
实时监控和控制，本文是它的快速上手参考。这是一款**工程调试 / 上电联调 / 演示
工具**，不是出厂校准工具——它没有校准模式界面。板卡校准请参阅
[`calibration-guide.zh.md`](calibration-guide.zh.md)。

*(English version: [demo-tui-guide.md](demo-tui-guide.md))*

---

## 1. 编译与启动

```bash
cmake -S tools -B tools/build
cmake --build tools/build --target psb_demo_tui -j
tools/bin/psb_demo_tui [-p PORT] [-b BAUD] [-i SLAVE_ID] [-t TIMEOUT_MS] [-s POLL_S]
```

| 参数 | 含义 | 默认值 |
|---|---|---|
| `-p` | 串口设备，如 `/dev/ttyUSB0`。给定后启动时自动连接。 | 需在界面中手动连接 |
| `-b` | 波特率 | 115200 |
| `-i` | Modbus 从站地址 | 1 |
| `-t` | 连接超时（毫秒） | 3000 |
| `-s` | 后台轮询间隔（秒） | 1 |

如果 `~/.psb_demo_app.toml` 已存在（由配套的 CLI 工具 `psb_demo_cli --save`
写入），其中的 `[connection]` 部分会为你未在命令行传入的参数提供默认值。
**TUI 本身从不写入该文件**——你在连接弹窗中输入的连接参数只对当前会话有效，
下次启动不会自动记住，除非你再次传入 `-p`/`-b`/`-i`，或用其他方式维护该
TOML 文件。

---

## 2. 界面布局

```
┌ PSB │ 2 Channels │ [Normal ▾] │ [ Save ]      ● 1234s | T:24.1C H:41.2%      [ Connect ] [ Quit ] ┐
├──────────────────────────────────────────────────────────────────────────────────────────────────┤
│  Monitor    CH0    CH1                                                                             │
├──────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                      │
│                         (当前标签页内容：Monitor 表格，或某个通道的详情面板)                          │
│                                                                                                      │
├──────────────────────────────────────────────────────────────────────────────────────────────────┤
│  OK: Target V                                    FW:0x0102  Proto:1.0    /dev/ttyUSB0 @115200 #1  [Setting] │
└──────────────────────────────────────────────────────────────────────────────────────────────────┘
```

| 区域 | 内容 |
|---|---|
| 菜单栏 | 标题、通道数、工作模式选择器、Save 按钮、连接指示与运行时间/温度/湿度（已连接时显示）、Connect/Disconnect/Abort 切换按钮、Quit |
| 标签栏 | `Monitor` + 板卡上报的每个通道各一个 `CHn` 标签 |
| 标签内容 | Monitor 表格，或所选通道的详情面板 |
| 状态栏 | 上一次操作结果、固件/协议版本、连接信息字符串、`[Setting]` 按钮 |

导航方式：**Tab** 在控件间移动焦点，方向键在同一行内移动并切换选择器取值，
**Enter** 提交文本框或选择器的修改，**Space**/**←**/**→** 同样可以切换
选择器。全程支持鼠标点击。**Esc** 关闭当前弹出的对话框。

---

## 3. 建立连接

点击菜单栏右上角的 **[ Connect ]** 打开连接设置弹窗：

```
┌ Connection Settings ─────────────┐
│ Port  : [/dev/ttyUSB0 ▾] [Rescan]│
│ Baud  : [115200]                 │
│ Slave : [1]                      │
│                                   │
│      [ Connect ]  [ Cancel ]     │
└───────────────────────────────────┘
```

- **Rescan** 重新扫描可用串口。
- 填写 Baud/Slave 后点击 **[ Connect ]**。同一个按钮在连接过程中显示为
  **[ Abort ]**，连接成功后显示为 **[ Disconnect ]**——它始终是同一个按钮。
- 连接指示圆点：空心灰色 = 离线，黄色沙漏 = 连接中，呼吸绿色 = 已连接。
- 启动时带 `-p` 参数会跳过弹窗直接连接。
- 断开连接后，所有通道标签会收起，只剩下 `Monitor`——下次连接并完成通道
  扫描后会重新出现。

---

## 4. 菜单栏

| 元素 | 行为 |
|---|---|
| `N Channels` | 板卡上报的通道数（未连接时为 `--`） |
| 工作模式 `[Normal ▾]` / `[Automatic ▾]` | 点击、Space 或 ←/→ 切换——**每次切换都立即生效**，这一点与下文第 6 节介绍的按通道选择器不同。各模式的具体差异见 [`operating-mode-guide.zh.md`](operating-mode-guide.zh.md)（如无中文版则见英文版 `operating-mode-guide.md`）。 |
| `[ Save ]` | 等同于工厂工具中的 `sys save`：把**所有**通道的运行参数 + 系统配置保存到 NVS。未连接时为暗色不可用状态。 |
| 中间区域 | 连接圆点、运行时间、板载温度、湿度——仅在已连接时显示 |
| `[ Connect / Disconnect / Abort ]` | 见第 3 节 |
| `[ Quit ]` | 退出应用 |

---

## 5. Monitor 标签页

每个通道一行，把关键控制字段都汇总在同一张表里：

```
┌────┬────────┬────────┬────────┬────────┬─────────┬─────┬─────┬───────┬───────┬───────┬──────┐
│    │  Vset  │ Status │  Vop   │  V (V) │  I (nA) │ Ru  │ Rd  │ Limit │ Fault │ Clear │ Save │
╞════╪════════╪════════╪════════╪════════╪═════════╪═════╪═════╪═══════╪═══════╪═══════╪══════╡
│CH0 │ +500.0 │ [ ON ] │ +500.0 │ +499.8 │  +12.3  │ 5.0 │ 5.0 │ 1000  │  --   │ Clear │ Save │
│CH1 │  +0.0  │ [ OFF ]│  +0.0  │  +0.0  │  +0.0   │ 5.0 │ 5.0 │ 1000  │  --   │ Clear │ Save │
└────┴────────┴────────┴────────┴────────┴─────────┴─────┴─────┴───────┴───────┴───────┴──────┘
```

| 列 | 含义 | 是否可编辑 |
|---|---|---|
| `Vset` | 设定的目标电压（V） | 输入数值后按 **Enter** 写入 |
| `Status` | `[ OFF ]` / `[ ON ]` / `[ RAMP ]`——点击行为见下文 | 点击切换 |
| `Vop` | 当前运行目标电压（实时值，会向 `Vset` 斜坡逼近） | 只读 |
| `V (V)` | 测量电压 | 只读 |
| `I (nA)` | 测量电流 | 只读 |
| `Ru` / `Rd` | 斜坡上升 / 下降步进（每个间隔的电压变化量） | 输入后按 **Enter** 写入 |
| `Limit` | 过流保护阈值（nA） | 输入后按 **Enter** 写入 |
| `Fault` | 与保护模式相关，见下文 | 只读 |
| `Clear` | 清除故障——具体清除对象取决于保护模式，见下文 | 点击 |
| `Save` | 把该通道的运行参数保存到 NVS（`param save`，仅该通道） | 点击 |

当某通道不具备相应能力时（例如没有电流测量能力），对应列会显示暗色的
`--` 而不是控件（没有 `I (nA)`、`Limit`、`Fault`、`Clear`）。

**Status 按钮点击行为**——单次点击会根据当前状态执行使能或平滑关闭：

| 当前状态 | 点击后的动作 |
|---|---|
| 关闭，目标电压为 0 | 无动作 |
| 关闭，目标电压非零 | 使能 |
| 正在斜坡中 | 无动作（等待其稳定） |
| 已开启（输出非零） | 平滑关闭 |

**Fault 列**与保护模式相关——具体显示什么取决于该通道当前的过流保护模式
（在通道标签页中设置，见第 6 节）：

| 保护模式 | Fault 列显示 | Clear 按钮清除对象 |
|---|---|---|
| `Apply-Action` | **当前故障**（保护已实际关闭输出） | 当前故障阻断 |
| `FlagOnly` | **故障历史**（唯一留下的痕迹——输出从未被动过） | 故障历史 |
| `Disabled` | 当前故障，暗色显示（对非电流类故障仍有意义） | 当前故障阻断 |

故障代码：`CL` 电流、`MI` 测量、`HW` 硬件、`IL` 联锁、`RE` 重试耗尽、
`CI` 配置无效、`ST` 数据陈旧。多个代码可能同时出现（例如自动重试耗尽后
显示 `CL RE`）。

---

## 6. 通道标签页（`CHn`）

```
 Live │ Vop: +500.0 V   V: +499.8 V   I: +12.3 nA   Status: ON   Retries: 0

┌ Control ─────────────────┐  ┌ Protection Policy ─────────┐
│ [Enable][Disable][Kill]  │  │ Limit  : 1000        nA    │
│ Vset    : +500.0      V  │  │ Mode   : [Apply-Action ▾]  │
│ Ramp up : 5.0        V/s │  │ Action : [Dis-Graceful ▾]  │
│ Ramp dn : 5.0        V/s │  │ [ClrActive]  [ClrHist]     │
└───────────────────────────┘  └─────────────────────────────┘
┌ Setting ──────────────────┐  ┌ Recovery Policy ────────────┐
│ [Save] [Load] [Factory]  │  │ Policy : [AutoRetry ▾]      │
└───────────────────────────┘  │ Max    : 3    Win: 60    s │
                                │ Delay  : 10               s │
                                │ Derate : 0               LSB│
                                │ Band   : 10                %│
                                └─────────────────────────────┘
```

### Live 面板
只读：运行目标电压、测量 V/I、状态标签（`OFF` / `ON` / `RAMP` / `ON RAMP` /
`FAULT` / `COOL` / `STALE`），以及当前滑动窗口内的自动重试次数。

### Control 面板
- `[Enable]` / `[Disable]` / `[Kill]` —— 点击后立即发送 `Enable` /
  `DisableGraceful` / `DisableImmediate`。若通道不支持输出能力则隐藏。
- `Vset`、`Ramp up`、`Ramp dn` —— 文本框，各自按 **Enter** 单独提交。

### Protection Policy 面板
仅当通道同时具备电压和电流测量能力时显示。`Limit` 为文本框（Enter 提交）。
`Mode` 和 `Action` 是选择器：点击、Space 或 ←/→ 逐个切换选项，但**与菜单栏
的工作模式选择器不同，这里切换后不会自动生效**——切换完成后需按 **Enter**
才会真正写入。`[ClrActive]` / `[ClrHist]` 直接清除当前故障阻断 / 故障历史，
与 Monitor 标签页中随模式变化的 Clear 按钮相互独立。

### Recovery Policy 面板
`Policy` 在 `ManualLatch` / `AutoRetry` / `AutoDerate` / `NeverRetry` 之间
切换（提交规则与上面的 Mode/Action 相同，需按 Enter）。`Max`、`Win`、
`Delay`、`Derate`、`Band` 均为文本框，各自独立按 Enter 提交。各字段的具体
含义见 [`operating-mode-guide.md`](operating-mode-guide.md) 第 4 节——这里
的界面只是那个模型的直接映射，本身不附加任何额外行为。

### Setting 面板
`[Save]` / `[Load]` / `[Factory]` —— 按通道的运行参数持久化操作
（`param save|load|factory-reset`，仅该通道）。与菜单栏的 `[ Save ]`
（一次性覆盖所有通道 + 系统配置）是两回事。

---

## 7. System Config 弹窗

通过状态栏（右下角）的 **[Setting]** 打开：

```
┌ System Config ────────────────┐
│ Working Mode  : [Normal ▾]    │
│ Startup Policy: [Load NVS ▾]  │
│                                │
│   [Save]  [Load]  [Factory]   │
│                                │
│  Modbus (next boot)           │
│ Slave Address : [1]           │
│ Baud Rate     : [115200 ▾]    │
│      [ Save Modbus ]          │
│                                │
│           [ Reset ]           │
│           [ Close ]           │
└────────────────────────────────┘
```

| 控件 | 作用 |
|---|---|
| Working Mode | 与菜单栏是同一个选择器（共享状态） |
| Startup Policy | `Load NVS Config` 或 `Factory Default`——决定板卡*下次*启动时加载哪套配置 |
| `[Save]` / `[Load]` / `[Factory]` | 系统级操作，覆盖所有通道——效果等同于菜单栏 Save，外加加载/恢复出厂 |
| Slave Address / Baud Rate + `[ Save Modbus ]` | 写入新的 Modbus 通信参数。**仅在复位后生效**——之后需要用新的地址/波特率重新连接。 |
| `[ Reset ]` | 发送软件复位并断开连接。板卡重启完成后需手动重新连接。 |
| `[ Close ]` / Esc | 关闭弹窗，不做其他操作 |

---

## 8. 状态栏

| 元素 | 含义 |
|---|---|
| 消息 | 上一次写操作的结果，绿色表示成功（`OK: ...`），红色表示失败（`Error: ...`） |
| `FW:` / `Proto:` | 固件版本与协议版本（仅连接时显示） |
| 连接信息字符串 | `<port> @<baud> #<slave>`、`Connecting...` 或 `offline` |
| `[Setting]` | 打开 System Config 弹窗（见第 7 节） |

---

## 9. 典型操作流程

把通道 0 升到 500 V，观察运行状态，再关闭：

1. 点击 `[ Connect ]` → 填写串口 → 弹窗中点击 `[ Connect ]`。
2. 选择 `Monitor` 标签页（或 `CH0` 标签页以查看完整的控制/保护视图）。
3. 在 CH0 的 `Vset` 中输入 `500.0`，按 **Enter**。
4. 点击 `Status` 按钮（此时目标电压已非零，`[ OFF ]` 会触发使能）。斜坡
   过程中显示 `[ RAMP ]`，`Vop` 到达 `Vset` 后变为 `[ ON ]`。
5. 观察 `V (V)` / `I (nA)` 按轮询间隔（`-s`，默认 1 秒）刷新。
6. 若 `Fault` 出现代码：先在通道标签页的 Protection Policy 面板确认保护
   模式，待触发条件消除后再点击对应的 Clear 按钮（见第 5 节）。
7. 再次点击 `Status` 平滑关闭，或使用 `CH0` 标签页的 `[Disable]` /
   `[Kill]` 分别执行平滑 / 立即停止。

---

## 10. 与其他工具的关系

| 工具 | 可执行文件 | 用途 |
|---|---|---|
| Demo TUI（本文档） | `psb_demo_tui` | 交互式监控/控制，用于工程调试与演示 |
| Demo CLI | `psb_demo_cli` | 可脚本化的一次性命令；`--save` 会写入 `~/.psb_demo_app.toml` |
| Factory TUI | `psb_factory_tui` | 出厂校准——见 [`calibration-guide.zh.md`](calibration-guide.zh.md) |

---

## 11. 故障排查

| 现象 | 原因 / 处理方法 |
|---|---|
| 连接立即失败 | 端口错误、板卡未上电，或另一个进程（如 `psb_demo_cli`、`psb_factory_tui`）已占用该串口——同一时刻只能有一个工具占用该端口。 |
| 输入新值后又变回原值 | 在按 **Enter** 之前切换了焦点，导致修改未被发送。重新输入并按 Enter。 |
| 选择器显示了新值，但板卡上没有任何变化 | 切换了 Mode/Action/Policy 选择器却没有按 Enter——除菜单栏的工作模式选择器外，其他选择器都不会自动生效（见第 6 节）。 |
| 通道标签页消失 | 连接已断开——`reconcileDisconnectedTabs` 会收起为只剩 `Monitor`。重新连接即可。 |
| 启动时输入的端口/波特率/从站地址下次没有记住 | TUI 从不写入 `~/.psb_demo_app.toml`——请再次传入 `-p`/`-b`/`-i`，或通过 `psb_demo_cli --save` 维护该文件。 |
| 修改了从站地址或波特率后板卡失去响应 | 这些修改仅在复位后生效（见第 7 节）——板卡重启后请使用*新*参数重新连接。 |
