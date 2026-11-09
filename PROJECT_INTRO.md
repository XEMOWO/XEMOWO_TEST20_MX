# emMCP-Home — AI 智能家居控制终端

> 说句话，就能控制空调、灯带，还能查询室内温湿度。基于 STM32 + emMCP + FreeRTOS。

---

## 这是什么？

**emMCP-Home** 是一个开源的 AI 智能家居控制终端。它让 STM32 单片机通过 **MCP 协议（Model Context Protocol）** 与 AI 模组通信，把空调、灯带、温湿度传感器等硬件变成 AI 可调用的"工具"，实现语音/文本自然交互控制。

**举个例子**：你对 AI 说"把空调开到 26 度制冷模式"，红外模块就会真的发射信号，空调就开了。

---

## 亮点

- 🤖 **3 个 MCP 工具** — 空调控制（电源/温度/模式/风速）、灯带、温湿度查询
- 🖥 **10 页 OLED 交互界面** — AI 表情/状态主界面 + 按键翻页，信息一目了然
- 💡 **灯带控制** — 语音开灯默认暖黄色，关灯完全熄灭，支持调色/亮度
- 🔴 **红外学习万能遥控** — 学习任意遥控器，告别找遥控
- 🎨 **WS2812 全彩灯带** — 远程调色、开关、亮度控制（雷达联动暂未启用）
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
| Rd-03L_V2 | 24GHz 毫米波雷达（人体存在检测） |
| GT20L16S | 中文字库 ROM |

**硬件总成本约 160 元。**

---

## 软件架构

```
FreeRTOS (3 任务)
  ├─ defaultTask     — emMCP 轮询 + UART 解析 + 消息分发
  ├─ sht3x_read      — 传感器读取 + OLED UI + 按键处理
  └─ ws2812_mode     — OLED 初始化 + WS2812 灯带初始化
        │
        ▼
    emMCP 框架 (MCP 协议处理)
        │
        ├─ ac                  — 空调控制(电源/温度/模式/风速)
        ├─ ledstrip            — 灯带控制
        └─ temperature_humidity — 温湿度查询
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
 AI:  → ac.set({power:"on", temperature:26, mode:"cool"})
MCU:  → 红外发射 → 空调启动

用户: "灯带调成暖黄色，亮度一半"
 AI:  → ledstrip.set({r:255, g:180, b:50, brightness:50})
MCU:  → WS2812 全灯变色

用户: "现在室内温度多少"
 AI:  → temperature_humidity.set({})
MCU:  → 返回 "temperature=25.6C,humidity=48.2%"

用户: "开灯"
 AI:  → ledstrip.set({mode:"on"})
MCU:  → WS2812 默认暖黄色点亮

用户: "关灯"
 AI:  → ledstrip.set({mode:"off"})
MCU:  → WS2812 完全熄灭
```

---

## MCP 工具清单

| 工具名 | 参数 | 说明 |
|--------|------|------|
| **ac** | `power` (on/off)、`temperature` (16-31)、`mode` (auto/cool/dry/fan/heat)、`fan` (auto/low/mid/high)，均可选 | 空调开关机/温度/模式/风速（红外） |
| **ledstrip** | `mode/r/g/b/brightness` | 灯带颜色控制 |
| **temperature_humidity** | 无参数 | 查询 SHT30 温度与湿度 |

---

## 快速测试（无需 AI 模组）

用串口助手接 USART2，波特率 115200，发：

```json
{"role":"AI board","msgType":"mcp_set","tools":"ac","data":{"power":"on"}}
```

MCU 返回 `mcp-responsive {"role":"MCU","msgType":"status","data":true}`，同时红外模块发射空调开机码。

---

## 项目结构

```
emMCP-Home/
├── Core/                # CubeMX 生成代码 + 核心逻辑
├── components/          # 外设驱动 (SHT3x/SSD1306/WS2812/HXD039B2/Radar)
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
