## ADDED Requirements

### Requirement: Chat line recognition
The parser SHALL recognize Minecraft server log lines of the form `[HH:MM:SS] [Thread/LEVEL]: <PlayerName> message` and extract the player name and message body. Player names SHALL be accepted only when composed of `[A-Za-z0-9_]` characters.

#### Scenario: Standard chat line
- **WHEN** the input is `[12:34:56] [Server thread/INFO]: <Steve> @pi 帮我写个挖矿脚本`
- **THEN** the parser reports a match with player `Steve` and message `@pi 帮我写个挖矿脚本`

#### Scenario: Non-chat line
- **WHEN** the input is `[12:34:56] [Server thread/INFO]: Steve joined the game`
- **THEN** the parser reports no chat message

#### Scenario: Angle brackets inside the message
- **WHEN** the input is `[12:34:56] [Server thread/INFO]: <Alex> @pi 这个 <tag> 是什么`
- **THEN** the parser reports player `Alex` and message `@pi 这个 <tag> 是什么`

### Requirement: Prefix detection and prompt extraction
The parser SHALL match the configured trigger prefix (default `@pi `) case-insensitively immediately after the player name and the separating space, and SHALL return the remaining text as the prompt. Leading and trailing whitespace of the prompt SHALL be trimmed.

#### Scenario: Case-insensitive prefix
- **WHEN** the message body is `@PI hello` with prefix configured as `@pi `
- **THEN** the parser reports a match with prompt `hello`

#### Scenario: Prefix not at start
- **WHEN** the message body is `帮我看看 @pi 这个` (prefix not at the beginning)
- **THEN** the parser reports no match (the trigger MUST be the first token after the player name)

#### Scenario: Empty prompt
- **WHEN** the message body is exactly `@pi ` with nothing after it
- **THEN** the parser reports a match with an empty prompt, and the bridge treats this as a usage hint rather than forwarding it to the AI

### Requirement: Whitespace handling in log lines
The parser SHALL tolerate the optional leading timestamp and thread/level tags and SHALL handle the trailing `\r` of CRLF-terminated logs.

#### Scenario: CRLF line
- **WHEN** the input line ends with `\r\n`
- **THEN** the parsed prompt contains no trailing `\r`
