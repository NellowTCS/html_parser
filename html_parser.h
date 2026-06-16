#ifndef HTML_PARSER_H
#define HTML_PARSER_H

#include <stdbool.h>

/*
 * Attribute key/value pair delivered to the tag callback.
 *
 * name            - NUL-terminated, lower-cased attribute name.
 * value           - NUL-terminated, entity-decoded attribute value.
 * raw_value_begin - pointer into the original source text where the raw
 *                   attribute value (including surrounding quotes) starts.
 * raw_value_len   - byte length of the raw value region.
 */
struct html_attr {
    char       *name;
    char       *value;
    const char *raw_value_begin;
    int         raw_value_len;

    /* Internal: byte offsets into the string pool used by the scanner. */
    int _name_pool_off;
    int _value_pool_off;
};

/*
 * Tag descriptor passed to the user callback on every recognised tag.
 *
 * name            - NUL-terminated, lower-cased tag name.
 * is_end_tag      - true when this is a closing tag (</foo>).
 * nattrs          - number of entries in attrs[].
 * attrs           - array of attribute descriptors (may be NULL when nattrs=0).
 * start           - pointer to the '<' that opens this tag in the source.
 * end             - pointer one past the '>' that closes this tag.
 * contents_begin  - for an end-tag that was matched against an earlier
 *                   start-tag: pointer to the first character after '>' of
 *                   the start-tag (i.e. the start of the tag's inner text).
 *                   NULL if no matching start-tag was found.
 * contents_end    - pointer to the '<' that opened the matching end-tag,
 *                   i.e. one past the last character of the inner text.
 *                   NULL when contents_begin is NULL.
 */
struct html_tag {
    char             *name;
    bool              is_end_tag;
    int               nattrs;
    struct html_attr *attrs;
    const char       *start;
    const char       *end;
    const char       *contents_begin;
    const char       *contents_end;
};

struct hash_table;   /* caller-supplied; opaque to the scanner */

/*
 * Flags for html_scan_tags()
 */

/* Use strict SGML comment parsing (<!-- ends only at -->).
 * Default (flag absent): any <!-- ... --> terminates on the first -->. */
#define HTML_SCAN_STRICT_COMMENTS  (1 << 0)

/* Strip leading/trailing ASCII whitespace from quoted attribute values, and
 * collapse embedded newlines within those values. */
#define HTML_SCAN_TRIM_VALUES      (1 << 1)

/*
 * html_scan_tags()
 *
 * Scans the HTML document in [text, text+len) and calls cb(tag, user_data)
 * once for each recognised tag.
 *
 * allowed_tags       - if non-NULL, only tags whose (lower-cased) names are
 *                      keys in this hash table are reported to the callback.
 *                      All other tags are still parsed and skipped correctly.
 * allowed_attributes - if non-NULL, only attributes whose (lower-cased)
 *                      names are keys in this hash table are included in the
 *                      attr array delivered to the callback.
 *
 * The struct html_tag and its attr array are owned by the scanner; they are
 * only valid for the duration of the callback and must not be freed or stored
 * by the caller.
 */
void html_scan_tags(const char *text, int len,
                    void (*cb)(struct html_tag *, void *), void *user_data,
                    int flags,
                    const struct hash_table *allowed_tags,
                    const struct hash_table *allowed_attributes);

#endif /* HTML_PARSER_H */
