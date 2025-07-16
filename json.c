/* file: json.c */
#include "json.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>    // malloc, free



/* Allocate next token in the pool */
static jsontok_t *json_alloc_token(json_parser *parser,
                                   jsontok_t *tokens,
                                   size_t max_tokens)
{
    if (parser->toknext >= max_tokens)
        return NULL;
    jsontok_t *tok = &tokens[parser->toknext++];
    tok->start = tok->end = tok->size = 0;
    return tok;
}

/* Fill token with provided type and boundaries */
static void json_fill_token(jsontok_t *token, jsontype_t type,
                            int start, int end)
{
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

void json_init(json_parser *parser)
{
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

static int tok_eq(const char *json, jsontok_t *t, const char *s)
{
    int lens = (int)strlen(s);
    return (t->end - t->start == lens) && (strncmp(json + t->start, s, lens) == 0);
}

int json_parse(json_parser *parser,
               const char *js, size_t len,
               jsontok_t *tokens, unsigned int num_tokens)
{
    int i;
    for (i = parser->pos; i < (int)len; i++)
    {
        char c = js[i];
        switch (c)
        {
        case '{':
        case '[':
        {
            jsontok_t *t = json_alloc_token(parser, tokens, num_tokens);
            if (!t)
                return JSON_ERROR_NOMEM;
            json_fill_token(t,
                            c == '{' ? JSON_OBJECT : JSON_ARRAY,
                            i, -1);
            /* —— NEW: count this object/array in its parent —— */
            if (parser->toksuper != -1)
            {
                tokens[parser->toksuper].size++;
            }
            /* now this becomes the new “parent” for nested tokens */
            parser->toksuper = parser->toknext - 1;
            break;
        }
        case '}':
        case ']':
        {
            int matched = -1;
            // 1) find & close the matching token
            for (int j = parser->toknext - 1; j >= 0; j--)
            {
                if (tokens[j].start != -1 && tokens[j].end == -1 && (tokens[j].type == JSON_OBJECT || tokens[j].type == JSON_ARRAY))
                {
                    tokens[j].end = i + 1;
                    matched = j;
                    break;
                }
            }
            // 2) restore super-token to the next-outer open object/array
            parser->toksuper = -1;
            if (matched >= 0)
            {
                for (int k = matched - 1; k >= 0; k--)
                {
                    if (tokens[k].start != -1 && tokens[k].end == -1 && (tokens[k].type == JSON_OBJECT || tokens[k].type == JSON_ARRAY))
                    {
                        parser->toksuper = k;
                        break;
                    }
                }
            }
            break;
        }
        case '"':
        {
            int start = i + 1;
            i++;
            for (; i < (int)len; i++)
            {
                if (js[i] == '"')
                    break;
                if (js[i] == '\\' && i + 1 < (int)len)
                    i++;
            }
            jsontok_t *t = json_alloc_token(parser, tokens, num_tokens);
            if (!t)
                return JSON_ERROR_NOMEM;
            json_fill_token(t, JSON_STRING, start, i);
            if (parser->toksuper != -1)
                tokens[parser->toksuper].size++;
            break;
        }
        case '\t':
        case '\r':
        case '\n':
        case ' ':
            break;
        case ':':
            parser->toksuper = parser->toknext - 1;
            break;
        case ',':
            break;
        default:
            if ((c >= '0' && c <= '9') || c == '-' || c == '+')
            {
                int start = i;
                while (i < (int)len && ((js[i] >= '0' && js[i] <= '9') ||
                                        js[i] == '.' || js[i] == 'e' ||
                                        js[i] == 'E' || js[i] == '+' ||
                                        js[i] == '-'))
                    i++;
                jsontok_t *t = json_alloc_token(parser, tokens, num_tokens);
                if (!t)
                    return JSON_ERROR_NOMEM;
                json_fill_token(t, JSON_PRIMITIVE, start, i);
                if (parser->toksuper != -1)
                    tokens[parser->toksuper].size++;
                i--;
            }
        }
    }
    parser->pos = i;
    return parser->toknext;
}

//_______________________________________________________-Talin SUN
static char *read_file_crimes(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}
#define MAX_TOKENS 512 /* enough to cover large JSON */
/**
 * Load a JSON file and produce its raw text and tokens.
 * @param path   filesystem path to JSON
 * @param out_js returns malloc'd JSON text (must be free’d)
 * @param tokens caller-allocated array of jsontok_t[MAX_JSON_TOKENS]
 * @param out_n  returns number of tokens produced
 * @return 0 on success, –1 on error
 */
int json_load(const char *path, char **out_js, jsontok_t *tokens, int *out_n)
{
    *out_js = read_file_crimes(path);
    if (!*out_js) { perror("read_file"); return -1; }

    json_parser parser;
    json_init(&parser);

    int n = json_parse(&parser, *out_js, strlen(*out_js), tokens, MAX_TOKENS);
    if (n < 0) {
        fprintf(stderr, "JSON parse error %d\n", n);
        free(*out_js);
        return -1;
    }

    *out_n = n;
    return 0;
}

/// print out all the crimes[]
// void parse_and_print_crimes(const char *js, jsontok_t *toks, int ntoks)
// {
//     int i;
//     for (i = 1; i < ntoks; i++)
//     {
//         // find the "crimes" key
//         if (tok_eq(js, &toks[i], "crimes"))
//         {
//             // the very next token is the array containing crime objects
//             int arr_i = i + 1;
//             int n_crimes = toks[arr_i].size;
//             int idx = arr_i + 1;

//             printf("Found %d crimes:\n", n_crimes);

//             // for each object in crimes[]
//             for (int c = 0; c < n_crimes; c++)
//             {
//                 jsontok_t *obj = &toks[idx++];
//                 int n_fields = obj->size;

//                 printf(" Crime #%d:\n", c + 1);
//                 // iterate its fields: each field is 1 key token + 1 value token
//                 for (int f = 0; f < n_fields; f++)
//                 {
//                     jsontok_t *key = &toks[idx++];
//                     jsontok_t *val = &toks[idx++];

//                     int keylen = key->end - key->start;
//                     char keybuf[32];
//                     strncpy(keybuf, js + key->start, keylen);
//                     keybuf[keylen] = 0;

//                     if (val->type == JSON_STRING)
//                     {
//                         int vlen = val->end - val->start;
//                         char vbuf[128];
//                         strncpy(vbuf, js + val->start, vlen);
//                         vbuf[vlen] = 0;
//                         printf("   %.*s: %s\n", keylen, keybuf, vbuf);
//                     }
//                     else if (val->type == JSON_ARRAY)
//                     {
//                         int arr_sz = val->size;
//                         int vidx = (idx); // first element of this inner array
//                         printf("   %.*s:\n", keylen, keybuf);
//                         // walk the array of strings
//                         for (int a = 0; a < arr_sz; a++)
//                         {
//                             jsontok_t *strt = &toks[vidx++];
//                             int slen = strt->end - strt->start;
//                             char sbuf[128];
//                             strncpy(sbuf, js + strt->start, slen);
//                             sbuf[slen] = 0;
//                             printf("     - %s\n", sbuf);
//                         }
//                         idx = vidx;
//                     }
//                     else
//                     {
//                         // handle numbers/primitives if you like
//                         int vlen = val->end - val->start;
//                         char vbuf[32];
//                         strncpy(vbuf, js + val->start, vlen);
//                         vbuf[vlen] = 0;
//                         printf("   %.*s: %s\n", keylen, keybuf, vbuf);
//                     }
//                 }
//             }
//             return; // done
//         }
//     }
//     fprintf(stderr, "ERROR: no \"crimes\" key found\n");
// }
