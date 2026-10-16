## Context

当前程序是一条写死的直线：`Config.prefix` 是**单个** `char*`（`src/config.h:19`）→ `parse_chat_line()`
→ `pi_ask()` → `pi_client.c:38` 的 `append_text_blocks()` 硬编码提取 `text` 块 → `worker.c:148`
的 `handle_prompt()` 只认「对话」一种行为 → tellraw。没有任何分层，无法表达「玩家加入时直接发欢迎语」
（不调 AI），也无法让不同关键词走不同处理逻辑。

用户要求保留底层 RCON 通信，把上层重构成四层：

```
底层 RCON 通信  →  对话器 / 默认回复器（并列的执行器）  →  Agent（顶层编排器）
                        ▲
                  对话器需要「对话触发器」，可配置多条触发条件
```

已确认的关键决定（来自与用户的澄清）：

1. 触发条件由**新增配置文件**承载；优先级 **命令行 > 环境变量 > 配置文件 > 默认值**。
2. **每条触发词可绑不同执行器**（`@pi`/`@ai` → 对话器，`joined the game` → 欢迎回复器）。
3. **Agent = 顶层编排器**：行 → 匹配触发器 → 选执行器 →（对话器）调 pi → 选回复整形器 → 回复。
4. **文本提取器作用于输入侧**：把日志行变成结构化的 `{player, text}`——**提取数据，而不是写提示词**。
   玩家名用于回复对象、白名单、限流，并以固定格式进入喂给 pi 的输入行（见 D3）。
5. **需要「默认回复器」**：不调 AI、直接用模板发文本的执行器（欢迎语等），支持 `{player}` 占位符。
6. **匹配能力**：两种内置行型（聊天行 / 字面关键词行）+ 字面关键词，不写正则引擎。

> **命令执行器不在本变更**：它只是预研，暂不开发，另行记录为独立变更 `add-command-executor`
> （默认关 + 白名单 + 审计 + 本地控制面）。本变更的执行器注册表只落地 `dialog` 与 `replier`；
> 配置里写 `executor = command` 按**未知执行器**报错拒绝（见 D9）。

底座前提（已确认）：`rcon_command()`（`src/rcon_client.c`）已支持任意命令 + 断线重连 + 超时，
（未来）命令执行器**无需新传输层**；`Makefile:6` 用 `$(wildcard src/*.c)` 自动收集，**新增模块不必改 Makefile**。

## Goals / Non-Goals

**Goals:**
- 把「匹配 → 提取 → 执行 → 回复」拆成四个可插拔的注册表 + 一个薄编排器，触发器可配置多条。
- 用配置文件承载触发器、关键词、执行器绑定与模板；四级优先级合并。
- 提供 `dialog`（对话器）与 `replier`（默认回复器）两个执行器。
- **完全向后兼容**：无配置文件时行为与当前版本逐字节一致（含 CLI/env/systemd/现有 78 项单测）。

**Non-Goals:**
- **不负责提示词**：不提供 `prompt_template`、system prompt、人格、上下文或任何提示词语义配置键；
  桥接只做数据提取与传输，`dialog` 的输入行按固定格式拼装且不可配置。定制 AI 行为在 **pi 侧**完成。
- **不实现命令执行器**：那是预研，暂不开发，由独立变更 `add-command-executor` 承担；
  本变更不留 `command` shaper、不留 `command` 执行器。
- 不做多轮对话记忆 / 会话持久化。
- 不写正则引擎；匹配只做字面关键词与内置行型。
- 不做多步工具循环（pi 已 `--no-tools` 单轮）；仅在 `dialog` 留下注释钩子。
- 不做多工作线程 / 多 RCON 连接的并发改造（沿用单工作线程串行，避免 pi 并发耗尽资源）。
- 不改 `rcon_client`、`logwatcher` 的现有协议实现。
- 不改动 MC 服务端或 pi 的全局配置。

## Decisions

### D1. 匹配与提取分层：触发器表 + 提取器注册表，而非合并成一个
概念上 `match`（行型）与 `extractor`（取字段）耦合，但用户明确认可「两种内置行型」这层抽象。
做法：两者保持独立配置键，但**实现上一个提取器同时负责匹配+取字段**（避免重复扫描）。
`trigger_match()` 按 `t->extractor` 查表调用即可。

- 备选：只保留 extractor（match 由 extractor 内部决定）——配置表达力更弱，`keywords` 属于哪一层会含糊。
- 备选：只保留 match（字段提取写死在 match 分支）——将来加第三种行型就要改核心 if 链，违背可插拔目标。

### D2. 关键词行型的玩家名提取：取关键词前紧邻的 token
`YYM joined the game` → keyword=`joined the game`，player=`YYM`。做法：`strstr(line, kw)`
命中后向前跳过空格，取紧邻的一个 token（`[A-Za-z0-9_]`），再用 `is_valid_player_name()`
（`src/parser.c:9`）校验；不合法则不产出 player（text 仍可产出）。`text = ""`（keyword 型无正文）。

- 备选：用「日志前缀后的第一个 token」——`[Not Secure]` 等标签会干扰，且不通用。
- 备选：正则捕获组——用户明确不要正则引擎。

### D3. 提取器只做数据提取，输入行按固定格式拼装（桥接不写提示词）
`chat` 提取器包一层现有 `parse_chat_line()`（`src/parser.c:101`），**逻辑一行不改**，
产出 `TriggerEvent{player, text}`。`dialog` 执行器按**固定格式** `<player>: <text>` 拼出喂给 pi 的输入行：
格式常量写死在代码里，**配置文件不提供任何提示词键**（无 `prompt_template`、无 `system`）。
玩家名的用途是：回复对象、白名单、限流 key，以及作为输入行里的说话人标识。

- 备选（已否决）：可配置 `prompt_template` —— 那是在桥接里写提示词，违背「薄传输层」定位；
  用户要定制 AI 行为应在 pi 侧配置。
- 备选（已否决）：把上下文拼进 pi 的 system prompt 参数 —— pi 是否支持该参数**未实测**，
  且属于提示词职责，越界。
- 备选（已否决）：纯文本透传、不带玩家名 —— 与用户「要让 AI 知道是谁在说话」的诉求相反。

### D3b. 向后兼容时的输入差异（已知且刻意）
旧实现把 keyword 之后的**全部原文**（含 `@pi`）作为 prompt 传给 pi；新实现按固定格式拼
`<player>: <text>`，且 keyword 被消费（text 不含 `@pi`）。因此**「输入到 pi 的文本」在配置文件模式下
会有一次性的形态变化**——这是本变更唯一的非字节级差异。无配置文件时同样走新格式（合成触发器），
故升级后 pi 看到的输入行会从 `@pi 你好` 变为 `Steve: 你好`。

- 取舍：保留「逐字节向后兼容」与「不在桥接里拼接玩家上下文」二者不可兼得，用户明确选择了后者
  （薄传输层 + 固定格式）。
- 缓解：CLI/env/systemd/退出码/游戏内表现均不变；仅在 pi 侧看到输入行格式变化。README 明记此点。

### D4. 默认回复器用模板 + 占位符，而不是「走对话器但空输入」
新增 `replier` 执行器，参数只有 `template`，支持 `{player}`/`{text}`/`{trigger}` 三个占位符
（`dynbuf_append_template()`，新增于 `src/util.c`）。**未知占位符原样保留**（不吞内容）。
模板只作用于**发给玩家的输出**，与提示词无关。不 fork pi，直接 `worker_build_tellraw` → RCON。

- 备选：让 replier 复用 dialog 但把输入当模板——会多一次 pi 调用，且 join 欢迎本就不该调 AI。

### D5. 配置文件用 INI 子集手写解析，未知键早失败
格式：`[global]` + 多个 `[trigger:<name>]`；`key = value`，`#`/`;` 注释，值两端 trim，
`keywords` 逗号分隔。**未知键 → 报错退出**（避免用户拼错键名后静默失效）。
不引入第三方 INI 库（与 <5MB、无第三方依赖目标一致）。

- 备选：JSON/YAML 配置——需要 JSON 解析器（虽有极简扫描器但不完整），且 YAML 需第三方库。
- 备选：忽略未知键——拼错 `keywrods` 会静默变成「无触发器」，排查成本高。选早失败。

### D6. 向后兼容：无配置文件时合成一条等价触发器
`config_load()` 末尾在「无配置文件」时，用现有 `prefix`/`cooldown`/`allow` 合成一条触发器：
`match=chat, keywords={prefix}, extractor=chat, executor=dialog` —— 保证现有 CLI/env/README/systemd
**用法全部不变**（唯一差异见 D3b：pi 侧输入行格式）。

- 备选：强制要求配置文件——破坏向后兼容，现有用户升级即挂。

### D7. 限流 key 从 `player` 改为 `trigger->name + player`
`worker.c` 的 `RATE_SLOTS` 表 key 由纯玩家名改为「触发器名 + 玩家名」，
冷却取 `trigger->cooldown_ms`（`<0` 时回退全局 `Config.cooldown_ms`）。
理由：「join 欢迎」与「@pi 对话」冷却本就该各自独立。

- 备选：保持按 player 限流——加入欢迎 @pi 会互相干扰冷却，玩家会抱怨「刚进服发 @pi 没反应」。

### D8. Agent 是独立模块但很薄，不做过早抽象
建 `src/agent.[ch]`，但职责仅：匹配触发器 → 白名单/队列满处理 → 入队。
真正的编排在选择执行器那一步（在 worker 里）。薄编排器取代 `main.c:58` 的 `on_log_line`。

- 备选：把编排逻辑塞进 `main.c`——命名不清晰，且 `main.c` 会变胖难测。

### D9. 命令执行器暂不开发（预研），本变更不落地任何 command 形态
命令执行器只是预研。**本变更不实现它**：执行器注册表只含 `dialog` 与 `replier`；
配置里写 `executor = command`（或任何未注册名）按**未知执行器**报错退出。
命令执行器的完整设计（默认关 + 白名单 + 审计 + pi 工具模式的本地控制面）见独立变更
`add-command-executor`，待其条件成熟再单独实施。

- 备选：本轮顺手实现命令执行器 —— 预研尚未收敛（载体从「文本标记 + shaper」已改为「pi 工具 + 控制面」），
  与本变更的「分层重构」主线耦合会拖慢主线，且引入 RCON 控制台权限风险，故拆开。

### D10. `reply` 整形器抽离，本轮只有 `text`
把 `pi_client.c:38` 的硬编码提取抽成 `ReplyShaper` 注册表，本轮只实现 `text`
（直接调现有 `pi_parse_stream`，行为不变）。原设想的 `command` shaper **不实现**（见 D9）。
`dialog` 使用 `text` shaper。

- 备选：让执行器各自解析 pi 输出——重复代码，且提取规则（取最后一条 assistant message_end、
  忽略 thinking 块）是实测得出的易错点，必须集中一处。

## Risks / Trade-offs

- **[输入行格式固定、无法在桥接侧调整]** → 这是刻意的边界（薄传输层），且属于非目标级决定；
  用户若需不同措辞/上下文，应在 pi 侧扩展或前置一个提示词层，本程序不承担该职责。
- **[pi 输入行格式发生一次性变化]** → 见 D3b；这是「薄传输层 + 固定格式」的必然结果，
  README 明确记录，pi 侧如需适配由用户在 pi 侧完成，桥接不做兼容层。
- **[限流 key 变更改变连发行为]** → 采纳新的 trigger+player key（见 D7），README 明确说明；
  默认合成触发器的 `name` 固定（如 `default`），保证旧用户冷却行为等价（单触发器时 key 等价）。
- **[`rate_evict_one` 破坏探测链（README §8.1）]** → 本轮顺带修复（改 key 时一并处理），
  避免限流表在高负载下失效。
- **[新增 6 个模块增加代码量与内存占用]** → 注册表都是静态 `const` 表，触发器/执行器状态按配置条目
  一次性分配，无运行时增长；常驻内存仍在 <5MB 目标内。`Makefile` 用 wildcard 自动收集，零改动。
- **[提取器与 match 双键配置可能不一致]**（如 `match=keyword` 配 `extractor=chat`）→
  配置校验阶段拒绝这种组合并报错退出。

## Migration Plan

分 5 阶段实现，**每阶段可编译可测**，无破坏性切换：

1. 提取器 + 触发器（无配置文件）→ 现有单 prefix 合成一条触发器，行为与今天一致。
2. 模板占位符 + `replier` 执行器 → worker 按 trigger 分派。
3. `dialog` 执行器 + `reply` 整形器（`text` shaper 包 `pi_parse_stream`）。
4. 配置文件解析 + 四级优先级 + 无文件回退合成触发。
5. README 新增「配置文件」章节与两条内置触发器示例、明确「不负责提示词」边界与输入行格式；
   systemd 示例补 `--config`。

（命令执行器不在本变更的迁移计划内；它由独立变更 `add-command-executor` 在条件成熟时实施。）

**回滚**：任一步骤失败，删掉配置文件即可退回「无配置文件」路径，行为等同当前版本；
代码层面每阶段独立提交，可单独 revert。

## Open Questions

- **配置热重载**（SIGHUP 重读配置文件）——本轮不做，需重启进程；记为未来增强。
- **命令执行器**（默认关 + 白名单 + 审计 + 本地控制面）——预研，暂不开发；
  完整设计见独立变更 `add-command-executor`。
