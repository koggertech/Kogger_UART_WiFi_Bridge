/*
 * proto.h - control-line helpers (docs/PROTOCOL.md, section 3).
 *
 * Field values are percent-encoded: bytes 0x00-0x20, '%' and 0x7F-0xFF become %XX
 * (upper-case hex). Everything else is literal. Portable C99, host-testable.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_MAX_ARGS 12

/**
 * Percent-encode n bytes into out (NUL-terminated).
 * @return encoded length (without NUL), or -1 if cap is too small (3*n+1 always suffices).
 */
int pct_encode(const uint8_t *in, size_t n, char *out, size_t cap);

/**
 * Decode a NUL-terminated percent-encoded string.
 * @return decoded length, or -1 on malformed input or if cap is too small.
 *         out is NUL-terminated when the result is >= 0 and fits (cap > result).
 */
int pct_decode(const char *in, uint8_t *out, size_t cap);

typedef struct {
    const char *tag;                 /**< first token, echoed in the reply */
    const char *cmd;                 /**< second token, upper-case command */
    int         argc;
    const char *argv[PROTO_MAX_ARGS];
} proto_line_t;

/**
 * Split a mutable NUL-terminated line in place on runs of spaces.
 * @return 0 on success, -1 if tag or command is missing or there are too many arguments.
 */
int proto_parse(char *line, proto_line_t *out);

/** Return the (still encoded) value of "key=value" among args, or NULL if absent. */
const char *proto_arg(const proto_line_t *l, const char *key);

#ifdef __cplusplus
}
#endif
