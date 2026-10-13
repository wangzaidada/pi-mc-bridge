## Why

Minecraft Java 版服务端本身没有任何可编程入口给玩家调用外部 AI —— 要么装模组/插件，要么手动在控制台跑命令。本机是纯原版服务端场景，且 Pi Coding Agent（`pi` CLI）已在机器上可用。我们希望在**不引入任何模组或插件**的前提下，让玩家在游戏聊天里用 `@pi` 前缀直接向 AI 提问并把回答显示回聊天栏。

现在做的理由：`pi` 的机器可读输出模式（`--mode json`，NDJSON 事件流）已在 2026-10-13 实测确认可用，服务端 RCON 是原版自带协议，两个接口都稳定，桥接层可以写得很薄。

## What Changes

- 新增一个常驻 C 程序 `pi-mc-bridge`（Linux/POSIX，单进程 + 多线程），桥接「MC 服务端日志 → pi CLI → RCON tellraw」三段链路。
- **F1 日志监控**：以 500ms 轮询 `logs/latest.log`，从文件末尾开始；识别 inode 变化（日志轮转）与文件截断并正确恢复。
- **F2 消息解析**：从日志行 `<玩家名> @pi <问题>` 提取玩家名与问题，前缀大小写不敏感，玩家名限定 `[A-Za-z0-9_]`。
- **F3 任务队列**：线程安全阻塞 FIFO，主线程（读日志）与工作线程解耦，队列有上限，满时丢弃并提示。
- **F4 Pi 调用**：`fork`+`execvp` 调 `pi --mode json --no-session --no-tools --model <m> -p <prompt>`，`poll` 超时，超时 `kill` 子进程；按实测 NDJSON 结构取**最后一条 assistant 的 `message_end`**，且只取 `content` 数组中 `type=="text"` 的块（忽略 `thinking` 块）。
- **F5 RCON 发送**：实现 RCON 协议（登录 3 / 命令 2 / 响应 0，小端 int32），长连接、断线重连、失败重试一次；发送 `/tellraw <player> {"text":"..."}`。
- **F6 限流**：按玩家冷却（默认 10s），超限忽略并记录。
- **F7 消息分段**：按 UTF-8 字符边界分段（默认每段 ≤200 字符），逐段 tellraw。
- **F8 配置管理**：命令行 `--rcon-host/--port/--pass/--log-file/--prefix/--pi-cmd/--pi-model/--timeout/--cooldown/--max-len/--allow` + 环境变量，优先级 命令行 > 环境变量 > 默认值。
- **F9 日志输出**：程序运行日志到 stdout/stderr，带级别与时间戳。
- 交付内容：`src/` 下 9 个模块、`Makefile`、单元测试、README，以及一份可复现的本地联调环境说明。

## Capabilities

### New Capabilities

- `log-watching`: 日志文件实时监控能力 —— 增量读取、日志轮转（inode 变化）与截断恢复、不完整行续读。
- `chat-parsing`: 从 MC 日志行解析聊天消息的能力 —— 玩家名提取、`@pi` 前缀识别、问题提取、非聊天行过滤。
- `task-queue`: 线程安全有界任务队列能力 —— 阻塞入队/出队、关停语义。
- `pi-invocation`: 调用 pi CLI 并解析其 NDJSON 输出的能力 —— 参数构造、超时与子进程回收、assistant 文本块提取（含 `thinking` 块忽略）、错误分类。
- `rcon-client`: RCON 协议客户端能力 —— 打包/解包、认证握手、命令往返、断线重连。
- `bridge-worker`: 工作线程端到端处理能力 —— 队列取任务、调用 pi、构造并发送 tellraw、限流、UTF-8 分段。
- `cli-config`: 配置能力 —— 命令行/环境变量解析、优先级、默认值、校验。

### Modified Capabilities

<!-- 本仓库 openspec/specs 下无既有 spec，无修改 -->

## Impact

- **新增目录**：`/home/duan/code/pi-mc-bridge/`（`src/`、`tests/`、`Makefile`、`README.md`、`openspec/`）。
- **运行期依赖（均已在机器上确认）**：
  - `pi` = `/home/duan/.npm-global/bin/pi`（`@mariozechner/pi-coding-agent`）
  - `gcc` / `make` / `pthread` / POSIX socket
  - MC 服务端需 `enable-rcon=true` 且日志路径可读
- **pi 凭据**：机器上 pi 的默认 provider 无可用凭据；本机已注册自定义 provider 扩展 `~/.pi/agent/extensions/mc-gateway.ts`，指向用户自建网关（OpenAI 兼容，模型 `deepseek/deepseek-v4.1-flash`）。桥接程序本身不读凭据，只依赖 `pi` 命令可跑通。
- **联调环境**：需一个开启 RCON 的原版 MC 服务端（含 `logs/latest.log`）用于端到端验证。
- **不影响**：不修改任何 MC 服务端文件；不改动 `pi` 全局配置（仅在 `~/.pi/agent/extensions/` 新增一个扩展）。
