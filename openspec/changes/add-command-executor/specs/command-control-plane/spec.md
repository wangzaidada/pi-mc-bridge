## ADDED Requirements

### Requirement: Local control socket
When a `command` trigger is enabled, the bridge SHALL listen on a local Unix domain socket to receive
tool calls from the pi extension shim. The socket path SHALL be configurable with a documented default.
The socket SHALL be chmod `0600` so that only the owning user can connect, and SHALL be removed on
shutdown so a stale socket does not block restart. The bridge SHALL NOT listen on a TCP port for this.

#### Scenario: Socket is user-only
- **WHEN** the command executor is enabled
- **THEN** the control socket exists with permissions `0600` and is not reachable by other users

#### Scenario: Stale socket cleaned up
- **WHEN** the bridge restarts and a previous socket file remains at the configured path
- **THEN** the bridge removes or reuses it and listening succeeds

#### Scenario: Command executor disabled by default
- **WHEN** no trigger configures `executor = command`
- **THEN** no control socket is created

### Requirement: Control protocol
The socket SHALL use line-delimited JSON: one request object per line and one response object per line.
A request SHALL carry at least the requesting `player`, the `tool` name, and the `command` string.
A response SHALL carry `ok` and, on success, the server `output`; on failure it SHALL carry `ok:false`
and an `error` string. The bridge SHALL treat the request's `command` field as untrusted input and SHALL
validate it exactly as a freshly received command, regardless of what the extension or model claims.

#### Scenario: Successful round trip
- **WHEN** the extension sends `{"player":"Steve","tool":"mc_command","command":"/time set day"}`
- **THEN** the bridge replies with a JSON object containing `ok:true` and the server output text

#### Scenario: Request command is untrusted
- **WHEN** a request arrives with a `command` that is not on the allow-list
- **THEN** the bridge replies `ok:false` with an error and does not execute the command

#### Scenario: Malformed request rejected
- **WHEN** a line is not valid JSON or lacks required fields
- **THEN** the bridge replies `ok:false` and the connection remains usable for further lines

### Requirement: Extension shim and single-tool injection
The repository SHALL ship a pi extension source file `extensions/mc-bridge.ts` that registers exactly one
custom tool `mc_command({ command })`, forwards each call to the bridge over the control socket, and returns
the bridge's result as the tool result (`content` on success; throw so pi marks `isError` on failure). The
extension SHALL contain no command policy: it SHALL NOT allow-list, rewrite, or drop commands. The extension
SHALL be installed into pi's own extensions directory so it sits alongside other pi extensions: the bridge
SHALL resolve the extension path preferring the explicit `extension` config value, then the project-level
`.pi/extensions/` path, then the user-level `~/.pi/agent/extensions/` path. The bridge SHALL load the
resolved file explicitly via `-e <absolute path>` (not relying on pi auto-discovery) and SHALL verify the
file exists and is readable at startup, reporting the searched paths on failure.

#### Scenario: One tool registered
- **WHEN** the extension is loaded
- **THEN** it exposes only `mc_command` and does not let the model call any other command tool

#### Scenario: Policy lives in the bridge
- **WHEN** the model calls `mc_command` with a command outside the allow-list
- **THEN** the extension forwards it unchanged and the bridge rejects it; the extension makes no decision

#### Scenario: Extension is loaded explicitly from a resolved path
- **WHEN** the bridge invokes pi in tool mode
- **THEN** the argv contains `-e <absolute path>` naming the resolved extension file, regardless of whether
  the file was found via the project-level `.pi/extensions/` path or the user-level `~/.pi/agent/extensions/` path

#### Scenario: Missing extension refused at startup
- **WHEN** a command trigger is enabled but no extension file is found at the explicit path, the project-level
  `.pi` path, or the user-level `~/.pi/agent` path
- **THEN** the bridge refuses to start and names the paths it searched

### Requirement: No prompt text introduced by the bridge
Enabling the tool mode SHALL NOT cause the bridge to pass any prompt or system-prompt text to pi. Model-facing
explanations of the tool SHALL live in the extension (tool `description` / prompt metadata). The bridge SHALL
continue to send only the fixed-format input line and its arguments.

#### Scenario: Tool mode adds no prompt argument
- **WHEN** the bridge invokes pi for a command trigger in tool mode
- **THEN** the argv contains no system-prompt argument and no prompt text beyond the fixed-format input line
