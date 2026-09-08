# 协作规范（CONTRIBUTING）

> 本仓库为团队协作开发使用。**核心原则：`main` 分支永远是稳定可交付版本，任何改动都不直接 push 到 `main`，一律走「功能分支 + Pull Request」。**

## 一、分支模型

| 分支 | 用途 | 谁能改 |
|---|---|---|
| `main` | 稳定主干，随时可交付 | 只有合并 PR 才能更新 |
| `feature/<名字>` | 各人开发自己的功能 | 本人 |

**分工分支建议**（对应仓库目录）：

| 分支 | 负责内容 |
|---|---|
| `feature/firmware` | `04_firmware/esp32p4_firmware`（ESP32-P4 固件） |
| `feature/daemon` | `daemon/` + `virtual_device/`（Node daemon 与虚拟设备） |
| `feature/docs` | 文档、README、需求文档、`00_项目文档` 等 |

## 二、第一次上手

```bash
# 1. 克隆仓库
git clone https://github.com/AAAI-11/cli_agent_company.git
cd cli_agent_company

# 2. 配置自己的身份（重要：否则提交会显示成别人的名字）
git config user.name "你的名字"
git config user.email "你的邮箱@example.com"
```

## 三、每次开发的标准流程

```bash
# 1. 开工前，先同步最新代码
git checkout main
git pull

# 2. 从 main 开自己的功能分支（首次；若分支已存在则直接 checkout）
git checkout -b feature/firmware

# 3. 改代码/文件……

# 4. 查看改动、提交
git status
git add .
git commit -m "feat: 六态宠物 UI 上屏"

# 5. 推送到自己的分支（首次加 -u）
git push -u origin feature/firmware
# 之后推送直接： git push
```

6. 打开 GitHub 仓库页 → 顶部会有黄色提示条 → 点 **Compare & pull request**（或切到自己的分支点 "New pull request"）→ 填说明 → 创建 PR。
7. 请至少一位队友 **review** 并同意，然后合并 PR。
8. 合并后，其他人更新本地：

```bash
git checkout main
git pull
```

## 四、Commit 信息规范（约定式提交）

用「类型: 说明」的格式，一眼看懂每条改动是干嘛的：

| 前缀 | 含义 | 示例 |
|---|---|---|
| `feat:` | 新功能 | `feat: 六态宠物 UI 上屏` |
| `fix:` | 修 bug | `fix: 修复 macOS 下误开 tty. 串口导致 TX 静默丢弃` |
| `docs:` | 文档 | `docs: 补充 PetLink 协议判活语义` |
| `chore:` | 杂项/配置 | `chore: 更新 .gitignore` |
| `refactor:` | 重构（不改功能） | `refactor: 拆分 hooks 归一化模块` |
| `test:` | 测试 | `test: 增加心跳防乒乓回归用例` |

## 五、常见问题

**Q：忘记开分支，直接在 main 上改了？**
把改动挪到新分支再提交：

```bash
git checkout -b feature/xxx   # 从当前 main 位置开新分支，改动会跟着过来
git add . && git commit -m "..."
git push -u origin feature/xxx
```

**Q：push 时提示冲突 / rejected？**
说明队友先更新了。先同步再推：

```bash
git checkout main && git pull
git checkout feature/xxx && git merge main   # 把最新 main 合进自己分支
# 解决冲突（如有）后：
git add . && git commit -m "merge: 同步 main"
git push
```

**Q：本地改了但想放弃？**

```bash
git checkout -- <文件名>     # 丢弃单个文件的改动
```

**Q：分支合并完没用了，想删？**

```bash
git checkout main
git branch -d feature/xxx    # 删本地分支
git push origin --delete feature/xxx   # 删远程分支（可选）
```

## 六、目录结构速查

项目根目录 `README.md` 有完整说明。常用：

- `daemon/` —— Node daemon（hooks 归一化 + 设备链路 + permission 仲裁）
- `virtual_device/` —— 浏览器虚拟设备（纯静态）
- `04_firmware/esp32p4_firmware/` —— ESP32-P4 固件（ESP-IDF v6.1 + LVGL）
- `docs/protocol.md` —— PetLink v1 协议规范
- `01_需求与方案/` —— 宪法：需求说明文档 v3.0

**提交前自查**：固件改了就跑 `sh 04_firmware/esp32p4_firmware/test/run.sh`（38 项），
daemon 改了就跑 `cd daemon && npm run smoke`（15 项）——全绿再提 PR。
