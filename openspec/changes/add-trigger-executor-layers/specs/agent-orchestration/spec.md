## ADDED Requirements

### Requirement: Line dispatch orchestration
The program SHALL provide a single entry point invoked for each complete log line that matches the line against the trigger table, and on a match, applies the trigger's allow-list, handles a full queue, and enqueues a task carrying the trigger, the player, and the text. When no trigger matches, the line SHALL be ignored without side effects.

#### Scenario: Matching line enqueues a task
- **WHEN** a line matches trigger `chat` and the queue has space
- **THEN** a task carrying trigger `chat`, the extracted player, and the extracted text is enqueued

#### Scenario: Non-matching line ignored
- **WHEN** a line matches no trigger
- **THEN** nothing is enqueued and nothing is logged at INFO level

#### Scenario: Allow-list blocks before enqueue
- **WHEN** the matched trigger's effective allow-list does not include the player
- **THEN** no task is enqueued and a notice is logged

#### Scenario: Full queue notifies the player
- **WHEN** the queue is full and a matching line arrives
- **THEN** the push fails, the line is not enqueued, and the player is sent a short "busy" notice

### Requirement: Executor selection at handling time
The worker SHALL select the executor to run from the trigger stored on the task, without re-parsing the line and without a registry lookup by name at handling time.

#### Scenario: Replier trigger does not invoke pi
- **WHEN** the task's trigger is bound to the `replier` executor
- **THEN** the worker produces the reply directly and never spawns pi

#### Scenario: Dialog trigger invokes pi
- **WHEN** the task's trigger is bound to the `dialog` executor
- **THEN** the worker invokes pi once for that task

### Requirement: Startup assembly order
The program SHALL assemble components in a defined order: load base configuration, load the configuration file (if any) or synthesize the compatibility trigger, resolve executors and extractors and build the trigger table, then start the queue, worker, RCON connection, and log watcher. Any earlier stage failing SHALL prevent later stages from starting.

#### Scenario: Config error prevents startup
- **WHEN** configuration load fails
- **THEN** the watcher, worker, and RCON connection are never started and the process exits non-zero

#### Scenario: Runtime line identifies the trigger
- **WHEN** `--log-level debug` is active and a line matches
- **THEN** the log names the matched trigger so operators can tell which rule fired

### Requirement: No new background threads
The orchestration SHALL reuse the existing single log-watching thread and single worker thread; adding triggers or executors SHALL NOT add threads.

#### Scenario: Execution remains serial
- **WHEN** several matching lines arrive in quick succession
- **THEN** their tasks are handled one at a time by the single worker, in queue order
