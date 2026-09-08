# PetLink —— CLI Agent 桌面伴侣设备

把 Claude Code 的状态变成桌上宠物的表情，把它要权限时的弹窗变成你手边的实体按键。

**这是什么**：一块 ESP32-P4 开发板 + 2.8″ MIPI 屏，跑一只六态宠物（思考中/干活/等你拍板/完成/出错/连接中）；
板上的 YES / NO 实体键直接裁决 Claude Code 的权限请求——离开键盘也不错过拍板时机。
PC 侧一个 Node daemon 归一化 Claude Code hooks 事件，经自研 PetLink v1 协议（NDJSON over USB 串口）
驱动实体板与浏览器虚拟设备，一实一虚同源。

**口径**：宪法文档为 [`01_需求与方案/需求说明文档.md`](01_需求与方案/需求说明文档.md)（v3.0）。
范围冻结于 F01 状态可视化 / F03 实体键授权仲裁 / F05 自研 NDJSON 协议 / F06 Node daemon；
语音、组件化 GUI、MQTT、自绘 PCB 等开题阶段设想已全部裁剪（当时的技术路线残留在根目录旧开题报告中，仅作存档）。

## 快速上手

```bash
cd daemon && npm install && npm start    # 起 daemon（macOS 需已装 Node 18+）
```

浏览器打开 http://localhost:7331/device —— 虚拟设备宠物随假状态流变脸。
插上烧好固件的实体板，daemon 自动识别（按 USB VID 扫描，插上即连）。
固件构建/烧录见 [`04_firmware/esp32p4_firmware/README.md`](04_firmware/esp32p4_firmware/README.md)。

## 仓库结构

**代码（本体的三件套）**：

| 目录 | 内容 | 测试 |
|---|---|---|
| `daemon/` | Node daemon：hooks 归一化 + USB/WS/TCP 链路 + permission 仲裁 + tmux 注入 | `npm run smoke` 15 项 |
| `virtual_device/` | 浏览器虚拟设备（纯静态，与实体板同协议同状态机） | daemon smoke 内联覆盖 |
| `04_firmware/esp32p4_firmware/` | ESP32-P4 固件（ESP-IDF v6.1 + LVGL）：PetLink 协议、六态 UI、PNG 素材槽 | `sh test/run.sh` 38 项宿主单测 |
| `docs/protocol.md` | PetLink v1 协议规范（帧格式/握手/心跳/判活/防乒乓） | 与固件单测同源 |
| `04_firmware/oled_board_test/` | 早期 OLED 板验证程序（历史存档） | — |

**文档目录（编号制）**：

| 目录 | 放什么 |
|---|---|
| `00_项目文档/` | 立项与过程文档（当前主要内容见根目录旧开题报告与 `01_`） |
| `01_需求与方案/` | **宪法：需求说明文档 v3.0**（范围冻结、宪法预算、验收口径） |
| `02_系统设计/` | 架构补充文档（主干已在宪法与 `docs/protocol.md`） |
| `03_硬件设计/` | 数据手册、选型记录（现用微雪成品板，无自绘 PCB） |
| `06_结构设计/` | 外壳 3D 模型（后期） |
| `07_测试验证/` | 硬件/固件/整机测试记录（日常排障见固件 `DEBUG.md`） |
| `08_生产与交付/` | 烧录装配指南、演示与答辩材料（冲刺期填充） |

`05_PC端服务/` 为开题阶段多服务架构的空壳，现由单体 `daemon/` 取代——公开前清理。

## 硬件事实（与开题设想的主要差异）

- 主控**微雪 ESP32-P4-WIFI6 成品板**（非自绘 ESP32-S3 板）：USB-C 经 CH343 桥接 P4 UART0；
  PSRAM 32MB；YES=GPIO21、NO=GPIO22 对 GND 按键
- 屏**淘宝 WLK2802MIPI-15P**（2.8″ 480×640 IPS ST7701S，MIPI-DSI 1 lane）非 SPI ST7789
- 通信 **USB 串口直连**（PetLink v1 NDJSON@115200），无 Wi-Fi/MQTT/云端环节——
  单根 USB 线既是电源又是信道，演示零网络依赖

## 里程碑

- [x] v0.1 USB 链路 + YES 键授权闭环（2026-09-02 真机）
- [x] v0.1.1 独立 NO 键（按下即拒）
- [x] v0.2 MIPI-DSI 点屏 + LVGL 六态宠物（2026-09-04 真机，上屏 max 30ms / 预算 500ms）
- [x] v0.3 文本上屏 + 五态 PNG 素材槽（2026-09-05 真机，首显 max 75ms / 缓存 max 87ms；
      实体键仲裁矩阵四格全验：YES 短按 allow / NO deny / YES 长按 deny / 超时回落弹窗）
- [ ] 冲刺：演示材料、答辩准备、**提交前仓库公开**（创新叙事 = 真开源补完）

## 协作与工具

- 分支模型与提交规范：[CONTRIBUTING.md](CONTRIBUTING.md)
- Git/GitHub 入门（写给队友）：[GitHub使用手册.md](GitHub使用手册.md)
