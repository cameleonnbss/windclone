/* WindClone -- a specimen source file, shipped in the initrd.
 *
 * This is data, not a build input. Nothing here compiles it: the Makefile
 * builds src/kernel/*.c and src/user/*.c, and no rule or include path ever
 * looks inside rootfs/. The IDE's C analyzer does not know that and may
 * report stdio.h or printf as missing, which is correct for a program and
 * irrelevant for a file whose only job is to be *read*.
 *
 * It exists because /bin/view needs something to open on a machine that has
 * no disk, and a viewer proved against an empty file proves nothing. So this
 * specimen carries everything a syntax highlighter has to tell apart -- a
 * block comment, a line comment, a preprocessor line, a type, a keyword, a
 * number and a string -- and enough lines to overflow one twelve-row screen,
 * so scrolling is tested rather than assumed.
 *
 * Written as if it were going to run, because a specimen that is nonsense
 * would highlight differently from a real file and the test would pass for
 * the wrong reason. Pure ASCII for the same reason: the 5x7 font has no
 * accented glyphs, and a byte the viewer has to guess about is a byte a test
 * cannot assert on.
 */
#include <stdio.h>

#define ANSWER 42

static int squares[8];

/* Fill the table with 1*1, 2*2 ... and stop if the caller asks for more
 * than the array can hold. */
int fill(int count)
{
    int i;

    if (count > 8)
        count = 8;
    for (i = 0; i < count; i++)
        squares[i] = i * i;
    return count;
}

void print_table(int count)
{
    // One line per entry; printf is the only output this specimen needs.
    for (int i = 0; i < count; i++)
        printf("%d squared is %d\n", i, squares[i]);
}

int main(void)
{
    int n = fill(6);

    print_table(n);
    printf("the answer is %d\n", ANSWER);
    return 0;
}
