## ADDED Requirements

### Requirement: Thread-safe bounded FIFO queue
The task queue SHALL provide a FIFO of tasks, safe for concurrent push from the log-watching thread and blocking pop from worker thread(s). It SHALL have a configurable maximum depth; when full, pushes SHALL fail immediately rather than block indefinitely.

#### Scenario: Blocking pop waits for work
- **WHEN** a worker thread calls pop on an empty queue
- **THEN** it blocks until a task is pushed or the queue is shut down, without busy-waiting

#### Scenario: Full queue rejects
- **WHEN** the queue is at its maximum depth and another task is pushed
- **THEN** the push returns a "queue full" failure, the task is not enqueued, and the caller can notify the player

#### Scenario: FIFO ordering
- **WHEN** tasks A, B, C are pushed in that order and popped by a single worker
- **THEN** they are popped in the order A, B, C

### Requirement: Shutdown semantics
The queue SHALL support a shutdown operation that wakes all blocked consumers; a blocked pop SHALL return a distinct "shutdown" result instead of blocking forever.

#### Scenario: Shutdown wakes blocked consumer
- **WHEN** a worker is blocked in pop and shutdown is called
- **THEN** pop returns the shutdown result and the worker can exit its loop

#### Scenario: Owner identification
- **WHEN** a task is created from a chat message
- **THEN** the task carries the player name and prompt, which the worker uses to address the reply
