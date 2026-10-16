## ADDED Requirements

### Requirement: Command executor is opt-in and guarded
The `command` executor SHALL be active only when a trigger explicitly sets `executor = command`. Its
construction SHALL additionally require (a) a non-empty `allow_commands` list and (b) a non-empty,
explicitly configured player `allow` list for that trigger; the command executor SHALL NOT inherit an
all-players default. If any prerequisite is absent, configuration load SHALL fail with an error naming
the trigger and the program SHALL exit non-zero, so the executor can never run unguarded.

#### Scenario: Missing allow_commands refused
- **WHEN** a trigger sets `executor = command` but no `allow_commands`
- **THEN** configuration load fails, names the trigger, and the program exits non-zero

#### Scenario: Missing player allow-list refused
- **WHEN** a command trigger has no explicit player `allow` list (or an empty one)
- **THEN** configuration load fails and the program exits non-zero, rather than defaulting to all players

#### Scenario: Disabled by default
- **WHEN** no trigger sets `executor = command`
- **THEN** no command execution path is active and the shipped example configuration has the command trigger commented out

### Requirement: Allow-list matching
Each candidate command SHALL be matched against the configured `allow_commands` entries. An entry not
prefixed with `=` SHALL match as a **prefix** (so `/time` accepts `/time set day`). An entry prefixed with
`=` SHALL match only the **exact whole command** (so `=/time set day` accepts only that string). A command
matching no entry SHALL be rejected with a logged warning, SHALL NOT be sent to the server, and SHALL be
reported back to the caller as a failure.

#### Scenario: Prefix entry matches
- **WHEN** the allow-list is `/time` and the command is `/time set day`
- **THEN** the command is accepted

#### Scenario: Exact entry matches only the whole command
- **WHEN** the allow-list is `=/time set day` and the command is `/time set night`
- **THEN** the command is rejected

#### Scenario: No match rejected
- **WHEN** the allow-list is `/time` and the command is `/give Steve diamond 64`
- **THEN** the command is not executed, a warning naming the rejected command is logged, and the caller receives a failure result

### Requirement: Separator and newline rejection
A candidate command containing an embedded newline or a command separator (`;`, `|`, `&`, backtick) SHALL
be rejected and SHALL NOT be executed, independent of allow-list matching, to prevent command chaining.

#### Scenario: Chained command rejected
- **WHEN** a candidate is `/time set day; /op Steve`
- **THEN** the candidate is rejected and nothing is executed

#### Scenario: Embedded newline rejected
- **WHEN** a candidate contains a newline character
- **THEN** the candidate is rejected and nothing is executed

### Requirement: Execution and status feedback
An accepted command SHALL be executed through the existing RCON command path, and the execution result
SHALL be returned to the caller as a structured status: `ok:true` with the server `output` on success, or
`ok:false` with an `error` on failure. This result SHALL be delivered as the tool result so that pi feeds
it back to the model, which can then continue or explain the outcome.

#### Scenario: Result fed back to the model
- **WHEN** an accepted command executes successfully
- **THEN** the tool result carries `ok:true` and the server output, and pi makes the outcome available to the model on its next turn

#### Scenario: RCON failure surfaces as failure
- **WHEN** the RCON call fails or times out
- **THEN** the caller receives `ok:false` with an error and the failure is logged

### Requirement: Audit to the command line via the replier
Each tool invocation SHALL produce an audit record rendered through the `replier` template mechanism and
written to the bridge's standard output (the command line), containing at least the requesting player, the
command text, and the execution status. Audit records SHALL NOT be sent to the game (no tellraw), and
player-visible replies SHALL remain the dialog executor's normal replies.

#### Scenario: Audit line in stdout
- **WHEN** the model runs `/time set day` for player `Steve`
- **THEN** a line identifying player, command, and status appears on the bridge's stdout

#### Scenario: Audit not shown to players
- **WHEN** an audit record is produced
- **THEN** no tellraw carrying the audit content is sent to the game

### Requirement: Server output is not re-executed
Text returned by the server from an executed command SHALL be treated as output only and SHALL NOT be
executed as a further command.

#### Scenario: Slash-leading output not executed
- **WHEN** the server replies to an executed command with text beginning with `/`
- **THEN** that text is not executed as a command
