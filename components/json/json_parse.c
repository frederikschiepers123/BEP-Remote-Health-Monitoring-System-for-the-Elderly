#define LOG_TAG "JSON_PARSE"
#include "json_parse.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Minimal jsmn-style tokenizer ─────────────────────────────────────────
 *
 * This is a compact, allocation-free JSON tokenizer.  It is intentionally
 * small — the full jsmn library is ~600 lines; this covers the same API
 * surface needed for the two inbound topics we parse (cmd, time/set). */

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int json_tokenize(const char *js, size_t len,
                  JsonToken *tokens, size_t max_tokens)
{
    int tok_idx = 0;
    int depth   = 0;
    /* parent token index stack — only tracks objects/arrays for size counting */
    int parent[16];
    int parent_top = -1;

    for (size_t i = 0; i < len; i++) {
        char c = js[i];
        if (is_ws(c)) continue;

        switch (c) {
        case '{':
        case '[': {
            if ((size_t)tok_idx >= max_tokens) return -1;
            JsonToken *t = &tokens[tok_idx];
            t->type  = (c == '{') ? JSON_TOK_OBJECT : JSON_TOK_ARRAY;
            t->start = (int)i;
            t->end   = -1;
            t->size  = 0;
            if (parent_top >= 0) {
                tokens[parent[parent_top]].size++;
            }
            if (parent_top < 15) {
                parent[++parent_top] = tok_idx;
            }
            tok_idx++;
            depth++;
            break;
        }
        case '}':
        case ']': {
            depth--;
            if (parent_top >= 0) {
                tokens[parent[parent_top]].end = (int)i + 1;
                parent_top--;
            }
            break;
        }
        case '"': {
            if ((size_t)tok_idx >= max_tokens) return -1;
            JsonToken *t = &tokens[tok_idx];
            t->type  = JSON_TOK_STRING;
            t->start = (int)i + 1;
            t->size  = 0;
            i++;
            for (; i < len; i++) {
                if (js[i] == '\\') { i++; continue; }
                if (js[i] == '"')  { break; }
            }
            t->end = (int)i;
            if (parent_top >= 0) tokens[parent[parent_top]].size++;
            tok_idx++;
            break;
        }
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        case 't': case 'f': case 'n': {
            if ((size_t)tok_idx >= max_tokens) return -1;
            JsonToken *t = &tokens[tok_idx];
            t->type  = JSON_TOK_PRIMITIVE;
            t->start = (int)i;
            t->size  = 0;
            for (; i < len; i++) {
                char p = js[i];
                if (p == ' ' || p == '\t' || p == '\r' || p == '\n' ||
                    p == ',' || p == '}' || p == ']') {
                    break;
                }
            }
            t->end = (int)i;
            i--;   /* outer loop will increment */
            if (parent_top >= 0) tokens[parent[parent_top]].size++;
            tok_idx++;
            break;
        }
        default:
            break;
        }
    }

    (void)depth;
    return tok_idx;
}

/* ── Field extractors ──────────────────────────────────────────────────── */

static bool tok_eq(const char *js, const JsonToken *t, const char *s) {
    size_t slen = strlen(s);
    int tlen = t->end - t->start;
    if (tlen < 0 || (size_t)tlen != slen) return false;
    return memcmp(js + t->start, s, slen) == 0;
}

err_t json_get_string(const char *js, const JsonToken *toks, int n,
                      const char *key, char *out, size_t cap)
{
    /* toks[0] is the root object; keys are at odd indices among its children */
    for (int i = 1; i < n - 1; i++) {
        if (toks[i].type == JSON_TOK_STRING && tok_eq(js, &toks[i], key)) {
            const JsonToken *val = &toks[i + 1];
            if (val->type != JSON_TOK_STRING) return ERR_NOT_FOUND;
            int vlen = val->end - val->start;
            if (vlen < 0 || (size_t)vlen >= cap) return ERR_OVERFLOW;
            memcpy(out, js + val->start, (size_t)vlen);
            out[vlen] = '\0';
            return ERR_OK;
        }
    }
    return ERR_NOT_FOUND;
}

err_t json_get_int64(const char *js, const JsonToken *toks, int n,
                     const char *key, int64_t *out)
{
    for (int i = 1; i < n - 1; i++) {
        if (toks[i].type == JSON_TOK_STRING && tok_eq(js, &toks[i], key)) {
            const JsonToken *val = &toks[i + 1];
            if (val->type != JSON_TOK_PRIMITIVE) return ERR_NOT_FOUND;
            char tmp[24];
            int vlen = val->end - val->start;
            if (vlen <= 0 || vlen >= (int)sizeof(tmp)) return ERR_INVALID_ARG;
            memcpy(tmp, js + val->start, (size_t)vlen);
            tmp[vlen] = '\0';
            char *end_ptr;
            *out = (int64_t)strtoll(tmp, &end_ptr, 10);
            return (*end_ptr == '\0') ? ERR_OK : ERR_INVALID_ARG;
        }
    }
    return ERR_NOT_FOUND;
}

/* ── High-level helpers ─────────────────────────────────────────────────── */

err_t json_parse_cmd(const char *payload, size_t len,
                     char *out_cmd, size_t out_cap)
{
#define CMD_MAX_TOKS 8
    JsonToken toks[CMD_MAX_TOKS];
    int n = json_tokenize(payload, len, toks, CMD_MAX_TOKS);
    if (n < 1) {
        LOG_W("cmd payload: tokenize failed (%d)", n);
        return ERR_INVALID_ARG;
    }
    err_t err = json_get_string(payload, toks, n, "cmd", out_cmd, out_cap);
    if (err != ERR_OK) {
        LOG_W("cmd payload: 'cmd' field missing");
    }
    return err;
}

err_t json_parse_time_set(const char *payload, size_t len, int64_t *out_epoch_ms)
{
#define TIME_MAX_TOKS 6
    JsonToken toks[TIME_MAX_TOKS];
    int n = json_tokenize(payload, len, toks, TIME_MAX_TOKS);
    if (n < 1) {
        LOG_W("time/set payload: tokenize failed (%d)", n);
        return ERR_INVALID_ARG;
    }
    err_t err = json_get_int64(payload, toks, n, "epoch_ms", out_epoch_ms);
    if (err != ERR_OK) {
        LOG_W("time/set payload: 'epoch_ms' field missing");
    }
    return err;
}
