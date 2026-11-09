# emMCP-Home — AI 智能家居控制终端

> 说句话，就能控制空调、灯带、继电器。基于 STM32 + emMCP + FreeRTOS。

---

## 这是什么？

**emMCP-Home** 是一个开源的 AI 智能家居控制终端。它让 STM32 单片机通过 **MCP 协议（Model Context Protocol）** 与 AI 模组通信，把空调、灯带、继电器等硬件变成 AI 可调用的"工具"，实现语音/文本自然交互控制。

**举个例子**：你对 AI 说"把空调开到 26 度制冷模式"，红外模块就会真的发射信号，空调就开了。

---

## 亮点

- 🤖 **7 个 MCP 工具** — 继电器、空调温度/模式/电源/风速、灯带、串口状态监控
- 🖥 **9 页 OLED 交互界面** — 按键翻页，信息一目了然
- 📡 **人体雷达联动** — 人走自动关灯，人来恢复灯效
- 🔴 **红外学习万能遥控** — 学习任意遥控器，告别找遥控
- 🎨 **WS2812 全彩灯带** — 彩虹呼吸、远程调色、亮度控制
- 🐱 **彩蛋：猫咪像素动画** — 28 帧 OLED 动画

---

## 硬件方案

| 模块 | 用途 |
|------|------|
| STM32F103CBTx | 主控 (128KB Flash / 20KB RAM) |
| AiPi-PalChatV1 | AI 语音模组 |
| SSD1306 128×64 | OLED 显示屏 |
| SHT30 | 温湿度传感器 |
| HXD039B2 | 红外学习/发射模块 |
| WS2812B ×60 | RGB 可寻址灯带 |
| Rd-03L_V2 | 24GHz 毫米波雷达 |
| GT20L16S | 中文字库 ROM |

**硬件总成本约 160 元。**

---

## 软件架构

```
FreeRTOS (3 任务)
  ├─ defaultTask     — emMCP 轮询 + UART 解析 + 消息分发
  ├─ sht3x_read      — 传感器读取 + OLED UI + 按键处理
  └─ ws2812_mode     — LED 动画 + 雷达检测 + 色彩过渡
        │
        ▼
    emMCP 框架 (MCP 协议处理)
        │
        ├─ relay        — 继电器(空调总电源)
        ├─ temperature  — 温度调节
        ├─ acmode       — 模式切换
        ├─ acpower      — 红外开关空调
        ├─ acfan        — 风速调节
        ├─ ledstrip     — 灯带控制
        └─ serial_monitor — 串口状态监控
        │
        ▼
    BSP 驱动层 (SPI / I²C / PWM+DMA / UART)
```

---

## 编译 & 烧录

```bash
git clone https://gitee.com/xemowo/emMCP-Home.git
cd emMCP-Home
cmake --preset Debug
cmake --build build/Debug
openocd -f openocd.cfg -c "program build/Debug/XEMOWO_TEST20.bin 0x08000000 verify reset exit"
```

**前置要求**：ARM GCC 10.3+ / CMake 3.22+ / Ninja / OpenOCD / ST-Link V2

---

## 对话演示

```
用户: "把空调打开，26 度，制冷模式"
 AI:  → acpower.set("on") → temperature.set(26) → acmode.set("cool")
MCU:  → 继电器吸合 → 红外发射 → 空调启动

用户: "灯带调成暖黄色，亮度一半"
 AI:  → ledstrip.set({r:255, g:180, b:50, brightness:50})
MCU:  → WS2812 全灯变色

用户: (离开房间)
MCU:  → 雷达检测无人 5 秒 → 自动关灯

用户: (回来)
MCU:  → 雷达检测有人 → 自动恢复彩虹灯效
```

---

## MCP 工具清单

| 工具名 | 参数 | 说明 |
|--------|------|------|
| **relay** | `state` (string: on/off) | 继电器开关（空调总电源） |
| **temperature** | `value` (number, 16-31) | 空调目标温度（红外） |
| **acmode** | `value` (string: auto/cool/dry/fan/heat) | 运行模式（红外） |
| **acpower** | `power` (string: on/off) | 红外开关空调 |
| **acfan** | `speed` (string: auto/low/mid/high) | 红外风速 |
| **ledstrip** | `mode/r/g/b/brightness` | 灯带颜色控制 |
| **serial_monitor** | `action` (string: open/close) | 串口 STATUS 监控 |

---

## 快速测试（无需 AI 模组）

用串口助手接 USART2，波特率 115200，发：

```json
{"role":"AI board","msgType":"mcp_set","tools":"relay","data":{"state":"on"}}
```

MCU 返回 `mcp-responsive {"role":"MCU","msgType":"status","data":true}`，同时继电器动作。

---

## 项目结构

```
emMCP-Home/
├── Core/                # CubeMX 生成代码 + 核心逻辑
├── components/          # 外设驱动 (SHT3x/SSD1306/WS2812/HXD039B2/Radar/Relay)
├── Bsp/                 # 板级支持 (SPI/I²C/PWM+DMA/Delay)
├── Drivers/             # STM32 HAL + CMSIS
├── Middlewares/         # FreeRTOS
├── cmake/               # 构建工具链
├── README.md            # 项目说明
├── GUIDE.md             # 新手上手指南
└── 9Mod_MCPBorad.ioc    # CubeMX 工程文件
```

---

## 致谢

基于 [Ai-Thinker-Open/emMCP](https://github.com/Ai-Thinker-Open/emMCP)（MIT 协议）构建，感谢安信可科技。

---

> 🐱 **emMCP-Home** — AI 智能家居，从一句"你好小安"开始。
>
> https://gitee.com/xemowo/emMCP-Home
