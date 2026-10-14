/* run-tests.c - unit tests for the pure/isolated parts of pi-mc-bridge. */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "configfile.h"
#include "executor.h"
#include "extractor.h"
#include "parser.h"
#include "pi_client.h"
#include "queue.h"
#include "rcon_client.h"
#include "reply.h"
#include "trigger.h"
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

/* ---------- trigger table & extractors ---------- */

static void set_keywords(Trigger *tr, const char *const *kws, int n)
{
    tr->keywords = calloc((size_t)n, sizeof(char *));
    for (int i = 0; i < n; i++)
        tr->keywords[i] = xstrdup(kws[i]);
    tr->keyword_count = n;
}

static void test_trigger(void)
{
    CASE("extractor: chat standard line");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("chat");
        tr->match = MATCH_CHAT;
        tr->extractor = xstrdup("chat");
        const char *kws[] = {"@pi"};
        set_keywords(tr, kws, 1);

        TriggerEvent ev;
        bool ok = trigger_match(
            &t, "[12:34:56] [Server thread/INFO]: <Steve> @pi 你好", &ev);
        CHECK(ok);
        if (ok) {
            CHECK_STR(ev.player, "Steve");
            CHECK_STR(ev.text, "你好");
            CHECK(ev.trigger == tr);
            trigger_event_free(&ev);
        }
        trigger_table_free(&t);
    }

    CASE("extractor: chat tolerates a bracketed tag");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("chat");
        tr->match = MATCH_CHAT;
        tr->extractor = xstrdup("chat");
        const char *kws[] = {"@pi"};
        set_keywords(tr, kws, 1);

        TriggerEvent ev;
        bool ok = trigger_match(
            &t, "[12:34:56] [Server thread/INFO]: [Not Secure] <Steve> @pi hi",
            &ev);
        CHECK(ok);
        if (ok) { CHECK_STR(ev.player, "Steve"); trigger_event_free(&ev); }
        trigger_table_free(&t);
    }

    CASE("extractor: chat without a valid speaker does not match");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("chat");
        tr->match = MATCH_CHAT;
        tr->extractor = xstrdup("chat");
        const char *kws[] = {"@pi"};
        set_keywords(tr, kws, 1);

        TriggerEvent ev;
        CHECK(!trigger_match(
            &t, "[12:34:56] [Server thread/INFO]: joined the game", &ev));
        trigger_table_free(&t);
    }

    CASE("extractor: keyword takes the preceding token as player");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("join");
        tr->match = MATCH_KEYWORD;
        tr->extractor = xstrdup("keyword");
        const char *kws[] = {"joined the game"};
        set_keywords(tr, kws, 1);

        TriggerEvent ev;
        bool ok = trigger_match(
            &t, "[12:34:56] [Server thread/INFO]: YYM joined the game", &ev);
        CHECK(ok);
        if (ok) {
            CHECK_STR(ev.player, "YYM");
            CHECK_STR(ev.text, "");
            trigger_event_free(&ev);
        }
        trigger_table_free(&t);
    }

    CASE("extractor: keyword with no valid name yields player none");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("join");
        tr->match = MATCH_KEYWORD;
        tr->extractor = xstrdup("keyword");
        const char *kws[] = {"joined the game"};
        set_keywords(tr, kws, 1);

        TriggerEvent ev;
        bool ok = trigger_match(&t, "joined the game", &ev);
        CHECK(ok);
        if (ok) {
            CHECK_STR(ev.player, "none");
            trigger_event_free(&ev);
        }
        trigger_table_free(&t);
    }

    CASE("extractor: keyword token with punctuation is rejected");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("join");
        tr->match = MATCH_KEYWORD;
        tr->extractor = xstrdup("keyword");
        const char *kws[] = {"joined the game"};
        set_keywords(tr, kws, 1);

        TriggerEvent ev;
        bool ok = trigger_match(&t, "[12:34:56] joined the game", &ev);
        CHECK(ok);
        if (ok) { CHECK_STR(ev.player, "none"); trigger_event_free(&ev); }
        trigger_table_free(&t);
    }

    CASE("trigger: multiple keywords on one trigger");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("chat");
        tr->match = MATCH_CHAT;
        tr->extractor = xstrdup("chat");
        const char *kws[] = {"@pi", "@ai"};
        set_keywords(tr, kws, 2);

        TriggerEvent ev;
        bool ok = trigger_match(
            &t, "[12:34:56] [Server thread/INFO]: <Alex> @ai 你好", &ev);
        CHECK(ok);
        if (ok) { CHECK_STR(ev.text, "你好"); trigger_event_free(&ev); }
        trigger_table_free(&t);
    }

    CASE("trigger: first match wins");
    {
        TriggerTable t = {0};
        Trigger *a = trigger_table_append(&t);
        a->name = xstrdup("chat");
        a->match = MATCH_CHAT;
        a->extractor = xstrdup("chat");
        const char *kwa[] = {"@pi"};
        set_keywords(a, kwa, 1);

        Trigger *b = trigger_table_append(&t);
        b->name = xstrdup("join");
        b->match = MATCH_KEYWORD;
        b->extractor = xstrdup("keyword");
        const char *kwb[] = {"hello"};
        set_keywords(b, kwb, 1);

        /* The line satisfies both triggers; the earlier one must win. */
        TriggerEvent ev;
        bool ok = trigger_match(
            &t, "[12:34:56] [Server thread/INFO]: <Steve> @pi hello", &ev);
        CHECK(ok);
        if (ok) { CHECK_STR(ev.trigger->name, "chat"); trigger_event_free(&ev); }
        trigger_table_free(&t);
    }

    CASE("trigger: no match produces no event");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("chat");
        tr->match = MATCH_CHAT;
        tr->extractor = xstrdup("chat");
        const char *kws[] = {"@pi"};
        set_keywords(tr, kws, 1);

        TriggerEvent ev;
        CHECK(!trigger_match(
            &t, "[12:34:56] [Server thread/INFO]: Steve: hello", &ev));
        trigger_table_free(&t);
    }

    CASE("trigger: validate rejects incompatible match/extractor");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("bad");
        tr->match = MATCH_KEYWORD;
        tr->extractor = xstrdup("chat");
        const char *kws[] = {"x"};
        set_keywords(tr, kws, 1);

        char err[256];
        CHECK(trigger_validate(&t, err, sizeof(err)) == -1);
        trigger_table_free(&t);
    }

    CASE("trigger: validate rejects unknown extractor");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("bad");
        tr->match = MATCH_CHAT;
        tr->extractor = xstrdup("regex");
        const char *kws[] = {"x"};
        set_keywords(tr, kws, 1);

        char err[256];
        CHECK(trigger_validate(&t, err, sizeof(err)) == -1);
        trigger_table_free(&t);
    }

    CASE("trigger: validate rejects duplicate names");
    {
        TriggerTable t = {0};
        for (int i = 0; i < 2; i++) {
            Trigger *tr = trigger_table_append(&t);
            tr->name = xstrdup("dup");
            tr->match = MATCH_CHAT;
            tr->extractor = xstrdup("chat");
            const char *kws[] = {"x"};
            set_keywords(tr, kws, 1);
        }
        char err[256];
        CHECK(trigger_validate(&t, err, sizeof(err)) == -1);
        trigger_table_free(&t);
    }

    CASE("trigger: validate rejects an empty keyword set");
    {
        TriggerTable t = {0};
        Trigger *tr = trigger_table_append(&t);
        tr->name = xstrdup("nokw");
        tr->match = MATCH_CHAT;
        tr->extractor = xstrdup("chat");

        char err[256];
        CHECK(trigger_validate(&t, err, sizeof(err)) == -1);
        trigger_table_free(&t);
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
        while (queue_push(q, NULL, "Steve", buf) == QUEUE_FULL)
            usleep(100);
    }
    return NULL;
}

static void test_queue(void)
{
    CASE("queue: fifo order");
    {
        TaskQueue q; queue_init(&q, 4);
        CHECK(queue_push(&q, NULL, "A", "1") == QUEUE_OK);
        CHECK(queue_push(&q, NULL, "B", "2") == QUEUE_OK);
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
        CHECK(queue_push(&q, NULL, "A", "1") == QUEUE_OK);
        CHECK(queue_push(&q, NULL, "B", "2") == QUEUE_OK);
        CHECK(queue_push(&q, NULL, "C", "3") == QUEUE_FULL);
        queue_free(&q);
    }

    CASE("queue: shutdown wakes a blocked consumer");
    {
        TaskQueue q; queue_init(&q, 2);
        queue_shutdown(&q);
        Task t;
        CHECK(queue_pop(&q, &t) == QUEUE_SHUTDOWN);
        CHECK(queue_push(&q, NULL, "A", "1") == QUEUE_SHUTDOWN);
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

/* ---------- template placeholders & executors ---------- */

/* Captures what an executor hands back through its WorkerCtx callbacks. */
static struct {
    int   send_calls;
    int   seg_calls;
    int   run_pi_calls;
    char  player[64];
    char  text[512];
    int   max_len;
    char  prompt[512];
    PiStatus ret_status;
    const char *ret_raw;
    int   ret_exit_status;
} g_fake;

static bool fake_send(void *user, const char *player, const char *text)
{
    (void)user;
    g_fake.send_calls++;
    snprintf(g_fake.player, sizeof(g_fake.player), "%s", player);
    snprintf(g_fake.text, sizeof(g_fake.text), "%s", text ? text : "");
    return true;
}

static bool fake_send_segmented(void *user, const char *player,
                                const char *text, int max_len)
{
    (void)user;
    g_fake.seg_calls++;
    snprintf(g_fake.player, sizeof(g_fake.player), "%s", player);
    snprintf(g_fake.text, sizeof(g_fake.text), "%s", text ? text : "");
    g_fake.max_len = max_len;
    return true;
}

static PiStatus fake_run_pi(PiClient *pi, const char *prompt, DynBuf *raw,
                            char **detail, int *exit_status)
{
    (void)pi;
    (void)detail;
    g_fake.run_pi_calls++;
    snprintf(g_fake.prompt, sizeof(g_fake.prompt), "%s", prompt ? prompt : "");
    if (g_fake.ret_raw)
        dynbuf_append_str(raw, g_fake.ret_raw);
    if (exit_status)
        *exit_status = g_fake.ret_exit_status;
    return g_fake.ret_status;
}

static void fake_reset(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.ret_status = PI_OK;
}

static WorkerCtx fake_ctx(Config *cfg, PiClient *pi)
{
    WorkerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;
    ctx.pi = pi;
    ctx.send = fake_send;
    ctx.send_segmented = fake_send_segmented;
    ctx.run_pi = fake_run_pi;
    return ctx;
}

/* Build a one-trigger table bound to its executor; caller frees the table. */
static Trigger *bind_one(TriggerTable *t, const char *exec)
{
    Trigger *tr = trigger_table_append(t);
    tr->name = xstrdup("t");
    tr->match = MATCH_CHAT;
    tr->extractor = xstrdup("chat");
    const char *kws[] = {"@pi"};
    set_keywords(tr, kws, 1);
    tr->executor = xstrdup(exec);
    return tr;
}

static void test_template(void)
{
    CASE("template: substitutes known placeholders");
    {
        DynBuf d; dynbuf_init(&d);
        KV vars[] = { {"player", "史蒂夫"}, {"text", "你好"},
                      {"trigger", "chat"} };
        dynbuf_append_template(&d, "欢迎 {player}！你说：{text} [{trigger}]",
                               vars, 3);
        CHECK_STR(d.buf, "欢迎 史蒂夫！你说：你好 [chat]");
        dynbuf_free(&d);
    }

    CASE("template: unknown placeholders are preserved verbatim");
    {
        DynBuf d; dynbuf_init(&d);
        KV vars[] = { {"player", "Steve"} };
        dynbuf_append_template(&d, "hi {unknown} {player} {also}!", vars, 1);
        CHECK_STR(d.buf, "hi {unknown} Steve {also}!");
        dynbuf_free(&d);
    }

    CASE("template: a '{' with no closing brace is literal");
    {
        DynBuf d; dynbuf_init(&d);
        KV vars[] = { {"player", "Steve"} };
        dynbuf_append_template(&d, "hi {player} {unclosed", vars, 1);
        CHECK_STR(d.buf, "hi Steve {unclosed");
        dynbuf_free(&d);
    }

    CASE("template: a placeholder name is up to the next '}'");
    {
        /* Only the next '}' terminates a name, so '{b {player}' is one unknown
         * placeholder and is preserved verbatim. */
        DynBuf d; dynbuf_init(&d);
        KV vars[] = { {"player", "Steve"} };
        dynbuf_append_template(&d, "a {b {player}", vars, 1);
        CHECK_STR(d.buf, "a {b {player}");
        dynbuf_free(&d);
    }

    CASE("template: a NULL value yields an empty string");
    {
        DynBuf d; dynbuf_init(&d);
        KV vars[] = { {"text", NULL} };
        dynbuf_append_template(&d, "[{text}]", vars, 1);
        CHECK_STR(d.buf, "[]");
        dynbuf_free(&d);
    }
}

static void test_replier(void)
{
    CASE("replier: substitutes template and never invokes pi");
    {
        Config cfg;
        memset(&cfg, 0, sizeof(cfg));
        PiClient pi;
        memset(&pi, 0, sizeof(pi));

        TriggerTable t = {0};
        Trigger *tr = bind_one(&t, "replier");
        tr->template = xstrdup("欢迎 {player} 加入服务器！");
        tr->max_len = 42;

        char err[256];
        CHECK(trigger_bind_executors(&t, err, sizeof(err)) == 0);

        fake_reset();
        WorkerCtx ctx = fake_ctx(&cfg, &pi);
        TriggerEvent ev = { .trigger = tr, .player = "YYM", .text = "" };
        tr->exec->run(tr->exec_state, &ev, &ctx);

        CHECK(g_fake.run_pi_calls == 0);      /* the replier must not call pi */
        CHECK(g_fake.seg_calls == 1);
        CHECK_STR(g_fake.player, "YYM");
        CHECK_STR(g_fake.text, "欢迎 YYM 加入服务器！");
        CHECK(g_fake.max_len == 42);

        trigger_unbind_executors(&t);
        trigger_table_free(&t);
    }

    CASE("replier: init rejects a missing template");
    {
        TriggerTable t = {0};
        Trigger *tr = bind_one(&t, "replier");   /* no template set */
        char err[256];
        CHECK(trigger_bind_executors(&t, err, sizeof(err)) == -1);
        CHECK(strstr(err, "replier") != NULL);
        trigger_table_free(&t);
    }
}

static void test_dialog(void)
{
    const char *ok_stream =
        "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
        "\"content\":[{\"type\":\"thinking\",\"thinking\":\"secret\"},"
        "{\"type\":\"text\",\"text\":\"回答\"}],\"stopReason\":\"stop\"}}\n";

    CASE("dialog: input line is literally '<player>: <text>'");
    {
        Config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.max_len = 200;
        cfg.reply_prefix = "P: ";
        PiClient pi;
        memset(&pi, 0, sizeof(pi));

        TriggerTable t = {0};
        Trigger *tr = bind_one(&t, "dialog");
        char err[256];
        CHECK(trigger_bind_executors(&t, err, sizeof(err)) == 0);

        fake_reset();
        g_fake.ret_raw = ok_stream;
        WorkerCtx ctx = fake_ctx(&cfg, &pi);
        TriggerEvent ev = { .trigger = tr, .player = "Steve", .text = "你好" };
        tr->exec->run(tr->exec_state, &ev, &ctx);

        CHECK_STR(g_fake.prompt, "Steve: 你好");
        CHECK(g_fake.run_pi_calls == 1);
        CHECK(g_fake.seg_calls == 1);
        CHECK_STR(g_fake.text, "P: 回答");
        CHECK_STR(g_fake.player, "Steve");

        trigger_unbind_executors(&t);
        trigger_table_free(&t);
    }

    CASE("dialog: timeout notice");
    {
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.max_len = 200; cfg.reply_prefix = "[AI] ";
        PiClient pi; memset(&pi, 0, sizeof(pi));
        TriggerTable t = {0};
        Trigger *tr = bind_one(&t, "dialog");
        char err[256];
        trigger_bind_executors(&t, err, sizeof(err));

        fake_reset();
        g_fake.ret_status = PI_ERR_TIMEOUT;
        WorkerCtx ctx = fake_ctx(&cfg, &pi);
        TriggerEvent ev = { .trigger = tr, .player = "Steve", .text = "hi" };
        tr->exec->run(tr->exec_state, &ev, &ctx);
        CHECK(g_fake.send_calls == 1);
        CHECK_STR(g_fake.text, "抱歉，生成回复超时了，请稍后再试。");

        trigger_unbind_executors(&t);
        trigger_table_free(&t);
    }

    CASE("dialog: spawn failure notice");
    {
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.max_len = 200; cfg.reply_prefix = "[AI] ";
        PiClient pi; memset(&pi, 0, sizeof(pi));
        TriggerTable t = {0};
        Trigger *tr = bind_one(&t, "dialog");
        char err[256];
        trigger_bind_executors(&t, err, sizeof(err));

        fake_reset();
        g_fake.ret_status = PI_ERR_SPAWN;
        WorkerCtx ctx = fake_ctx(&cfg, &pi);
        TriggerEvent ev = { .trigger = tr, .player = "Steve", .text = "hi" };
        tr->exec->run(tr->exec_state, &ev, &ctx);
        CHECK_STR(g_fake.text, "抱歉，AI 服务暂时不可用。");

        trigger_unbind_executors(&t);
        trigger_table_free(&t);
    }

    CASE("dialog: pi error stopReason surfaces as the stop notice");
    {
        const char *err_stream =
            "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
            "\"content\":[],\"stopReason\":\"error\","
            "\"errorMessage\":\"boom\"}}\n";
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.max_len = 200; cfg.reply_prefix = "[AI] ";
        PiClient pi; memset(&pi, 0, sizeof(pi));
        TriggerTable t = {0};
        Trigger *tr = bind_one(&t, "dialog");
        char err[256];
        trigger_bind_executors(&t, err, sizeof(err));

        fake_reset();
        g_fake.ret_raw = err_stream;
        WorkerCtx ctx = fake_ctx(&cfg, &pi);
        TriggerEvent ev = { .trigger = tr, .player = "Steve", .text = "hi" };
        tr->exec->run(tr->exec_state, &ev, &ctx);
        /* Shaper sees stop error -> PI_ERR_STOP -> stop notice. */
        CHECK(g_fake.seg_calls == 0);
        CHECK_STR(g_fake.text, "抱歉，AI 生成失败，请稍后再试。");

        trigger_unbind_executors(&t);
        trigger_table_free(&t);
    }

    CASE("dialog: empty stream with a crash exit becomes the exit notice");
    {
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.max_len = 200; cfg.reply_prefix = "[AI] ";
        PiClient pi; memset(&pi, 0, sizeof(pi));
        TriggerTable t = {0};
        Trigger *tr = bind_one(&t, "dialog");
        char err[256];
        trigger_bind_executors(&t, err, sizeof(err));

        fake_reset();
        g_fake.ret_raw = "";                 /* no assistant message */
        g_fake.ret_exit_status = 1 << 8;     /* WIFEXITED, status 1 */
        WorkerCtx ctx = fake_ctx(&cfg, &pi);
        TriggerEvent ev = { .trigger = tr, .player = "Steve", .text = "hi" };
        tr->exec->run(tr->exec_state, &ev, &ctx);
        CHECK_STR(g_fake.text, "抱歉，AI 服务返回了错误。");

        trigger_unbind_executors(&t);
        trigger_table_free(&t);
    }
}

static void test_reply_shaper(void)
{
    CASE("reply shaper: registry exposes text, rejects unknown");
    {
        CHECK(reply_shaper_lookup("text") != NULL);
        CHECK(reply_shaper_lookup("command") == NULL);
        CHECK(reply_shaper_lookup(NULL) == NULL);
    }

    CASE("dialog: init rejects an unknown reply shaper");
    {
        TriggerTable t = {0};
        Trigger *tr = bind_one(&t, "dialog");
        tr->reply_shaper = xstrdup("nope");
        char err[256];
        CHECK(trigger_bind_executors(&t, err, sizeof(err)) == -1);
        CHECK(strstr(err, "shaper") != NULL);
        trigger_table_free(&t);
    }
}

static void test_executor_registry(void)
{
    CASE("executor registry: only dialog and replier are built in");
    {
        CHECK(executor_lookup("dialog") != NULL);
        CHECK(executor_lookup("replier") != NULL);
        CHECK(executor_lookup("command") == NULL);   /* D9: next change */
        CHECK(executor_lookup(NULL) == NULL);
    }

    CASE("executor registry: accepted keys are per-executor");
    {
        const ExecutorOps *dlg = executor_lookup("dialog");
        const ExecutorOps *rep = executor_lookup("replier");
        CHECK(executor_accepts_key(dlg, "model"));
        CHECK(executor_accepts_key(rep, "template"));
        CHECK(!executor_accepts_key(dlg, "template"));
        CHECK(!executor_accepts_key(rep, "model"));
    }

    CASE("executor registry: executor=command is an unknown-executor error");
    {
        TriggerTable t = {0};
        Trigger *tr = bind_one(&t, "command");
        char err[256];
        CHECK(trigger_bind_executors(&t, err, sizeof(err)) == -1);
        CHECK(strstr(err, "unknown executor 'command'") != NULL);
        trigger_table_free(&t);
    }
}

static void test_pi_argv(void)
{
    CASE("pi argv: fixed list, no system prompt argument");
    {
        PiClient c;
        memset(&c, 0, sizeof(c));
        c.cmd = (char *)"pi";
        char **argv = pi_build_argv(&c, "Steve: hello");
        int n = 0;
        while (argv[n])
            n++;
        /* pi --mode json --no-session --no-tools -p <prompt> */
        CHECK(n == 7);
        CHECK_STR(argv[0], "pi");
        CHECK_STR(argv[1], "--mode");
        CHECK_STR(argv[2], "json");
        CHECK_STR(argv[3], "--no-session");
        CHECK_STR(argv[4], "--no-tools");
        CHECK_STR(argv[5], "-p");
        CHECK_STR(argv[6], "Steve: hello");
        for (int i = 0; i < n; i++)
            CHECK(strstr(argv[i], "system") == NULL);
        free(argv);
    }

    CASE("pi argv: --model inserted before -p");
    {
        PiClient c;
        memset(&c, 0, sizeof(c));
        c.cmd = (char *)"pi";
        c.model = (char *)"prov/model";
        char **argv = pi_build_argv(&c, "p");
        CHECK_STR(argv[5], "--model");
        CHECK_STR(argv[6], "prov/model");
        CHECK_STR(argv[7], "-p");
        CHECK_STR(argv[8], "p");
        free(argv);
    }
}

/* ---------- worker rate-limit key ---------- */

static void test_rate_key(void)
{
    CASE("rate limit: key distinguishes different triggers");
    {
        Config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.cooldown_ms = 60000;   /* long, so repeats are denied */

        Worker w;
        worker_init(&w, NULL, NULL, NULL, &cfg);

        TriggerTable t = {0};
        Trigger *a0 = trigger_table_append(&t);
        a0->name = xstrdup("chat");
        a0->cooldown_ms = -1;
        Trigger *b0 = trigger_table_append(&t);
        b0->name = xstrdup("join");
        b0->cooldown_ms = -1;
        /* Reacquire: the second append reallocs t.items, so the pointers
         * returned by the appends above may dangle. */
        Trigger *a = &t.items[0];
        Trigger *b = &t.items[1];

        CHECK(worker_rate_allow(&w, a, "Steve"));   /* first time */
        CHECK(!worker_rate_allow(&w, a, "Steve"));  /* same trigger: cooling */
        CHECK(worker_rate_allow(&w, b, "Steve"));   /* other trigger: allowed */
        CHECK(!worker_rate_allow(&w, b, "Steve"));
        CHECK(worker_rate_allow(&w, a, "Alex"));    /* other player: allowed */

        trigger_table_free(&t);
        worker_free(&w);
    }

    CASE("rate limit: a zero cooldown always allows");
    {
        Config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.cooldown_ms = 0;
        Worker w;
        worker_init(&w, NULL, NULL, NULL, &cfg);
        TriggerTable t = {0};
        Trigger *a = trigger_table_append(&t);
        a->name = xstrdup("chat");
        a->cooldown_ms = -1;
        CHECK(worker_rate_allow(&w, a, "Steve"));
        CHECK(worker_rate_allow(&w, a, "Steve"));
        trigger_table_free(&t);
        worker_free(&w);
    }
}

/* ---------- configuration file ---------- */

static char g_tmpfile[80];

static const char *write_tmp(const char *content)
{
    static int seq = 0;
    snprintf(g_tmpfile, sizeof(g_tmpfile), "/tmp/pi-mc-bridge-test-%d-%d.conf",
             (int)getpid(), seq++);
    FILE *f = fopen(g_tmpfile, "w");
    if (!f)
        return NULL;
    fputs(content, f);
    fclose(f);
    return g_tmpfile;
}

static void test_configfile(void)
{
    CASE("configfile: minimal trigger section");
    {
        const char *path = write_tmp(
            "# a minimal config\n"
            "\n"
            "[trigger:chat]\n"
            "match = chat\n"
            "keywords = @pi\n");
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.allow_list = NULL; cfg.allow_count = 0;
        char err[256];
        CHECK(configfile_load(path, &cfg, err, sizeof(err)) == 0);
        CHECK(cfg.triggers.count == 1);
        if (cfg.triggers.count == 1) {
            Trigger *tr = &cfg.triggers.items[0];
            CHECK_STR(tr->name, "chat");
            CHECK(tr->match == MATCH_CHAT);
            CHECK_STR(tr->extractor, "chat");     /* defaulted from match */
            CHECK(tr->cooldown_ms == -1);         /* inherit global */
        }
        trigger_table_free(&cfg.triggers);
        unlink(path);
    }

    CASE("configfile: keywords are comma-split and trimmed");
    {
        const char *path = write_tmp(
            "[trigger:chat]\n"
            "keywords = @pi, @ai ,@ci\n");
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        char err[256];
        CHECK(configfile_load(path, &cfg, err, sizeof(err)) == 0);
        CHECK(cfg.triggers.count == 1);
        if (cfg.triggers.count == 1) {
            Trigger *tr = &cfg.triggers.items[0];
            CHECK(tr->keyword_count == 3);
            CHECK_STR(tr->keywords[0], "@pi");
            CHECK_STR(tr->keywords[1], "@ai");
            CHECK_STR(tr->keywords[2], "@ci");
        }
        trigger_table_free(&cfg.triggers);
        unlink(path);
    }

    CASE("configfile: comments and blank lines are ignored");
    {
        const char *path = write_tmp(
            "; leading comment\n"
            "\n"
            "[global]\n"
            "# inline-style comment line\n"
            "max_len = 120\n"
            "\n");
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.max_len = 0;
        char err[256];
        CHECK(configfile_load(path, &cfg, err, sizeof(err)) == 0);
        CHECK(cfg.max_len == 120);
        free(cfg.rcon_host); free(cfg.rcon_pass); free(cfg.log_file);
        free(cfg.prefix); free(cfg.reply_prefix); free(cfg.pi_cmd);
        free(cfg.pi_model); free(cfg.allow);
        trigger_table_free(&cfg.triggers);
        unlink(path);
    }

    CASE("configfile: unknown global key is a hard error");
    {
        const char *path = write_tmp(
            "[global]\n"
            "bogus = 1\n");
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        char err[256];
        CHECK(configfile_load(path, &cfg, err, sizeof(err)) == -1);
        CHECK(strstr(err, "unknown key 'bogus'") != NULL);
        trigger_table_free(&cfg.triggers);
        unlink(path);
    }

    CASE("configfile: prompt-related keys are rejected");
    {
        const char *p1 = write_tmp(
            "[trigger:chat]\n"
            "keywords = @pi\n"
            "prompt_template = {text}\n");
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        char err[256];
        CHECK(configfile_load(p1, &cfg, err, sizeof(err)) == -1);
        CHECK(strstr(err, "prompt_template") != NULL);
        trigger_table_free(&cfg.triggers);
        unlink(p1);

        const char *p2 = write_tmp(
            "[global]\n"
            "system = you are helpful\n");
        Config cfg2; memset(&cfg2, 0, sizeof(cfg2));
        CHECK(configfile_load(p2, &cfg2, err, sizeof(err)) == -1);
        CHECK(strstr(err, "system") != NULL);
        unlink(p2);
    }

    CASE("configfile: an empty keywords list is rejected");
    {
        const char *path = write_tmp(
            "[trigger:chat]\n"
            "keywords = \n");
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        char err[256];
        CHECK(configfile_load(path, &cfg, err, sizeof(err)) == -1);
        CHECK(strstr(err, "empty keywords") != NULL);
        trigger_table_free(&cfg.triggers);
        unlink(path);
    }

    CASE("configfile: executor = command is rejected as unknown");
    {
        const char *path = write_tmp(
            "[trigger:chat]\n"
            "keywords = @pi\n"
            "executor = command\n");
        Config cfg; memset(&cfg, 0, sizeof(cfg));
        char err[256];
        CHECK(configfile_load(path, &cfg, err, sizeof(err)) == -1);
        CHECK(strstr(err, "unknown executor 'command'") != NULL);
        unlink(path);
    }
}

static void test_config_precedence(void)
{
    /* Make sure ambient environment does not leak into the precedence checks. */
    unsetenv("CONFIG_FILE");
    unsetenv("MAX_LEN");

    CASE("config: CLI overrides the config file");
    {
        const char *path = write_tmp(
            "[global]\n"
            "max_len = 100\n"
            "[trigger:chat]\n"
            "keywords = @pi\n");
        Config cfg;
        char *argv[] = { "prog", "--config", (char *)path, "--max-len", "50" };
        CHECK(config_load(&cfg, 5, argv) == 0);
        CHECK(cfg.max_len == 50);
        config_free(&cfg);
        unlink(path);
    }

    CASE("config: env overrides the config file");
    {
        const char *path = write_tmp(
            "[global]\n"
            "max_len = 100\n"
            "[trigger:chat]\n"
            "keywords = @pi\n");
        setenv("MAX_LEN", "77", 1);
        Config cfg;
        char *argv[] = { "prog", "--config", (char *)path };
        CHECK(config_load(&cfg, 3, argv) == 0);
        CHECK(cfg.max_len == 77);
        config_free(&cfg);
        unsetenv("MAX_LEN");
        unlink(path);
    }

    CASE("config: a trigger field falls back to the merged global");
    {
        const char *path = write_tmp(
            "[global]\n"
            "cooldown = 12\n"
            "[trigger:chat]\n"
            "keywords = @pi\n");
        Config cfg;
        char *argv[] = { "prog", "--config", (char *)path };
        CHECK(config_load(&cfg, 3, argv) == 0);
        CHECK(cfg.cooldown_ms == 12000);
        CHECK(cfg.triggers.count == 1);
        if (cfg.triggers.count == 1) {
            CHECK(cfg.triggers.items[0].cooldown_ms == -1); /* inherit */
            CHECK(cfg.triggers.items[0].max_len == 0);      /* inherit */
        }
        config_free(&cfg);
        unlink(path);
    }

    CASE("config: with no file one trigger is synthesized");
    {
        Config cfg;
        char *argv[] = { "prog", "--prefix", "@pi", "--rcon-pass", "x" };
        CHECK(config_load(&cfg, 5, argv) == 0);
        CHECK(cfg.triggers.count == 1);
        if (cfg.triggers.count == 1) {
            CHECK_STR(cfg.triggers.items[0].name, "default");
            CHECK_STR(cfg.triggers.items[0].extractor, "chat");
            CHECK_STR(cfg.triggers.items[0].executor, "dialog");

            /* The synthesized trigger yields {Steve, hello} for a chat line. */
            TriggerEvent ev;
            bool ok = trigger_match(
                &cfg.triggers,
                "[12:34:56] [Server thread/INFO]: <Steve> @pi hello", &ev);
            CHECK(ok);
            if (ok) {
                CHECK_STR(ev.player, "Steve");
                CHECK_STR(ev.text, "hello");
                trigger_event_free(&ev);
            }
        }
        config_free(&cfg);
    }
}

int main(void)
{
    test_util();
    test_parser();
    test_trigger();
    test_template();
    test_replier();
    test_dialog();
    test_reply_shaper();
    test_executor_registry();
    test_pi_parse();
    test_pi_argv();
    test_rcon();
    test_worker_tellraw();
    test_rate_key();
    test_queue();
    test_configfile();
    test_config_precedence();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
