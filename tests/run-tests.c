/* run-tests.c - unit tests for the pure/isolated parts of pi-mc-bridge. */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parser.h"
#include "pi_client.h"
#include "queue.h"
#include "rcon_client.h"
#include "util.h"
#include "worker.h"

/* ---------- tiny test framework ---------- */

static int g_checks;
static int g_fails;
static const char *g_case = "";

#define CHECK(cond) do {                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fails++;                                                    \
            fprintf(stderr, "  FAIL [%s] %s:%d: %s\n", g_case, __FILE__,  \
                    __LINE__, #cond);                                     \
        }                                                                 \
    } while (0)

#define CHECK_STR(a, b) do {                                              \
        g_checks++;                                                       \
        const char *_a = (a), *_b = (b);                                  \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                          \
            g_fails++;                                                    \
            fprintf(stderr, "  FAIL [%s] %s:%d: \"%s\" != \"%s\"\n",      \
                    g_case, __FILE__, __LINE__, _a ? _a : "(null)",       \
                    _b ? _b : "(null)");                                  \
        }                                                                 \
    } while (0)

#define CASE(name) do { g_case = (name); printf("- %s\n", (name)); } while (0)

/* ---------- parser ---------- */

static void test_parser(void)
{
    CASE("parser: basic chat line");
    {
        ParsedRequest r;
        bool ok = parse_chat_line(
            "[12:34:56] [Server thread/INFO]: <Steve> @pi hello world",
            "@pi", &r);
        CHECK(ok);
        if (ok) {
            CHECK_STR(r.player, "Steve");
            CHECK_STR(r.prompt, "hello world");
            parsed_request_free(&r);
        }
    }

    CASE("parser: not a chat line");
    {
        ParsedRequest r;
        CHECK(!parse_chat_line("[12:34:56] [Server thread/INFO]: Steve joined",
                               "@pi", &r));
    }

    CASE("parser: chat without the trigger");
    {
        ParsedRequest r;
        CHECK(!parse_chat_line(
            "[12:34:56] [Server thread/INFO]: <Steve> hello", "@pi", &r));
    }

    CASE("parser: case-insensitive trigger");
    {
        ParsedRequest r;
        bool ok = parse_chat_line(
            "[12:34:56] [Server thread/INFO]: <Alex> @PI Explain this",
            "@pi", &r);
        CHECK(ok);
        if (ok) { CHECK_STR(r.prompt, "Explain this");
                  parsed_request_free(&r); }
    }

    CASE("parser: trigger must be a whole token");
    {
        ParsedRequest r;
        CHECK(!parse_chat_line(
            "[12:34:56] [Server thread/INFO]: <Steve> @pixel hello",
            "@pi", &r));
    }

    CASE("parser: empty prompt still matches");
    {
        ParsedRequest r;
        bool ok = parse_chat_line(
            "[12:34:56] [Server thread/INFO]: <Steve> @pi", "@pi", &r);
        CHECK(ok);
        if (ok) { CHECK_STR(r.prompt, ""); parsed_request_free(&r); }
    }

    CASE("parser: extra spaces around the prompt are trimmed");
    {
        ParsedRequest r;
        bool ok = parse_chat_line(
            "[12:34:56] [Server thread/INFO]: <Steve> @pi     hi   ",
            "@pi", &r);
        CHECK(ok);
        if (ok) { CHECK_STR(r.prompt, "hi"); parsed_request_free(&r); }
    }

    CASE("parser: angle brackets inside the message");
    {
        ParsedRequest r;
        bool ok = parse_chat_line(
            "[12:34:56] [Server thread/INFO]: <Steve> @pi what is <html>?",
            "@pi", &r);
        CHECK(ok);
        if (ok) {
            CHECK_STR(r.player, "Steve");
            CHECK_STR(r.prompt, "what is <html>?");
            parsed_request_free(&r);
        }
    }

    CASE("parser: trailing CR handled");
    {
        ParsedRequest r;
        bool ok = parse_chat_line(
            "[12:34:56] [Server thread/INFO]: <Steve> @pi hi\r", "@pi", &r);
        CHECK(ok);
        if (ok) { CHECK_STR(r.prompt, "hi"); parsed_request_free(&r); }
    }

    CASE("player name validation");
    {
        CHECK(is_valid_player_name("Steve"));
        CHECK(is_valid_player_name("a_b_C9"));
        CHECK(!is_valid_player_name(""));
        CHECK(!is_valid_player_name("has space"));
        CHECK(!is_valid_player_name("bad-name"));
        CHECK(!is_valid_player_name("toolongname1234567890"));
    }
}

/* ---------- util: json escaping & utf8 ---------- */

static void test_util(void)
{
    CASE("json escape: quotes, backslashes, control chars");
    {
        DynBuf d; dynbuf_init(&d);
        json_escape_append(&d, "a\"b\\c\nd\te");
        CHECK_STR(d.buf, "a\\\"b\\\\c\\nd\\te");
        dynbuf_free(&d);
    }

    CASE("json escape: control char becomes \\u");
    {
        DynBuf d; dynbuf_init(&d);
        json_escape_append(&d, "\x01");
        CHECK_STR(d.buf, "\\u0001");
        dynbuf_free(&d);
    }

    CASE("json escape: utf-8 passes through");
    {
        DynBuf d; dynbuf_init(&d);
        json_escape_append(&d, "你好");
        CHECK_STR(d.buf, "你好");
        dynbuf_free(&d);
    }

    CASE("utf8 segment: ascii");
    {
        CHECK(utf8_segment_len("abcdef", 3) == 3);
        CHECK(utf8_count("abcdef") == 6);
    }

    CASE("utf8 segment: never splits a multi-byte char");
    {
        /* 你 = 3 bytes, 好 = 3 bytes. Asking for 2 chars must stop at 3 bytes. */
        CHECK(utf8_segment_len("你好", 1) == 3);
        CHECK(utf8_segment_len("你好", 2) == 6);
        CHECK(utf8_count("你好") == 2);
        CHECK(utf8_segment_len("a你", 1) == 1);
        CHECK(utf8_segment_len("a你", 2) == 4);
        CHECK(utf8_count("a你") == 2);
    }
}

/* ---------- pi NDJSON parsing ---------- */

static void test_pi_parse(void)
{
    CASE("pi: picks the text block, ignores thinking");
    {
        const char *stream =
            "{\"type\":\"session\",\"id\":\"x\"}\n"
            "{\"type\":\"agent_start\"}\n"
            "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"thinking\",\"thinking\":\"secret\"},"
            "{\"type\":\"text\",\"text\":\"你好\"}],\"stopReason\":\"stop\"}}\n"
            "{\"type\":\"agent_end\"}\n";
        DynBuf a; dynbuf_init(&a);
        bool stop_error = true; char *em = NULL;
        bool has = pi_parse_stream(stream, strlen(stream), &a, &stop_error, &em);
        CHECK(has);
        CHECK_STR(a.buf, "你好");
        CHECK(!stop_error);
        CHECK(em == NULL);
        dynbuf_free(&a);
        free(em);
    }

    CASE("pi: takes the LAST assistant message_end");
    {
        const char *stream =
            "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"first\"}],"
            "\"stopReason\":\"stop\"}}\n"
            "{\"type\":\"auto_retry_start\"}\n"
            "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"second\"}],"
            "\"stopReason\":\"stop\"}}\n";
        DynBuf a; dynbuf_init(&a);
        pi_parse_stream(stream, strlen(stream), &a, NULL, NULL);
        CHECK_STR(a.buf, "second");
        dynbuf_free(&a);
    }

    CASE("pi: concatenates multiple text blocks");
    {
        const char *stream =
            "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"foo \"},"
            "{\"type\":\"text\",\"text\":\"bar\"}],\"stopReason\":\"stop\"}}\n";
        DynBuf a; dynbuf_init(&a);
        pi_parse_stream(stream, strlen(stream), &a, NULL, NULL);
        CHECK_STR(a.buf, "foo bar");
        dynbuf_free(&a);
    }

    CASE("pi: decodes escapes inside text");
    {
        const char *stream =
            "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"a\\\"b\\\\c\\nd\"}],"
            "\"stopReason\":\"stop\"}}\n";
        DynBuf a; dynbuf_init(&a);
        pi_parse_stream(stream, strlen(stream), &a, NULL, NULL);
        CHECK_STR(a.buf, "a\"b\\c\nd");
        dynbuf_free(&a);
    }

    CASE("pi: ] inside a text block does not end the array early");
    {
        const char *stream =
            "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"a]b\"}],"
            "\"stopReason\":\"stop\"}}\n";
        DynBuf a; dynbuf_init(&a);
        pi_parse_stream(stream, strlen(stream), &a, NULL, NULL);
        CHECK_STR(a.buf, "a]b");
        dynbuf_free(&a);
    }

    CASE("pi: error stopReason is reported with the message");
    {
        const char *stream =
            "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
            "\"content\":[],\"stopReason\":\"error\","
            "\"errorMessage\":\"boom\"}}\n";
        DynBuf a; dynbuf_init(&a);
        bool stop_error = false; char *em = NULL;
        pi_parse_stream(stream, strlen(stream), &a, &stop_error, &em);
        CHECK(stop_error);
        CHECK_STR(em, "boom");
        dynbuf_free(&a);
        free(em);
    }

    CASE("pi: ignores non-JSON noise lines");
    {
        const char *stream =
            "not json at all\n"
            "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],"
            "\"stopReason\":\"stop\"}}\n"
            "garbage\n";
        DynBuf a; dynbuf_init(&a);
        bool has = pi_parse_stream(stream, strlen(stream), &a, NULL, NULL);
        CHECK(has);
        CHECK_STR(a.buf, "ok");
        dynbuf_free(&a);
    }

    CASE("pi: user messages are not mistaken for the reply");
    {
        const char *stream =
            "{\"type\":\"message_end\",\"message\":{\"role\":\"user\","
            "\"content\":[{\"type\":\"text\",\"text\":\"question\"}]}}\n";
        DynBuf a; dynbuf_init(&a);
        bool has = pi_parse_stream(stream, strlen(stream), &a, NULL, NULL);
        CHECK(!has);
        CHECK(a.len == 0);
        dynbuf_free(&a);
    }
}

/* ---------- rcon framing ---------- */

static void test_rcon(void)
{
    CASE("rcon: pack layout is little-endian with two NULs");
    {
        DynBuf d; dynbuf_init(&d);
        rcon_pack(&d, 1, RCON_TYPE_AUTH, "pw");
        /* length field = 4+4+2+2 = 12; total buffer = 4 + 12 = 16 */
        CHECK(d.len == 16);
        const unsigned char *b = (const unsigned char *)d.buf;
        CHECK(b[0] == 12 && b[1] == 0 && b[2] == 0 && b[3] == 0);
        CHECK(b[4] == 1 && b[5] == 0 && b[6] == 0 && b[7] == 0);   /* id */
        CHECK(b[8] == 3 && b[9] == 0 && b[10] == 0 && b[11] == 0); /* type */
        CHECK(b[12] == 'p' && b[13] == 'w');
        CHECK(b[14] == 0 && b[15] == 0);
        dynbuf_free(&d);
    }

    CASE("rcon: unpack round-trips a command response");
    {
        DynBuf d; dynbuf_init(&d);
        rcon_pack(&d, 7, RCON_TYPE_COMMAND, "list");
        /* body starts after the 4-byte length */
        int id = 0, type = 0;
        DynBuf payload; dynbuf_init(&payload);
        int rc = rcon_unpack(d.buf + 4, d.len - 4, &id, &type, &payload);
        CHECK(rc == RCON_ERR_NONE);
        CHECK(id == 7);
        CHECK(type == RCON_TYPE_COMMAND);
        CHECK_STR(payload.buf, "list");
        dynbuf_free(&payload);
        dynbuf_free(&d);
    }

    CASE("rcon: unpack rejects a short body");
    {
        int id, type;
        CHECK(rcon_unpack("\x01\x00", 2, &id, &type, NULL) == RCON_ERR_PROTO);
    }

    CASE("rcon: player validation");
    {
        CHECK(rcon_valid_player("Steve"));
        CHECK(!rcon_valid_player("a b"));
    }
}

/* ---------- worker: tellraw ---------- */

static void test_worker_tellraw(void)
{
    CASE("tellraw: escapes the text and wraps it in JSON");
    {
        DynBuf d; dynbuf_init(&d);
        bool ok = worker_build_tellraw(&d, "Steve", "he said \"hi\"\nbye");
        CHECK(ok);
        CHECK_STR(d.buf,
                  "tellraw Steve {\"text\":\"he said \\\"hi\\\"\\nbye\"}");
        dynbuf_free(&d);
    }

    CASE("tellraw: rejects an invalid player name");
    {
        DynBuf d; dynbuf_init(&d);
        CHECK(!worker_build_tellraw(&d, "bad name", "x"));
        dynbuf_free(&d);
    }
}

/* ---------- queue ---------- */

static void *push_thread(void *arg)
{
    TaskQueue *q = arg;
    for (int i = 0; i < 200; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "p%d", i);
        while (queue_push(q, "Steve", buf) == QUEUE_FULL)
            usleep(100);
    }
    return NULL;
}

static void test_queue(void)
{
    CASE("queue: fifo order");
    {
        TaskQueue q; queue_init(&q, 4);
        CHECK(queue_push(&q, "A", "1") == QUEUE_OK);
        CHECK(queue_push(&q, "B", "2") == QUEUE_OK);
        Task t;
        CHECK(queue_pop(&q, &t) == QUEUE_OK);
        CHECK_STR(t.player, "A"); CHECK_STR(t.prompt, "1"); task_free(&t);
        CHECK(queue_pop(&q, &t) == QUEUE_OK);
        CHECK_STR(t.player, "B"); CHECK_STR(t.prompt, "2"); task_free(&t);
        queue_free(&q);
    }

    CASE("queue: rejects when full");
    {
        TaskQueue q; queue_init(&q, 2);
        CHECK(queue_push(&q, "A", "1") == QUEUE_OK);
        CHECK(queue_push(&q, "B", "2") == QUEUE_OK);
        CHECK(queue_push(&q, "C", "3") == QUEUE_FULL);
        queue_free(&q);
    }

    CASE("queue: shutdown wakes a blocked consumer");
    {
        TaskQueue q; queue_init(&q, 2);
        queue_shutdown(&q);
        Task t;
        CHECK(queue_pop(&q, &t) == QUEUE_SHUTDOWN);
        CHECK(queue_push(&q, "A", "1") == QUEUE_SHUTDOWN);
        queue_free(&q);
    }

    CASE("queue: concurrent producers, one consumer");
    {
        TaskQueue q; queue_init(&q, 8);
        pthread_t th[3];
        for (int i = 0; i < 3; i++)
            pthread_create(&th[i], NULL, push_thread, &q);

        int got = 0;
        while (got < 600) {
            Task t;
            if (queue_pop(&q, &t) != QUEUE_OK)
                break;
            got++;
            task_free(&t);
        }
        for (int i = 0; i < 3; i++)
            pthread_join(th[i], NULL);
        CHECK(got == 600);
        queue_free(&q);
    }
}

int main(void)
{
    test_util();
    test_parser();
    test_pi_parse();
    test_rcon();
    test_worker_tellraw();
    test_queue();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
