#ifndef JSON_PARSE_H
#define JSON_PARSE_H

/* Minimal JSON tokenizer for inbound MQTT payloads (commands and time sync).
 *
 * Inbound topics that carry structured JSON:
 *   rmms/<uuid>/cmd       — {"cmd":"activate"} etc.  (§9.3)
 *   rmms/<uuid>/time/set  — {"epoch_ms":1716210000000} (§9.2.5)
 *
 * Both have a closed, shallow schema (nesting depth = 1).  A full DOM
 * library is not needed.  This module provides:
 *   1. A tokenizer (jsmn-compatible, ~600 lines in .c) that splits a JSON
 *      string into an array of tokens.
 *   2. Helper functions to extract named string and integer fields from the
 *      token array.
 *
 * No allocation.  Token array is caller-supplied (stack-allocated). */

#include "err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Token type ─────────────────────────────────────────────────────────── */

typedef enum {
    JSON_TOK_UNDEFINED = 0,
    JSON_TOK_OBJECT,
    JSON_TOK_ARRAY,
    JSON_TOK_STRING,
    JSON_TOK_PRIMITIVE,   /* number, bool, null */
} JsonTokType;

typedef struct {
    JsonTokType type;
    int         start;   /* byte offset into input string */
    int         end;
    int         size;    /* number of child tokens (for objects/arrays) */
} JsonToken;

/* ── Tokenizer ──────────────────────────────────────────────────────────── */

/* Parse json_str of length len into tokens[0..max_tokens-1].
 * Returns number of tokens parsed (>= 1 for valid JSON), or negative on error:
 *   -1  insufficient token buffer
 *   -2  invalid JSON
 *   -3  partial JSON (needs more input) */
int json_tokenize(const char *json_str, size_t len,
                  JsonToken *tokens, size_t max_tokens);

/* ── Field extractors ───────────────────────────────────────────────────── */

/* Find a string-typed field by key in a top-level JSON object and copy its
 * value into out_buf (NUL-terminated).  Returns ERR_OK or ERR_NOT_FOUND. */
err_t json_get_string(const char *json_str, const JsonToken *tokens, int n_tokens,
                      const char *key, char *out_buf, size_t out_cap);

/* Find an integer field by key.  Returns ERR_OK or ERR_NOT_FOUND. */
err_t json_get_int64(const char *json_str, const JsonToken *tokens, int n_tokens,
                     const char *key, int64_t *out);

/* ── High-level helpers ─────────────────────────────────────────────────── */

/* Parse a command payload and copy the "cmd" string into out (max 32 bytes).
 * Returns ERR_OK on success, ERR_NOT_FOUND if "cmd" field is missing,
 * ERR_INVALID_ARG if payload is malformed. */
err_t json_parse_cmd(const char *payload, size_t len,
                     char *out_cmd, size_t out_cap);

/* Parse a time-set payload and extract epoch_ms.
 * Returns ERR_OK on success, ERR_NOT_FOUND or ERR_INVALID_ARG on failure. */
err_t json_parse_time_set(const char *payload, size_t len, int64_t *out_epoch_ms);

#endif /* JSON_PARSE_H */
