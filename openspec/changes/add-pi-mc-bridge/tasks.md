## 1. 项目骨架

- [x] 1.1 建立 `src/`、`tests/`、`examples/` 目录与 `Makefile`（`-O2 -Wall -Wextra -pthread`，目标 `pi-mc-bridge` 与 `tests/run-tests`）
- [x] 1.2 写 `src/util.h/.c`：动态缓冲区（`dynbuf`）、按码点计数的 UTF-8 分段、JSON 字符串转义、本机时间戳日志、`xstrdup` 等
- [x] 1.3 写 `README.md`（依赖、编译、配置表、运行示例、systemd 单元、pi 实测版本说明）

## 2. 配置模块

- [x] 2.1 定义 `Config` 结构体（照原文档 4.2，追加 `reply_prefix`、`poll_interval_ms`、`queue_max`）
- [x] 2.2 解析命令行（`--rcon-host/--rcon-port/--rcon-pass/--log-file/--prefix/--reply-prefix/--pi-cmd/--pi-model/--timeout/--cooldown/--max-len/--allow/--poll-ms/--queue-max/--help`）
- [x] 2.3 读取环境变量并实现优先级 命令行 > 环境变量 > 默认值
- [x] 2.4 校验必填项与数值范围，失败打印用法并以非零码退出；实现 `--help`
- [x] 2.5 白名单解析（逗号分隔）与玩家名匹配函数

## 3. 日志监控模块

- [x] 3.1 `logwatcher_init/free`：打开文件、记录 inode、定位到末尾
- [x] 3.2 `logwatcher_poll`：`stat` 检测轮转（inode 变）/ 截断（size < offset）/ 文件缺失
- [x] 3.3 按行切分新增内容，保留跨轮询的不完整行；超长行丢弃并告警
- [x] 3.4 通过回调把完整行交给解析层；轮询间隔来自配置

## 4. 消息解析模块

- [x] 4.1 `parse_chat_line`：容忍时间戳/线程标签，定位 `<名字>`，校验名字 `[A-Za-z0-9_]+`
- [x] 4.2 前缀大小写不敏感匹配（必须是名字后的第一个 token），提取并 trim 问题
- [x] 4.3 处理 CRLF 行尾；消息内含 `<` `>` 不受影响

## 5. 任务队列模块

- [x] 5.1 `queue_init/shutdown/free`，pthread mutex + cond
- [x] 5.2 `queue_push`：有界 FIFO，满时返回明确失败码
- [x] 5.3 `queue_pop`：阻塞，shutdown 时返回独立码唤醒消费者

## 6. Pi 客户端模块

- [x] 6.1 `pi_ask`：`fork`+`execvp` 构造 `pi --mode json --no-session --no-tools [--model m] -p <prompt>`，stdin 指 `/dev/null`，捕获 stdout/stderr 管道
- [x] 6.2 `poll` + 超时读取，超时时 `SIGTERM`→`SIGKILL`，`waitpid` 回收；输出超缓冲自动扩容
- [x] 6.3 手写 NDJSON 扫描器：逐行解析，找**最后一条** assistant `message_end`
- [x] 6.4 从 `content` 数组拼接 `type=="text"` 的 `text`，忽略 `thinking`；处理 `\" \\ \n \uXXXX` 等转义
- [x] 6.5 错误分类：超时 / 无文本块 / `stopReason=="error"`（带 `errorMessage`）分别返回可区分的错误码

## 7. RCON 客户端模块

- [x] 7.1 打包/解包函数（小端 int32，长度 = 4+4+len+1+1，尾部两个 0）
- [x] 7.2 `rcon_connect`：TCP connect + 类型 3 登录握手，校验返回 request id
- [x] 7.3 `rcon_send_command`：类型 2 发送，循环读响应直到匹配 request id；容忍分片空包
- [x] 7.4 `SO_RCVTIMEO`/`SO_SNDTIMEO` 超时；断线重连并重试一次
- [x] 7.5 玩家名合法性校验（`[A-Za-z0-9_]+`）供 worker 复用

## 8. 工作线程模块

- [x] 8.1 线程入口：循环 `queue_pop`，shutdown 时退出
- [x] 8.2 限流表（128 槽线性探测，记录上次接受时间），冷却内丢弃并记录
- [x] 8.3 调用 `pi_ask`，按错误码向玩家发送超时/失败提示
- [x] 8.4 构造 `tellraw <player> {"text":"<转义>"}`，按 UTF-8 边界分段逐段发送
- [x] 8.5 发送失败时记录并（在重连失败后）降级为日志告警

## 9. 主程序

- [x] 9.1 `main`：解析配置 → 初始化日志 → 建队列 → 建 RCON/Pi 客户端 → 建日志监控（定位末尾）→ 起工作线程 → 进入监控循环
- [x] 9.2 `SIGINT`/`SIGTERM` 处理：优雅关停（shutdown 队列 → join 线程 → 断开 RCON）
- [x] 9.3 白名单与冷却在入队前过滤，队列满时通过 RCON 告知玩家

## 10. 测试

- [x] 10.1 单元测试：`parse_chat_line`（正常/非聊天/大小写/空 prompt/CRLF/含尖括号）
- [x] 10.2 单元测试：JSON 转义与 pi NDJSON 解析（thinking+text 混排、多条 message_end、转义、错误 stopReason、噪声行）
- [x] 10.3 单元测试：RCON 打包/解包字节级断言；UTF-8 分段不切断多字节
- [x] 10.4 单元测试：任务队列多线程入队/出队与 shutdown
- [x] 10.5 `tests/run-tests` 一键编译并运行全部单测，`make test` 接入

## 11. 联调环境与端到端验证

- [x] 11.1 搭一个开启 RCON 的原版 MC 服务端，写好 `server.properties`（`enable-rcon`、`rcon.password`）与启动脚本
- [x] 11.2 用真实 pi（网关 provider）跑通：游戏内 `@pi 你好` → 桥接 → pi → tellraw 回游戏
- [x] 11.3 验证日志轮转（重命名 latest.log）与 RCON 断线（重启服务端）后的自恢复
- [x] 11.4 验证限流、超出队列、超长回复分段三条边界行为
- [x] 11.5 把联调步骤与结论写进 `README.md` 的「本地联调」一节
