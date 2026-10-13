## ADDED Requirements

### Requirement: Pi CLI invocation
The bridge SHALL invoke the Pi Coding Agent as a child process via `fork`+`execvp` using the configured command (default `pi`), passing the prompt through argv (not through a shell). The invocation SHALL use `--mode json` and `--no-session`, and SHALL append `--model <model>` when a model is configured. Because argv is used directly, the prompt SHALL NOT be interpreted by a shell and shell metacharacters in player input SHALL have no special effect.

#### Scenario: Prompt reaches pi as a single argument
- **WHEN** the prompt is `hello; rm -rf /`
- **THEN** pi receives it as exactly one argument value and no shell is spawned; the literal text is passed through unchanged

#### Scenario: Model flag only when configured
- **WHEN** no model is configured
- **THEN** the `--model` flag is not passed

#### Scenario: No interactive stdin
- **WHEN** the child process is spawned
- **THEN** its stdin is pointed at `/dev/null` so pi cannot block waiting for terminal input

### Requirement: Timeout and child reaping
The bridge SHALL enforce the configured timeout (default 60 s) on each pi invocation by reading the child's stdout with `poll`. On timeout it SHALL terminate the child (`SIGTERM`, then `SIGKILL` if needed) and reap it with `waitpid`, and SHALL report a distinct timeout error. The child SHALL never be left as a zombie.

#### Scenario: Timeout kills child
- **WHEN** pi does not finish within the timeout
- **THEN** the bridge kills the child, reaps it, and returns a timeout error distinguishable from other failures

#### Scenario: Completion within timeout
- **WHEN** pi exits within the timeout
- **THEN** the bridge reaps it and returns its captured stdout

#### Scenario: Large output
- **WHEN** pi's stdout output exceeds the initial read buffer
- **THEN** the bridge grows its capture buffer and reads to EOF via the pipe

### Requirement: NDJSON assistant text extraction
The bridge SHALL parse pi's stdout as newline-delimited JSON and extract the reply text from the **last** `message_end` event whose `message.role` is `assistant`. The `message.content` field is an **array of content blocks**; the bridge SHALL concatenate the `text` fields of blocks whose `type` is `text`, and SHALL ignore blocks of type `thinking`. If no assistant text block is found, the bridge SHALL report an error.

#### Scenario: Text block alongside thinking block
- **WHEN** the final assistant `message_end` contains `content:[{"type":"thinking",...},{"type":"text","text":"你好"}]`
- **THEN** the extracted reply is `你好` and the thinking text is not included

#### Scenario: Multiple assistant message_end events
- **WHEN** the stream contains more than one assistant `message_end` (e.g. after an auto-retry)
- **THEN** the reply from the last one is returned

#### Scenario: Escaped characters
- **WHEN** the text block contains JSON escapes such as `\"`, `\\`, `\n`, `你`
- **THEN** they are decoded into the actual characters before returning

#### Scenario: Error stop reason
- **WHEN** the assistant `message_end` has `stopReason:"error"` with an `errorMessage` and no text block
- **THEN** the bridge reports a distinct "AI error" result carrying the error message, and does not send an empty reply

#### Scenario: Non-JSON noise on stdout
- **WHEN** a stdout line is not valid JSON (e.g. a warning banner)
- **THEN** that line is skipped and parsing continues; it does not abort extraction
