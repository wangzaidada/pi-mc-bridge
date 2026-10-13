## ADDED Requirements

### Requirement: End-to-end task handling
A worker SHALL take tasks from the queue, invoke pi with the task's prompt, and deliver the resulting text to the requesting player via RCON, prefixing the reply so players can tell AI output apart from chat.

#### Scenario: Happy path
- **WHEN** the queue contains a task for player `Steve` with prompt `你好`
- **THEN** the worker calls pi, receives the reply text, and sends it to `Steve` through RCON

#### Scenario: Known failure replies
- **WHEN** pi times out, pi fails, or pi returns an error stop reason
- **THEN** the worker sends the player a short human-readable failure notice (e.g. a timeout notice) instead of silence, and logs the underlying reason

### Requirement: Per-player rate limiting
The worker SHALL enforce a per-player cooldown (default 10 s) on accepted requests; a request from a player within their cooldown window SHALL be dropped with a logged notice rather than being sent to pi.

#### Scenario: Cooldown blocks a rapid repeat
- **WHEN** player `Steve` successfully triggers `@pi` and sends another `@pi` 3 s later with a 10 s cooldown
- **THEN** the second request is not forwarded to pi and the drop is logged

#### Scenario: Cooldown expires
- **WHEN** the same player sends another request after the cooldown has elapsed
- **THEN** it is accepted normally

### Requirement: Unicode-safe message segmentation
Replies longer than the configured maximum segment length (default 200 characters) SHALL be split into multiple `/tellraw` commands sent in order. Splitting SHALL occur on UTF-8 character boundaries and SHALL NOT split a multi-byte character.

#### Scenario: Long ASCII reply
- **WHEN** the reply is 450 characters with max length 200
- **THEN** three tellraw commands are sent (200 + 200 + 50)

#### Scenario: Multi-byte boundary
- **WHEN** a cut position would fall inside a multi-byte UTF-8 sequence
- **THEN** the split moves to the nearest preceding character boundary so no segment contains a partial code point

### Requirement: Tellraw construction and injection safety
The worker SHALL build the reply command as `tellraw <player> {"text":"<escaped>"}` where the player name has been validated against `[A-Za-z0-9_]+` and the text is JSON-escaped (`"`, `\`, control characters, and newlines escaped). Player-controlled content SHALL never be concatenated into the command unescaped.

#### Scenario: Quote in reply
- **WHEN** the reply text contains a double quote and a backslash
- **THEN** the emitted JSON escapes them as `\"` and `\\` so the command remains valid JSON

#### Scenario: Newline in reply
- **WHEN** the reply contains newline characters
- **THEN** they are escaped (`\n`) so the tellraw JSON stays on one logical line

#### Scenario: Invalid player name rejected
- **WHEN** a task carries a player name containing characters outside `[A-Za-z0-9_]`
- **THEN** the worker refuses to build the command and logs an error

### Requirement: Allowed-player filtering
When an allow-list is configured, requests from players not on the list SHALL be ignored (with a logged notice); when no list is configured all players are allowed.

#### Scenario: Player not on the list
- **WHEN** the allow-list is `Steve,Alex` and player `Herobrine` sends `@pi hi`
- **THEN** the request is dropped before being sent to pi
