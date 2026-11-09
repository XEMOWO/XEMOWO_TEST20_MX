# 🏠 emMCP-Home 新手上手指南

> **读完这篇指南，即使你是嵌入式小白，也能理解这个项目在做什么、怎么跑起来。**

---

## 📖 目录

1. [这项目是干嘛的？](#1-这项目是干嘛的)
2. [它能做什么？](#2-它能做什么)
3. [你需要准备什么硬件？](#3-你需要准备什么硬件)
4. [它是怎么工作的？](#4-它是怎么工作的)
5. [环境搭建（新手最怕的环节）](#5-环境搭建)
6. [编译 & 烧录](#6-编译--烧录)
7. [跑起来之后怎么看效果？](#7-跑起来之后怎么看效果)
8. [MCP 工具详解](#8-mcp-工具详解)
9. [常见问题 FAQ](#9-常见问题-faq)
10. [下一步可以玩什么？](#10-下一步可以玩什么)

---

## 1. 这项目是干嘛的？

### 一句话版

**用 AI 语音控制家电的 STM32 单片机项目。**

你对 AI 模组说"把空调开到 26 度"，MCU 收到指令后通过红外模块发射信号，空调就真的开了。

### 展开说说

这个项目的核心是把三件事串在了一起：

```
你的语音 → AI 理解 → JSON 指令 → 单片机 → 硬件动作
```

| 环节 | 谁干的 | 举例 |
|------|--------|------|
| 听懂你说什么 | Ai-Thinker AI 模组 (PalChatV1) | "把灯带调成暖黄色" |
| 把意图变成指令 | emMCP 框架 | `{"name":"ledstrip","r":255,"g":180,"b":50}` |
| 执行硬件操作 | STM32F103 MCU | PWM 驱动 WS2812 灯带变色 |

### 为什么叫 emMCP-Home？

- **emMCP** = Easy MCU MCP，安信可开源的一个让单片机支持 MCP 协议的小框架
- **Home** = 智能家居场景
- 合起来就是"用 emMCP 做的智能家居"

---

## 2. 它能做什么？

### 🎯 四大核心功能

| 功能 | 怎么触发 | 实际效果 |
|------|----------|----------|
| **开关空调** | 语音 "打开空调" / "关空调" | 继电器吸合 + 红外发射开机码 |
| **调温度** | 语音 "调到 26 度" | 红外发射 26°C 温度码 |
| **切换模式** | 语音 "制冷模式" | 红外发射模式码（自动/制冷/除湿/送风/制热） |
| **控制灯带** | 语音 "灯带调成蓝色，亮度 50%" | WS2812 全灯变色 |

### 🧠 智能联动（不需要语音！）

| 场景 | 怎么触发 | 实际效果 |
|------|----------|----------|
| **人走灯灭** | Rd-03L 雷达检测无人超过 5 秒 | 灯带自动熄灭 |
| **人回灯亮** | 雷达检测到有人回来 | 灯带恢复彩虹呼吸效果 |

### 🖥 OLED 屏幕显示

板子上有一块 128x64 的 OLED 屏幕，用两个按键翻页，共 9 个页面：

```
第1页 → 系统信息（项目名、传感器状态）
第2页 → 实时温度
第3页 → 实时湿度
第4页 → FreeRTOS 任务监控（栈余量、状态）
第5页 → AI 模组串口原始数据（调试用）
第6页 → AI 事件状态 + WiFi 连接状态
第7页 → 🐱 猫咪像素动画（28 帧）
第8页 → 已注册的 MCP 工具列表
第9页 → 红外学习（按一下按键进入学习模式）
```

### 🐱 那个猫咪是什么？

一个 28 帧的像素猫咪动画，占 ~28KB Flash。默认是**禁用**的（因为占空间），想开启的话把 `freertos.c` 第 31 行改成：

```c
#define CAT_ANIMATION_ENABLED 1
```

---

## 3. 你需要准备什么硬件？

### 必须有的

| 硬件 | 型号 | 大概多少钱 | 干嘛用 |
|------|------|-----------|--------|
| 主控板 | STM32F103CBTx 开发板 | ~30 元 | 项目的大脑 |
| AI 模组 | Ai-Thinker AiPi-PalChatV1 | ~80 元 | 语音识别 + MCP 通信 |
| OLED 屏 | SSD1306 128×64 (SPI 接口) | ~8 元 | 显示信息 |
| 温湿度 | SHT30 模块 | ~5 元 | 测温度湿度 |
| 红外模块 | HXD039B2-MML | ~15 元 | 学习/发射红外码 |
| WS2812 | WS2812B 灯带 (1米 60灯) | ~10 元 | RGB 灯效 |
| 调试器 | ST-Link V2 | ~15 元 | 烧录程序 |
| 按键 | 轻触开关 ×2 | ~1 元 | OLED 翻页 |
| 杜邦线 | 公母头各一把 | ~5 元 | 接线 |

**总成本：约 160 元**

### 可选的

| 硬件 | 型号 | 大概多少钱 | 干嘛用 |
|------|------|-----------|--------|
| 雷达 | Rd-03L_V2 毫米波 | ~15 元 | 人体存在检测（人走关灯） |
| 继电器 | SRD-05VDC 模块 | ~3 元 | 控制大功率设备 |
| 中文字库 | GT20L16S | ~8 元 | 显示中文不用自己造字库 |

---

## 4. 它是怎么工作的？

### 整体架构（用大白话说）

把整个系统想象成一个公司：

```
你 (用户) ──说话──→ 前台 (AI模组) ──写工单(JSON)──→ 经理 (emMCP) ──派活──→ 员工 (硬件驱动)
                                                                        │
                                                             ┌──────────┼──────────┐
                                                             │          │          │
                                                          红外员工   灯带员工   继电器员工
                                                          (发信号)   (变色)    (开关)
```

### 技术架构（正经版）

```
┌────────────────────────────┐
│     FreeRTOS 操作系统       │  ← 管理 3 个任务
│                             │
│  ┌───────┐ ┌───────┐ ┌────┐│
│  │default│ │sht3x  │ │ws  ││
│  │Task   │ │_read  │ │2812││
│  │       │ │       │ │mode││
│  │• 串口 │ │• 传感器│ │•LED││
│  │• MCP  │ │• OLED │ │•雷达││
│  └───┬───┘ └───┬───┘ └──┬─┘│
│      │         │        │   │
│      └─────────┼────────┘   │
│                ▼            │
│         ┌──────────┐        │
│         │  emMCP   │        │  ← MCP 协议处理
│         │  框架    │        │
│         └────┬─────┘        │
│              │              │
│    ┌─────────┼─────────┐    │
│    ▼         ▼         ▼    │
│ ┌────┐  ┌────┐  ┌────────┐ │
│ │红外│  │灯带│  │温湿度  │ │  ← 硬件驱动层
│ └────┘  └────┘  └────────┘ │
│                             │
│  下面还有 HAL 库 + 硬件      │
└────────────────────────────┘
```

### 数据怎么流动的？

举个例子，你说"把空调开到 26 度"：

```
步骤1: 你说 "把空调开到26度"
       ↓
步骤2: AI 模组识别意图，通过 UART2 发 JSON 给 STM32：
       {"name":"temperature","value":26}
       ↓
步骤3: STM32 串口收到，emMCP 解析 JSON
       ↓
步骤4: emMCP 找到 "temperature" 对应的处理函数
       ↓
步骤5: 处理函数调用 HXD039B2 红外模块，发射 26°C 温度码
       ↓
步骤6: 空调收到红外信号，设定为 26°C
       ↓
步骤7: STM32 回复 {"ctrl":"ok"} 给 AI 模组
       ↓
步骤8: AI 模组语音回复 "好的，已调到 26 度"
```

整个过程不到 1 秒。

---

## 5. 环境搭建

> ⚠️ 这是新手最容易卡住的环节，请仔细跟着做。

### 5.1 操作系统

**推荐 Ubuntu 22.04**（WSL 也行，本指南以 WSL 为例）。

如果你是 Windows 用户：
1. 打开 PowerShell，输入 `wsl --install -d Ubuntu-22.04`
2. 重启电脑
3. 打开 Ubuntu，设置用户名密码

### 5.2 安装 ARM 交叉编译器

```bash
sudo apt update
sudo apt install -y gcc-arm-none-eabi
```

验证：`arm-none-eabi-gcc --version` 应该输出 10.3 或更高。

### 5.3 安装 CMake + Ninja

```bash
sudo apt install -y cmake ninja-build
```

验证：`cmake --version` 应该输出 3.22 或更高。

### 5.4 安装 STM32CubeMX（可选）

如果你要改引脚配置，才需要 CubeMX。去 [ST 官网](https://www.st.com/en/development-tools/stm32cubemx.html) 下载。

### 5.5 安装 OpenOCD（烧录用）

```bash
sudo apt install -y openocd
```

验证：`openocd --version`

### 5.6 下载项目代码

```bash
git clone https://gitee.com/xemowo/emMCP-Home.git
cd emMCP-Home
```

---

## 6. 编译 & 烧录

### 6.1 编译

```bash
# 在项目根目录下
cmake --preset Debug
cmake --build build/Debug
```

如果一切正常，你会看到：

```
[57/57] Linking C executable XEMOWO_TEST20.elf
Memory region         Used Size  Region Size  %age Used
             RAM:       16736 B        20 KB     81.72%
           FLASH:       98788 B       128 KB     75.37%
```

### 6.2 烧录到开发板

用 ST-Link 连接开发板，然后：

```bash
openocd -f openocd.cfg -c "program build/Debug/XEMOWO_TEST20.bin 0x08000000 verify reset exit"
```

看到 `** Verified OK **` 就说明烧录成功。

### 6.3 接线

烧录完成后，确保这些线接对了：

| STM32 引脚 | 连接到 | 说明 |
|-----------|--------|------|
| PA2 (TX) | AI 模组 RX | UART MCP 通信 |
| PA3 (RX) | AI 模组 TX | UART MCP 通信 |
| PA9 (TX) | USB 转串口 RX | 调试日志 (1500000 波特率) |
| PB10 (TX) | HXD039B2 RX | 红外模块 (57600 波特率) |
| PB11 (RX) | HXD039B2 TX | 红外模块 |
| PA7 (MOSI) | OLED DIN | SPI 显示 |
| PA5 (SCK) | OLED CLK | SPI 时钟 |
| PB6 (SDA) | SHT3x | I²C 数据 |
| PB7 (SCL) | SHT3x | I²C 时钟 |
| PA11 | WS2812 DIN | 灯带数据 |
| PA8 | Rd-03L OT1 | 雷达人体检测 |
| PB5 | 继电器 IN | 开关控制 |
| PB4 | 按键 1 | OLED 上翻页 |
| PB8 | 按键 2 | OLED 下翻页 |
| PB1 | HXD039B2 BUSY | 红外忙检测 |

---

## 7. 跑起来之后怎么看效果？

### 7.1 看 OLED 屏幕

上电后 OLED 会显示第 1 页（系统信息），按 PB4/PB8 翻页。

如果第 2、3 页显示温度湿度，说明 SHT3x 正常工作。如果显示 "ERR"，说明传感器没接好或 I²C 线有问题。

### 7.2 看串口日志

用 USB 转串口接 PA9 (TX)，波特率 1500000，打开串口助手（推荐 MobaXterm 或 Putty），你会看到：

```
[INFO] emMCP init done, MAX_TOOLS=7
[INFO] sht3x init OK!
[INFO] system start
```

### 7.3 测试 MCP 通信

**第一步**：确认 AI 模组已配网（参考 AiPi-PalChatV1 文档）

**第二步**：对 AI 模组说"你好小安"，串口日志应该出现：

```
[DEBUG] emMCP_EventCallback: event:8, type:4, param:2.WakeUP
```

**第三步**：看到这个就说明 emMCP 通信成功！然后可以试着说"打开空调"。

### 7.4 如果没有 AI 模组

你也可以用串口直接发 JSON 模拟 AI 模组的行为来测试。

接上 PA3 (USART2 RX)，用串口助手发送：

```json
{"role":"AI board","msgType":"mcp_set","tools":"relay","data":{"state":"on"}}
```

MCU 应该回复：

```
mcp-responsive {"role":"MCU","msgType":"status","data":true}
```

同时继电器吸合。

---

## 8. MCP 工具详解

项目向 AI 注册了 7 个"工具"，AI 可以通过发送 JSON 来调用它们。

| 工具名 | 参数 | 实际动作 |
|--------|------|----------|
| `relay` | `state`: on/off | PB5 继电器吸合/释放（空调总电源） |
| `temperature` | `value`: 16-31 | 红外发射对应温度码 |
| `acmode` | `value`: auto/cool/dry/fan/heat | 红外发射对应模式码 |
| `acpower` | `power`: on/off | 红外发射空调开机/关机码 |
| `acfan` | `speed`: auto/low/mid/high | 红外发射对应风速码 |
| `ledstrip` | `mode`, `r`, `g`, `b`, `brightness` | WS2812 灯带开关/调色/亮度 |
| `serial_monitor` | `action`: open/close | 解析串口 `STATUS:n` 并驱动灯带颜色 |

**AI 调用示例**：
```json
{"name":"temperature","args":{"value":26}}
{"name":"ledstrip","args":{"mode":"set","r":255,"g":180,"b":50,"brightness":50}}
```

**实际动作**：红外发射 26°C 温度码；WS2812 灯带全部变暖黄色、50% 亮度

---

## 9. 常见问题 FAQ

### Q: 编译报错 `arm-none-eabi-gcc: not found`

A: 没装交叉编译器，执行 `sudo apt install -y gcc-arm-none-eabi`

### Q: 烧录失败 `Error: init mode failed`

A: ST-Link 没接好，检查 SWDIO/SWCLK/GND/VCC 四根线

### Q: OLED 黑屏

A: 检查 SPI 接线（PA5/PA7），检查 OLED 供电（3.3V），检查 OLED 的 RES 引脚有没有拉高

### Q: SHT3x 显示 ERR

A: 检查 I²C 接线（PB6/PB7），检查 SHT30 供电

### Q: AI 模组没反应

A:
1. 确认 AI 模组已配网（手机热点或 WiFi）
2. 检查 UART2 接线（PA2 TX ↔ 模组 RX, PA3 RX ↔ 模组 TX）
3. 波特率必须是 115200
4. 说"你好小安"看日志有没有 WakeUP 事件

### Q: 猫咪动画开了之后黑屏

A: 动画数据占了 ~28KB Flash，开启后可能超出 STM32F103CBTx 的 128KB Flash 限制。代码里有注释提醒这个。要么换 STM32F103RETx（512KB），要么关掉动画。

### Q: 红外学习怎么用？

A:
1. 按 PB4/PB8 翻到第 9 页（红外学习页）
2. 进入第 9 页会自动进入学习模式
3. 此时拿遥控器对着 HXD039B2，按开关键
4. 看到 "学习完成" 就 OK 了
5. 退出学习模式：按翻页键翻到别的页面

### Q: 怎么添加自己的 MCP 工具？

A: 在 `freertos.c` 的 `StartDefaultTask` 函数里，参考已有的 7 个工具添加新的。步骤：

```c
// 1. 定义工具 + 处理函数
static emMCP_tool_t my_tool;
static void my_tool_handler(void *arg) {
    // 你的处理逻辑
    cJSON *root = (cJSON *)arg;
    // ...
    emMCP_ResponseValue(emMCP_CTRL_OK);
}

// 2. 在 StartDefaultTask 里注册
my_tool.name = "my_tool";
my_tool.description = "我的自定义工具";
my_tool.inputSchema.properties[0].name = "param1";
my_tool.inputSchema.properties[0].type = MCP_SERVER_TOOL_TYPE_NUMBER;
my_tool.setRequestHandler = my_tool_handler;
emMCP_AddToolToToolList(&my_tool);
```

### Q: 怎么改 AI 模组的波特率？

A: 在 `emMCP_config.h` 里没有直接的波特率配置，因为默认 115200 是 AI 模组的固定波特率。如果想改，需要同时改模组 firmware 和 MCU 这边的 USART2 配置。

---

## 10. 下一步可以玩什么？

### 🟢 入门级（不需要改代码）

- 把猫咪动画打开（改一个宏定义）
- 用串口助手手动发 JSON 测试 7 个工具
- 改 OLED 第 1 页显示的文案

### 🟡 进阶级（需要写一点代码）

- 加第 8 个 MCP 工具（比如控制风扇、窗帘）
- 修改雷达检测阈值（改 `AXK_RADAR_ABSENT_THRESHOLD`）
- 加一个新的 OLED 页面（比如显示系统运行时间）

### 🔴 硬核级

- 移植到其他 STM32 型号（F4、H7）
- 把 HXD039B2 替换成学习更多按键（风速、摆风）
- 加上 WiFi 直接联网（用 ESP32 代替 UART 透传）
- 接入 HomeAssistant 生态

---

## 📚 参考链接

| 资源 | 链接 |
|------|------|
| emMCP 框架 | https://github.com/Ai-Thinker-Open/emMCP |
| UART-MCP 协议文档 | https://docs.ai-thinker.com/uart_mcp/ |
| AiPi-PalChatV1 | https://docs.ai-thinker.com/aipi-palchatv1/ |
| STM32 HAL 文档 | https://www.st.com/en/embedded-software/stm32cube-mcu-mpu-packages.html |
| FreeRTOS 文档 | https://www.freertos.org/Documentation/RTOS_book.html |
| 本项目 Gitee | https://gitee.com/xemowo/emMCP-Home |

---

> 🐱 有问题提 Issue，欢迎 Star & PR！
