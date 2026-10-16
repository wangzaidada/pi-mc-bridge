## ADDED Requirements

### Requirement: Input-side extraction registry
The program SHALL resolve extractors by name from a registry containing at least `chat` and `keyword`. An extractor SHALL accept a trigger and a log line, and on a match SHALL produce an event carrying an owning player name and an owning message text. An unknown extractor name SHALL be rejected at configuration load.

#### Scenario: Unknown extractor rejected
- **WHEN** a trigger sets `extractor = regex`
- **THEN** configuration load fails with an error naming the unknown extractor and the program exits non-zero

#### Scenario: Extraction produces player and text
- **WHEN** the `chat` extractor matches the line `<Steve> @pi 你好` via keyword `@pi`
- **THEN** the produced event has player `Steve` and text `你好` (the matched keyword is consumed, not part of the text)

### Requirement: Chat extractor semantics
The `chat` extractor SHALL reuse the existing chat-line parsing behavior, accepting lines of the form `[HH:MM:SS] [Thread/LEVEL]: <PlayerName> <keyword> <body>` with player names restricted to `[A-Za-z0-9_]{1,16}`, tolerating optional bracketed tags between the prefix and the speaker, and returning the text after the matched keyword as the message text. A line without a valid speaker name SHALL NOT match.

#### Scenario: Standard chat trigger
- **WHEN** the line is `[12:34:56] [Server thread/INFO]: <Steve> @pi 帮我写个脚本` matching keyword `@pi`
- **THEN** the event has player `Steve` and text `帮我写个脚本`

#### Scenario: Bracketed tag before the speaker
- **WHEN** the line is `[12:34:56] [Server thread/INFO]: [Not Secure] <Steve> @pi hi`
- **THEN** the event has player `Steve`

#### Scenario: No valid speaker
- **WHEN** the line is `[12:34:56] [Server thread/INFO]: joined the game`
- **THEN** the `chat` extractor produces no event

### Requirement: Keyword extractor semantics
The `keyword` extractor SHALL match when its keyword appears as a substring of the line and SHALL derive the player name as the single token immediately preceding the keyword occurrence, validated against the player-name rules. The produced message text SHALL be empty. When no valid player-name token precedes the keyword, the event SHALL still be produced with a player name of `none` (`<none>` in templates) rather than being discarded.

#### Scenario: Player name precedes the keyword
- **WHEN** the extractor keyword is `joined the game` and the line is `[12:34:56] [Server thread/INFO]: YYM joined the game`
- **THEN** the event has player `YYM` and text `` (empty)

#### Scenario: No valid name token
- **WHEN** the extractor keyword is `joined the game` and the line is `joined the game`
- **THEN** the event is produced with player `none`

### Requirement: Player name validation reused
Both extractors SHALL validate candidate player names with the existing rule `[A-Za-z0-9_]{1,16}`, so that log decorations or message text are never mistaken for a player name.

#### Scenario: Token with punctuation rejected
- **WHEN** the token preceding the keyword is `[12:34:56]`
- **THEN** it is not accepted as a player name
