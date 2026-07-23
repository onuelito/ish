#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "token.h"

static enum tk_type tk_get_type(const char *);
static void tk_items_grow(int, struct tk_item ***, int *, const char *);

struct tk_queue
tk_queue_init()
{
    struct tk_queue queue;
    TAILQ_INIT(&queue);
    return queue;
}

void
tk_queue_free(struct tk_queue *queue)
{
    struct tk_item *np;

    while ((np = TAILQ_FIRST(queue))) {
        TAILQ_REMOVE(queue, np, entries);
        free((void *)np->literal);
        free((void *)np);
    }
}

void
tk_queue_add(struct tk_queue *queue, enum tk_type type, const char *literal, unsigned int line)
{
    if (strlen(literal) == 0)
        return;

    struct tk_item *item = malloc(sizeof(struct tk_item));

    if (item == NULL)
        err(1, "failed to allocate item");

    item->literal = malloc(strlen(literal) + 1);
    memcpy((void *)item->literal, literal, strlen(literal));
    item->literal[strlen(literal)] = '\0';

    item->type = type;
    item->line = line;

    if (TAILQ_EMPTY(queue)) {
        TAILQ_INSERT_HEAD(queue, item, entries);
    } else {
        TAILQ_INSERT_TAIL(queue, item, entries);
    }
}

void
tk_queue_print(struct tk_queue *list)
{
	struct tk_item *node;
	TAILQ_FOREACH(node, list, entries) {
		printf("%s\n", node->literal);
	}
}

/*
 * Generates a NULL terminated array of a NULL value
 */
void
tokenize(struct tk_queue *list, unsigned int lindx, const char *entry, int len)
{
    char *c;
    int qpos = 0;       /* Position in the queue */
    int icount = 0;
    char queue[len];    /* String to be copied in tk_item.literal */
    struct tk_item **items = NULL;

    c = (char *)entry;

    while (*c) {
        switch (*c) {
        case '\t':
        case ' ':
        case '\n':
            queue[qpos] = '\0';
            qpos = 0;

            tk_queue_add(list, TK_STRING, queue, lindx);
            break;

		case '1': case '2': case '3': case '4': case '5':
		case '6': case '7': case '8': case '9': case '0':
			while (*c &&
				 	(*c == '1' || *c == '2' || *c == '3' || *c == '4' || *c == '5' ||
					 *c == '6' || *c == '7' || *c == '8' || *c == '9' || *c == '0')) {
				queue[qpos++] = *c;
				(void)*c++;
			}
			queue[qpos] = '\0';
			qpos = 0;
			tk_queue_add(list, TK_NUMBER, queue, lindx);
			break;

        case ',':
            queue[qpos] = '\0';
            qpos = 0;
            tk_queue_add(list, TK_STRING, queue, lindx);
            tk_queue_add(list, TK_COMMA, ",", lindx);
            break;
        case '+':
            queue[qpos] = '\0';
            qpos = 0;
            tk_queue_add(list, TK_STRING, queue, lindx);
            tk_queue_add(list, TK_PLUS, "+", lindx);
            break;

        case '"': {
            (void)*c++;

            while (*c && *c != '"') {
                queue[qpos++] = *c;
                (void)*c++;
            }

            queue[qpos] = '\0';
            qpos = 0;
            tk_queue_add(list, TK_STRING, queue, lindx);
            break;
        }

        default:
            queue[qpos++] = *c;
            break;
        }
        c++;
    }

    if (qpos > 0) {
        queue[qpos] = '\0';
        tk_queue_add(list, TK_STRING, queue, lindx);
    }
}
