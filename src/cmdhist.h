#ifndef XENOED_CMDHIST_H
#define XENOED_CMDHIST_H

#include <time.h>

/* Soft cap on remembered commands per file. Oldest/least-used entries
 * fall off after cmdhist_bump() resorts. */
#define CMDHIST_MAX_ENTRIES 200

typedef struct {
    char *cmd;
    unsigned count;
    time_t mtime;
} CmdHistEntry;

typedef struct {
    CmdHistEntry *entries;
    int n;
} CmdHist;

void cmdhist_init(CmdHist *h);
void cmdhist_free(CmdHist *h);

/* Load from `path`. Missing/unreadable file => empty hist (returns 0).
 * Malformed lines are skipped. Always sorts by frequency afterward. */
int cmdhist_load(CmdHist *h, const char *path);

/* Ensure ~/.xenoed exists and write the hist. Returns 0 on success. */
int cmdhist_save(const CmdHist *h, const char *path);

/* Insert or update `cmd`: bump count, refresh mtime, re-sort, trim. */
void cmdhist_bump(CmdHist *h, const char *cmd);

/* Sort in place: count desc, mtime desc, cmd asc. */
void cmdhist_sort(CmdHist *h);

/* Malloc "$HOME/.xenoed/<basename>". NULL if HOME is unset. Caller frees. */
char *cmdhist_path(const char *basename);

#endif
