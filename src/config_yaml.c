#include "config_yaml.h"
#include "cutils/config.h"
#include "cutils/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void doc_add_entry(yaml_doc_t *doc, const char *section,
                          const char *key, const char *value)
{
    if (doc->nentries >= doc->capacity) {
        int newcap = doc->capacity ? doc->capacity * 2 : 16;
        yaml_entry_t *tmp = realloc(doc->entries,
                                    (size_t)newcap * sizeof(yaml_entry_t));
        if (!tmp) return;
        doc->entries = tmp;
        doc->capacity = newcap;
    }

    yaml_entry_t *e = &doc->entries[doc->nentries++];
    e->section = strdup(section);
    e->key = strdup(key);
    e->value = strdup(value);
}

static void doc_add_line(yaml_doc_t *doc, const char *line)
{
    if (doc->nlines >= doc->lines_capacity) {
        int newcap = doc->lines_capacity ? doc->lines_capacity * 2 : 64;
        char **tmp = realloc(doc->lines, (size_t)newcap * sizeof(char *));
        if (!tmp) return;
        doc->lines = tmp;
        doc->lines_capacity = newcap;
    }
    doc->lines[doc->nlines++] = strdup(line);
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

    while (fgets(line, sizeof(line), f)) {
        /* Store raw line (with newline stripped) */
        char raw[1024];
        strncpy(raw, line, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
        strip_trailing_whitespace(raw);
        doc_add_line(doc, raw);

        /* Skip blank lines and comments */
        const char *trimmed = skip_whitespace(line);
        if (*trimmed == '\0' || *trimmed == '\n' || *trimmed == '#')
            continue;

        strip_trailing_whitespace(line);

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
                    continue;
                }
            }
        }

        /* Key: value (indented under a section) */
        if ((line[0] == ' ' || line[0] == '\t') && current_section[0]) {
            char kvbuf[1024];
            snprintf(kvbuf, sizeof(kvbuf), "%s", trimmed);
            char *colon = strchr(kvbuf, ':');
            if (colon) {
                *colon = '\0';
                char *key = kvbuf;
                /* Walk past whitespace on the mutable local — no const cast. */
                char *val = colon + 1;
                while (*val == ' ' || *val == '\t') val++;
                strip_trailing_whitespace(key);
                strip_comment_and_unquote(val);
                strip_trailing_whitespace(val);
                doc_add_entry(doc, current_section, key, val);
            }
        }
    }

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

    free(doc);
}

/* --- Lookup --- */

const char *yaml_get(const yaml_doc_t *doc, const char *dotkey)
{
    /* Split "section.key" */
    const char *dot = strchr(dotkey, '.');
    if (!dot) return NULL;

    size_t seclen = (size_t)(dot - dotkey);
    const char *key = dot + 1;

    for (int i = 0; i < doc->nentries; i++) {
        if (strlen(doc->entries[i].section) == seclen &&
            strncmp(doc->entries[i].section, dotkey, seclen) == 0 &&
            strcmp(doc->entries[i].key, key) == 0) {
            return doc->entries[i].value;
        }
    }

    return NULL;
}

/* --- Mutation --- */

int yaml_set(yaml_doc_t *doc, const char *dotkey, const char *value)
{
    /* Update structured entry */
    const char *dot = strchr(dotkey, '.');
    if (!dot) return -1;

    size_t seclen = (size_t)(dot - dotkey);
    const char *key = dot + 1;
    int found = 0;

    for (int i = 0; i < doc->nentries; i++) {
        if (strlen(doc->entries[i].section) == seclen &&
            strncmp(doc->entries[i].section, dotkey, seclen) == 0 &&
            strcmp(doc->entries[i].key, key) == 0) {
            free(doc->entries[i].value);
            doc->entries[i].value = strdup(value);
            found = 1;
            break;
        }
    }

    if (!found) return -1;

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

    return 0;
}
