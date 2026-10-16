## Why

AI 目前只能在游戏里聊天，无法对服务器做任何操作。前一个变更为此预留了「命令执行器(预研)」，
原设想是 pi 输出带标记的行 → 桥接做前缀白名单 → 执行 RCON。该设想有两个缺陷：
（1）**模型拿不到执行结果**——命令是否成功、服务端返回什么，都无法回流，模型只能盲发；
（2）桥接被迫把 AI 的**自由文本**解析成命令，脆弱且难以审计。
用户的真实诉求是命令执行**带状态回流**（"输出 + 状态"），并据此提出"是否有必要做成 MCP"。

实测结论（本机 pi 0.73.0 发布物）：pi **没有内置 MCP**
（`docs/usage.md:275`、`README.md:470`），但支持用 TypeScript **扩展**注册自定义工具；
工具返回 `{content, isError, details}`，**pi 会把工具结果喂回模型**（原生 agentic 循环），
并在 `--mode json` 事件流中给出 `tool_execution_start` / `tool_execution_end`。
因此 "MCP 的价值"（工具调用 + 结果回流）pi 原生即具备，**无需引入 MCP** 这层协议与额外进程。

## What Changes

- **不引入 MCP**：host 是 pi 而非 MCP 客户端。实现 MCP 需要额外写 MCP server + pi 扩展做协议转换，
  换来的是 pi 已原生提供的能力。改为走 pi **自定义工具**。
- 新增 `command` 执行器的**控制面**形态（默认关闭）：
  - 桥接启用时开一个**本地 Unix domain socket**（默认 `0600`，仅属主可访问）接收工具调用。
  - 随仓库分发一个 **pi 扩展 shim**（`extensions/mc-bridge.ts`），注册**唯一**工具
    `mc_command({ command })`，经该 socket 转发到桥接。
  - 该触发器的 pi 调用由「`--no-tools` 单轮」切换为**工具模式**：
    `--no-builtin-tools --tools mc_command -e <extension>`；仅放行本工具，禁掉内建 read/bash/edit/write。
    桥接**不传任何提示词文本**（不用 `--append-system-prompt`）——模型面向的说明全部放在**扩展**里，
    以恪守「桥接不负责提示词」的前置边界。
- **桥接仍是策略与传输的唯一所有者**：socket 侧对每条 `command` 做 `allow_commands` 校验，
  通过才 `rcon_command`；执行结果 `{ok, output}` 回填给工具 → pi 的 agentic 循环 → 模型看到真实状态。
- **白名单粒度**：`/time` 前缀匹配；`=/time set day` 整条精确匹配（`=` 前缀标注）。
  含换行 / 命令分隔符的候选一律拒绝。
- **玩家授权**：`command` 触发器必须有**显式、非空**的 `allow` 玩家名单，否则启动即拒（不继承全局）。
- **审计**：每次工具调用经 `replier` 模板把「玩家 / 命令 / 结果状态」输出到**桥接进程命令行（stdout）**，
  供运维查看；**不发给游戏内玩家**。
- **撤销（相对前一个变更）**：`command` 执行器不再解析 pi 文本产出命令；`reply-shaping` 不再提供
  `command` shaper。

## Capabilities

### New Capabilities
- `command-control-plane`: 本地控制 socket 的生命周期 / 权限 / 协议，pi 扩展 shim 与单工具注入，
  工具模式的 pi 调用形态。
- `command-execution`: `allow_commands` 校验（前缀 + `=` 精确）、RCON 执行、`{ok, output}` 回填、
  经 replier 输出到命令行的审计。

### Modified Capabilities
- `executor-registry`: `command` 执行器由「调 pi → command shaper → 前缀白名单」重定义为
  「开控制面 → 工具结果回填 → 白名单 → RCON」；缺 `allow_commands` 或缺玩家 `allow` 拒启；默认关闭不变。
- `pi-invocation`: 新增「工具模式」调用变体（仅在该触发器启用命令器时使用）：
  放行单一自定义工具、禁用内建工具、不传提示词文本。

## Impact

- **新增模块**：`src/control.[ch]`（Unix socket server + 每请求校验/执行/回填）、
  `extensions/mc-bridge.ts`（pi 扩展 shim，随仓库分发）。
- **修改模块**：`src/executor.c`（`command` 执行器重定义为控制面宿主）、
  `src/pi_client.[ch]`（工具模式 argv）、`src/config.[ch]` / `src/configfile.[ch]`
  （`command` 触发器的 `allow`（必填）、`allow_commands`、`socket`、`extension` 键）、
  `src/util.[ch]`（审计模板占位符扩到 `{player}/{tool}/{command}/{status}/{result}`）、
  `src/worker.c`（命令器经 replier 审计到 stdout）。
- **不改动**：`src/rcon_client.[ch]`（`rcon_command` 已支持任意命令 + 重连 + 超时）、
  `src/logwatcher.[ch]`、`Makefile`（wildcard 自动收集 C 源文件）。
- **明确不做**：不实现 MCP server；不做多类型化工具；不做跨主机部署（socket 与扩展需与 pi 同机）。
- **安全**：RCON = 服务端控制台权限。命令器**默认关闭**；必须同时给出 `allow_commands` 与玩家 `allow`，
  否则启动即拒；socket 本地且 `0600`；AI 输出视为不可信（提示词注入），白名单在**桥接侧**强制执行，
  扩展与模型均无法绕过；审计成对记录。README 需醒目警告。
- **依赖**：桥接仍为 C11 + POSIX，无新增第三方依赖；pi 侧扩展用 TypeScript（Node 内置 `node:net`），
  随仓库分发、`-e` 显式加载。
