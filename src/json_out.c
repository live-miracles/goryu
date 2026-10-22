#include "json_out.h"

/* Escapes s for use inside a JSON string: quote, backslash and control characters;
 * bytes of 0x20 and above (including UTF-8) pass through unchanged. */
void json_write_escaped(FILE *f, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '\\':
            fputs("\\\\", f);
            break;
        case '"':
            fputs("\\\"", f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            if (*p < 0x20) {
                fprintf(f, "\\u%04x", *p);
            } else {
                fputc(*p, f);
            }
            break;
        }
    }
}

/* Writes ,"<key>": (no leading comma for the first field in an object),
 * advancing *first. Callers write the value immediately after. Output is
 * compact (no pretty-printing) since nothing requires this JSON to be
 * human-formatted. */
void json_key(FILE *f, int *first, const char *key) {
    fprintf(f, "%s\"%s\":", *first ? "" : ",", key);
    *first = 0;
}

void json_bool(FILE *f, int *first, const char *key, int val) {
    json_key(f, first, key);
    fputs(val ? "true" : "false", f);
}

void json_str(FILE *f, int *first, const char *key, const char *val) {
    json_key(f, first, key);
    if (!val) {
        fputs("null", f);
        return;
    }
    fputc('"', f);
    json_write_escaped(f, val);
    fputc('"', f);
}

void json_ll(FILE *f, int *first, const char *key, long long val) {
    json_key(f, first, key);
    fprintf(f, "%lld", val);
}

void json_int(FILE *f, int *first, const char *key, int val) {
    json_key(f, first, key);
    fprintf(f, "%d", val);
}

void json_ll_opt(FILE *f, int *first, const char *key, int valid, long long val) {
    json_key(f, first, key);
    if (valid)
        fprintf(f, "%lld", val);
    else
        fputs("null", f);
}

void json_int_opt(FILE *f, int *first, const char *key, int valid, int val) {
    json_key(f, first, key);
    if (valid)
        fprintf(f, "%d", val);
    else
        fputs("null", f);
}

void json_dbl_opt(FILE *f, int *first, const char *key, int valid, double val) {
    json_key(f, first, key);
    if (valid)
        fprintf(f, "%.3f", val);
    else
        fputs("null", f);
}
