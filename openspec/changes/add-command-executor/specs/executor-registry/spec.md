## MODIFIED Requirements

### Requirement: Command executor (opt-in)
The `command` executor SHALL be constructed only when a trigger explicitly configures `executor = command`,
and SHALL be inactive by default with the shipped example configuration keeping the command trigger
commented out. When active it SHALL open a local control socket, cause that trigger's pi invocation to run
in tool mode with only the `mc_command` extension tool enabled, and handle each tool call by validating the
requested command against the mandatory allow-list before executing it through the existing RCON command
path. It SHALL NOT parse commands out of pi's free text.

#### Scenario: Allow-list and player allow-list are mandatory
- **WHEN** a trigger uses `executor = command` but sets no `allow_commands`, or no explicit non-empty player allow-list
- **THEN** configuration load fails with a clear error and the program exits non-zero, so the executor can never run unguarded

#### Scenario: Not enabled by default
- **WHEN** no trigger configures `executor = command`
- **THEN** no control socket is opened, no tool is offered to pi, and the shipped example configuration has the command trigger commented out

#### Scenario: No free-text command extraction
- **WHEN** a command trigger's pi output contains a line that looks like a command
- **THEN** it is not treated as a command; commands arrive only through the structured `mc_command` tool call

### Requirement: Command allow-list enforcement
Every candidate command SHALL be accepted only if it matches one of the configured `allow_commands` entries:
an entry without a leading `=` SHALL match as a prefix, and an entry with a leading `=` SHALL match only the
exact whole command. A candidate containing an embedded newline or a command separator SHALL be rejected
regardless of allow-list matching. A rejected candidate SHALL be discarded with a logged warning and SHALL NOT
be sent to the server. AI output SHALL be treated as untrusted input, and enforcement SHALL occur in the bridge
so that neither the extension nor the model can bypass it.

#### Scenario: Prefix entry executes
- **WHEN** the allow-list is `/time` and the candidate is `/time set day`
- **THEN** the command is executed and its result is returned to the caller

#### Scenario: Exact entry rejects a different whole command
- **WHEN** the allow-list is `=/time set day` and the candidate is `/time set night`
- **THEN** the command is not executed and a warning is logged naming the rejected command

#### Scenario: Command outside the allow-list rejected
- **WHEN** the allow-list is `/time` and the candidate is `/give Steve diamond 64`
- **THEN** the command is not executed and a warning is logged naming the rejected command

#### Scenario: Command separator or newline rejected
- **WHEN** a candidate contains a newline or a command separator
- **THEN** the candidate is rejected and not executed

#### Scenario: Audit logging
- **WHEN** a command is executed
- **THEN** an audit record is written, containing the requesting player, the command text, and the execution result

## REMOVED Requirements

### Requirement: Wrapper text in a command reply
**Reason**: Superseded by the structured control plane: commands no longer come from parsing server or model
text, so the "wrapper text" concern is instead covered by the `command-execution` capability's
"Server output is not re-executed" requirement.
**Migration**: No behavior change; the guarantee is retained under the `command-execution` capability.
