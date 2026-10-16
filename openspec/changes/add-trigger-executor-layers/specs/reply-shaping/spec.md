## ADDED Requirements

### Requirement: Reply shaper registry
The program SHALL resolve reply shapers by name from a registry containing `text` (and any future shapers). A shaper SHALL accept the raw pi stdout and append the reply text, reporting failure through an error out-parameter. An unknown shaper name SHALL be rejected at configuration load.

#### Scenario: Unknown shaper rejected
- **WHEN** a dialog trigger sets `reply_shaper = markdown`
- **THEN** configuration load fails with an error naming the unknown shaper and the program exits non-zero

#### Scenario: Text shaper is the default
- **WHEN** a dialog trigger sets no `reply_shaper`
- **THEN** the `text` shaper is used

### Requirement: Text shaper preserves existing pi parsing
The `text` shaper SHALL reuse the existing pi NDJSON parsing: take the LAST assistant `message_end`, concatenate only `type == "text"` content blocks, ignore `type == "thinking"` blocks, and surface a pi error stop reason as a failure.

#### Scenario: Thinking blocks ignored
- **WHEN** pi output contains an assistant message with a `thinking` block followed by a `text` block
- **THEN** only the `text` block content is used as the reply

#### Scenario: Last message wins after retries
- **WHEN** pi emits two assistant `message_end` records because of an automatic retry
- **THEN** the reply is taken from the second (last) record

#### Scenario: No text produced
- **WHEN** pi output has no text block
- **THEN** the shaper reports a failure and the worker sends the corresponding notice
