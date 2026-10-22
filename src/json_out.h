#ifndef JSON_OUT_H
#define JSON_OUT_H

#include <stdio.h>

/* Small writers for the status API's compact JSON. Each json_* function writes
 * `"key":value`, preceded by a comma unless *first is set, and clears *first. */

void json_write_escaped(FILE *f, const char *s);
void json_key(FILE *f, int *first, const char *key);
void json_bool(FILE *f, int *first, const char *key, int val);
void json_str(FILE *f, int *first, const char *key, const char *val);
void json_ll(FILE *f, int *first, const char *key, long long val);
void json_int(FILE *f, int *first, const char *key, int val);
/* The *_opt writers emit null when valid is zero. */
void json_ll_opt(FILE *f, int *first, const char *key, int valid, long long val);
void json_int_opt(FILE *f, int *first, const char *key, int valid, int val);
void json_dbl_opt(FILE *f, int *first, const char *key, int valid, double val);

#endif
