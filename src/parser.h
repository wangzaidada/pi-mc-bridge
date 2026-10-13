/* parser.h - recognize chat lines and extract @pi requests. */
#ifndef PI_MC_BRIDGE_PARSER_H
#define PI_MC_BRIDGE_PARSER_H

#include <stdbool.h>

typedef struct {
    char *player;  /* heap copy, NUL-terminated; caller frees */
    char *prompt;  /* heap copy, trimmed; caller frees */
} ParsedRequest;

/* Try to interpret one raw log line as a chat request.
 * On success fills *out (both fields heap-allocated) and returns true.
 * `prefix` is the trigger, matched case-insensitively as the first token after
 * the player name (e.g. "@pi"). A line that is chat but has no prompt still
 * matches, with prompt = "" so the caller can send a usage hint.
 * Returns false for non-chat lines or chat lines without the trigger. */
bool parse_chat_line(const char *line, const char *prefix, ParsedRequest *out);

void parsed_request_free(ParsedRequest *r);

/* Validate a Minecraft player name: [A-Za-z0-9_]{1,16}. */
bool is_valid_player_name(const char *name);

#endif /* PI_MC_BRIDGE_PARSER_H */
