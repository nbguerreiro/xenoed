#define _POSIX_C_SOURCE 200809L
#include "../src/cmdline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } } while (0)

static void check_argv(const char *expected[], int expected_n, char **argv, int argc) {
    if (argc != expected_n) {
        printf("  argc mismatch: got %d, expected %d\n", argc, expected_n);
        failures++;
        return;
    }
    for (int i = 0; i < expected_n; i++) {
        if (strcmp(argv[i], expected[i]) != 0) {
            printf("  argv[%d] mismatch: got \"%s\", expected \"%s\"\n", i, argv[i], expected[i]);
            failures++;
        }
    }
}

#define CHECK_SPLIT(cmd, ...) do {                                            \
    const char *expect[] = { __VA_ARGS__ };                                    \
    char **av;                                                                 \
    int ac = xenoed_split_cmdline(cmd, &av);                                    \
    const int en = (int)(sizeof(expect) / sizeof(expect[0]));                   \
    printf("Test: \"%s\"\n", cmd);                                              \
    for (int i = 0; i < ac; i++) printf("  argv[%d] = \"%s\"\n", i, av[i]);     \
    check_argv(expect, en, av, ac);                                             \
    CHECK(ac == en);                                                            \
    xenoed_free_argv(av);                                                       \
} while (0)

int main(void) {
    /* plain words and extra whitespace */
    CHECK_SPLIT("perl -pe x", "perl", "-pe", "x");
    CHECK_SPLIT("  hello   world  ", "hello", "world");
    CHECK_SPLIT("indent.sh", "indent.sh");
    {
        char **av = (char **)0x1;
        int ac = xenoed_split_cmdline("", &av);
        printf("Test: \"\"\n");
        CHECK(ac == 0);
        CHECK(av == NULL);
    }

    /* the TODO #17 one-liners themselves */
    CHECK_SPLIT("perl -pe '$_ = lc'", "perl", "-pe", "$_ = lc");
    CHECK_SPLIT("column -t -s '|' -o '|'", "column", "-t", "-s", "|", "-o", "|");

    /* single quotes: everything literal, and adjacent tokens concatenate */
    CHECK_SPLIT("x''y", "xy");
    CHECK_SPLIT("echo 'a b'", "echo", "a b");

    /* double quotes: "" groups, \" escapes a double quote, \\ escapes a backslash */
    CHECK_SPLIT("echo \"a b\"", "echo", "a b");
    CHECK_SPLIT("echo \"a\\\"b\"", "echo", "a\"b");
    CHECK_SPLIT("echo \"a\\\\b\"", "echo", "a\\b");

    /* backslash outside quotes escapes the next char */
    CHECK_SPLIT("echo a\\ b", "echo", "a b");
    CHECK_SPLIT("echo a\\\"b", "echo", "a\"b");

    /* a quote inside a word splits nothing: 'a b'c stays one token */
    CHECK_SPLIT("a'b c'd", "ab cd");

    /* malformed: unterminated quote */
    {
        char **av = (char **)0x1;
        int ac = xenoed_split_cmdline("perl -pe 'foo", &av);
        printf("Test: unterminated single quote -> %d (av=%p)\n", ac, (void *)av);
        CHECK(ac == -1);
        CHECK(av == NULL);
    }
    {
        char **av = (char **)0x1;
        int ac = xenoed_split_cmdline("echo \"foo", &av);
        printf("Test: unterminated double quote -> %d (av=%p)\n", ac, (void *)av);
        CHECK(ac == -1);
        CHECK(av == NULL);
    }

    printf("\n%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}