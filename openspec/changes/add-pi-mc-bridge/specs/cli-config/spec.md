## ADDED Requirements

### Requirement: Configuration sources and precedence
The program SHALL read configuration from command-line options and environment variables, with precedence: command line > environment variable > built-in default. Every option SHALL have a documented environment-variable equivalent and a default where sensible.

#### Scenario: Command line overrides environment
- **WHEN** `RCON_PORT=25576` is set and `--rcon-port 25577` is passed
- **THEN** the effective port is 25577

#### Scenario: Environment overrides default
- **WHEN** no command-line option is given for the password but `RCON_PASSWORD` is set
- **THEN** the effective password is the value of `RCON_PASSWORD`

#### Scenario: Defaults applied
- **WHEN** neither command line nor environment provides a value
- **THEN** the documented default is used (host `127.0.0.1`, port `25575`, prefix `@pi `, pi command `pi`, timeout 60 s, cooldown 10 s, max length 200)

### Requirement: Required configuration validation
The program SHALL validate configuration at startup and SHALL refuse to start (with a clear message on stderr and a non-zero exit status) when a required value is missing or out of range.

#### Scenario: Missing password
- **WHEN** no RCON password is provided by any source
- **THEN** the program prints a usage/error message and exits non-zero without starting the watcher

#### Scenario: Invalid numeric value
- **WHEN** a port, timeout, cooldown, or max length is not a positive integer within range
- **THEN** the program reports which value is invalid and exits non-zero

#### Scenario: Help flag
- **WHEN** `--help` is passed
- **THEN** the program prints usage describing all options and their environment variables and exits zero
