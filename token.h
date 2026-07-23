#ifndef LEXER_H
#define LEXER_H

#include <sys/queue.h>

enum tk_type {
    TK_STRING,
    TK_COMMA,
    TK_NUMBER,
    TK_PLUS,
};

TAILQ_HEAD(tk_queue, tk_item);
struct tk_item {
    enum tk_type type;
    unsigned int line;
    char *literal;
    TAILQ_ENTRY(tk_item) entries;
};

struct tk_queue tk_queue_init(void);
void tk_queue_add(struct tk_queue *, enum tk_type, const char *, unsigned int);
void tk_queue_free(struct tk_queue *);
void tk_queue_print(struct tk_queue *);       /* debugging */
void tokenize(struct tk_queue *, unsigned int, const char *, int);

#endif
