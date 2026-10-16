## ADDED Requirements

### Requirement: Trigger table
The program SHALL hold a table of one or more triggers. Each trigger SHALL have a unique name, a match kind, a non-empty set of literal keywords, an extractor name, and a bound executor. A log line SHALL be offered to the triggers in table order and the FIRST trigger that matches SHALL win; at most one trigger produces an event per line.

#### Scenario: First match wins
- **WHEN** the table contains trigger `chat` (keyword `@pi`) before trigger `join` (keyword `joined the game`) and the line is `<Steve> @pi hi`
- **THEN** trigger `chat` produces the event and `join` is not consulted

#### Scenario: No trigger matches
- **WHEN** the line is `[12:34:56] [Server thread/INFO]: Steve: hello` with no trigger keyword present
- **THEN** no event is produced and the line is ignored

#### Scenario: Multiple keywords on one trigger
- **WHEN** trigger `chat` has keywords `@pi` and `@ai` and the line is `<Alex> @ai 你好`
- **THEN** trigger `chat` matches via its second keyword

### Requirement: Built-in line-type matching
The program SHALL support exactly two built-in match kinds, without a regular-expression engine: a chat-line kind and a literal-keyword kind. Keywords SHALL be matched literally; chat-line keywords SHALL be matched case-insensitively at the first token position after the speaker name, and keyword-line keywords SHALL be matched as a substring anywhere in the line.

#### Scenario: Chat-line kind requires the keyword as the first token
- **WHEN** the match kind is chat and the message body is `帮我看看 @pi 这个` (keyword not first)
- **THEN** the trigger does not match

#### Scenario: Chat-line kind is case-insensitive
- **WHEN** the match kind is chat, keyword is `@pi`, and the message body is `@PI hello`
- **THEN** the trigger matches

#### Scenario: Keyword-line kind matches anywhere
- **WHEN** the match kind is keyword, keyword is `joined the game`, and the line is `[12:34:56] [Server thread/INFO]: YYM joined the game`
- **THEN** the trigger matches

### Requirement: Per-trigger cooldown and allow-list
Each trigger SHALL carry an optional cooldown that overrides the global cooldown, and an optional allow-list that overrides the global allow-list. A trigger that does not specify them SHALL inherit the global defaults.

#### Scenario: Trigger overrides cooldown
- **WHEN** the global cooldown is 10 s and trigger `join` sets cooldown 0
- **THEN** a second matching join event is accepted immediately

#### Scenario: Trigger inherits global allow-list
- **WHEN** trigger `chat` does not specify an allow-list and the global allow-list is `Steve,Alex`
- **THEN** only `Steve` and `Alex` may trigger `chat`

#### Scenario: Trigger-specific allow-list overrides global
- **WHEN** the global allow-list is `Steve` and trigger `cmd` sets its allow-list to `Alex`
- **THEN** `Alex` may trigger `cmd` even though the global list excludes them

### Requirement: Match/extractor consistency
A trigger whose match kind and extractor name are incompatible (for example match kind `keyword` with extractor `chat`) SHALL be rejected at configuration load with a clear error, and the program SHALL refuse to start.

#### Scenario: Incompatible combination rejected
- **WHEN** a trigger sets `match = keyword` and `extractor = chat`
- **THEN** the program reports the mismatched trigger name and exits non-zero
