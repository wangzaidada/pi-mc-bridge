## Why

现在整条链路是写死的直线：只有一个触发词 `Config.prefix`（`@pi`），日志行解析后一律调 `pi`，
再从 NDJSON 里硬编码提取 `text` 块，最后 tellraw。无法表达「玩家加入时直接发欢迎语」这种不调 AI 的行为，
也无法让 `@pi` / `@ai` 之外的关键词走不同逻辑。玩家与运维希望把上层重构成可配置、可插拔的分层架构，
同时保持底层 RCON 通信与 pi 调用方式不变。

## What Changes

- **明确边界（非目标）**：本程序是 AI 与游戏服务器之间的**薄传输层**，**不负责提示词**——
  不提供 `prompt_template`、system prompt、人格、上下文或任何形式的提示词/指令配置键。
  桥接只做：提取结构化数据 `{player, text}`、按**固定格式**拼成喂给 pi 的输入行、转发 pi 的回复。
- 新增**触发器表**：一条日志行由多条触发器（触发器条件）匹配，每条触发器绑定**自己的关键词组、
  提取器与执行器**；匹配能力为「聊天行型 + 字面关键词行型」两种内置行型，不引入正则引擎。
- 新增**输入侧文本提取器**（`chat` / `keyword`）：把日志行变成结构化的 `{player, text}`——桥接只负责
  **取数据**，不负责措辞。`dialog` 执行器再以**固定格式**（`<player>: <text>`）拼成喂给 pi 的输入行，
  格式本身不可配置（见 design D3）。
- 新增**执行器注册表**（可插拔），本变更实现两种：
  - `dialog`（对话器）：按固定格式拼输入行 → 调 pi → 回复整形 → tellraw（现 `handle_prompt` 的行为迁移到此）。
  - `replier`（**默认回复器**，新增）：模板 + `{player}`/`{text}`/`{trigger}` 占位符 → tellraw，**不调 AI**。
- 新增**输出侧回复整形器**（本轮仅 `text`）：pi 的 NDJSON 提取逻辑从 `pi_client.c` 抽成可选能力。
- 新增**配置文件**（INI 子集，手写解析，无第三方库）：`[global]` 全局默认 + `[trigger:<name>]` 每条触发器；
  优先级 **命令行 > 环境变量 > 配置文件 > 默认值**。
- 新增**Agent 顶层编排器**（薄）：日志行 → 匹配触发器 → 入队（任务携带 trigger 指针）→ 由 worker 选执行器。
- **BREAKING（内部接口）**：`Task` 增加 `const Trigger *trigger`；`worker` 限流 key 由 `player` 改为
  `trigger->name + player`（不同触发器各自独立冷却）。对外 CLI/env 行为**不变**。
- **向后兼容**：无配置文件时，由现有 `prefix`/`cooldown`/`allow` **合成一条等价触发器**，
  现有命令行、环境变量、systemd 单元、README 用法全部保持不变。

## Capabilities

### New Capabilities
- `trigger-matching`: 触发器表、两种内置行型（chat / keyword）匹配、每触发器多关键词、
  每触发器独立冷却与白名单继承、命中产出 `TriggerEvent`。
- `text-extraction`: 输入侧提取器注册表（`chat` / `keyword`），把日志行转成结构化的 `{player, text}`；
  只取数据，不含任何措辞/提示词逻辑。
- `executor-registry`: 执行器注册表与两个实现（`dialog` 对话器 / `replier` 默认回复器），
  以及每执行器的参数从配置读取与实例化。`dialog` 的输入行由桥接按固定格式拼装，无提示词配置键。
  命令执行器只是预研、**暂不开发**，记录为独立变更 `add-command-executor`，本变更不留 `command` 实现。
- `reply-shaping`: 输出侧回复整形器注册表（本轮仅 `text`），从 pi NDJSON 提取回复文本。
- `config-file`: INI 子集配置文件格式（`[global]` + `[trigger:<name>]`）、加载与校验、
  四级优先级合并、未知键早失败报错。
- `agent-orchestration`: Agent 顶层编排器：行 → 匹配 → 白名单/队列满处理 → 入队；程序装配顺序。

### Modified Capabilities
- `chat-parsing`: 现有聊天行解析逻辑保持不变，改为被 `chat` 提取器调用；新增「关键词行型」的
  玩家名提取要求（关键词前紧邻 token，须为合法玩家名）；提取结果不再包含触发词本身。
- `task-queue`: `Task` 携带触发该任务的具体触发器；入队/出队语义随之扩展。
- `bridge-worker`: 由「固定流程」改为「按触发器绑定的执行器分派」；限流 key 改为 `trigger + player`；
  冷却取值回退链（触发器 → 全局默认）。
- `cli-config`: 新增 `--config <path>` / `CONFIG_FILE`；现有 `prefix`/`cooldown`/`allow` 语义
  由「唯一触发词」下沉为「全局默认值」；配置校验要求至少存在一条触发器。

## Impact

- **新增模块**：`src/trigger.[ch]`、`src/extractor.[ch]`、`src/executor.[ch]`、`src/reply.[ch]`、
  `src/agent.[ch]`、`src/configfile.[ch]`。
- **修改模块**：`src/config.[ch]`、`src/parser.[ch]`（仅被包装，逻辑不变）、`src/pi_client.[ch]`、
  `src/queue.[ch]`、`src/worker.[ch]`、`src/main.c`、`src/util.[ch]`（新增模板替换 `dynbuf_append_template`，
  仅用于 `replier` 的输出模板，不用于任何 pi 输入构造）。
- **不改动**：`src/rcon_client.[ch]`（`rcon_command` 已支持任意命令、重连、超时）、
  `src/logwatcher.[ch]`、`Makefile`（`$(wildcard src/*.c)` 自动收集）。
- **明确不做**：不提供任何提示词/系统提示/人格/上下文配置；不向 pi 传递 system prompt；
  不做多轮对话记忆。若要定制 AI 行为，请在 **pi 侧**配置（本程序不感知）。
- **安全**：RCON 等于服务端控制台权限。本变更**不实现命令执行器**（它只是预研、暂不开发，
  记录为独立变更 `add-command-executor`：默认关闭 + 白名单 + 审计 + pi 工具模式控制面）；
  本轮执行器注册表只落地 `dialog` 与 `replier`。AI 输出一律视为不可信。
- **向后兼容**：无配置文件时行为与当前版本完全一致；现有 78 项单测保持不变并通过。
- **依赖**：无新增第三方依赖，仍为 C11 + POSIX + pthread。
