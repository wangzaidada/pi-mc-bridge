/* rcon_client.h - minimal Minecraft RCON client (protocol 3 login / 2 command
 * / 0 response, little-endian int32 framing, two trailing NUL bytes). */
#ifndef PI_MC_BRIDGE_RCON_CLIENT_H
#define PI_MC_BRIDGE_RCON_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "util.h"

#define RCON_TYPE_RESPONSE 0
#define RCON_TYPE_COMMAND  2
#define RCON_TYPE_AUTH     3

#define RCON_ERR_NONE     0
#define RCON_ERR_SOCKET  -1   /* connect/send/recv failure */
#define RCON_ERR_AUTH    -2   /* server rejected the password */
#define RCON_ERR_PROTO   -3   /* malformed packet / unexpected id */
#define RCON_ERR_TIMEOUT -4   /* send/recv timed out */

typedef struct {
    int    fd;
    char  *host;
    int    port;
    char  *password;
    int    timeout_ms;   /* SO_RCVTIMEO/SO_SNDTIMEO */
    int    next_id;
    pthread_mutex_t lock; /* serialises the shared connection across threads */
} RconClient;

/* Encode a packet into `out` (appended). Exposed for unit tests. */
void rcon_pack(DynBuf *out, int id, int type, const char *payload);

/* Decode a length-prefixed packet body (the bytes *after* the 4-byte length).
 * Fills the id and type out-params and appends the payload to `payload_out`
 * (which may be NULL). */
int  rcon_unpack(const char *body, size_t body_len, int *id, int *type,
                 DynBuf *payload_out);

/* Connect and authenticate. Returns 0 on success, negative RCON_ERR_* otherwise. */
int  rcon_connect(RconClient *c, const char *host, int port,
                  const char *password, int timeout_ms);

/* Disconnect (idempotent). Does not free the strings. */
void rcon_disconnect(RconClient *c);

/* Fully release (disconnect + free strings). */
void rcon_free(RconClient *c);

/* Send a command and read the response into `out` (reset first). On a socket
 * error the client transparently reconnects and retries once. */
int  rcon_command(RconClient *c, const char *cmd, DynBuf *out);

/* Validate a player name: [A-Za-z0-9_]{1,16}. */
bool rcon_valid_player(const char *name);

#endif /* PI_MC_BRIDGE_RCON_CLIENT_H */
