#ifndef CUTILS_CONFIG_YAML_H
#define CUTILS_CONFIG_YAML_H

/* Internal YAML parser/writer for the config system.
 *
 * Format supported (c-utils controlled, no general YAML):
 *   # Comment
 *   section:
 *     key: value
 *     key: value with spaces
 *     key: value  # inline comment (stripped)
 *     key: "value with # hash"  (quoted to preserve #)
 *
 * Values are always strings. No multiline, no flow syntax, no anchors.
 * Inline comments (" # ...") are stripped unless the value is quoted.
 * Double quotes around a value are removed during parse.
 * Values containing " #" are auto-quoted on write for safe roundtrip.
 * Comments and blank lines are preserved during mutation. */

#include <stddef.h>

/* Parsed key-value entry */
typedef struct {
    char *section;  /* "ups" (without trailing colon) */
    char *key;      /* "device" */
    char *value;    /* "9600" */
} yaml_entry_t;

/* Parsed YAML document — maintains both structured data and raw lines */
typedef struct {
    /* Structured entries for lookup */
    yaml_entry_t *entries;
    int            nentries;
    int            capacity;

    /* Raw lines for comment-preserving rewrite */
    char **lines;
    int    nlines;
    int    lines_capacity;
} yaml_doc_t;

/* Parse a YAML file. Returns NULL on read failure.
 * Empty/missing file returns an empty document (not NULL). */
yaml_doc_t *yaml_parse_file(const char *path);

/* Free a parsed document. */
void yaml_free(yaml_doc_t *doc);

/* Look up a value by dot-notation key ("ups.device").
 * Returns NULL if not found. */
const char *yaml_get(const yaml_doc_t *doc, const char *dotkey);

/* Update a value in the document (both structured and raw lines).
 * Returns 0 on success, -1 if key not found. */
int yaml_set(yaml_doc_t *doc, const char *dotkey, const char *value);

/* Write the document back to a file, preserving comments and blank lines.
 * Returns 0 on success, -1 on write failure. */
int yaml_write_file(const yaml_doc_t *doc, const char *path);

/* Generate a template YAML file from key definitions and section headers.
 * Keys are grouped by section prefix. Each section gets a header comment.
 * Each key gets its description as a comment above it.
 * Returns 0 on success, -1 on write failure. */
int yaml_generate_template(const char *path,
                           const void *keys, int nkeys,
                           const void *sections, int nsections);

#endif
