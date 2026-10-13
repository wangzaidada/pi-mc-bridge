## Context

原设计文档（用户提供）已经给出了模块划分、协议格式与数据结构。本设计文档不重复它，只记录在**实测 pi 输出**与**本机环境**之后需要偏离或补充文档的地方，以及几个有取舍的技术决策。

已实测确认的接口事实（2026-10-13）：

1. `pi --mode json --no-session --no-tools --model <provider>/<model> -p "<prompt>"` 退出码 0，stdout 为 NDJSON。
2. `content` 是**块数组**，不是字符串 —— 这是原设计文档 4.6 节的主要错误。真实样本：
   ```json
   {"type":"message_end","message":{"role":"assistant","content":[
     {"type":"thinking","thinking":"...","thinkingSignature":"reasoning_content"},
     {"type":"text","text":"你好"}
   ],"stopReason":"stop", ...}}
   ```
   若按文档写「提取 `"content":"..."`」，对 block 数组会解析失败或抓错内容。
3. 失败时 assistant 的 `message_end` 是 `content:[]` + `stopReason:"error"` + `errorMessage`，且 pi 会自己 auto-retry（`auto_retry_start`），因此流里可能出现**多条** assistant `message_end` —— 必须取最后一条。
4. 本机 pi 默认 provider 无凭据，已通过扩展 `~/.pi/agent/extensions/mc-gateway.ts` 注册自建 OpenAI 兼容网关（`deepseek/deepseek-v4.1-flash`）。桥接层不感知凭据。

约束：Linux 优先；常驻内存目标 < 5 MB；不引入第三方 C 库；遵循原文档「单进程 + 多线程」拓扑（不改成多进程 broker）。

## Goals / Non-Goals

**Goals:**
- 按原设计文档实现 9 个模块 + Makefile，Linux 上编译即用。
- pi 的输出解析严格对齐上面实测的 NDJSON 结构，并能容错非 JSON 噪声行。
- 日志轮转 / RCON 断线 / pi 超时三类异常可自恢复，不需要重启进程。
- 核心纯函数（解析、JSON 转义、UTF-8 分段、RCON 打包、pi NDJSON 解析）可脱离真实服务端做单元测试。

**Non-Goals:**
- 不做 Windows / macOS 支持（原文档列为未来扩展）。
- 不做多轮对话上下文持久化、不做 HTTP API 替代 pi CLI。
- 不引入第三方 JSON 库；不追求完整 JSON 兼容，只解析 pi 输出的必要字段。
- 不改动 MC 服务端任何文件、不改动 pi 全局配置。

## Decisions

### D1. 手写极简 NDJSON 解析，而非引入 JSON 库
pi 的输出是「每行一个 JSON 对象」，我们只关心固定路径 `type` / `message.role` / `message.content[].type` / `message.content[].text` / `message.stopReason` / `message.errorMessage`。引入 cJSON 会增加依赖与内存占用（与 <5MB 目标冲突）。因此实现一个约 150 行的**按键扫描器**：定位 `"key":` 后按 JSON 规则取字符串值（处理 `\" \\ \/ \b \f \n \r \t \uXXXX`），够用且可单测。
- 备选：cJSON / jsmn —— 依赖或额外代码量，且 cJSON 也是手写生态。选扫描器。

### D2. 取最后一条 assistant 的 message_end，取 text 块拼接
如上「已实测」第 2、3 条。`thinking` 块必须丢弃 —— 否则会把模型的思考过程（英文、含内部推理）发给玩家。
- 备选：取 `message_update` 的 `text_delta` 流式拼接 —— 更复杂且需要处理重试导致的重复 delta，放弃。

### D3. RCON 用阻塞 socket + SO_RCVTIMEO，而非非阻塞 + poll
原文档 4.7 要求「非阻塞超时」。实际握手与命令往返都是小包且时序固定，用阻塞 socket 配 `SO_RCVTIMEO`/`SO_SNDTIMEO` 即可获得超时能力，代码量远小于自管 poll 状态机。
- 注意：真实服务端会把响应**分成两个包**（第二个为空）发回，读取循环必须在匹配到期望 request id 后才停止，并保持「读到空包就继续读」的行为。
- 备选：非阻塞 + poll —— 收益是能同时管多个连接，但本程序只有 1 个工作线程 1 个连接，无收益。

### D4. 单工作线程 + 单 RCON 长连接
原文档 6.2 的取舍照抄：单工作线程串行处理，避免并发调用 pi 打爆内存（本机 14 GiB 长期吃紧）；RCON 长连接省握手。工作线程数留常量便于未来加锁后扩展。

### D5. 限流用固定容量开放寻址表
玩家数少，用 128 槽的线性探测表（key=玩家名，value=上次接受时间）。满时按 LRU 近似（记录插入序号）淘汰最旧的一条。避免引入哈希表实现。

### D6. UTF-8 分段按码点边界回退
分段时从目标位置向前回退到非续接字节（`(b & 0xC0) != 0x80`）处切断，保证不切断多字节字符。段长按**码点**计（1 个非续接字节 = 1 个字符），与 MC 的显示宽度近似。

### D7. 日志轮转检测用 inode + size 双判据
每次 poll 先 `stat` 路径：inode 变 → 重新 open 并 offset=0；inode 同但 size < offset → 视为截断，offset=0；文件不存在 → 记一次警告并退避重试（不刷屏）。

### D8. 队列有界 + 满时告知玩家
原文档压力测试一节要求「队列不会无限增长」。默认容量 64，满时 `queue_push` 返回失败，调用方通过 RCON 发一条「当前请求过多，请稍后再试」。这样丢弃是可见的，不静默。

## Risks / Trade-offs

- **pi 的 NDJSON schema 是非稳定接口**（`content` 块结构可能随版本变）→ 缓解：解析层对未知块类型一律跳过，只认 `type=="text"`；`specs/pi-invocation` 把结构写成可测场景；README 记录实测版本 `@mariozechner/pi-coding-agent 0.73.1`。
- **手写 JSON 扫描器不处理嵌套转义以外的复杂情况**（如 `content` 中出现 `]` 在字符串内）→ 缓解：扫描字符串时正确处理转义，`[...]` 数组边界只在字符串外判定；单测覆盖 `]` 出现在 text 内的用例。
- **`--no-tools` 与未来 pi 参数变化** → 参数集中在 `pi_client.c` 一处构造，便于改。
- **协程/线程数固定为 1**：吞吐受 pi 延迟限制（实测单次 2–5 s）→ 缓解：在设计上保留多 worker 的注释钩子；真正的扩容需要给 RCON 客户端加锁（原文档 6.2 已指出）。
- **RCON 明文密码**（协议本身如此）→ 缓解：仅通过命令行/环境变量传入，不落盘；文档建议桥接程序跑在受限用户下。
- **`/tellraw` 文本不经过 MC 的聊天长度限制**，但过长会刷屏 → 缓解：默认每段 200 字符，超过分段。

## Migration Plan

全新程序，无迁移。部署即 systemd 服务（原文档 8.4）。回滚 = 停止服务。

## Open Questions

- 是否需要把回复前缀（如 `[AI]`）做成可配置项？当前**先做成可配置**（`--reply-prefix`，默认 `[AI] `），比原文档更保守。
- 多工作线程是否要做？当前不做，等单线程吞吐成为瓶颈再说。
