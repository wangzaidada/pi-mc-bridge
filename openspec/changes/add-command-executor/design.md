## Context

前一个变更 `add-trigger-executor-layers` 引入了触发器 → 提取器 → 执行器 → Agent 的分层，
并为**命令执行器**留了一个预研位：它的原始设想是「pi 输出带标记行 → `command` reply shaper
提取候选 → 桥接前缀白名单 → `rcon_command`"。本变更落实该预研位，但**改换实现形态**，
理由见下。

### 实测前提（本机 pi 0.73.0 发布物，勿凭文档臆测）

- pi **没有内置 MCP**：`docs/usage.md:275`（"It intentionally does not include built-in MCP"）、
  `README.md:470`（"**No MCP.** … or build an extension that adds MCP support"）。
- pi 支持 **TypeScript 扩展**：`~/.pi/agent/extensions/*.ts` 自动发现，或 `-e <path>` 显式加载
  （`docs/extensions.md`）。扩展可用 `pi.registerTool()` 注册自定义工具。
- 工具 `execute()` 返回 `{content:[{type:"text",text}], details, ...}`；**抛出异常**即
  `isError: true`。pi 会把工具结果**喂回模型**并继续，形成原生 agentic 循环。
- `--mode json` 的事件流含 `tool_execution_start` / `tool_execution_end`（`docs/json.md`），
  工具成功/失败有独立事件——桥接可据此审计。
- 工具可见性开关：`--no-tools` 连扩展工具一起禁；**`--no-builtin-tools` 保留扩展工具**；
  `--tools <names>` 是白名单。要"只放行本工具、禁掉 read/bash/edit/write"，
  用 `--no-builtin-tools --tools mc_command`。
- 扩展可直接 import Node 内置模块（`node:fs`/`node:net`/…），无需额外 npm 依赖。

### 为什么不是 MCP

用户诉求的本质是**命令执行带状态回流**（"输出 + 状态"），这正是 MCP 的价值。但本项目的 host 是 **pi**，
而 pi 刻意不含 MCP。要在此处用 MCP，需自行编写一个 MCP server **再**写一个把 MCP 转成 pi 工具的扩展，
多出两个进程与一层协议转换，而 pi 的自定义工具**原生**就提供「工具调用 + 结果回流 + 结构化状态」。
结论：**不引入 MCP**，用 pi 扩展 + 单一工具承载。

### 边界继承

前一个变更确立了「薄桥接、不负责提示词」。本变更**不破这条线**：桥接只
（a）提供一个**类型化控制面**（本地 socket + 白名单 + RCON），（b）在启用命令器时把 pi 调用切到工具模式。
**面向模型的措辞/说明放在扩展（pi 侧），桥接不传任何 system/提示词文本**。

## Goals / Non-Goals

**Goals:**
- 让 AI 能**带状态回流**地操作服务器：执行结果（成功/失败/输出）回到模型，模型据此继续或解释。
- 桥接**仍是策略与传输的唯一所有者**：白名单、玩家授权、审计都在桥接侧，扩展与模型无法绕过。
- 命令器**默认关闭**；开启需同时满足「显式 `executor = command` + 非空 `allow_commands` + 非空玩家 `allow`」。
- 审计状态经 `replier` 模板输出到**桥接进程命令行（stdout）**，运维可见。

**Non-Goals:**
- 不实现 MCP server，不做协议转换层。
- 不做多类型化工具（只注册**单一** `mc_command({command})`）；不把参数级命令策略语言化。
- 不做跨主机部署：socket 与扩展必须与 pi 同机（同用户）。
- 不做多人环确认（前一个变更已定「不做确认，仅限低危命令」）。
- 不改变对话器（`dialog`）的既有行为；命令器是**独立触发器**。
- 不向游戏内玩家展示命令执行详情（那是运维视角；玩家只收到 dialog 的正常回复）。

## Decisions

### C1. 载体：pi 自定义工具 + 本地控制 socket，而非 MCP，也非纯文本标记
桥接启用命令触发器时开一个 **Unix domain socket**（默认路径如 `<runtime_dir>/pi-mc-bridge.sock`，
权限 `0600`）。扩展（随仓库分发，安装进 **pi 自带的扩展目录**，见 C10）注册**唯一**工具 `mc_command`，
`execute` 时连该 socket，发一行 JSON 请求，读回一行 JSON 响应。

- 备选（已否决）：**MCP server**——host 是 pi，pi 无内置 MCP；需额外 server + 转换扩展，净增复杂度。
- 备选（已否决）：**pi 输出标记行 + 桥接解析**——模型拿不到执行结果（回流缺失），
  且把自由文本当命令解析脆弱。这正是本变更要修掉的原设想。

### C2. 单一工具 `mc_command({ command: string })`
不做 `mc_time`/`mc_give` 等类型化工具。约束集中在**桥接的白名单**，而非工具 schema。
理由：符合「薄桥接」定位；白名单是单一事实来源，改白名单不改扩展。

- 备选：多类型化工具——约束更强，但把命令策略拆散进 schema，且需为每类命令维护扩展，违背薄桥接。

### C3. pi 调用切到「工具模式」，且只放行本工具
命令触发器的 pi 调用 argv：
`pi --mode json --no-session --no-builtin-tools --tools mc_command -e <resolved extension path> [--model M] -p "<input>"`
即：保留扩展工具、禁用内建工具、只放行 `mc_command`、显式加载解析出的扩展文件（路径解析见 C10）。
**不传** `--append-system-prompt` 或任何提示词文本——面向模型的说明在扩展内（`description` / `promptGuidelines`）。

- 备选：继续 `--no-tools`——连扩展工具也禁，命令器直接失效。
- 备选：`--append-system-prompt` 由桥接注入工具说明——越界（桥接写提示词），否决。

### C4. `command` 执行器 = 控制面宿主（默认关闭）
触发器显式写 `executor = command` 才构建。启动时：
1. 校验 `allow_commands` **非空**，否则报错退出；
2. 校验该触发器的 **玩家 `allow` 非空且显式**（命令器**不继承**全局 all-players），否则报错退出；
3. 建立控制 socket（`0600`），进入工具模式。
每条请求在**桥接侧**用 `allow_commands` 校验后执行 RCON，把 `{ok, output}` 作为工具结果回填。

- 备选：允许继承全局 `allow`（= 所有玩家）——授权口子开得太大，与"混淆代理"风险相悖，否决。

### C5. 白名单粒度：前缀 + `=` 精确，含一定危险的命令族（如 `/execute`）**不得放行**
`/time` = 前缀匹配（放行 `/time set day`）；`=/time set day` = 整条精确（仅此一条）。
条目以 `=` 开头表示精确匹配，否则前缀匹配。候选含换行 / 命令分隔符（`;`、`|`、`&`、`` ` ``）一律拒绝。
README 明列**永不建议放行**的命令族：`/execute`、`/op`、`/deop`、`/ban`、`/kick`、`/stop`、`/fill`、`/setblock`、`/reload`。

- 备选：纯前缀——`/time` 会放行任意参数，粒度不足；备选：参数级规则——等于写策略语言，越出薄桥接。

### C6. 请求/响应协议（行分隔 JSON，私有协议）
请求（扩展 → 桥接）：
`{"player":"Steve","tool":"mc_command","command":"/time set day"}\n`
响应（桥接 → 扩展）：
`{"ok":true,"output":"Set the time to day"}\n` 或 `{"ok":false,"error":"command not allowed"}\n`
桥接不信任 `command` 字段：一律走 C5 校验。`player` 用于审计与（可选的）玩家级限流。

- 备选：JSON-RPC——为单方法接口引入规范负担，无收益。
- 备选：复用 RCON 端口——RCON 是明文 TCP；引入本地 socket 以收紧攻击面（仅属主可连）。

### C7. 审计：经 `replier` 模板输出到桥接命令行，**不发给玩家**
每次工具调用后，用 `replier` 的模板机制渲染一条审计行并写 **stdout**（桥接进程的命令行），
例如模板 `[audit] player={player} tool={tool} cmd={command} status={status} result={result}`。
为此 `dynbuf_append_template` 的占位符扩到 `{player}/{text}/{trigger}/{tool}/{command}/{status}/{result}`。
**玩家在游戏内只收到 dialog 的正常回复**，审计行不 tellraw。

- 备选：审计直接 tellraw 给玩家——既泄露运维信息又与 dialog 回复格式冲突，否决。
- 备选：审计只写日志文件——用户明确要「命令行可见」，stdout 即命令行，采纳。

### C8. `{ok, output}` 由模型消费；桥接不解析模型对结果的措辞
工具结果喂回模型后，模型会在后续 `text` 块里（面向玩家的回复）解释执行情况。
「模型看到的状态」= 工具结果本身（结构化、可信来源）。
桥接**不做**额外解析来"确认模型是否看懂了"——避免再一次脆弱的文本解析。

- 备选：桥接解析模型的后续文本判断成败——脆弱，且模型层回流已由 pi 保证。

### C9. 与既有 reply-shaper/`command` shaper 的关系：删除后者
原设想的 `command` reply shaper（从 pi 文本提取候选命令）在本形态下**不再需要**。
命令来自**结构化工具调用**，而非文本提取。故：前一个变更中的 `command` shaper 需求**撤销**，
`reply-shaping` 只保留 `text` shaper。

- 备选：保留 `command` shaper 作为"无扩展降级路径"——两套并存会分裂安全模型，否决。

### C10. 扩展的安装位置与解析路径：放进 pi 自带扩展目录，`.pi` 一并考虑
扩展**随仓库分发**（源文件在 `extensions/mc-bridge.ts`），但**安装进 pi 自带的扩展目录**，
与 pi 的其它扩展同处一地。解析顺序（桥接在配置里收到的是一个"扩展"值，可为路径或名字）：
1. 显式配置了 `extension = <path>` → 直接按该路径解析（相对路径相对配置文件所在目录）；
2. 否则按 pi 自带的扩展目录查找，**优先 `.pi` 项目级、回退用户级**：
   `./.pi/extensions/mc-bridge.ts`（项目级，pi 的 `.pi` 路径）→ `~/.pi/agent/extensions/mc-bridge.ts`（用户级）；
3. 全部未命中 → 启动失败并**指明搜索过的路径**。

无论走自动发现目录还是命中路径，桥接都**用 `-e <解析出的绝对路径>` 显式加载**——不依赖 pi 的自动发现，
这样 argv 里加载了哪个文件可审计、可复现。安装方式（README）：把 `extensions/mc-bridge.ts`
复制到 `~/.pi/agent/extensions/`（全局）或项目的 `.pi/extensions/`（按项目）。

- 备选：只放在仓库 `extensions/` 下、argv 写仓库相对路径——扩展不能与其它 pi 扩展并列被管理，
  且用户换工作目录后路径失效；用户明确要求放进 pi 自带扩展目录。
- 备选：只依赖 pi 自动发现、argv 不写 `-e`——无法保证加载到的就是本仓库那份，审计性差，否决。

## Risks / Trade-offs

- **[命令器的授权面 = 服务端控制台]** → 默认关闭；开启需三重条件（显式 executor + 非空 allow_commands +
  非空玩家 allow）；socket `0600`；README 醒目警告 + 高危命令族黑名单提示。**AI 输出不可信**，
  白名单在桥接侧强制，扩展/模型无法绕过。
- **[模型可在一次工具调用中请求任意命令]** → 桥接逐条校验，越权即 `{ok:false}` 回填（模型能看到为何失败），
  同时记审计。不做"相信模型只发合法命令"的假设。
- **[pi 从单轮变 agentic 多轮，耗时/成本上升]** → 仅**命令触发器**进入工具模式；对话器仍是单轮。
  沿用既有 pi 超时；工具调用次数由 pi 的循环控制，桥接不改。
- **[扩展文件路径/加载失败]** → 桥接按 C10 解析扩展路径（显式配置 → 项目级 `.pi` → 用户级 `~/.pi/agent`），
  启动时校验解析结果存在且可读，否则**拒绝启动并打印搜索过的全部路径**；
  工具模式下 pi 若因扩展加载失败而退出，走既有 `PI_ERR_*` 分支（玩家收到 dialog 提示，不在命令器上）。
- **[socket 与 pi 必须同机同用户]** → 明确为 Non-Goal 边界；文档说明多主机部署不被支持。
- **[审计行经 stdout 可能与程序自身日志混流]** → 审计行使用固定前后缀（如 `[audit]`），便于 grep；
  可选后续：审计写入独立 fd/文件（本轮不做）。
- **[`--no-builtin-tools` 的行为随 pi 版本变化]** → 已实测 0.73.0；README 记录最低版本与实测结论，
  升级 pi 后需回归 7.x 用例。

## Migration Plan

在 `add-trigger-executor-layers` 落地之后实施（命令器依赖其触发器/执行器/配置骨架）：

1. 控制面骨架：`src/control.[ch]`（socket 生命周期、逐请求校验/执行/回填）+ 扩展路径解析（C10），
   仅监听不接工具。
2. 工具模式 pi 调用：`pi_client` 支持 `--no-builtin-tools --tools <name> -e <ext>` 变体。
3. `command` 执行器接入控制面 + 三重启动校验（executor/allow_commands/玩家 allow）。
4. 白名单校验（前缀 + `=` 精确，分隔符拒绝）+ RCON 执行 + `{ok,output}` 回填。
5. 扩展 shim（源文件 `extensions/mc-bridge.ts`，单工具，`node:net` 转发）+ **路径解析**（C10：`-e` 显式绝对路径，
   `.pi` 项目级 → `~/.pi/agent/` 用户级回退）+ 安装说明。
6. `replier` 审计模板 + 占位符扩展 + stdout 审计。
7. 文档：README 安全警告、示例配置（命令触发器整段注释）、扩展安装说明、高风险命令族清单。

**回滚**：删除/注释命令触发器即可退回「无命令器」；代码每步独立提交，可单独 revert。

## Open Questions

- **审计行的输出通道**：本轮 `[audit]` 前缀写 stdout；若希望与运行日志彻底分离（独立文件或 fd），
  记为后续增强。
- **玩家级限流**：命令器是否对每玩家单独限频（如每分钟 N 条）？本轮沿用触发器的 `cooldown_ms`，
  不额外加命令维度的限流。
- **多工具拆分**：若日后需要更强约束（`mc_give` 只允许指定物品/上限），可在不改变控制面的前提下
  追加工具；本轮明确不做。
