#define _POSIX_C_SOURCE 200809L
#include "cmdhist.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void cmdhist_init(CmdHist *h) {
    h->entries = NULL;
    h->n = 0;
}

void cmdhist_free(CmdHist *h) {
    if (!h) return;
    for (int i = 0; i < h->n; i++) free(h->entries[i].cmd);
    free(h->entries);
    h->entries = NULL;
    h->n = 0;
}

static int cmdhist_cmp(const void *a, const void *b) {
    const CmdHistEntry *ea = a;
    const CmdHistEntry *eb = b;
    if (ea->count != eb->count)
        return (eb->count > ea->count) - (eb->count < ea->count);
    if (ea->mtime != eb->mtime)
        return (eb->mtime > ea->mtime) - (eb->mtime < ea->mtime);
    return strcmp(ea->cmd, eb->cmd);
}

void cmdhist_sort(CmdHist *h) {
    if (!h || h->n < 2) return;
    qsort(h->entries, (size_t)h->n, sizeof(h->entries[0]), cmdhist_cmp);
}

static void cmdhist_trim(CmdHist *h) {
    while (h->n > CMDHIST_MAX_ENTRIES) {
        h->n--;
        free(h->entries[h->n].cmd);
        h->entries[h->n].cmd = NULL;
    }
}

char *cmdhist_path(const char *basename) {
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (!home || !home[0] || !basename) return NULL;

    char *path;

    if (!state || !*state) {
        size_t len = strlen(home) + strlen("/.local/state/xenoed/") + strlen(basename) + 1;
        path = malloc(len);
        if (!path) return NULL;
        snprintf(path, len, "%s/.local/state/xenoed/%s", home, basename);
    } else {
        size_t len = strlen(state) + strlen("/xenoed/") + strlen(basename) + 1;
        path = malloc(len);
        if (!path) return NULL;
        snprintf(path, len, "%s/xenoed/%s", state, basename);
    }

    return path;
}

static int ensure_parent_dir(const char *filepath) {
    char *dir = strdup(filepath);
    if (!dir) return -1;
    char *slash = strrchr(dir, '/');
    if (!slash) {
        free(dir);
        return -1;
    }
    *slash = '\0';
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        free(dir);
        return -1;
    }
    free(dir);
    return 0;
}

int cmdhist_load(CmdHist *h, const char *path) {
    cmdhist_free(h);
    cmdhist_init(h);
    if (!path) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        *tab2 = '\0';

        char *end = NULL;
        unsigned long count = strtoul(line, &end, 10);
        if (end == line || *end != '\0') continue;
        long mtime = strtol(tab1 + 1, &end, 10);
        if (end == tab1 + 1 || *end != '\0') continue;
        const char *cmd = tab2 + 1;
        if (cmd[0] == '\0') continue;

        CmdHistEntry *fresh = realloc(h->entries, (size_t)(h->n + 1) * sizeof(*h->entries));
        if (!fresh) break;
        h->entries = fresh;
        h->entries[h->n].cmd = strdup(cmd);
        if (!h->entries[h->n].cmd) break;
        h->entries[h->n].count = (unsigned)count;
        h->entries[h->n].mtime = (time_t)mtime;
        h->n++;
    }
    fclose(f);
    cmdhist_sort(h);
    cmdhist_trim(h);
    return 0;
}

int cmdhist_save(const CmdHist *h, const char *path) {
    if (!h || !path) return -1;
    if (ensure_parent_dir(path) != 0) return -1;

    char tmp[4096];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    for (int i = 0; i < h->n; i++) {
        if (fprintf(f, "%u\t%ld\t%s\n",
                    h->entries[i].count,
                    (long)h->entries[i].mtime,
                    h->entries[i].cmd) < 0) {
            fclose(f);
            unlink(tmp);
            return -1;
        }
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

void cmdhist_bump(CmdHist *h, const char *cmd) {
    if (!h || !cmd || cmd[0] == '\0') return;

    time_t now = time(NULL);
    for (int i = 0; i < h->n; i++) {
        if (strcmp(h->entries[i].cmd, cmd) == 0) {
            h->entries[i].count++;
            h->entries[i].mtime = now;
            cmdhist_sort(h);
            return;
        }
    }

    CmdHistEntry *fresh = realloc(h->entries, (size_t)(h->n + 1) * sizeof(*h->entries));
    if (!fresh) return;
    h->entries = fresh;
    h->entries[h->n].cmd = strdup(cmd);
    if (!h->entries[h->n].cmd) return;
    h->entries[h->n].count = 1;
    h->entries[h->n].mtime = now;
    h->n++;
    cmdhist_sort(h);
    cmdhist_trim(h);
}
