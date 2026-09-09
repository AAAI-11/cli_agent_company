# petlink-fw —— ESP32-P4 固件

PetLink v1 设备端。**v0.6.0 token 可视版**：串口链路（**921600**）+ YES/NO 实体键 +
MIPI-DSI 六态宠物 UI + 文本上屏 + 五态 PNG 素材槽 + 板载 ES8311 麦克风录音上行 +
**多会话气泡行**（state 帧可选 `bubbles` 字段：各会话状态色圆点、焦点加大加白环；
空闲态 NO 键循环切换焦点——F02R，协议 §3.4）+
**token 底行**（state 帧可选 `tokens` 字段：焦点会话 ctx/tot 常驻小字——F15，协议 §3.4）。
v0.3 里程碑（2026-09-05 真机闭环）：状态切换→上屏实测首显 max 75ms / 缓存命中
max 87ms，宪法预算 500ms。
驱动基底 = 乐鑫官方示例 + espressif 组件（Apache-2.0，宪法 L109）；屏幕参数走卖家
资料通道（register 值属硬件接口事实）。

**链路实情**：板上 USB-C 经 **CH343 桥接芯片 → P4 的 UART0**（macOS 枚举 vid=1a86 WCH），
P4 原生 USB-Serial-JTAG 在这块微雪板上没引出。控制台日志与 PetLink 帧同走 UART0@921600
（v0.4 升档：16kHz×16bit 语音 PCM = 32KB/s，115200 只有 ~11.5KB/s 净载）。

## 目录

```
esp32p4_firmware/
├── CMakeLists.txt          # 工程根
├── sdkconfig.defaults      # target=esp32p4，控制台/PetLink 走 UART0@921600（v1.3 芯片修正）
├── partitions.csv          # 自定义分区：factory 4MB + asset 2MB（v0.3 图片素材预留坑）
├── components/cjson/       # 上游 cJSON（MIT），覆盖 IDF 内置版，与宿主机测试同源
├── main/
│   ├── petlink.h/.c        # 协议核心：纯 C、无 IDF 依赖（宿主单测；v0.4 增 audio 帧 + b64）
│   ├── mic.h/.c            # ES8311 codec 录音任务：esp_codec_dev → PSRAM 缓冲 → audio 帧上行（v0.4）
│   ├── main.c              # IDF 接线：UART0 收发（TX 互斥锁）+ 按键 + 语音触发 + STAGE 分派
│   ├── disp_dsi.h/.c       # MIPI-DSI 点屏：WLK2802MIPI-15P 卖家参数（见文件头注释）
│   ├── pet_ui.h/.c         # LVGL 六态宠物：矢量小狗 + 动画 + 多会话气泡行 + token 底行（与 pet.js 同源）
│   ├── pet_assets.h/.c     # 五态 PNG 素材槽：FAT 挂载 + 预解码 + 矢量兜底（v0.3）
│   ├── ui_map.h/.c         # 状态→样式映射：纯 C 查表（宿主可测，颜色与 pet.js 同源）
│   ├── font_pet_32.c       # 中文字体子集（~500 字，重生成命令见文件头注释）
│   └── Kconfig.projbuild   # YES/NO GPIO、STAGE 旋钮、LDO/RST、麦引脚/录音时长可配
├── tools/                  # 素材工具：gen_test_pngs.py（纯 stdlib 造五态图）+ mkassets.sh（打包烧录）
└── test/                   # 宿主机单元测试 56 项（不装 ESP-IDF 也能跑）
```

## 一次性环境安装（macOS）

本机已就绪（2026-09-01 验证）：**ESP-IDF v6.1**（VS Code 扩展装的），
位于 `~/.espressif/v6.1/esp-idf`，激活：

```bash
. ~/.espressif/v6.1/esp-idf/export.sh
```

新机器从零装：VS Code 装扩展 **ESP-IDF** → `Cmd+Shift+P` →
`ESP-IDF: Configure ESP-IDF Extension` → 选 v6.1 → 默认安装。
装完若编译报缺 mbedtls/lwip 头文件，是子模块空壳（扩展安装中断的已知坑）：

```bash
cd ~/.espressif/v6.1/esp-idf
git submodule update --init --recursive --force
```

**四个坑（全踩过，别再踩）**：
1. **工程路径必须全 ASCII**——IDF 构建系统不支持中文路径
   （本仓库因此把固件目录命名为 `04_firmware` 而非中文）
2. **IDF 6.x 拆掉了 legacy `driver` 组件**——GPIO 用 `esp_driver_gpio`、
   UART 用 `esp_driver_uart`（见 main/CMakeLists.txt）
3. **IDF 6.1 默认 P4 最低芯片版本 v3.1**，本板是 v1.3——烧录时 esptool 报
   `requires chip revision [v3.1 - v3.99]`。sdkconfig.defaults 里两行修复：
   `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y`
4. **板上 USB-C 不是 P4 原生 USB**——是 CH343→UART0（见上"链路实情"）；
   daemon 开此串口必须 `dtr:false, rts:false`（断言会复位芯片甚至让 macOS 丢设备），
   且优先 `/dev/cu.*` 节点——daemon/src/links.js 已内置

## 构建 / 烧录 / 看日志

**烧录前必须让 daemon 松开串口**（它按 VID 自动认领会一直占着口，
esptool 直接开就是 `Resource busy`——已踩过）：

先让 daemon 松开串口（它按 VID 自动认领会一直占着口，esptool 直接开就是
`Resource busy`——已踩过）：

```bash
curl -X POST http://127.0.0.1:7331/usb/pause
```

然后烧录（首次需 `idf.py set-target esp32p4`）：

```bash
cd 04_firmware/esp32p4_firmware
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

烧完恢复扫描：

```bash
curl -X POST http://127.0.0.1:7331/usb/resume
```

> ⚠️ 注意：**zsh 交互模式不识别行内 `#` 注释**（bash 才认）——把带注释的命令
> 整行粘贴会把注释文字当参数执行。本文档所有命令块都不带行内注释，整块可粘。

daemon 没在跑就跳过 pause/resume 两步（或者干脆停掉再烧也行）。
退出 monitor：`Ctrl+]`。任何第三方烧录工具（官方恢复包、esptool 直用）同理。

## 接线（杜邦线阶段）

- **YES 键**：一脚接 GPIO21（默认，= 排针第 27 脚；`idf.py menuconfig → PetLink` 可改），一脚接 GND（第 28 脚在隔壁）。GPIO39 已废弃——那是 TF 卡 SDIO D0，不在排针上
- **NO 键**（v0.1.1）：一脚接 GPIO22（= 排针第 29 脚，就在 GND(28) 下面），一脚接 GND（与 YES 共用第 28 脚即可）
- 按键语义与虚拟设备一致：**YES 短按 <600ms = 放行，YES 长按 ≥600ms = 拒绝，NO 键按下即拒**
- 屏幕：WLK2802MIPI-15P（2.8″ 480×640 IPS，ST7701S）插板上 MIPI DSI 座——
  FFC 注意方向（触点朝向按板上丝印），插反不亮但不损坏

## 屏幕与 UI（v0.2）

**屏幕硬件事实**（卖家资料《WLK2802MIPI-15P V1》核验，全部固化在 `disp_dsi.c` 头注释）：

- FPC 仅 **1 对数据 lane**（无 D1）→ 总线配 1 lane @ 700Mbps；**无 RST 脚** → 软复位
- 背光 5 串 LED 直吃 VCC 3.3V，板上/模块上**均无驱动芯片**（I2C 实测 0x45 无应答）→ 常亮无代码
- Normally Black：面板收不到正确的 init+视频流就是纯黑（曾因 2-lane@1250+48MHz 错参数黑屏一天）
- 验证过的参数链：25MHz 像素钟、hsync 30/10/30、vsync 50/8/50、39 条 init 序列（卖家组件包）
- `LCD ID: FF FF FF` 是正常现象——这块屏 LP 模式不支持回读，组件只告警不中断
- **横屏（v0.2）**：面板物理扫描固定 480×640 竖 raster 不可改——UI 以 640×480 横屏
  画布布局，flush_cb 逐脏区**转置**成 packed 缓冲再上屏；模块物理**顺时针转 90°**
  安装，方向反了改 `pet_ui.c` 的 `PET_UI_ROT_CW=0` 重烧

**STAGE 旋钮**（`idf.py menuconfig → PetLink UI`，或改 `sdkconfig` 后重编）：

| 值 | 行为 | 用途 |
|---|---|---|
| 0 | 无屏，纯协议（v0.1 行为） | 一键回退保险 |
| 1 | 硬件彩条 | 点链路验证（已过） |
| 2 | LVGL 静态页 | 显示管线验证（已过） |
| 3 | 六态宠物 + 协议（**默认/正式**） | 日常 |

`PETLINK_UI_SELFTEST=y` 叠加在 STAGE=3 上：六态每 2 秒轮播（验收演示用，平时关）。

**中文字体再生成**（改标签/加符号时，命令与 `ui_map.h` 的 `PET_FONT_SYMBOLS` 逐字一致）：

```bash
npx --yes lv_font_conv --no-compress --no-prefilter --bpp 4 --size 32 \
  --font DroidSansFallback.ttf -r 0x20-0x7F \
  --symbols "思考中干活等你拍板完成了出错正在连接★！…" \
  --format lvgl --force-fast-kern-format -o main/font_pet_32.c
```

## PNG 素材槽（v0.3）

五态表情图走 FAT 资产分区（`partitions.csv` 的 `asset` 2MB @ 0x410000），
挂载在 `/asset`，LVGL 盘符 `A:`。**素材规格：352×352 PNG**（RGBA），
运行时 `lv_image_set_scale` 缩到 176px 上屏。

- **状态映射**：thinking/working/waiting_confirm/done/error 五张同名 PNG；
  CONNECTING 复用 thinking 图。素材缺失/解码失败的态自动回退矢量小狗（不报错）
- **开机预解码**：挂载后立刻把在位 PNG 全部解码沉淀进 LVGL 缓存——
  lodepng 单张解码 ~500ms 会阻塞渲染期，若发生在状态切换时就撞 500ms 预算
  （实测度量踩过：首显 520ms 贴死线）。预解码后实测 **首显 max 75ms / 缓存 max 87ms**
- **解码成立依赖 sdkconfig 三件套**（缺一则静默回退矢量，见 `pet_assets.c` 头注释）：
  `LV_USE_CLIB_MALLOC=y`（内置 TLSF 池 64KB 装不下 494KB 解码缓冲）、
  `LV_CACHE_DEF_SIZE` 按字节（3MB 容 5 张）、stat 走 VFS 绝对路径
- **缺字兜底**：text 上屏遇字库外字符显示 `?`，不截断不乱码

**素材更新流程**（测试图纯 stdlib 生成，正式素材替换 `tools/assets_png/` 下同名文件即可）：

```bash
python3 tools/gen_test_pngs.py
sh tools/mkassets.sh
```

`mkassets.sh` 自动完成：daemon 暂停 USB → fatfsgen 打包 → parttool 烧 `asset` 分区 →
恢复扫描。烧完按 RESET，开机日志应出现 `asset 挂载完成：PNG 6/6 张就位，预解码 5 张入缓存`。

## 语音输入（v0.4，F14）

**触发**：空闲态（非 `waiting_confirm`）**长按 YES ≥600ms 松开** → 板载麦录音
6s（屏幕显示"录音中"）→ `audio` 分块帧上行 → daemon whisper 转写 → 注入 tmux。
授权等待期间长按仍是**拒绝手势**，不会触发录音。

- 录音参数：16kHz / 16bit / 单声道（whisper 舒适区），6s ≈ 192KB PSRAM 缓冲，
  921600 波特率下上行约 3s
- 硬件（微雪 wiki「I2S Audio Demo」2026-09-08 核实）：板载**模拟麦走 ES8311
  codec**，I²C 0x18 @ SCL=GPIO8/SDA=GPIO7；P4 做 I2S master：MCLK=13/SCLK=12/
  LRCK=10/ASDOUT=11。引脚为板载固定布线，硬编码在 `mic.c`
- 音频栈：`espressif/esp_codec_dev`（esp-box/P4 EV 板官方栈）——open(16k/mono)
  自动重配 STD MONO+LEFT 槽，read 直接吐 mono PCM；增益 Kconfig
  `PETLINK_MIC_GAIN_DB`（默认 24dB，实测语音段 ~21% 满幅无削顶）
- daemon 侧准备（模型下载等）见 `daemon/README.md` 语音输入一节

## 端到端验证（对着 daemon）

1. Mac 上起 daemon：`cd daemon && npm start`
2. 插板子、烧录——daemon 日志应出现：
   ```
   UsbCdcLink 连接 /dev/cu.usbmodemXXX（Espressif）@921600
   [cdc#N] 握手完成 → p4-xxxxxx (fw=0.6.0 hw=p4-board caps=button,mic)
   ```
3. 浏览器开 http://localhost:7331/device —— 虚拟设备与实体板**同步**显示同一状态
   （daemon 向所有已握手设备广播，一实一虚同源）
4. 让任意终端里的 Claude Code 干活，按板上 YES → daemon 日志 `已消费（permission 仲裁）`
5. 空闲态长按 YES 说句话 → daemon 日志 `[voice-in] ... 语音已注入 pane=...`

## 宿主机单元测试（不需要 ESP-IDF）

```bash
sh test/run.sh    # 56 项：编码/分帧/握手/防乒乓/五态/容错/判活/文本截断/audio 帧 b64 与分块/bubbles+tokens 解析 + ui_map 六态映射
```

## 已知事项

- **日志与协议共用 UART0**：ESP-IDF 日志和 PetLink 帧在同一条串口流上交错。
  daemon 对坏行静默丢弃，互不影响（首次交换若被日志撞坏，hello 5s 重试自愈——真机验证过）
- **心跳防乒乓**（协议 §3.3）：daemon 与固件都实现"应答不再回"，宿主机测试第 4 组是回归用例
- **cjson 覆盖内置组件**的编译提示属预期
- **语音音频不重传**（协议 §3.7 v1 语义）：分块丢失/乱序即弃段——增强通道尽力而为，
  按键授权主链路（button/permission）与此完全隔离

## 路线

- [x] v0.1 USB 链路 + YES 键（2026-09-02 真机打通）
- [x] v0.1.1 独立 NO 键 GPIO22（按下即拒；动机：长按 600ms 在演示现场太难按准）
- [x] v0.2 MIPI-DSI 点屏 + LVGL 六态宠物 UI（2026-09-04 真机闭环，
      状态→上屏 max 30ms / 预算 500ms；素材与虚拟设备同源，图片槽留 v0.3）
- [x] v0.3 文本上屏（~500 字库 + 缺字 `?` 兜底）、五态 PNG 素材槽（2026-09-05
      真机闭环，首显 max 75ms / 缓存 max 87ms；**亮度除名**——模块无背光驱动芯片，
      常亮无软件控制点，需求文档 v3.0 已删）
- [x] v0.4 语音输入：板载麦 + audio 分块帧 + UART 921600（2026-09-07，
      协议 §3.7；录音→转写→注入全链路 E2E 见根目录《功能与排障手册.md》§13）
- [x] v0.4.2 麦克风正路：v0.4.0/0.4.1 误信网上资料走 PDM @ GPIO23/24（两槽皆
      精确零——那两脚上没有麦），据微雪 wiki 改走 **ES8311 codec + I2S STD +
      esp_codec_dev**（2026-09-08 真机转写逐字命中；细节见《功能与排障手册.md》§13）
- [x] v0.5.0 多会话气泡（F02R，2026-09-09）：state 帧可选 `bubbles` 数组 →
      顶栏气泡行；daemon 焦点管理（waiting_confirm 抢占 / NO 键空闲切换），
      虚拟设备同款渲染——一实一虚同源
- [x] v0.6.0 token 底行（F15，2026-09-09）：state 帧可选 `tokens` 字段 →
      底部常驻 `ctx 45.1k / tot 1.2M` 小字（焦点会话口径，daemon 增量解析
      transcript usage；帧未带/非法则隐藏——旧 daemon 行为不变）
