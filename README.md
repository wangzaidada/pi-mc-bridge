# pi-mc-bridge — Minecraft `@pi` 桥接程序

把 **原版 Minecraft Java 服务端**（无需任何 mod/插件）的聊天消息桥接到
[Pi Coding Agent](https://www.npmjs.com/package/@mariozechner/pi-coding-agent)。
玩家在游戏里发 `@pi <问题>`，程序 tail 服务端日志发现这条消息，调用 `pi` CLI
取得回答，再通过 RCON 用 `tellraw` 把回答发回给该玩家。

```
                 tail -f                     fork/exec pi --mode json
server logs  ─────────────►  pi-mc-bridge  ─────────────────────────►  pi CLI
   ▲                              │                                        │
   │                              │ NDJSON(stdout) 取最后一条 assistant 文本
   │                              ▼
   └──── RCON  tellraw <player> {"text":"..."} ◄──────────────────────────┘
```

数据流：`logwatcher`（轮询日志增量）→ `parser`（识别 `@pi`）→ `queue`（有界任务队列）
→ `worker`（限流 + 调用 pi + 分段发送）→ `rcon_client`（tellraw 回游戏）。

---

## 1. 依赖

| 组件 | 版本要求 | 说明 |
|------|----------|------|
| C 编译器 | C11（实测 `cc 15.2.0` / GCC 13+ 均可） | 只用 POSIX + pthread，无第三方库 |
| make | 任意 | 构建 |
| Pi Coding Agent | **实测 `pi 0.73.0`** | 提供 `--mode json` NDJSON 流式输出 |
| Minecraft 服务端 | 实测 **1.20.4** 原版 `server.jar` | 需开启 `enable-rcon` |
| Java | 17+（原版 1.20.4 要求） | 仅服务端需要 |
| RCON 客户端（可选） | `mcrcon` 等 | 手工联调时用 |

`pi` 需已配置可用的 provider/model。本项目联调用的是自定义扩展
`~/.pi/agent/extensions/mc-gateway.ts` 注册的 `mc-gw` provider，凭据通过
环境变量 `MC_GW_API_KEY` 注入（见 §5）。

---

## 2. 编译与测试

```bash
make            # 生成 ./pi-mc-bridge
make test       # 编译并运行 tests/run-tests（78 项断言）
make clean
```

单元测试覆盖：UTF-8 分段 / JSON 转义、聊天行解析（含 `[Not Secure]` 标签、CRLF、
大小写、含尖括号的消息）、pi NDJSON 解析（thinking+text 混排、多条 `message_end`、
错误 `stopReason`、噪声行）、RCON 打包/解包字节级断言与错误密码、任务队列多线程行为。

---

## 3. 配置

优先级：**命令行 > 环境变量 > 默认值**。`--rcon-pass` 无默认值，必填。

| 命令行 | 环境变量 | 默认值 | 说明 |
|--------|----------|--------|------|
| `--rcon-host HOST` | `RCON_HOST` | `127.0.0.1` | RCON 主机 |
| `--rcon-port PORT` | `RCON_PORT` | `25575` | RCON 端口 |
| `--rcon-pass PASS` | `RCON_PASSWORD` | *(必填)* | RCON 密码，须与 `server.properties` 一致 |
| `--log-file PATH` | `LOG_FILE` | `logs/latest.log` | 要 tail 的服务端日志 |
| `--prefix TEXT` | `CHAT_PREFIX` | `@pi` | 触发词，大小写不敏感，须为名字后第一个 token |
| `--reply-prefix TEXT` | `REPLY_PREFIX` | `[AI] ` | 回复前缀 |
| `--pi-cmd PATH` | `PI_CMD` | `pi` | pi 可执行文件路径 |
| `--pi-model MODEL` | `PI_MODEL` | *(pi 默认)* | 形如 `provider/model` |
| `--timeout SECONDS` | `PI_TIMEOUT` | `60` | 单次 pi 调用超时，超时后 `SIGTERM`→`SIGKILL` |
| `--cooldown SECONDS` | `COOLDOWN` | `10` | 每玩家冷却（限流窗口） |
| `--max-len CHARS` | `MAX_LEN` | `200` | 单条回复最大**字符**数（按码点，非字节） |
| `--poll-ms MS` | `POLL_MS` | `500` | 日志轮询间隔 |
| `--queue-max N` | `QUEUE_MAX` | `64` | 任务队列容量，满则回「当前请求过多」 |
| `--allow LIST` | `ALLOW_PLAYERS` | *(全部)* | 白名单，逗号分隔玩家名 |
| `--log-level LEVEL` | `LOG_LEVEL` | `info` | `debug`\|`info`\|`warn`\|`error` |
| `-h`, `--help` | — | — | 打印用法 |

---

## 4. 运行示例

```bash
# 1) 起原版服务端（见 /mnt/data/mc-server/start.sh），确认 RCON 已开
# 2) 启动桥接（前置：pi 的 provider 凭据已 export 到当前环境）
export MC_GW_API_KEY=...          # 本项目用 .env 保存，勿提交
./pi-mc-bridge \
  --rcon-host 127.0.0.1 --rcon-port 25575 --rcon-pass your-rcon-password \
  --log-file /mnt/data/mc-server/logs/latest.log \
  --pi-cmd /home/duan/.npm-global/bin/pi \
  --pi-model mc-gw/deepseek/deepseek-v4.1-flash \
  --cooldown 3 --max-len 200 --log-level debug
# 3) 游戏里（或测试 bot）发：@pi 你好
```

> ⚠️ **pi 的凭据必须出现在桥接进程的环境里**。桥接只 `execvp("pi")` 并继承自身环境，
> 不会去读 `.env`。本地联调脚本 `/tmp/run-bridge.sh` 的写法是
> `set -a; . ./.env; set +a; exec ./pi-mc-bridge ...`。systemd 下用
> `EnvironmentFile=`（见 §6）。

---

## 5. 本地联调（实测记录）

环境：Ubuntu / Linux 7.0.0，Java 17.0.20，1.20.4 原版服务端 + RCON，
`pi 0.73.0`（`mc-gw` provider），测试玩家用 [mineflayer] 无头 bot
（`/tmp/mcbot/`，`node bot.js <玩家名> "<消息>" <监听秒数>`）。

### 5.1 步骤

1. **起服务端**：`cd /mnt/data/mc-server && ./start.sh`
   `server.properties` 关键项：
   ```
   enable-rcon=true
   rcon.port=25575
   rcon.password=your-rcon-password
   online-mode=false
   enforce-secure-profile=false
   ```
   等日志出现 `Done (x.xxxs)!` 即就绪。
2. **起桥接**：加载 `.env` 后按 §4 启动，日志出现
   `rcon: connected` 与 `watching .../latest.log` 即正常。
3. **发消息**：`node bot.js TestBot "@pi 你好" 30`。
4. **看结果**：bot 侧应打印 `CHAT: [AI] 你好！…`；桥接侧依次打印
   `request from …` / `asking pi for …` / `replied to …`。

### 5.2 结论（逐条对照设计文档）

| 能力 | 编号 | 实测结果 |
|------|------|----------|
| 游戏内 `@pi 你好` → 桥接 → pi → 回游戏 | F1–F5 | ✅ 端到端跑通，回复 1–3 s 内送达 |
| 解析 `[Not Secure]` 标签 | F2 | ✅ 真实日志形如 `[..] [Server thread/INFO]: [Not Secure] <TestBot> @pi 你好`，解析器先跳过 `]:` 后的方括号标签再取 `<名字>` |
| 日志轮转自恢复 | F1 | ✅ 重启服务端（`latest.log` 被 gzip 归档、新文件新 inode），桥接打印 `log rotation detected … reopening` 并继续工作 |
| RCON 断线自恢复 | F5 | ✅ 服务端重启后 `WARN rcon: connection lost, reconnecting and retrying once`，随后回复照常送达 |
| 限流 | F6 | ✅ 同一玩家 200 ms 内连发 4 条，仅第 1 条被处理，另 3 条 `rate-limited`（含队列里排队的那条也在出队时被拦） |
| 超长回复分段 | F7 | ✅ `--max-len 24` 时一段 269 字符回复被切成 5 段，边界按 UTF-8 码点切分，多字节字符未被截断 |
| 队列满提示 | F3 | ✅ 满时回 `当前请求过多，请稍后再试。` |

### 5.3 排障

- **pi 返回 `401 Invalid token`** → `MC_GW_API_KEY` 没进桥接进程的环境；见 §4 警告。
- **`pi: No such file or directory`（spawn 失败）** → `--pi-cmd` 未用绝对路径，或
  非登录 shell 的 PATH 里没有 `~/.npm-global/bin`。
- **游戏内看不到回复** → 先看桥接日志有没有 `replied to`；有则查 RCON 密码/端口，
  用 `--log-level debug` 会打印每条 `tellraw` 原文。
- **桥接收不到消息** → 确认 `--log-file` 指向真正在写的文件（不是 `*.log.gz`）；
  玩家名/触发词之间有额外前缀时，触发词必须是**名字后第一个 token**。

---

## 6. systemd 单元

生产环境建议用 systemd 常驻，并用 `EnvironmentFile` 注入 pi 凭据。

`/etc/systemd/system/pi-mc-bridge.service`：

```ini
[Unit]
Description=Minecraft @pi bridge (pi-mc-bridge)
After=network-online.target minecraft.service
Wants=network-online.target

[Service]
Type=simple
User=mc
WorkingDirectory=/opt/pi-mc-bridge
# pi 的 provider 凭据（600 权限，勿入库）
EnvironmentFile=/etc/pi-mc-bridge.env
ExecStart=/opt/pi-mc-bridge/pi-mc-bridge \
  --rcon-host 127.0.0.1 --rcon-port 25575 --rcon-pass ${RCON_PASSWORD} \
  --log-file /srv/minecraft/logs/latest.log \
  --pi-cmd /usr/local/bin/pi --pi-model mc-gw/deepseek/deepseek-v4.1-flash \
  --cooldown 5 --max-len 200
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

启用：`systemctl daemon-reload && systemctl enable --now pi-mc-bridge`。

---

## 7. 已知边界

- **单 worker 串行**：同一时刻只处理一个 pi 调用，其余在队列里排队（容量
  `--queue-max`）。这是刻意的：pi 调用较重，且回复顺序与提问顺序一致。
- **限流在「出队时」而非「入队时」判定**：连发时后来者会在拿到 worker 时才被拦，
  表现为「入队若干、执行一条」。想更早拦截可调小 `--queue-max`。
- **日志按轮询增量读取**：启动时定位到文件末尾，**不重放历史**；`poll-ms` 决定延迟。
- **回复不做换行处理**：`tellraw` 的 `{"text":...}` 会把换行原样输出，多行长回答靠
  `--max-len` 分段，不保证按语义切段。
- **仅支持原版日志格式**：解析依赖 `[时间] [线程/级别]: <玩家名> <消息>`；插件改动
  日志格式后可能需微调 `parser.c`。

---

## 8. 更新计划

> 2026-10-13 代码审查（WSL 构建零告警、78 项单测全过）后整理的待办。
> 功能性变更建议先走 OpenSpec 流程（`/opsx:propose`）登记再实现。

### 8.1 修复与加固（低风险，不改变对外行为）

- **`main.c` 连接超时死条件**：`src/main.c:118` 的
  `pi_timeout_ms < 5000 ? 5000 : 5000` 恒为 5000，应按意图写成固定值或
  「下限 5 s」。
- **限流表删除破坏探测链**：出现第 129 名不同玩家触发驱逐时，`rate_evict_one`
  会在线性探测链中间留下空槽，链上后续玩家的记录从此不可查找（冷却被绕过、
  产生重复条目）。修复：驱逐后回填（rehash 后续簇），见 `src/worker.c`。
- **`drain_trailing` 固定开销**：每条命令都 poll 30 ms 等尾包；仅当上一包正好
  4096 字节（才可能有空尾包）时才需要探测，可省掉每次 tellraw 的固定延迟，
  见 `src/rcon_client.c`。
- **配置解析健壮性**：`atoi` 无校验（`COOLDOWN` 等非法值静默变 0），改用
  `strtol` + 报错；环境变量 `LOG_LEVEL` 非法值目前静默忽略，与 CLI 行为统一。

### 8.2 体验与健壮性改进（需设计取舍）

- **优雅关停**：Ctrl-C 目前不打断进行中的 pi 调用（最长 60 s），且会排空整个
  队列后才退出；考虑收到信号即丢弃队列、终止子进程。
- **日志轮转空窗**：检测到轮转后直接 seek 新文件末尾，轮转与检测之间
  （≤1 个轮询周期）写入的行会丢；可改为先读完旧文件再切换。
- **限流反馈**：被冷却拦下的请求目前只记日志、玩家无感知；可回一条带剩余
  冷却时间的提示。
- **语义分段**：回复按 `--max-len` 硬切字符；可优先在换行/句读处切分，减少
  句子被拦腰截断。

### 8.3 功能规划

- **AI 执行游戏命令**（高风险，需完整设计）：让 AI 回复中带标记的行
  （如 `!cmd /give …`）经 RCON 执行。前置设计：命令白名单、玩家权限分级、
  审计日志、防提示词注入（AI 输出不可信）。⚠️ RCON 即服务端控制台权限——
  放行等于把 op 能力交给任何能触发 `@pi` 的玩家。
- **回复富文本**：支持颜色/悬停文本等 tellraw 组件（目前只有纯文本
  `{"text":…}`）。

### 8.4 明确不做 / 远期（见 design.md Non-Goals）

- Windows / macOS 支持（当前仅 Linux/POSIX）；
- 多工作线程（pi 调用重且需保证回复顺序，等吞吐成为瓶颈再说）；
- HTTP API 替代 pi CLI；多轮对话上下文持久化。

[mineflayer]: https://github.com/PrismarineJS/mineflayer
