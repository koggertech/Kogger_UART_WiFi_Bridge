#include "proto.h"

#include <string.h>

static const char HEX[] = "0123456789ABCDEF";

static int must_escape(uint8_t c)
{
    return c <= 0x20 || c == '%' || c >= 0x7F;
}

int pct_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        size_t need = must_escape(c) ? 3 : 1;
        if (pos + need + 1 > cap)
            return -1;
        if (need == 3) {
            out[pos++] = '%';
            out[pos++] = HEX[c >> 4];
            out[pos++] = HEX[c & 15];
        } else {
            out[pos++] = (char)c;
        }
    }
    if (pos + 1 > cap)
        return -1;
    out[pos] = '\0';
    return (int)pos;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int pct_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t pos = 0;
    for (size_t i = 0; in[i]; i++) {
        uint8_t c = (uint8_t)in[i];
        if (c == '%') {
            int hi = hexval(in[i + 1]);
            int lo = hi < 0 ? -1 : hexval(in[i + 2]);
            if (lo < 0)
                return -1;
            c = (uint8_t)(hi << 4 | lo);
            i += 2;
        }
        if (pos >= cap)
            return -1;
        out[pos++] = c;
    }
    if (pos < cap)
        out[pos] = '\0';
    return (int)pos;
}

int proto_parse(char *line, proto_line_t *out)
{
    const char *tok[PROTO_MAX_ARGS + 2];
    int n = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ')
            *p++ = '\0';
        if (!*p)
            break;
        if (n == PROTO_MAX_ARGS + 2)
            return -1;
        tok[n++] = p;
        while (*p && *p != ' ')
            p++;
    }
    if (n < 2)
        return -1;
    out->tag = tok[0];
    out->cmd = tok[1];
    out->argc = n - 2;
    for (int i = 0; i < out->argc; i++)
        out->argv[i] = tok[i + 2];
    return 0;
}

const char *proto_arg(const proto_line_t *l, const char *key)
{
    size_t kl = strlen(key);
    for (int i = 0; i < l->argc; i++) {
        if (strncmp(l->argv[i], key, kl) == 0 && l->argv[i][kl] == '=')
            return l->argv[i] + kl + 1;
    }
    return NULL;
}
