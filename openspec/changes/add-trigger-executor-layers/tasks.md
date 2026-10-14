## 1. 提取器 + 触发器（无配置文件，行为向后兼容）

- [ ] 1.1 新增 `src/extractor.h` / `src/extractor.c`：`Extractor{name, extract}` 注册表，实现 `chat`（包 `parse_chat_line`，逻辑不改）与 `keyword`（`strstr` 命中后取关键词前紧邻 token，`is_valid_player_name` 校验；无合法名时 player 置 `<none>`）
- [ ] 1.2 新增 `src/trigger.h` / `src/trigger.c`：`Trigger{name, match, keywords, extractor, exec, exec_state, cooldown_ms, allow}`、`TriggerEvent{trigger, player, text}`、`TriggerTable`、`trigger_match`（按表序命中第一个即停）、`trigger_event_free`、`trigger_table_free`
- [ ] 1.3 `src/trigger.c` 内做 match/extractor 一致性校验（`keyword`+`chat` 等不兼容组合报错）
- [ ] 1.4 `src/config.c`：`Config` 保留现有字段作为全局默认；在 `config_load()` 末尾**无配置文件时合成一条 chat 触发器**（keywords={prefix}, extractor=chat, executor=dialog），保证现有 CLI/env/用法不变（唯一差异见 design D3b：pi 侧输入行格式）
- [ ] 1.5 `src/main.c`：`on_log_line`（`main.c:58`）改为调用 `trigger_match`，命中后入队；先验证与今天行为逐字节一致
- [ ] 1.6 单测：`chat` 提取器（标准行 / `[Not Secure]` 标签 / 无合法名）、`keyword` 提取器（player 前置 / 无合法名 → `none`）、多关键词命中、首中即停、无命中不产出事件

## 2. 模板占位符 + replier 执行器

- [ ] 2.1 `src/util.[ch]`：新增 `dynbuf_append_template(DynBuf*, const char *tpl, const KV *vars)`，支持 `{player}`/`{text}`/`{trigger}`，**未知占位符原样保留**
- [ ] 2.2 新增 `src/executor.h` / `src/executor.c`：`ExecutorOps{name, init, run, free}` 注册表与 `executor_lookup`
- [ ] 2.3 实现 `replier` 执行器：`init` 读 `template`（缺失即报错拒绝）；`run` 模板替换 → `worker_build_tellraw` → RCON，**不 fork pi**
- [ ] 2.4 `src/queue.[ch]`：`Task` 增加 `const Trigger *trigger;`（指针，表常驻无需拷贝）
- [ ] 2.5 `src/worker.c`：`handle_task`（`worker.c:201`）改为按 `t->trigger->exec` 分派；限流 key 由 player 改为 `trigger->name + player`，冷却取 `trigger->cooldown_ms`（`<0` 回退全局）
- [ ] 2.6 顺带修复 README §8.1 记录的 `rate_evict_one` 探测链破坏问题
- [ ] 2.7 单测：占位符替换（含未知占位符保留、`{player}` 为多字节中文名）；replier 不调用 pi；限流 key 区分不同触发器

## 3. dialog 执行器 + reply shaper 抽象

- [ ] 3.1 新增 `src/reply.h` / `src/reply.c`：`ReplyShaper{name, shape}` 注册表；实现 `text` shaper（直接调用现有 `pi_parse_stream`，行为不变）
- [ ] 3.2 把 `worker.c:148` 的 `handle_prompt` 迁入 `dialog` 执行器，保留全部错误码分支（timeout/spawn/exit/no-text/stop → 玩家提示 + 日志）
- [ ] 3.3 `dialog` 按**固定格式** `<player>: <text>` 拼 pi 输入行（格式常量写死在代码里，**不可配置**）；参数只有 `model`、`reply_prefix`、`reply_shaper`（默认 `text`）、`max_len`、`timeout`——**无任何提示词键**
- [ ] 3.4 复用 `worker.c` 的 `send_text_segmented` 做超长分段回复
- [ ] 3.5 单测：dialog 走假 PiClient 的成功 / 超时 / 错误分支；**输入行格式为 `<player>: <text>`**；text shaper 忽略 thinking 块、取最后一条 assistant message_end
- [ ] 3.6 配置校验拒绝一切提示词相关键（`prompt_template`、`system` 等）作为未知键报错，并确认 pi 调用参数中不含 system prompt

## 4. 配置文件解析（INI 子集）

- [ ] 4.1 新增 `src/configfile.h` / `src/configfile.c`：解析 `[global]` + `[trigger:<name>]`，`key = value`，`#`/`;` 注释，值两端 trim，`keywords` 逗号切分
- [ ] 4.2 未知键 → 报错退出并指明「段名 + 键名」（早失败）；`[trigger:x]` 缺 keywords → 报错；**提示词类键（如 `prompt_template`/`system`）一律按未知键拒绝**
- [ ] 4.3 四级优先级打通：命令行 > 环境变量 > 配置文件 `[global]` > 内置默认；trigger 未写字段回退合并后的全局值
- [ ] 4.4 `src/config.[ch]`：新增 `char *config_file`（`--config` / `CONFIG_FILE`）；`validate()`（`config.c:160`）扩展为「至少一条触发器」；`config_print_usage` 补 `--config` 说明
- [ ] 4.5 `src/main.c` 装配顺序：config_load → configfile_load（或合成触发器）→ 解析 executor/extractor 并绑定 → queue → worker → rcon → watcher；任一阶段失败则不启动后续阶段、非零退出
- [ ] 4.6 单测：最小配置、注释/空行、关键词逗号切分、未知键报错、提示词键被拒、缺 keywords 报错、优先级三项（CLI 覆盖文件 / env 覆盖文件 / trigger 回退全局）、无文件时合成触发器且事件为 `{Steve, hello}`

## 5. 执行器注册表边界（命令执行器不在本变更）

- [ ] 5.1 确认执行器注册表只落地 `dialog` 与 `replier` 两个实现，且不引入 `command` shaper
- [ ] 5.2 配置里写 `executor = command` → 按未知执行器报错拒绝（命令执行器由独立变更 `add-command-executor` 提供）

## 6. 文档与示例配置

- [ ] 6.1 新增示例配置文件（含 `[trigger:chat]` `@pi,@ai` 与 `[trigger:join]` 欢迎语；**不含任何提示词键**）
- [ ] 6.2 README 新增「配置文件」章节：格式说明、两条内置触发器示例、四级优先级、`replier` 模板占位符列表（并注明模板只用于发给玩家的输出）
- [ ] 6.3 README 明确**职责边界**：本程序是 AI 与游戏服务器之间的薄传输层，**不负责提示词**；pi 收到的固定格式为 `<player>: <text>`；定制 AI 行为请在 pi 侧配置
- [ ] 6.4 README 点名**命令执行器不在本变更**：由独立变更 `add-command-executor` 承担（默认关 + 白名单 + 审计 + 控制面），并预置其安全警告（RCON = 服务端控制台权限；AI 输出视为不可信）
- [ ] 6.5 README systemd 示例补 `--config`；已知边界补「限流 key 变更」与「pi 侧输入行格式变化」（design D3b）
- [ ] 6.6 `make && make test` 全绿（含新增用例，零告警，沿用 `-Wall -Wextra`）；确认现有 78 项单测全部保留通过

## 7. 端到端验证

- [ ] 7.1 无配置文件回归：`--prefix @pi` 启动，游戏内发 `@pi 你好`，游戏内表现与今天一致；日志确认 pi 输入行为 `Steve: 你好`
- [ ] 7.2 配置文件模式：bot 加入游戏收到欢迎语；发 `@ai hi` 走对话器并带回复前缀（验证多关键词路由）
- [ ] 7.3 `--log-level debug` 下每条命中行都能看到触发器名
- [ ] 7.4 边界验证：配置里写 `prompt_template` 或 `system` 键 → 启动即报错拒绝；写 `executor = command` → 报未知执行器拒绝
