#ifndef XENOED_CMDLINE_H
#define XENOED_CMDLINE_H

/* Tokenize a command line into an argv array, the way a shell would split a
 * command: words are separated by runs of spaces/tabs, and single quotes and
 * double quotes (with backslash escapes inside) keep characters together.
 * No shell variables, globbing, or other expansion happens -- the token is
 * inserted into the argv exactly as quoted. This is what lets
 * XENOED_COMMANDS entries be one-liners like "perl -pe '$_ = lc'" instead
 * of requiring a separate script file.
 *
 * Returns the number of words (>= 0), or -1 on a malformed command line
 * (an unterminated quote). On success *out_argv is a NULL-terminated array
 * of malloc'd words; the caller must release it with xenoed_free_argv().
 * On failure *out_argv is NULL and nothing needs to be freed.
 */
int xenoed_split_cmdline(const char *cmdline, char ***out_argv);

/* Free an argv array returned by xenoed_split_cmdline(). */
void xenoed_free_argv(char **argv);

#endif