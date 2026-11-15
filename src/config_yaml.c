#include "config_yaml.h"
#include "cutils/config.h"
#include "cutils/mem.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- Helpers --- */

static char *strip_trailing_whitespace(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t'))
        s[--len] = '\0';
    return s;
}

static const char *skip_whitespace(const char *s)
{
    while (*s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

/* Strip inline comment and extract quoted values.
 * For unquoted values: strips " # ..." when # is preceded by whitespace.
 * For quoted values: extracts content between quotes, ignores trailing comment.
 * Modifies in place. */
static void strip_comment_and_unquote(char *s)
{
    /* Quoted value: extract content between quotes */
    if (s[0] == '"') {
        char *close = strchr(s + 1, '"');
        if (close) {
            size_t len = (size_t)(close - s - 1);
            memmove(s, s + 1, len);
            s[len] = '\0';
            return;
        }
        /* No closing quote — treat as literal (leave as-is) */
        return;
    }

    /* Unquoted value: strip " #" inline comment */
    char *p = s;
    while (*p) {
        if (*p == '#' && (p == s || *(p - 1) == ' ' || *(p - 1) == '\t')) {
            /* Trim the comment and trailing whitespace before it */
            if (p > s) p--;
            while (p > s && (*(p - 1) == ' ' || *(p - 1) == '\t')) p--;
            *p = '\0';
            return;
        }
        p++;
    }
}

/* Check if a value needs quoting for safe roundtrip (contains " #"). */
static int needs_quoting(const char *value)
{
    const char *p = value;
    while (*p) {
        if (*p == '#' && p > value && (*(p - 1) == ' ' || *(p - 1) == '\t'))
            return 1;
        p++;
    }
    return 0;
}

/* Returns 0 on success, -1 on NOMEM (either the realloc or any of the
 * three strdups). The previous design silently swallowed alloc
 * failures, leaving the doc with a missing entry that surfaced later
 * as a confusing "key not found" instead of the real OOM. NOMEM
 * branches are LCOV_EXCL'd — alloc failure on a few-hundred-byte
 * realloc / small strdup is not exercisable from test code without
 * fault injection. */
static int doc_add_entry(yaml_doc_t *doc, const char *section,
                         const char *key, const char *value, int line)
{
    if (doc->nentries >= doc->capacity) {
        int newcap = doc->capacity ? doc->capacity * 2 : 16;
        yaml_entry_t *tmp = realloc(doc->entries,
                                    (size_t)newcap * sizeof(yaml_entry_t));
        if (!tmp) return -1;  /* LCOV_EXCL_LINE */
        doc->entries = tmp;
        doc->capacity = newcap;
    }

    char *s = strdup(section);
    char *k = strdup(key);
    char *v = strdup(value);
    /* LCOV_EXCL_START */
    if (!s || !k || !v) {
        free(s); free(k); free(v);
        return -1;
    }
    /* LCOV_EXCL_STOP */

    yaml_entry_t *e = &doc->entries[doc->nentries++];
    e->section = s;
    e->key = k;
    e->value = v;
    e->line = line;
    return 0;
}

/* Record a line outside the supported subset. Returns 0, or -1 on NOMEM
 * (LCOV_EXCL'd like the other allocators here). */
__attribute__((format(printf, 3, 4)))
static int doc_add_issue(yaml_doc_t *doc, int line, const char *fmt, ...)
{
    if (doc->nissues >= doc->issues_capacity) {
        int newcap = doc->issues_capacity ? doc->issues_capacity * 2 : 8;
        yaml_issue_t *tmp = realloc(doc->issues,
                                    (size_t)newcap * sizeof(*tmp));
        if (!tmp) return -1;  /* LCOV_EXCL_LINE */
        doc->issues = tmp;
        doc->issues_capacity = newcap;
    }
    yaml_issue_t *is = &doc->issues[doc->nissues++];
    is->line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(is->msg, sizeof(is->msg), fmt, ap);
    va_end(ap);
    return 0;
}

/* --- Subset checks ---
 *
 * The parser below predates these and skips what it does not understand,
 * which is how `log: [unclosed` or a nested mapping used to load as if it
 * were not there. These functions only describe what is wrong; they never
 * change which entries the parser produces, so an application that has not
 * opted into validation sees the 1.3 result. */

/* A value as written, after the colon and its whitespace, before quotes
 * and inline comments are stripped. */
static int check_value(yaml_doc_t *doc, int line, const char *val)
{
    if (val[0] == '"') {
        const char *close = strchr(val + 1, '"');
        if (!close)
            return doc_add_issue(doc, line,
                "unterminated double-quoted value");
        const char *rest = skip_whitespace(close + 1);
        if (*rest && *rest != '#')
            return doc_add_issue(doc, line,
                "unexpected text after the closing quote: '%.40s' "
                "(escapes are not supported inside quotes)", rest);
        return 0;
    }
    if (val[0] == '\'')
        return doc_add_issue(doc, line,
            "single-quoted values are not supported; use double quotes");
    if (val[0] == '[' || val[0] == '{')
        return doc_add_issue(doc, line,
            "flow collections ('[...]', '{...}') are not supported");
    if (val[0] && strchr("&*!|>", val[0]))
        return doc_add_issue(doc, line,
            "anchors, aliases, tags and block scalars ('%c') are not "
            "supported; double-quote the value if it is literal", val[0]);

    /* Unquoted: a ": " before any inline comment is a nested mapping in
     * real YAML and an error in most parsers. Here it would be taken as
     * part of the value, so say so instead. */
    for (const char *p = val; *p; p++) {
        if (*p == '#' && (p == val || p[-1] == ' ' || p[-1] == '\t'))
            break;
        if (*p == ':' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\0'))
            return doc_add_issue(doc, line,
                "': ' inside an unquoted value; double-quote the value");
    }
    return 0;
}

/* A column-0 line that is not "name:" on its own. */
static int check_top_level(yaml_doc_t *doc, int line, const char *trimmed)
{
    const char *colon = strchr(trimmed, ':');
    if (!colon)
        return doc_add_issue(doc, line,
            "'%.40s' is not a section header ('name:') or a comment", trimmed);
    int namelen = (int)(colon - trimmed > 40 ? 40 : colon - trimmed);
    const char *after = skip_whitespace(colon + 1);
    if (*after == '[' || *after == '{')
        return doc_add_issue(doc, line,
            "flow collections ('[...]', '{...}') are not supported; put "
            "each key on its own indented line under '%.*s:'",
            namelen, trimmed);
    if (colon[1] != ' ' && colon[1] != '\t' && colon[1] != '\0')
        return doc_add_issue(doc, line,
            "'%.40s' is not a section header ('name:') or a comment", trimmed);
    return doc_add_issue(doc, line,
        "'%.*s: %.40s' at the top level is not supported; keys go indented "
        "under a section", namelen, trimmed, after);
}

static int issue_cmp(const void *a, const void *b)
{
    const yaml_issue_t *ia = a;
    const yaml_issue_t *ib = b;
    return (ia->line > ib->line) - (ia->line < ib->line);
}

/* Returns 0 on success, -1 on NOMEM. Same propagation rationale as
 * doc_add_entry above; same LCOV_EXCL for the alloc-failure branches. */
static int doc_add_line(yaml_doc_t *doc, const char *line)
{
    if (doc->nlines >= doc->lines_capacity) {
        int newcap = doc->lines_capacity ? doc->lines_capacity * 2 : 64;
        char **tmp = realloc(doc->lines, (size_t)newcap * sizeof(char *));
        if (!tmp) return -1;  /* LCOV_EXCL_LINE */
        doc->lines = tmp;
        doc->lines_capacity = newcap;
    }
    char *l = strdup(line);
    if (!l) return -1;  /* LCOV_EXCL_LINE */
    doc->lines[doc->nlines++] = l;
    return 0;
}

/* --- Entry lookup ---
 *
 * Entries are sorted by (section, key) at the end of yaml_parse_file,
 * letting yaml_get / yaml_set find a row via bsearch in O(log n).
 * The sort is stable across the doc's lifetime: yaml_set only updates
 * an existing entry's value (never changes section/key or appends),
 * and the doc is otherwise immutable post-parse. */

static int yaml_entry_cmp(const void *a, const void *b)
{
    const yaml_entry_t *ea = a;
    const yaml_entry_t *eb = b;
    int rc = strcmp(ea->section, eb->section);
    if (rc) return rc;
    return strcmp(ea->key, eb->key);
}

/* Find the entry index for a dot-notation key. Returns -1 if not
 * found or if dotkey is malformed (no dot, or section name overflows
 * the small stack buffer). */
static int yaml_find_entry(const yaml_doc_t *doc, const char *dotkey)
{
    if (!doc || doc->nentries == 0) return -1;
    const char *dot = strchr(dotkey, '.');
    if (!dot) return -1;
    size_t seclen = (size_t)(dot - dotkey);

    char secbuf[128];
    char keybuf[256];
    if (seclen >= sizeof(secbuf)) return -1;
    memcpy(secbuf, dotkey, seclen);
    secbuf[seclen] = '\0';

    size_t klen = strlen(dot + 1);
    if (klen >= sizeof(keybuf)) return -1;
    memcpy(keybuf, dot + 1, klen + 1);

    /* bsearch's search key is a yaml_entry_t with section / key
     * populated; the comparator only reads those fields. Stack copies
     * avoid casting away const on the caller's dotkey. */
    yaml_entry_t needle;
    needle.section = secbuf;
    needle.key     = keybuf;
    needle.value   = NULL;
    needle.line    = 0;

    const yaml_entry_t *hit = bsearch(
        &needle, doc->entries, (size_t)doc->nentries,
        sizeof(*doc->entries), yaml_entry_cmp);
    if (!hit) return -1;
    return (int)(hit - doc->entries);
}

/* --- Parser --- */

yaml_doc_t *yaml_parse_file(const char *path)
{
    yaml_doc_t *doc = calloc(1, sizeof(*doc));
    if (!doc) return NULL;

    CUTILS_AUTOCLOSE FILE *f = fopen(path, "r");
    if (!f) return doc; /* Empty document for missing file */

    char line[1024];
    char current_section[128] = "";
    int  lineno = 0;
    int  continuation = 0;   /* this chunk is the tail of an overlong line */
    int  sec_indent = -1;    /* indent of the current section's first key */
    int  rc = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t got = strlen(line);
        int whole = (got > 0 && line[got - 1] == '\n') || feof(f);
        if (!continuation) lineno++;
        if (!whole && !continuation)
            rc |= doc_add_issue(doc, lineno,
                "line is longer than %zu bytes", sizeof(line) - 1);
        int first_chunk = lineno == 1 && !continuation;
        continuation = !whole;

        /* A UTF-8 byte order mark is legal YAML and what several Windows
         * editors write. Left in, it glued itself to the first section
         * name and the line was reported as something it visibly was not.
         * Dropped here, it also does not survive a rewrite. */
        if (first_chunk && strncmp(line, "\xEF\xBB\xBF", 3) == 0)
            memmove(line, line + 3, strlen(line + 3) + 1);

        /* Store raw line (with newline stripped) */
        char raw[1024];
        strncpy(raw, line, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
        strip_trailing_whitespace(raw);
        /* LCOV_EXCL_START - NOMEM propagation; not exercisable from
         * test code without fault injection. */
        if (doc_add_line(doc, raw) != 0 || rc != 0) {
            yaml_free(doc);
            return NULL;
        }
        /* LCOV_EXCL_STOP */

        /* Trailing whitespace goes first, CR included, so a CRLF file
         * parses like an LF one. A blank CRLF line used to be "\r", not
         * blank, and was reported as "'' is not a section header" with
         * the CR invisible in the message. */
        strip_trailing_whitespace(line);

        /* Skip blank lines and comments */
        const char *trimmed = skip_whitespace(line);
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        /* Section header: "section:" at column 0, no leading whitespace */
        if (line[0] != ' ' && line[0] != '\t') {
            char secbuf[128] = "";
            size_t tlen = strlen(trimmed);
            if (tlen >= sizeof(secbuf)) tlen = sizeof(secbuf) - 1;
            memcpy(secbuf, trimmed, tlen);
            secbuf[tlen] = '\0';
            strip_comment_and_unquote(secbuf);
            strip_trailing_whitespace(secbuf);
            char *colon = strchr(secbuf, ':');
            if (colon && (*(colon + 1) == '\0' || *(colon + 1) == ' ')) {
                const char *after = skip_whitespace(colon + 1);
                if (*after == '\0') {
                    *colon = '\0';
                    snprintf(current_section, sizeof(current_section), "%s", secbuf);
                    sec_indent = -1;
                    continue;
                }
            }
            /* A document start marker is harmless; anything else at
             * column 0 is outside the subset. The section in force does
             * not change, as in 1.3. */
            if (strcmp(trimmed, "---") != 0)
                rc |= check_top_level(doc, lineno, trimmed);
            continue;
        }

        /* Key: value (indented under a section) */
        if (!current_section[0]) {
            rc |= doc_add_issue(doc, lineno,
                "indented line outside any section");
            continue;
        }

        /* YAML forbids tabs in indentation. This parser measured them
         * as one column each, so a tab-indented section loaded, and a
         * file mixing tabs and spaces reported a mismatch nobody could
         * see. Say what it is. */
        if (memchr(line, '\t', (size_t)(trimmed - line)))
            rc |= doc_add_issue(doc, lineno,
                "tab in the indentation; YAML indents with spaces only");

        int indent = (int)(trimmed - line);
        if (sec_indent < 0)
            sec_indent = indent;
        else if (indent > sec_indent)
            rc |= doc_add_issue(doc, lineno,
                "indented deeper than the other keys of section '%s'; "
                "nested mappings are not supported", current_section);
        else if (indent < sec_indent)
            rc |= doc_add_issue(doc, lineno,
                "indentation does not match the other keys of section '%s'",
                current_section);

        char kvbuf[1024];
        snprintf(kvbuf, sizeof(kvbuf), "%s", trimmed);
        char *colon = strchr(kvbuf, ':');
        if (!colon) {
            rc |= doc_add_issue(doc, lineno, "expected 'key: value'");
            continue;
        }
        if (colon[1] != '\0' && colon[1] != ' ' && colon[1] != '\t')
            rc |= doc_add_issue(doc, lineno,
                "missing space after ':' (write 'key: value')");
        *colon = '\0';
        char *key = kvbuf;
        /* Walk past whitespace on the mutable local - no const cast. */
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        strip_trailing_whitespace(key);
        if (key[0] == '\0')
            rc |= doc_add_issue(doc, lineno, "empty key");
        else if (strpbrk(key, " \t\"'"))
            rc |= doc_add_issue(doc, lineno, "invalid key name '%.60s'", key);
        rc |= check_value(doc, lineno, val);
        strip_comment_and_unquote(val);
        strip_trailing_whitespace(val);
        /* LCOV_EXCL_START - NOMEM propagation. */
        if (rc != 0 ||
            doc_add_entry(doc, current_section, key, val, lineno) != 0) {
            yaml_free(doc);
            return NULL;
        }
        /* LCOV_EXCL_STOP */
    }

    /* Sort entries by (section, key) so yaml_get / yaml_set can
     * bsearch in O(log n) instead of walking every entry. The raw
     * doc->lines array (used for comment-preserving rewrite in
     * yaml_write_file) is independent of entries and stays in source
     * order. yaml_set updates an existing entry's value in place but
     * never adds new entries, so the sort invariant is stable for
     * the lifetime of the doc. */
    qsort(doc->entries, (size_t)doc->nentries, sizeof(*doc->entries),
          yaml_entry_cmp);

    /* A key set twice: bsearch would return either one, so which value
     * won was unspecified. Sorted, the duplicates are neighbours. */
    for (int i = 1; i < doc->nentries; i++) {
        const yaml_entry_t *a = &doc->entries[i - 1];
        const yaml_entry_t *b = &doc->entries[i];
        if (yaml_entry_cmp(a, b) != 0) continue;
        int first = a->line < b->line ? a->line : b->line;
        int later = a->line < b->line ? b->line : a->line;
        /* LCOV_EXCL_START - NOMEM propagation. */
        if (doc_add_issue(doc, later, "duplicate key '%s.%s' (also on line %d)",
                          a->section, a->key, first) != 0) {
            yaml_free(doc);
            return NULL;
        }
        /* LCOV_EXCL_STOP */
    }
    qsort(doc->issues, (size_t)doc->nissues, sizeof(*doc->issues), issue_cmp);

    return doc;
}

void yaml_free(yaml_doc_t *doc)
{
    if (!doc) return;

    for (int i = 0; i < doc->nentries; i++) {
        free(doc->entries[i].section);
        free(doc->entries[i].key);
        free(doc->entries[i].value);
    }
    free(doc->entries);

    for (int i = 0; i < doc->nlines; i++)
        free(doc->lines[i]);
    free(doc->lines);
    free(doc->issues);

    free(doc);
}

/* --- Lookup --- */

const char *yaml_get(const yaml_doc_t *doc, const char *dotkey)
{
    int idx = yaml_find_entry(doc, dotkey);
    return idx >= 0 ? doc->entries[idx].value : NULL;
}

int yaml_get_line(const yaml_doc_t *doc, const char *dotkey)
{
    int idx = yaml_find_entry(doc, dotkey);
    return idx >= 0 ? doc->entries[idx].line : 0;
}

/* --- Mutation --- */

int yaml_set(yaml_doc_t *doc, const char *dotkey, const char *value)
{
    /* Update structured entry. The bsearch finds the entry; updating
     * value in place doesn't change the sort key, so the sorted-by-
     * (section, key) invariant established at parse time is
     * preserved. */
    int idx = yaml_find_entry(doc, dotkey);
    if (idx < 0) return -1;
    free(doc->entries[idx].value);
    doc->entries[idx].value = strdup(value);

    const char *dot = strchr(dotkey, '.');
    if (!dot) return -1;
    size_t seclen = (size_t)(dot - dotkey);
    const char *key = dot + 1;

    /* Update raw lines — find the line with this key under the right section */
    char cur_sec[128] = "";

    for (int i = 0; i < doc->nlines; i++) {
        const char *line = doc->lines[i];
        const char *trimmed = skip_whitespace(line);

        /* Skip blanks and comments */
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        /* Section header */
        if (line[0] != ' ' && line[0] != '\t') {
            char secbuf[128];
            snprintf(secbuf, sizeof(secbuf), "%s", trimmed);
            char *colon = strchr(secbuf, ':');
            if (colon) {
                const char *after = skip_whitespace(colon + 1);
                if (*after == '\0') {
                    *colon = '\0';
                    snprintf(cur_sec, sizeof(cur_sec), "%s", secbuf);
                }
            }
            continue;
        }

        /* Key line under matching section */
        if (strlen(cur_sec) == seclen &&
            strncmp(cur_sec, dotkey, seclen) == 0) {
            char linebuf[1024];
            strncpy(linebuf, trimmed, sizeof(linebuf) - 1);
            linebuf[sizeof(linebuf) - 1] = '\0';
            char *colon = strchr(linebuf, ':');
            if (colon) {
                *colon = '\0';
                char *linekey = linebuf;
                strip_trailing_whitespace(linekey);
                if (strcmp(linekey, key) == 0) {
                    /* Reconstruct the line preserving indentation */
                    size_t indent = (size_t)(trimmed - line);
                    char newline[1024];
                    if (needs_quoting(value))
                        snprintf(newline, sizeof(newline), "%.*s%s: \"%s\"",
                                 (int)indent, line, key, value);
                    else
                        snprintf(newline, sizeof(newline), "%.*s%s: %s",
                                 (int)indent, line, key, value);
                    free(doc->lines[i]);
                    doc->lines[i] = strdup(newline);
                    break;
                }
            }
        }
    }

    return 0;
}

/* --- Write --- */

int yaml_write_file(const yaml_doc_t *doc, const char *path)
{
    CUTILS_AUTOCLOSE FILE *f = fopen(path, "w");
    if (!f) return -1;

    for (int i = 0; i < doc->nlines; i++)
        fprintf(f, "%s\n", doc->lines[i]);

    return 0;
}

/* --- Template generation --- */

int yaml_generate_template(const char *path,
                           const void *keys_ptr, int nkeys,
                           const void *sections_ptr, int nsections)
{
    const config_key_t *keys = keys_ptr;
    const config_section_t *sections = sections_ptr;

    CUTILS_AUTOCLOSE FILE *f = fopen(path, "w");
    if (!f) return -1;

    /* Track which sections we've already written */
    char last_section[128] = "";

    for (int i = 0; i < nkeys; i++) {
        /* Extract section prefix from dot-notation key */
        const char *dot = strchr(keys[i].key, '.');
        if (!dot) continue;

        char section[128];
        size_t seclen = (size_t)(dot - keys[i].key);
        if (seclen >= sizeof(section)) seclen = sizeof(section) - 1;
        memcpy(section, keys[i].key, seclen);
        section[seclen] = '\0';

        /* New section? */
        if (strcmp(section, last_section) != 0) {
            if (last_section[0] != '\0')
                fprintf(f, "\n");

            /* Find display name */
            const char *display = NULL;
            for (int s = 0; s < nsections; s++) {
                if (strcmp(sections[s].prefix, section) == 0) {
                    display = sections[s].display_name;
                    break;
                }
            }

            if (display)
                fprintf(f, "# %s\n", display);

            fprintf(f, "%s:\n", section);
            snprintf(last_section, sizeof(last_section), "%s", section);
        }

        /* Key with description comment */
        const char *subkey = dot + 1;

        if (keys[i].description && keys[i].description[0])
            fprintf(f, "  # %s\n", keys[i].description);

        if (keys[i].default_value && keys[i].default_value[0]) {
            if (needs_quoting(keys[i].default_value))
                fprintf(f, "  %s: \"%s\"\n", subkey, keys[i].default_value);
            else
                fprintf(f, "  %s: %s\n", subkey, keys[i].default_value);
        } else {
            fprintf(f, "  %s:\n", subkey);
        }
    }

    /* Default-safe permissions: generated templates often hold blank
     * credential slots (pushover.token, etc.) that the user will fill
     * in. Make the file 0600 from the start so it doesn't sit at the
     * umask default (often 0644 = world-readable) between template
     * generation and the user's first chmod. Caller may chmod after
     * the fact if a more relaxed mode is intended. */
    chmod(path, 0600);
    return 0;
}
