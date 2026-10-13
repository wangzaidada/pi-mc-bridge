## ADDED Requirements

### Requirement: RCON packet encoding/decoding
The RCON client SHALL encode and decode packets in the Minecraft RCON wire format: a little-endian int32 length field, then little-endian int32 request id, little-endian int32 type, an ASCII payload terminated by `\0`, and a trailing `\0`. The length field SHALL equal `4 + 4 + strlen(payload) + 1 + 1`. Type values SHALL be 3 (login), 2 (command), 0 (response).

#### Scenario: Encode a command packet
- **WHEN** the client encodes a command packet with request id 1 and payload `list`
- **THEN** the first int32 equals `4 + 4 + 4 + 1 + 1 = 14`, followed by id=1, type=2, the bytes `list\0`, and a final `\0`

#### Scenario: Decode a response
- **WHEN** the client receives a well-formed packet with type 0 and id 1
- **THEN** it extracts the id and the NUL-terminated payload, tolerating an extra empty packet

### Requirement: Authentication handshake
The client SHALL authenticate by sending a type-3 packet carrying the password and SHALL treat the connection as established only when a response with a matching request id is received. A mismatched or failed login SHALL be reported as an authentication failure.

#### Scenario: Successful login
- **WHEN** the server accepts the password
- **THEN** the client receives a response with the same request id and marks the connection authenticated

#### Scenario: Bad password
- **WHEN** the password is wrong (server responds with id `-1`)
- **THEN** the client reports an authentication failure and does not proceed to send commands

### Requirement: Command round-trip and reconnection
The client SHALL send command packets with monotonically increasing request ids and SHALL read responses until it finds one matching the sent id, returning the response body. On send/receive failure (broken connection) the client SHALL close the socket, reconnect, re-authenticate, and retry the command once.

#### Scenario: Successful command
- **WHEN** a `/tellraw` command is sent on an authenticated connection
- **THEN** the client returns the server's response text for that request id

#### Scenario: Split response packets
- **WHEN** the server sends a response split across two packets (the second empty)
- **THEN** the client combines them into one response body and matches the pending request id

#### Scenario: Connection dropped
- **WHEN** the send or receive fails because the server went away
- **THEN** the client reconnects and re-authenticates once and retries the command, and only reports failure if the retry also fails

#### Scenario: Non-blocking timeouts
- **WHEN** the server accepts the connection but never replies
- **THEN** the client's read times out after the configured timeout and is treated as a connection failure rather than hanging forever
