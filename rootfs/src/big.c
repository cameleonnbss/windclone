/* WindClone -- a second specimen, longer than one editor's buffer.
 *
 * Also data, and also never compiled: nothing in the Makefile looks inside
 * rootfs/. This one exists because /bin/note had to meet a file bigger than the
 * RAM it owns, and a specimen that fits proves nothing about what happens when
 * one does not.
 *
 * NOTE_DOC_MAX in src/user/note.c is 2048 and the store holds 4096, so the
 * interesting case is a file in between: readable in part, unwritable in whole.
 * An editor that showed the first two thousand bytes and saved them back would
 * answer an ordinary Ctrl+S by destroying a source file, while reporting a clean
 * write. This file is the thing that has to be refused, and the headless test
 * that opens it is the reason the refusal is a feature and not a rumour.
 *
 * Written as a plausible translation unit, because a specimen of noise would
 * exercise different lexer branches than a specimen somebody might really open:
 * a block comment, a line comment, a preprocessor line, types, keywords,
 * numbers, and a string. Pure ASCII -- the 5x7 font has no other glyphs.
 */
#include <stdio.h>

#define TABLE_SIZE   64
#define STEP         7

struct bucket {
    unsigned long key;
    unsigned long hits;
};

static struct bucket table[TABLE_SIZE];
static unsigned long inserted;
static int collisions;

/* One index per key, by multiply-shift: no division anywhere, and a
 * distribution a reader can check by eye. */
static unsigned index_of(unsigned long key)
{
    unsigned long mixed = key * 2654435761UL;

    mixed ^= (mixed >> 16);
    return (unsigned)(mixed & (TABLE_SIZE - 1));
}

unsigned long lookup(unsigned long key)
{
    unsigned i = index_of(key);
    unsigned probes = 0;

    while (probes < TABLE_SIZE) {
        if (table[i].key == key)
            return table[i].hits;
        if (table[i].key == 0)
            return 0;                 /* never inserted */
        i = (i + 1) & (TABLE_SIZE - 1);
        probes++;
    }
    return 0;
}

/* Insert, or bump the count. A probe that travels the whole table is refused
 * rather than allowed to loop, and the refusal is counted: a table that drops a
 * record silently is the kind of bug that takes a release to notice. */
int insert(unsigned long key)
{
    unsigned i = index_of(key);
    unsigned probes = 0;

    if (key == 0)
        return -1;                    /* zero means empty here, not a key */
    while (probes < TABLE_SIZE) {
        if (table[i].key == 0) {
            table[i].key = key;
            table[i].hits = 1;
            inserted++;
            return 0;
        }
        if (table[i].key == key) {
            table[i].hits++;
            return 0;
        }
        collisions++;
        i = (i + 1) & (TABLE_SIZE - 1);
        probes++;
    }
    return -1;
}

void report(void)
{
    unsigned long total = 0;

    for (int i = 0; i < TABLE_SIZE; i++)
        total += table[i].hits;
    printf("entries %lu, keys %lu, collisions %d\n",
           inserted, total, collisions);
}

int main(void)
{
    unsigned long key = 1;

    for (int i = 0; i < 40; i++) {
        if (insert(key) != 0) {
            printf("table full at step %d\n", i);
            break;
        }
        key += STEP;
    }
    report();
    return 0;
}
