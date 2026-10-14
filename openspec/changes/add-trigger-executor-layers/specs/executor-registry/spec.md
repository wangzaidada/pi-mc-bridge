## ADDED Requirements

### Requirement: Executor registry
The program SHALL resolve executors by name from a registry containing at least `dialog` and `replier`. An executor SHALL expose an init step that reads its parameters from the trigger's configuration keys and an instance, a run step that handles one matched event, and a free step. An unknown executor name SHALL be rejected at configuration load.

#### Scenario: Unknown executor rejected
- **WHEN** a trigger sets `executor = shell`
- **THEN** configuration load fails with an error naming the unknown executor and the program exits non-zero

#### Scenario: Executor is bound at build time
- **WHEN** the trigger table is built
- **THEN** each trigger resolves its executor once and stores the resolved pointer, so the per-line path performs no name lookup

### Requirement: Default replier executor
The `replier` executor SHALL construct a reply from a configured template, substitute placeholders `{player}`, `{text}`, and `{trigger}`, and send it to the player through RCON without invoking the AI. Unknown placeholders SHALL be preserved verbatim. A `replier` trigger without a template SHALL be rejected at configuration load.

#### Scenario: Join welcome
- **WHEN** trigger `join` uses `executor = replier` with template `欢迎 {player} 加入服务器！` and the event player is `YYM`
- **THEN** the player receives `欢迎 YYM 加入服务器！` and no AI process is spawned

#### Scenario: Missing template rejected
- **WHEN** a trigger uses `executor = replier` but sets no template
- **THEN** configuration load fails with an error naming the trigger

#### Scenario: Unknown placeholder preserved
- **WHEN** the template is `hi {player} {unknown}` and the player is `Steve`
- **THEN** the reply is `hi Steve {unknown}`

### Requirement: Dialog executor
The `dialog` executor SHALL build pi's input line in a fixed format `<player>: <text>` (a literal format defined in code, not configurable), invoke pi with it, extract the reply text through the configured reply shaper, and deliver it to the player with the configured reply prefix, subject to the existing maximum-length segmentation. pi failures SHALL produce a short human-readable notice to the player and a logged underlying reason, as today.

#### Scenario: Fixed input format
- **WHEN** trigger `chat` uses `executor = dialog` and the event is player `Steve`, text `你好`
- **THEN** pi is invoked with the input line `Steve: 你好` and the reply is sent to `Steve` with the reply prefix

#### Scenario: Player name identifies the speaker
- **WHEN** two different players trigger the same dialog trigger
- **THEN** each pi invocation carries that player's name at the start of the input line, with no other prompt scaffolding added by the bridge

#### Scenario: Dialog pi failure
- **WHEN** pi times out for a dialog trigger
- **THEN** the player receives a short timeout notice and the underlying reason is logged, without crashing the worker

### Requirement: No prompt configuration in the bridge
The bridge SHALL NOT expose any prompt, system-prompt, persona, or context configuration. A configuration file containing a prompt-related key SHALL be rejected as an unknown key. Requested AI behavior SHALL be configured on the pi side, which the bridge does not inspect.

#### Scenario: Prompt key rejected
- **WHEN** a trigger section contains `prompt_template = {player}说: {text}` or `system = ...`
- **THEN** configuration load fails with an error naming the offending key

#### Scenario: No system prompt passed to pi
- **WHEN** the bridge invokes pi
- **THEN** it passes only the fixed-format input line and its existing arguments (`--mode json --no-session --no-tools`, optional `--model`), with no system-prompt argument

#### Scenario: Replier template is output-only
- **WHEN** a `replier` trigger defines a `template`
- **THEN** the template is used only to compose the message sent to the player and is never sent to pi
