## ADDED Requirements

### Requirement: Incremental log reading
The log watcher SHALL open the configured log file, seek to its current end on startup, and on each poll read only newly appended bytes, splitting them into complete lines. A trailing partial line SHALL be retained in an internal buffer and prepended to the next read. Complete lines SHALL be delivered to a caller-supplied callback one at a time.

#### Scenario: Startup ignores history
- **WHEN** the watcher starts and the log file already contains N existing lines
- **THEN** those N lines are not delivered to the callback; only lines appended afterwards are

#### Scenario: Partial line buffered across reads
- **WHEN** a poll reads the bytes `<Steve> @pi hel` with no trailing newline
- **THEN** nothing is delivered yet, and when the next poll reads `lo\n`, the callback receives the single complete line `<Steve> @pi hello`

#### Scenario: Lines longer than the buffer
- **WHEN** a single line exceeds the line buffer capacity
- **THEN** the watcher MUST NOT overflow the buffer; the over-long line is dropped (or truncated) and a warning is logged, and normal reading resumes at the next newline

### Requirement: Log rotation and truncation recovery
The watcher SHALL detect that the log file has been rotated (the path now resolves to a different inode) or truncated (size smaller than the last read offset) and SHALL reopen and/or reset its read offset so that new content continues to be delivered without restarting the program.

#### Scenario: Rotation via rename
- **WHEN** the log file is renamed (rotation) and a new file is created at the same path
- **THEN** the watcher reopens the path, continues reading from offset 0 of the new inode, and delivers subsequently appended lines

#### Scenario: Truncation
- **WHEN** the file at the path keeps the same inode but its size becomes smaller than the watcher's current offset
- **THEN** the watcher resets its offset to 0 and reads from the beginning of the truncated file

#### Scenario: Missing file
- **WHEN** the configured log file does not exist (e.g. server not started yet)
- **THEN** the watcher does not crash or busy-spin; it logs a warning, retries on the next poll, and begins delivering lines once the file appears

### Requirement: Configurable poll interval and non-blocking idle
The watcher SHALL poll at a configurable interval (default 500 ms) and SHALL NOT consume measurable CPU while idle.

#### Scenario: Idle CPU
- **WHEN** the log file receives no new content for a sustained period
- **THEN** the watcher sleeps for the poll interval between checks rather than spinning in a busy loop
