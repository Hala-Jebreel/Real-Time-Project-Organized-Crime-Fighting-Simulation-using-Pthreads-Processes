/* file: json.h */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>  // for size_t

#ifdef __cplusplus
extern "C" {
#endif

/* JSON token types */
typedef enum {
    JSON_UNDEFINED = 0,
    JSON_OBJECT    = 1,
    JSON_ARRAY     = 2,
    JSON_STRING    = 3,
    JSON_PRIMITIVE = 4
} jsontype_t;

/* JSON parse error codes */
enum {
    JSON_ERROR_NOMEM = -1,
    JSON_ERROR_INVAL = -2,
    JSON_ERROR_PART  = -3
};

/* JSON token descriptor */
typedef struct {
    jsontype_t type;
    int        start;
    int        end;
    int        size;
} jsontok_t;

/* JSON parser state */
typedef struct {
    unsigned int pos;
    unsigned int toknext;
    int          toksuper;
} json_parser;

/* Initialize parser */
void json_init(json_parser *parser);

/* Parse JSON text into tokens; returns number of tokens or <0 on error */
int json_parse(json_parser *parser,
               const char *js, size_t len,
               jsontok_t *tokens, unsigned int num_tokens);
//______________________________________________________________Talin SUN
int json_load(const char *path, char **out_js, jsontok_t *tokens, int *out_n);

#ifdef __cplusplus
}
#endif

#endif /* JSON_H */
