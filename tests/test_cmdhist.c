#define _POSIX_C_SOURCE 200809L
#include "../src/cmdhist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } } while (0)

int main(void) {
    char template[] = "/tmp/xenoed_cmdhist_XXXXXX";
    int fd = mkstemp(template);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
    unlink(template); /* cmdhist_save creates the file */

    /* Test 1: bump + sort by frequency */
    {
        CmdHist h;
        cmdhist_init(&h);
        cmdhist_bump(&h, "sort");
        cmdhist_bump(&h, "tr a-z A-Z");
        cmdhist_bump(&h, "sort");
        cmdhist_bump(&h, "sort");
        cmdhist_bump(&h, "uniq");
        printf("Test 1 (bump sorts by count): n=%d top=\"%s\" count=%u\n",
               h.n, h.entries[0].cmd, h.entries[0].count);
        CHECK(h.n == 3);
        CHECK(strcmp(h.entries[0].cmd, "sort") == 0);
        CHECK(h.entries[0].count == 3);
        CHECK(h.entries[1].count == 1);
        CHECK(h.entries[2].count == 1);
        cmdhist_free(&h);
    }

    /* Test 2: save + load round-trip */
    {
        CmdHist h;
        cmdhist_init(&h);
        cmdhist_bump(&h, "w");
        cmdhist_bump(&h, "w notes.txt");
        cmdhist_bump(&h, "w");
        CHECK(cmdhist_save(&h, template) == 0);

        CmdHist loaded;
        cmdhist_init(&loaded);
        CHECK(cmdhist_load(&loaded, template) == 0);
        printf("Test 2 (save/load): n=%d top=\"%s\" count=%u\n",
               loaded.n, loaded.entries[0].cmd, loaded.entries[0].count);
        CHECK(loaded.n == 2);
        CHECK(strcmp(loaded.entries[0].cmd, "w") == 0);
        CHECK(loaded.entries[0].count == 2);
        CHECK(strcmp(loaded.entries[1].cmd, "w notes.txt") == 0);
        CHECK(loaded.entries[1].count == 1);
        cmdhist_free(&h);
        cmdhist_free(&loaded);
    }

    /* Test 3: missing file loads as empty */
    {
        CmdHist h;
        cmdhist_init(&h);
        CHECK(cmdhist_load(&h, "/tmp/xenoed_cmdhist_does_not_exist_xyz") == 0);
        CHECK(h.n == 0);
        printf("Test 3 (missing file => empty): ok\n");
        cmdhist_free(&h);
    }

    /* Test 4: empty / NULL bump is ignored */
    {
        CmdHist h;
        cmdhist_init(&h);
        cmdhist_bump(&h, "");
        cmdhist_bump(&h, NULL);
        CHECK(h.n == 0);
        printf("Test 4 (empty bump ignored): ok\n");
        cmdhist_free(&h);
    }

    unlink(template);

    if (failures == 0) {
        printf("\nAll cmdhist tests passed.\n");
        return 0;
    }
    printf("\n%d check(s) FAILED.\n", failures);
    return 1;
}
