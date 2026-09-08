# oled_board_test —— ESP32-P4-WIFI6 主控板体检程序

**目的**：在 MIPI 屏点亮、结构设计动工之前，先用一块 SSD1306 OLED（F01 需求变式）
把整板关键通路全部过一遍——**板子有问题在这一步暴露，别带到设计后面**。

数据通路与正式固件 `esp32p4_firmware` 完全一致（复用同一份 `petlink.c` / cJSON）：
daemon 广播的**真实 Claude 状态**（思考中/执行中/等待授权/完成/出错）直接上屏；
不接电脑时 DEMO 轮播五态兜底。

## 一次烧录测什么

| # | 通路 | 怎么算过 |
|---|------|----------|
| 1 | 供电/CPU/Flash/PSRAM | 启动日志 + HW INFO 屏（FLASH 32M / PSRAM 32M） |
| 2 | USB-Serial-JTAG | 能烧录、monitor 见日志、daemon 能握手 |
| 3 | I2C 总线 | 自检扫描屏列出 `3C OLED` + `18 ES8311`（板载，不依赖你的接线） |
| 4 | OLED 面板 | 全亮/棋盘两屏目检无坏点、断码、残影 |
| 5 | PetLink 协议链路 | hello/心跳/state 全走真协议（右上角 LIVE） |
| 6 | YES 实体键（可选） | 短按切态/放行，长按暂停 DEMO/拒绝 |

## 接线（杜邦线，OLED 四线 I2C）

```
OLED 模块        微雪 ESP32-P4-WIFI6 40PIN 排针（按物理脚号找）
  VCC    ──────  3V3     第 25 脚
  GND    ──────  GND     第 18 脚（与 SDA 相邻）
  SDA    ──────  GPIO7   第 17 脚，丝印 SDA/GPIO7（板默认 I2C，板载上拉）
  SCL    ──────  GPIO8   第 16 脚，丝印 SCL/GPIO8

YES 键（可选）：一脚 GPIO21（第 27 脚），一脚 GND（第 28 脚，就在隔壁）
```

> 排针第 1 脚在靠 USB Type-C 一端（板角丝印 `1`）；奇偶两列各 20 脚。
> 注意 **GPIO 编号 ≠ 排针脚号**：物理第 7/8 脚是 GPIO28/GND，跟 I2C 无关。
> GPIO39 曾被误用作 YES 键——它是 TF 卡 SDIO D0，根本不在排针上。

地址/引脚/翻转全在 `idf.py menuconfig → OLED 板测`：
OLED 不亮先看自检的 I2C 扫描——地址不是 0x3C 就改成扫出来的（少数模块 0x3D）。

## 上电后会发生什么（自检时序）

```
开机闪屏(1.5s) → 全亮(0.7s) → 棋盘(0.7s) → 清屏 → I2C 扫描(2s) → 硬件信息(2s)
→ 状态主屏：右上角 DEMO，五态每 2.8s 轮播（LINK.. THINKING WORKING CONFIRM? DONE ERROR）
→ daemon 上线握手后右上角变 LIVE，切真实 Claude 状态
```

OLED 没接好不卡序列：日志照走（USB 侧可完整判读），主循环每 2s 重试挂屏，接线修好即恢复。

## 烧录

环境没装先看 `esp32p4_firmware/README.md`（ESP-IDF ≥ v5.5）。
**daemon 在跑必须先让它松开串口**（否则 esptool `Resource busy`）：

```bash
curl -X POST http://127.0.0.1:7331/usb/pause     # daemon 不在跑就跳过
cd 04_firmware/oled_board_test
idf.py set-target esp32p4                        # 首次
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
curl -X POST http://127.0.0.1:7331/usb/resume    # 烧完恢复
```

## 判读表（症状 → 结论）

| 症状 | 结论 |
|------|------|
| USB 不枚举 / 无任何日志 | 供电、USB 线（换数据线）、晶振；按住 BOOT 再 RST 进 download 试烧 |
| 日志在跑，屏幕始终全黑 | 看 I2C 扫描行：无 0x3C → OLED 接线/地址；有 0x3C 仍黑 → 模块 VCC/对比度（坏了） |
| 扫描连 0x18 都没有 | **板子或总线问题**（ES8311 板载，与你的接线无关）→ 换 SDA/SCL 确认，再不行板子有问题 |
| 全亮/棋盘屏有坏点、亮点 | 面板瑕疵，记录位置（测试留档） |
| DEMO 轮播正常，daemon 连不上 | 板子没问题；查 daemon 侧（串口占用/VID 识别），看 daemon 日志 |
| LIVE 了但状态不动 | 板子没问题；daemon hooks 侧（`~/.claude/settings.json`）或压根没 Claude 在干活 |
| 图标/文字残缺 | 对照本文下方渲染图，如果 ASCII 版一致则是设计如此；不一致 → I2C 信号质量（降时钟到 100k 试） |

## 与 daemon 联调（真实 Claude 状态上屏）

```bash
cd daemon && npm start            # 或 node src/server.js --fake （无 Claude 时的假状态流）
```

让任意终端里的 Claude Code 干活（hooks 已装），板上状态随之变化——
这就是 F01 在实体硬件上的最小闭环。**CONFIRM? 状态时短按 YES = 授权、长按 = 暂停 DEMO**。

不启动 daemon 也能手动喂状态（monitor 里直接粘贴回车）：

```
{"ver":1,"type":"state","session":"man","agent":"claude-code","status":"thinking","text":"HI"}
```

## 宿主机渲染测试（不需要 ESP-IDF / 不需要板子）

```bash
sh test/run.sh    # 23 项断言 + 全部屏幕 ASCII 渲染目检（字库逐字过目）
```

改字库/布局/新加屏幕后必须重跑重看——`oled_ui.c` 是纯 C，就是为的这个。

## 目录

```
oled_board_test/
├── CMakeLists.txt          # 复用 esp32p4_firmware/components（cjson）
├── sdkconfig.defaults      # target=esp32p4，控制台走 USB-Serial-JTAG
├── main/
│   ├── main.c              # 自检序列 + petlink 绑定 + DEMO 兜底 + 主循环
│   ├── oled_ui.h/.c        # 渲染层：纯 C，自绘 5×7 字库 + 五状态布局（可宿主机单测）
│   ├── ssd1306.h/.c        # SSD1306 I2C 驱动（ESP-IDF 专用）
│   └── Kconfig.projbuild   # 引脚/地址/时钟/翻转/YES 键
└── test/                   # 宿主机渲染测试
```

## 已知边界（测试程序，不是产品）

- **中文 text 显示为 `?`**——5×7 ASCII 字库；中文是 MIPI+LVGL（v0.2）的事
- 协议 `text` 超 21 字符滚动显示；session 截前 4 位
- 日志与协议帧共用一条 USB CDC（与正式固件同策略，daemon 对坏行静默丢弃）
- 设备号 `p4t-<MAC>`（t=test），与正式固件 `p4-` 区分，两者同插不冲突
