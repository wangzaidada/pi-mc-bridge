# 实施清单：add-command-executor

前置：`add-trigger-executor-layers` 已落地（提供触发器表、执行器注册表、配置文件、`replier` 模板机制）。
本变更把其中的 `command` 执行器从「文本标记 + `command` shaper」改为「pi 自定义工具 + 本地控制 socket」。

## 1. 扩展路径解析与校验

- [ ] 1.1 在 `src/config.[ch]` 增加命令触发器配置键 `extension`（可选，值为路径或留空走自动查找）、
      `socket_path`（可选，默认 `<runtime_dir>/pi-mc-bridge.sock`），并为二者补文档化默认值
- [ ] 1.2 新增扩展路径解析函数（`src/control.[ch]` 或 `src/util.[ch]`）：显式 `extension` →
      `./.pi/extensions/mc-bridge.ts`（项目级）→ `~/.pi/agent/extensions/mc-bridge.ts`（用户级），
      返回**绝对路径**；全部未命中时返回失败并记录搜索过的路径
- [ ] 1.3 `command` 执行器启动时校验解析结果：文件存在且可读（`access(R_OK)`），否则拒绝启动并打印
      搜索过的全部路径（`design.md` C10）

## 2. 控制面（Unix domain socket）

- [ ] 2.1 新增模块 `src/control.[ch]`：socket 生命周期（`socket`/`bind`/`listen`/`accept`/`close`），
      `chmod 0600`，路径可配置，注册 atexit/信号清理以在退出时 `unlink` 套接字文件
- [ ] 2.2 处理启动时已存在的陈旧套接字：若为套接字文件则 `unlink` 后重建，非套接字文件则报错退出
- [ ] 2.3 实现按行读取请求：`{"player":...,"tool":...,"command":...}\n`，逐行处理，
      单条坏行不影响后续行（连接保持可用）
- [ ] 2.4 实现极简行分隔 JSON 的**读取**（取 `player`/`tool`/`command` 三个字符串字段，
      手写、无第三方库），缺字段或非法 JSON → 生成 `ok:false` 响应
- [ ] 2.5 实现响应的**序列化**并做 JSON 字符串转义（`"`、`\`、控制字符、换行），
      成功 `{"ok":true,"output":"..."}`，失败 `{"ok":false,"error":"..."}`
- [ ] 2.6 控制面默认不监听：仅当存在 `executor = command` 的触发器时才创建 socket

## 3. pi 工具模式调用

- [ ] 3.1 `src/pi_client.[ch]` 的 `build_argv` 支持工具模式变体：
      `--no-builtin-tools --tools mc_command -e <绝对路径>`，并保留 `--mode json --no-session`、
      可选 `--model`
- [ ] 3.2 默认路径保持 `--no-tools` 不变（`dialog` / `replier` 触发器）；仅 `command` 触发器走工具模式
- [ ] 3.3 工具模式**不追加**任何 system-prompt / 提示词文本，仍只传固定格式输入行（`-p` 单参数）
- [ ] 3.4 单测：断言默认 argv 含 `--no-tools`；工具模式 argv 含
      `--no-builtin-tools --tools mc_command -e <path>` 且不含任何提示词参数

## 4. 命令执行器接入控制面

- [ ] 4.1 实现 `command` 执行器 `init`：读取并校验三重启动条件——
      显式 `executor = command`、非空 `allow_commands`、该触发器**显式非空玩家 `allow`**（不继承全局），
      任一不满足 → 报错退出并指明触发器名
- [ ] 4.2 实现 `run`：进入工具模式调用 pi，并把 pi 循环中产生的每次工具调用路由到控制面处理
- [ ] 4.3 明确不做自由文本命令提取：pi 输出中的疑似命令行一律不执行（仅结构化工具调用可执行）

## 5. 白名单校验与 RCON 执行

- [ ] 5.1 实现 `allow_commands` 匹配：不以 `=` 开头 → 前缀匹配；以 `=` 开头 → 整条精确匹配
- [ ] 5.2 候选命令含换行或命令分隔符（`;`、`|`、`&`、`` ` ``）→ 一律拒绝（与白名单是否命中无关）
- [ ] 5.3 未命中白名单 → 不执行、`LOG_WARN` 记名被拒命令、回填 `{ok:false, error}`，
      并保持连接可继续
- [ ] 5.4 命中白名单 → 经既有 `rcon_command()` 执行，成功回填 `{ok:true, output}`，
      RCON 失败回填 `{ok:false, error}`
- [ ] 5.5 明确「服务器输出不再被当作命令执行」（响应文本原样回填，不做二次解析）
- [ ] 5.6 单测：前缀命中执行、`=` 精确拒绝不同的整条命令、越权拒绝、分隔符/换行拒绝

## 6. 扩展 shim

- [ ] 6.1 新增 `extensions/mc-bridge.ts`：注册**唯一**工具 `mc_command({ command })`，
      `execute` 用 `node:net` 连本地 socket，发一行 JSON 请求、读回一行 JSON 响应
- [ ] 6.2 成功时返回 `{content:[{type:"text",text:<output>}]}`；失败（`ok:false` 或连接错误）→ 抛出异常，
      使 pi 标记 `isError`
- [ ] 6.3 扩展内**不含任何策略**：不校验白名单、不改写命令、不丢弃命令；
      面向模型的工具说明（`description` / `promptGuidelines`）与 socket 路径取自环境/配置
- [ ] 6.4 README 增加安装说明：把 `extensions/mc-bridge.ts` 复制到 `~/.pi/agent/extensions/`（用户级）
      或项目的 `.pi/extensions/`（项目级）

## 7. 审计（经 replier 模板输出到命令行）

- [ ] 7.1 扩展 `dynbuf_append_template` 的占位符集合到
      `{player}`/`{text}`/`{trigger}`/`{tool}`/`{command}`/`{status}`/`{result}`
- [ ] 7.2 每次工具调用后用 `replier` 模板渲染一条审计行并写 **stdout**（桥接进程的命令行），
      固定前后缀 `[audit]` 便于 grep
- [ ] 7.3 审计行**不** tellraw 给玩家；玩家侧只保留 `dialog` 的正常回复
- [ ] 7.4 单测：模板占位符替换（含未知占位符原样保留、多字节玩家名）

## 8. 文档与示例

- [ ] 8.1 README/`docs/usage.md` 增加控制面说明：本地 socket（`0600`、仅属主可连、非 TCP）、
      扩展与 pi 必须同机同用户、多主机部署不支持
- [ ] 8.2 示例配置文件新增 `[trigger:cmd]`（`executor = command`，含 `allow_commands` 与显式玩家 `allow`）
      并**整段注释掉**，保证默认关闭
- [ ] 8.3 README 醒目安全警告：RCON = 服务端控制台权限；放行等于把 op 能力交给能触发该触发器的玩家；
      列出**永不建议放行**的命令族（`/execute` `/op` `/deop` `/ban` `/kick` `/stop` `/fill` `/setblock` `/reload`）
- [ ] 8.4 记录 pi 最低版本与「`--no-builtin-tools` 保留扩展工具」的实测结论，供 pi 升级后回归

## 9. 端到端验证

- [ ] 9.1 未启用命令触发器时：不创建 socket、pi 仍收 `--no-tools`、行为与既有版本一致
- [ ] 9.2 启用命令触发器（`allow_commands = /time`）：`/time set day` 执行成功且模型收到 `{ok:true, output}`
- [ ] 9.3 越权命令（`/give ...`）被拒：日志 `WARN`、审计记录成对出现、模型收到 `{ok:false, error}`
- [ ] 9.4 注入尝试（含换行/分隔符的命令）被拒且不执行
- [ ] 9.5 `make && make test` 全绿、零告警（沿用 `-Wall -Wextra`）
