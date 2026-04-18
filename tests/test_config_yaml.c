#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config_yaml.h"
#include "cutils/config.h"

#define TEST_YAML "/tmp/cutils_test_yaml.yaml"

static int teardown(void **state)
{
    (void)state;
    unlink(TEST_YAML);
    return 0;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fputs(content, f);
    fclose(f);
}

/* --- Parse tests --- */

static void test_parse_basic(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: app.db\n"
        "  timeout: 5000\n"
        "ups:\n"
        "  device: /dev/ttyUSB0\n"
        "  baud: 9600\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_int_equal(doc->nentries, 4);

    assert_string_equal(yaml_get(doc, "db.path"), "app.db");
    assert_string_equal(yaml_get(doc, "db.timeout"), "5000");
    assert_string_equal(yaml_get(doc, "ups.device"), "/dev/ttyUSB0");
    assert_string_equal(yaml_get(doc, "ups.baud"), "9600");

    yaml_free(doc);
}

static void test_parse_comments_and_blanks(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "# Top comment\n"
        "\n"
        "db:\n"
        "  # DB path\n"
        "  path: test.db\n"
        "\n"
        "  timeout: 1000\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_int_equal(doc->nentries, 2);
    assert_string_equal(yaml_get(doc, "db.path"), "test.db");
    assert_string_equal(yaml_get(doc, "db.timeout"), "1000");

    /* Raw lines should include comments and blanks */
    assert_true(doc->nlines >= 7);

    yaml_free(doc);
}

static void test_parse_missing_file(void **state)
{
    (void)state;
    yaml_doc_t *doc = yaml_parse_file("/tmp/cutils_nonexistent_yaml_file");
    assert_non_null(doc);
    assert_int_equal(doc->nentries, 0);
    assert_int_equal(doc->nlines, 0);
    yaml_free(doc);
}

static void test_parse_empty_file(void **state)
{
    (void)state;
    write_file(TEST_YAML, "");
    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_int_equal(doc->nentries, 0);
    yaml_free(doc);
}

static void test_parse_value_with_spaces(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "app:\n"
        "  name: My Cool App\n"
        "  path: /usr/local/bin/my app\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "app.name"), "My Cool App");
    assert_string_equal(yaml_get(doc, "app.path"), "/usr/local/bin/my app");
    yaml_free(doc);
}

static void test_parse_empty_value(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path:\n"
        "  timeout: 5\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "db.path"), "");
    assert_string_equal(yaml_get(doc, "db.timeout"), "5");
    yaml_free(doc);
}

static void test_parse_tabs_indentation(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "\tpath: tabbed.db\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "db.path"), "tabbed.db");
    yaml_free(doc);
}

/* --- Lookup tests --- */

static void test_get_not_found(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: app.db\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_null(yaml_get(doc, "db.nope"));
    assert_null(yaml_get(doc, "nope.path"));
    assert_null(yaml_get(doc, "nodot"));
    yaml_free(doc);
}

/* --- Mutation tests --- */

static void test_set_existing_key(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: old.db\n"
        "  timeout: 1000\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);

    assert_int_equal(yaml_set(doc, "db.path", "new.db"), 0);
    assert_string_equal(yaml_get(doc, "db.path"), "new.db");
    /* timeout unchanged */
    assert_string_equal(yaml_get(doc, "db.timeout"), "1000");

    yaml_free(doc);
}

static void test_set_not_found(void **state)
{
    (void)state;
    write_file(TEST_YAML, "db:\n  path: app.db\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_int_equal(yaml_set(doc, "db.nope", "val"), -1);
    assert_int_equal(yaml_set(doc, "nodot", "val"), -1);
    yaml_free(doc);
}

static void test_set_preserves_comments(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "# Header comment\n"
        "db:\n"
        "  # DB path comment\n"
        "  path: old.db\n"
        "  timeout: 1000\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    yaml_set(doc, "db.path", "new.db");

    assert_int_equal(yaml_write_file(doc, TEST_YAML), 0);

    /* Re-parse and verify comments survived */
    yaml_doc_t *doc2 = yaml_parse_file(TEST_YAML);
    assert_non_null(doc2);
    assert_string_equal(yaml_get(doc2, "db.path"), "new.db");
    assert_string_equal(yaml_get(doc2, "db.timeout"), "1000");
    /* Comments should be in raw lines */
    assert_true(doc2->nlines >= 5);

    yaml_free(doc2);
    yaml_free(doc);
}

/* --- Write tests --- */

static void test_write_roundtrip(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "app:\n"
        "  name: test\n"
        "  port: 8080\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);

    /* Write to a different file, re-read */
    const char *out = "/tmp/cutils_test_yaml_out.yaml";
    assert_int_equal(yaml_write_file(doc, out), 0);

    yaml_doc_t *doc2 = yaml_parse_file(out);
    assert_non_null(doc2);
    assert_string_equal(yaml_get(doc2, "app.name"), "test");
    assert_string_equal(yaml_get(doc2, "app.port"), "8080");

    yaml_free(doc2);
    yaml_free(doc);
    unlink(out);
}

static void test_write_to_invalid_path(void **state)
{
    (void)state;
    write_file(TEST_YAML, "db:\n  path: x\n");
    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_int_equal(yaml_write_file(doc, "/nonexistent/dir/file.yaml"), -1);
    yaml_free(doc);
}

/* --- Template generation tests --- */

static void test_generate_template(void **state)
{
    (void)state;
    const config_key_t keys[] = {
        { "db.path", CFG_STRING, "app.db", "Database file path",
          CFG_STORE_FILE, 1 },
        { "db.timeout", CFG_INT, "5000", "Connection timeout in ms",
          CFG_STORE_FILE, 0 },
        { "app.name", CFG_STRING, "myapp", "Application name",
          CFG_STORE_FILE, 0 },
        { "app.debug", CFG_BOOL, "", "",
          CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    const config_section_t sections[] = {
        { "db", "Database" },
        { "app", "Application" },
        { NULL, NULL }
    };

    assert_int_equal(yaml_generate_template(TEST_YAML, keys, 4, sections, 2), 0);

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "db.path"), "app.db");
    assert_string_equal(yaml_get(doc, "db.timeout"), "5000");
    assert_string_equal(yaml_get(doc, "app.name"), "myapp");
    /* Empty default should parse as empty string */
    assert_string_equal(yaml_get(doc, "app.debug"), "");

    yaml_free(doc);
}

static void test_generate_template_no_sections(void **state)
{
    (void)state;
    const config_key_t keys[] = {
        { "db.path", CFG_STRING, "app.db", "DB path", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    assert_int_equal(yaml_generate_template(TEST_YAML, keys, 1, NULL, 0), 0);

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "db.path"), "app.db");
    yaml_free(doc);
}

static void test_generate_template_invalid_path(void **state)
{
    (void)state;
    const config_key_t keys[] = {
        { "db.path", CFG_STRING, "x", "", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };
    assert_int_equal(yaml_generate_template("/nonexistent/dir/t.yaml",
                                            keys, 1, NULL, 0), -1);
}

static void test_generate_template_key_without_dot(void **state)
{
    (void)state;
    const config_key_t keys[] = {
        { "nodot", CFG_STRING, "val", "desc", CFG_STORE_FILE, 0 },
        { "db.path", CFG_STRING, "x", "desc", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    assert_int_equal(yaml_generate_template(TEST_YAML, keys, 2, NULL, 0), 0);
    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    /* "nodot" should be skipped */
    assert_string_equal(yaml_get(doc, "db.path"), "x");
    yaml_free(doc);
}

/* --- yaml_free NULL safety --- */

static void test_free_null(void **state)
{
    (void)state;
    yaml_free(NULL); /* should not crash */
}

/* --- Multiple sections in set --- */

static void test_set_correct_section(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: db_path\n"
        "app:\n"
        "  path: app_path\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);

    /* Set app.path, db.path should remain */
    yaml_set(doc, "app.path", "new_app_path");
    assert_string_equal(yaml_get(doc, "db.path"), "db_path");
    assert_string_equal(yaml_get(doc, "app.path"), "new_app_path");

    yaml_free(doc);
}

/* --- Section header with value on same line --- */

static void test_section_with_inline_value(void **state)
{
    (void)state;
    /* "db: something" should NOT be treated as a section header */
    write_file(TEST_YAML,
        "db: inline_value\n"
        "real:\n"
        "  key: val\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    /* "db: inline_value" is not a section, so no entries for db.* */
    assert_null(yaml_get(doc, "db.inline_value"));
    assert_string_equal(yaml_get(doc, "real.key"), "val");
    yaml_free(doc);
}

/* --- Parse: multiple sections, blank between sections --- */

static void test_parse_multiple_sections_spacing(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: a.db\n"
        "\n"
        "\n"
        "app:\n"
        "  name: test\n"
        "\n"
        "log:\n"
        "  level: debug\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_int_equal(doc->nentries, 3);
    assert_string_equal(yaml_get(doc, "db.path"), "a.db");
    assert_string_equal(yaml_get(doc, "app.name"), "test");
    assert_string_equal(yaml_get(doc, "log.level"), "debug");
    yaml_free(doc);
}

/* --- Set: key in different section than first --- */

static void test_set_key_second_section(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: old.db\n"
        "app:\n"
        "  name: oldname\n"
        "  port: 8080\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);

    /* Set a key in the second section */
    assert_int_equal(yaml_set(doc, "app.port", "9090"), 0);
    assert_string_equal(yaml_get(doc, "app.port"), "9090");
    assert_string_equal(yaml_get(doc, "app.name"), "oldname");
    assert_string_equal(yaml_get(doc, "db.path"), "old.db");

    /* Write and verify roundtrip */
    assert_int_equal(yaml_write_file(doc, TEST_YAML), 0);
    yaml_doc_t *doc2 = yaml_parse_file(TEST_YAML);
    assert_string_equal(yaml_get(doc2, "app.port"), "9090");
    yaml_free(doc2);

    yaml_free(doc);
}

/* --- Template with multiple sections with display names --- */

static void test_generate_template_multi_section(void **state)
{
    (void)state;
    const config_key_t keys[] = {
        { "db.path", CFG_STRING, "app.db", "DB path", CFG_STORE_FILE, 0 },
        { "db.timeout", CFG_INT, "5000", "Timeout", CFG_STORE_FILE, 0 },
        { "app.name", CFG_STRING, "myapp", "App name", CFG_STORE_FILE, 0 },
        { "app.port", CFG_INT, "8080", "Port", CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    const config_section_t sections[] = {
        { "db", "Database" },
        { "app", "Application" },
        { NULL, NULL }
    };

    assert_int_equal(yaml_generate_template(TEST_YAML, keys, 4, sections, 2), 0);

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_int_equal(doc->nentries, 4);
    assert_string_equal(yaml_get(doc, "db.path"), "app.db");
    assert_string_equal(yaml_get(doc, "app.port"), "8080");
    yaml_free(doc);
}

/* --- Key with no description and no default --- */

static void test_generate_template_minimal_key(void **state)
{
    (void)state;
    const config_key_t keys[] = {
        { "db.path", CFG_STRING, NULL, NULL, CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    assert_int_equal(yaml_generate_template(TEST_YAML, keys, 1, NULL, 0), 0);

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    /* Key should exist with empty value */
    assert_string_equal(yaml_get(doc, "db.path"), "");
    yaml_free(doc);
}

/* --- Indented line before any section --- */

static void test_parse_indented_without_section(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "  orphan_key: value\n"
        "db:\n"
        "  path: test.db\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    /* orphan_key has no section — should be ignored */
    assert_string_equal(yaml_get(doc, "db.path"), "test.db");
    assert_int_equal(doc->nentries, 1);
    yaml_free(doc);
}

/* --- Indented line without colon --- */

static void test_parse_indented_no_colon(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: test.db\n"
        "  no_colon_line\n"
        "  timeout: 5\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "db.path"), "test.db");
    assert_string_equal(yaml_get(doc, "db.timeout"), "5");
    /* no_colon_line should be ignored */
    assert_int_equal(doc->nentries, 2);
    yaml_free(doc);
}

/* --- Non-section line at column 0 without colon --- */

static void test_parse_bare_word_no_colon(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: test.db\n"
        "bare_word_no_colon\n"
        "app:\n"
        "  name: test\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "db.path"), "test.db");
    assert_string_equal(yaml_get(doc, "app.name"), "test");
    yaml_free(doc);
}

/* --- Section with space after colon (still a section) --- */

static void test_parse_section_trailing_space(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:   \n"
        "  path: test.db\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "db.path"), "test.db");
    yaml_free(doc);
}

/* --- Set with comment and blank lines between sections --- */

static void test_set_with_comments_between_sections(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "# Section 1\n"
        "db:\n"
        "  path: old.db\n"
        "\n"
        "# Section 2\n"
        "app:\n"
        "  name: old\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);

    /* Set in second section — must skip blank/comment lines correctly */
    yaml_set(doc, "app.name", "new");
    assert_string_equal(yaml_get(doc, "app.name"), "new");
    assert_string_equal(yaml_get(doc, "db.path"), "old.db");

    yaml_free(doc);
}

/* --- Inline comment stripping --- */

static void test_parse_inline_comment(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: test.db  # database file\n"
        "  timeout: 5000 # milliseconds\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "db.path"), "test.db");
    assert_string_equal(yaml_get(doc, "db.timeout"), "5000");
    yaml_free(doc);
}

static void test_parse_hash_without_space_preserved(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "app:\n"
        "  url: http://example.com/page#anchor\n"
        "  tag: v1.0#beta\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    /* No space before #, so it's part of the value */
    assert_string_equal(yaml_get(doc, "app.url"), "http://example.com/page#anchor");
    assert_string_equal(yaml_get(doc, "app.tag"), "v1.0#beta");
    yaml_free(doc);
}

static void test_parse_value_is_only_comment(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db:\n"
        "  path: # this is just a comment\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    /* Everything after ": " is " # ...", which should strip to empty */
    assert_string_equal(yaml_get(doc, "db.path"), "");
    yaml_free(doc);
}

static void test_parse_section_with_inline_comment(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "db: # database config\n"
        "  path: test.db\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    /* Section header should be recognized despite inline comment */
    assert_string_equal(yaml_get(doc, "db.path"), "test.db");
    yaml_free(doc);
}

/* --- Quoted value support --- */

static void test_parse_quoted_value(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "app:\n"
        "  name: \"my app\"\n"
        "  desc: \"has # hash in it\"\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "app.name"), "my app");
    assert_string_equal(yaml_get(doc, "app.desc"), "has # hash in it");
    yaml_free(doc);
}

static void test_parse_quoted_empty(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "app:\n"
        "  empty: \"\"\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "app.empty"), "");
    yaml_free(doc);
}

static void test_parse_unmatched_quote(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "app:\n"
        "  broken: \"unterminated\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    /* Unmatched quote is kept as literal */
    assert_string_equal(yaml_get(doc, "app.broken"), "\"unterminated");
    yaml_free(doc);
}

static void test_parse_quoted_value_with_trailing_comment(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "app:\n"
        "  name: \"quoted\" # comment after\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    /* Quoted value should not have the inline comment stripped from inside,
       but the comment after the closing quote is in the raw line.
       Since the value starts with ", strip_inline_comment skips it,
       and strip_quotes removes the quotes. */
    assert_string_equal(yaml_get(doc, "app.name"), "quoted");
    yaml_free(doc);
}

/* --- Set with value that needs quoting --- */

static void test_set_value_with_hash(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "app:\n"
        "  desc: old\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);

    /* Set a value that contains " #" — should be quoted on write */
    yaml_set(doc, "app.desc", "has # hash");
    assert_string_equal(yaml_get(doc, "app.desc"), "has # hash");

    /* Write and re-parse — should roundtrip correctly */
    yaml_write_file(doc, TEST_YAML);
    yaml_free(doc);

    yaml_doc_t *doc2 = yaml_parse_file(TEST_YAML);
    assert_non_null(doc2);
    assert_string_equal(yaml_get(doc2, "app.desc"), "has # hash");
    yaml_free(doc2);
}

static void test_set_value_without_hash(void **state)
{
    (void)state;
    write_file(TEST_YAML,
        "app:\n"
        "  name: old\n");

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);

    /* Normal value — should NOT be quoted */
    yaml_set(doc, "app.name", "new value");
    yaml_write_file(doc, TEST_YAML);
    yaml_free(doc);

    yaml_doc_t *doc2 = yaml_parse_file(TEST_YAML);
    assert_non_null(doc2);
    assert_string_equal(yaml_get(doc2, "app.name"), "new value");
    yaml_free(doc2);
}

/* --- Template generation with hash in default --- */

static void test_generate_template_hash_in_default(void **state)
{
    (void)state;
    const config_key_t keys[] = {
        { "app.tag", CFG_STRING, "v1.0 # beta", "Version tag",
          CFG_STORE_FILE, 0 },
        { NULL, 0, NULL, NULL, 0, 0 }
    };

    assert_int_equal(yaml_generate_template(TEST_YAML, keys, 1, NULL, 0), 0);

    yaml_doc_t *doc = yaml_parse_file(TEST_YAML);
    assert_non_null(doc);
    assert_string_equal(yaml_get(doc, "app.tag"), "v1.0 # beta");
    yaml_free(doc);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_parse_basic, teardown),
        cmocka_unit_test_teardown(test_parse_comments_and_blanks, teardown),
        cmocka_unit_test_teardown(test_parse_missing_file, teardown),
        cmocka_unit_test_teardown(test_parse_empty_file, teardown),
        cmocka_unit_test_teardown(test_parse_value_with_spaces, teardown),
        cmocka_unit_test_teardown(test_parse_empty_value, teardown),
        cmocka_unit_test_teardown(test_parse_tabs_indentation, teardown),
        cmocka_unit_test_teardown(test_get_not_found, teardown),
        cmocka_unit_test_teardown(test_set_existing_key, teardown),
        cmocka_unit_test_teardown(test_set_not_found, teardown),
        cmocka_unit_test_teardown(test_set_preserves_comments, teardown),
        cmocka_unit_test_teardown(test_write_roundtrip, teardown),
        cmocka_unit_test_teardown(test_write_to_invalid_path, teardown),
        cmocka_unit_test_teardown(test_generate_template, teardown),
        cmocka_unit_test_teardown(test_generate_template_no_sections, teardown),
        cmocka_unit_test_teardown(test_generate_template_invalid_path, teardown),
        cmocka_unit_test_teardown(test_generate_template_key_without_dot, teardown),
        cmocka_unit_test_teardown(test_free_null, teardown),
        cmocka_unit_test_teardown(test_set_correct_section, teardown),
        cmocka_unit_test_teardown(test_section_with_inline_value, teardown),
        cmocka_unit_test_teardown(test_parse_multiple_sections_spacing, teardown),
        cmocka_unit_test_teardown(test_set_key_second_section, teardown),
        cmocka_unit_test_teardown(test_generate_template_multi_section, teardown),
        cmocka_unit_test_teardown(test_generate_template_minimal_key, teardown),
        cmocka_unit_test_teardown(test_parse_indented_without_section, teardown),
        cmocka_unit_test_teardown(test_parse_indented_no_colon, teardown),
        cmocka_unit_test_teardown(test_parse_bare_word_no_colon, teardown),
        cmocka_unit_test_teardown(test_parse_section_trailing_space, teardown),
        cmocka_unit_test_teardown(test_set_with_comments_between_sections, teardown),
        cmocka_unit_test_teardown(test_parse_inline_comment, teardown),
        cmocka_unit_test_teardown(test_parse_hash_without_space_preserved, teardown),
        cmocka_unit_test_teardown(test_parse_value_is_only_comment, teardown),
        cmocka_unit_test_teardown(test_parse_section_with_inline_comment, teardown),
        cmocka_unit_test_teardown(test_parse_quoted_value, teardown),
        cmocka_unit_test_teardown(test_parse_quoted_empty, teardown),
        cmocka_unit_test_teardown(test_parse_unmatched_quote, teardown),
        cmocka_unit_test_teardown(test_parse_quoted_value_with_trailing_comment, teardown),
        cmocka_unit_test_teardown(test_set_value_with_hash, teardown),
        cmocka_unit_test_teardown(test_set_value_without_hash, teardown),
        cmocka_unit_test_teardown(test_generate_template_hash_in_default, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
