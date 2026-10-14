## MODIFIED Requirements

### Requirement: Pi CLI invocation
The bridge SHALL invoke the Pi Coding Agent as a child process via `fork`+`execvp` using the configured
command (default `pi`), passing the prompt through argv (not through a shell). The invocation SHALL use
`--mode json` and `--no-session`, and SHALL append `--model <model>` when a model is configured. Because
argv is used directly, the prompt SHALL NOT be interpreted by a shell and shell metacharacters in player
input SHALL have no special effect. By default the invocation SHALL pass `--no-tools`. When a trigger uses
the `command` executor's tool mode, the invocation SHALL instead pass `--no-builtin-tools --tools mc_command`
together with an explicit `-e <extension>` loading the bridge's extension shim, so that only the `mc_command`
tool is available and the built-in read/bash/edit/write tools are disabled; in that mode the bridge SHALL add
no prompt or system-prompt text.

#### Scenario: Prompt reaches pi as a single argument
- **WHEN** the prompt is `hello; rm -rf /`
- **THEN** pi receives it as exactly one argument value and no shell is spawned; the literal text is passed through unchanged

#### Scenario: Model flag only when configured
- **WHEN** no model is configured
- **THEN** the `--model` flag is not passed

#### Scenario: No interactive stdin
- **WHEN** the child process is spawned
- **THEN** its stdin is pointed at `/dev/null` so pi cannot block waiting for terminal input

#### Scenario: Default invocation disables all tools
- **WHEN** a trigger uses the `dialog` or `replier` executor
- **THEN** the invocation passes `--no-tools` and offers no tools to the model

#### Scenario: Command trigger runs in tool mode
- **WHEN** a trigger uses the `command` executor
- **THEN** the invocation passes `--no-builtin-tools --tools mc_command -e <extension>` so only the `mc_command` tool is enabled

#### Scenario: Tool mode adds no prompt text
- **WHEN** the invocation is in tool mode
- **THEN** the argv contains no system-prompt argument and no prompt text beyond the fixed-format input line
