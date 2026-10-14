## ADDED Requirements

### Requirement: Configuration file format
The program SHALL accept an INI-subset configuration file consisting of a `[global]` section providing defaults and zero or more `[trigger:<name>]` sections, each defining one trigger. Within a section, lines SHALL have the form `key = value`, `#` and `;` SHALL begin comments, and values SHALL be trimmed of surrounding whitespace. The `keywords` key SHALL be a comma-separated list.

#### Scenario: Minimal file
- **WHEN** the file contains `[global]` with `rcon_port = 25575` and one `[trigger:chat]` section
- **THEN** configuration loads with one trigger named `chat` and port 25575

#### Scenario: Comments and blank lines ignored
- **WHEN** the file contains comment lines and blank lines mixed with settings
- **THEN** they are ignored and loading succeeds

#### Scenario: Keyword list split
- **WHEN** a trigger sets `keywords = @pi, @ai`
- **THEN** the trigger has two keywords `@pi` and `@ai` with surrounding whitespace trimmed

#### Scenario: Trigger without keywords rejected
- **WHEN** a `[trigger:chat]` section sets no keywords
- **THEN** configuration load fails with an error naming the trigger

### Requirement: Configuration precedence
Configuration SHALL be merged with precedence: command line > environment variable > configuration file > built-in default. Command-line and environment settings SHALL act as overrides on top of the file's `[global]` defaults. A trigger key that is absent SHALL fall back to the merged global value where the global equivalent exists.

#### Scenario: Command line beats the file
- **WHEN** the file sets `[global] rcon_port = 25575` and `--rcon-port 25577` is passed
- **THEN** the effective port is 25577

#### Scenario: Environment beats the file
- **WHEN** the file sets `[global] cooldown = 10` and `COOLDOWN=3` is set
- **THEN** the effective global cooldown is 3 s

#### Scenario: Trigger field falls back to global
- **WHEN** a trigger omits `cooldown` and `[global]` sets `cooldown = 10`
- **THEN** the trigger uses a 10 s cooldown

### Requirement: Unknown keys fail fast
An unrecognized key in any section SHALL cause configuration load to fail with a message identifying the section and the key, rather than being silently ignored, so that a misspelled key cannot silently disable a trigger.

#### Scenario: Misspelled key
- **WHEN** a section contains `keywrods = @pi`
- **THEN** loading fails and the error names the offending key and section

### Requirement: Backward compatibility without a file
When no configuration file is present or configured, the program SHALL synthesize exactly one chat trigger from the existing prefix, cooldown, and allow-list settings, equivalent to `match = chat`, `keywords = {prefix}`, `extractor = chat`, and `executor = dialog`. Existing command-line options, environment variables, and defaults SHALL behave as before. The one intended difference from pre-change behavior is the input line pi receives: the bridge now composes it in the fixed `<player>: <text>` format and the trigger keyword is consumed rather than passed through.

#### Scenario: No file, existing CLI
- **WHEN** the program is started with `--prefix @pi` and no configuration file
- **THEN** a single trigger is synthesized and a line `<Steve> @pi hello` produces an event with player `Steve` and text `hello`

#### Scenario: Input line uses the fixed format
- **WHEN** the synthesized trigger handles that event
- **THEN** pi receives the input line `Steve: hello`

### Requirement: Configuration load diagnostics
Configuration load SHALL report every problem it detects with the offending section, key, or trigger name so the cause is identifiable; on any error the program SHALL exit non-zero before starting the watcher.

#### Scenario: Multiple triggers with distinct names
- **WHEN** the file defines `[trigger:chat]`, `[trigger:join]`, and `[trigger:cmd]`
- **THEN** three triggers are loaded and a runtime log line identifies which trigger matched each event by name
