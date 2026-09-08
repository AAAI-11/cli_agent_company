# GitHub 使用手册（小白速成 · 本项目专用）

> 目标：让你从零做到「克隆代码 → 改 → 提交 → 推到自己的分支 → 开 PR 等合并」。
> 仓库地址：**https://github.com/AAAI-11/cli_agent_company**（私有，需先被加入）
> 看完这篇照着做即可，命令都能直接复制。有不懂的术语见文末「名词速查」。

---

## 一、准备（只做一次）

### 1. 注册并加入仓库
1. 打开 https://github.com 注册账号（记好用户名和邮箱）。
2. 让**组长**在组织 `AAAI-11` 里把你加为仓库协作者，你的邮箱会收到一封邀请邮件 → 点 **Accept invitation**。
3. 确认能打开 https://github.com/AAAI-11/cli_agent_company （能看到文件列表就对了）。

### 2. 安装 Git
- **Windows**：去 https://git-scm.com 下载，一路「下一步」默认安装即可。装完在开始菜单找「Git Bash」打开。
- **macOS**：打开「终端」，输入 `xcode-select --install` 回车；或装过 Homebrew 的话 `brew install git`。
- **验证**（任意终端里）：
  ```bash
  git --version
  ```
  能打印版本号就 OK。

### 3. 配置身份（只做一次，否则提交会显示成别人）
```bash
git config --global user.name "你的名字"
git config --global user.email "你的GitHub邮箱"
```

### 4. 准备 token（⚠️ 小白最容易卡在这一步）
GitHub 从 2021 年起**不能用账号密码推送**，必须用 token：

1. 打开 https://github.com/settings/tokens
2. 点 **Generate new token → Generate new token (classic)**
3. Note 随便填（如 `my-pc`），Expiration 选 **30 days**
4. 勾选 **`repo`**（就这一个）
5. 点 **Generate token**，复制 `ghp_` 开头的字符串，**自己存好（只显示一次）**

> 之后命令行任何地方让你输 **Username / Password**：用户名 = 你的 GitHub 用户名，**密码 = 粘贴这个 token**（不是你的登录密码）。

---

## 二、把代码拉到本地（clone）

```bash
git clone https://github.com/AAAI-11/cli_agent_company.git
cd cli_agent_company
```

首次会让你输用户名 + token。成功后当前目录就是整个项目。

---

## 三、日常开发流程（核心 6 步，背下来）

每次开工到收工，按这个顺序：

```bash
# ① 开工前先同步最新代码（重要，否则容易和别人冲突）
git checkout main
git pull

# ② 开一个自己的分支（分支名按你负责的模块来）
git checkout -b feature/firmware        # 例子：固件；改成你自己的

# ③ 改代码 / 画图 / 写文档…… 正常保存文件

# ④ 看看自己改了啥
git status

# ⑤ 提交（把改动记录到本地）
git add .
git commit -m "feat: 完成了 MQTT 通信"   # 引号里写清楚你做了什么

# ⑥ 推送到 GitHub（首次加 -u，之后直接 git push 即可）
git push -u origin feature/firmware
```

**分支名建议**（对应你负责的部分）：

| 你负责 | 分支名 | 对应目录 |
|---|---|---|
| 硬件 | `feature/hardware` | `03_硬件设计/` |
| 固件 | `feature/firmware` | `04_firmware/` |
| PC 端服务 | `feature/host` | `05_PC端服务/` |
| 文档 | `feature/docs` | 各文档 |

---

## 四、开 Pull Request（PR）—— 让队友合并你的代码

push 成功后：

1. 打开仓库页面 https://github.com/AAAI-11/cli_agent_company
2. 顶部会出现**黄色提示条**：`feature/xxx had recent pushes` → 点 **Compare & pull request**
   （没看到提示条的话，点页面上的 **Pull requests** → 绿色 **New pull request** → base 选 `main`、compare 选你的分支 → **Create pull request**）
3. 填标题（写清楚这波干了啥）和说明 → 点 **Create pull request**
4. 叫队友来 **review**，点 **Merge pull request → Confirm merge** 合并
5. 合并完，切回 main 同步：

```bash
git checkout main
git pull
```

> 没有开 PR、直接 push 到 main 是**禁止**的，请一定走分支 + PR（详见仓库里的 `CONTRIBUTING.md`）。

---

## 五、报错急救（高频坑）

| 报错 / 现象 | 原因 | 怎么解决 |
|---|---|---|
| `fatal: Authentication failed` / `403` | token 错或过期，或电脑记住了旧密码 | 重新生成 token；Windows 在「凭据管理器」删掉 github 记录、macOS 运行 `git credential-osxkeychain erase` 后重试 |
| `rejected / non-fast-forward` | 别人先更新了 main | 先 `git pull`（或 `git checkout main && git pull`），再 push |
| `CONFLICT (content)` / 出现 `<<<<<<<` `=======` `>>>>>>>` | 你和别人改了同一处 | 手动编辑文件：保留要的内容、删掉这三行标记 → `git add .` → `git commit -m "fix: 解决冲突"` → `git push` |
| `Couldn't connect to server` / 卡住超时 | 网络访问 GitHub 不稳（国内常见） | 多试几次、换网络、挂代理；不是配置问题 |
| 改错了想撤回单个文件 | —— | `git checkout -- 文件名`（丢弃这个文件的未提交改动） |
| 提交信息写错了想改 | —— | `git commit --amend -m "新信息"` |
| 本地突然很乱想整体回到远程版本 | —— | `git checkout main && git fetch && git reset --hard origin/main`（⚠️ 会丢弃本地未推送改动，慎用） |

---

## 六、名词速查（一句话版）

| 词 | 一句话解释 |
|---|---|
| 仓库 repository | 放项目文件+历史的地方（就是 `AAAI-11/cli_agent_company`） |
| 分支 branch | 一条独立的"开发线"，互不干扰（`main` 是主线） |
| clone | 把远程仓库下载到本地 |
| commit | 一次"存档"，记录这次改了什么 |
| push | 把本地 commit 上传到 GitHub |
| pull | 把 GitHub 上的新改动拉到本地 |
| PR (Pull Request) | 请求把你的分支合并进 main |
| merge | 合并（把分支合到一起） |
| fork | 复制一份仓库到自己名下（本项目用不到，直接分支即可） |

---

## 七、Commit 信息规范（简版）

格式：`类型: 说明`，类型用这几个：

- `feat:` 新功能　`fix:` 修 bug　`docs:` 文档　`chore:` 杂项　`refactor:` 重构　`test:` 测试

✅ 好例子：`feat: 增加按键外部中断`　`fix: 修复屏幕花屏`
❌ 坏例子：`update`、`改了一下`、`111`

---

## 八、如果实在不想用命令行

装 **GitHub Desktop**（https://desktop.github.com），图形界面点按钮就能 clone / 改分支 / commit / push / 开 PR，和上面命令一一对应。先图形化上手，命令慢慢补也行。
