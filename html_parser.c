#include "html_parser.h"

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Portability shims
 */

#ifndef HTML_PARSER_STANDALONE
/*
 * In real builds these come from Gnulib / wget's utils.h.  Define them here
 * only when building standalone so the file is self-contained.
 */
#  include "utils.h"   /* xmalloc, xrealloc, xfree, c_is* */
#  include "hash.h"
#else
#  define xmalloc  malloc
#  define xrealloc realloc
#  define xfree    free

#  define c_isspace(c)  isspace((unsigned char)(c))
#  define c_isalnum(c)  isalnum((unsigned char)(c))
#  define c_isxdigit(c) isxdigit((unsigned char)(c))
#  define c_isdigit(c)  isdigit((unsigned char)(c))
#  define c_tolower(c)  tolower((unsigned char)(c))

struct hash_table { int _unused; };
static void *hash_table_get(const struct hash_table *ht, void *key)
{
    (void)ht;
    return key;   /* standalone: every name is "allowed" */
}
#endif /* HTML_PARSER_STANDALONE */

/*
 * Resizable byte pool
 *
 * The pool is initially backed by a small stack buffer.  If appended data
 * would overflow it, a heap buffer is allocated and the stack data is copied
 * across.  Subsequent growths use realloc.
 *
 * All tag-name and attribute name/value strings produced during parsing live
 * in the pool, separated by NUL bytes, which avoids per-string allocations.
 */

typedef struct {
    char *buf;          /* pointer to the current buffer               */
    int   cap;          /* total capacity                               */
    int   len;          /* bytes written so far (NUL terminators incl.) */
    bool  heap;         /* true once we have switched to heap storage   */
    char *orig_buf;     /* original (stack) buffer, for POOL_RESET      */
    int   orig_cap;
} Pool;

/* Initialise POOL to use the caller-provided stack buffer. */
#define POOL_INIT(pool, stack_buf, stack_cap) do {  \
    Pool *_p = (pool);                              \
    _p->buf      = (stack_buf);                     \
    _p->cap      = (stack_cap);                     \
    _p->len      = 0;                               \
    _p->heap     = false;                           \
    _p->orig_buf = (stack_buf);                     \
    _p->orig_cap = (stack_cap);                     \
} while (0)

/* Ensure POOL has room for at least EXTRA more bytes. */
static void pool_reserve(Pool *pool, int extra)
{
    if (pool->len + extra <= pool->cap)
        return;

    int new_cap = pool->cap;
    while (new_cap < pool->len + extra)
        new_cap <<= 1;

    if (pool->heap) {
        pool->buf = xrealloc(pool->buf, new_cap);
    } else {
        char *new_buf = xmalloc(new_cap);
        memcpy(new_buf, pool->buf, pool->len);
        pool->buf  = new_buf;
        pool->heap = true;
    }
    pool->cap = new_cap;
}

/* Append [beg, end) to the pool without NUL-terminating. */
static void pool_append(Pool *pool, const char *beg, const char *end)
{
    int n = (int)(end - beg);
    pool_reserve(pool, n);
    memcpy(pool->buf + pool->len, beg, n);
    pool->len += n;
}

/* Append a single character to the pool. */
static void pool_append_char(Pool *pool, char c)
{
    pool_reserve(pool, 1);
    pool->buf[pool->len++] = c;
}

/* Reset the pool's write position without releasing memory. */
#define POOL_REWIND(pool)  ((pool)->len = 0)

/* Release heap memory (if any) and restore the pool to its initial state. */
#define POOL_FREE(pool) do {                    \
    Pool *_p = (pool);                          \
    if (_p->heap) xfree(_p->buf);              \
    _p->buf  = _p->orig_buf;                   \
    _p->cap  = _p->orig_cap;                   \
    _p->len  = 0;                               \
    _p->heap = false;                           \
} while (0)

/*
 * Growable array helper
 *
 * Like the pool, this starts stack-allocated and switches to the heap on
 * first overflow, then uses realloc for subsequent growths.
 */

/* Ensure BASEVAR (of element type TYPE, current capacity CAPVAR, flag
 * HEAP_FLAG) can hold at least NEEDED elements. */
#define GROW_ARRAY(basevar, capvar, needed, heap_flag, type) do {       \
    long _need = (needed);                                              \
    long _cap  = (capvar);                                              \
    while (_cap < _need) _cap <<= 1;                                   \
    if (_cap != (capvar)) {                                             \
        if (heap_flag)                                                  \
            (basevar) = xrealloc((basevar), _cap * sizeof(type));      \
        else {                                                          \
            void *_nb = xmalloc(_cap * sizeof(type));                  \
            memcpy(_nb, (basevar), (capvar) * sizeof(type));           \
            (basevar) = _nb;                                            \
            (heap_flag) = true;                                         \
        }                                                               \
        (capvar) = (int)_cap;                                           \
    }                                                                   \
} while (0)

/*
 * Character classification helpers
 */

/*
 * Any character > 0x20, < 0x7f, not '=', '<', '>', '/' is a valid tag/
 * attribute name character.  This is deliberately more permissive than
 * strict HTML so we can survive real-world markup.
 */
#define NAME_CH(c) ((unsigned char)(c) > 32 && (unsigned char)(c) < 127 \
                    && (c) != '=' && (c) != '<' && (c) != '>' && (c) != '/')

/*
 * HTML entity decoder
 *
 * Called when '&' is encountered inside an attribute value.  *pp points to
 * the '&'; on success, *pp is advanced past the entity and the decoded ASCII
 * character is returned.  Returns -1 (leaving *pp unchanged) on failure.
 *
 * Supported: &lt; &gt; &amp; &apos; &quot;  &#DDD;  &#xHH;
 * Only ASCII values 1-127 are decoded; higher code points are left as-is.
 */

/* Does a potential entity name of length N fit before END, terminated by a
 * non-alnum character or end-of-buffer?  (We refuse IE-style unterminated
 * entities like "&ltfoo" → "<foo".)  */
#define ENTITY_FITS(p, n, end) \
    ((p)+(n) == (end) || ((p)+(n) < (end) && !c_isalnum((p)[n])))

/* Advance P by INC characters, then skip one optional ';'. */
#define SKIP_SEMI(p, inc, end) ((p) += (inc), (p) < (end) && *(p) == ';' ? ++(p) : (p))

static int decode_entity(const char **pp, const char *end)
{
    const char *p = *pp + 1;   /* skip the '&' */
    if (p == end) return -1;

    char lead = *p++;
    int  val  = -1;

    switch (lead) {
    case '#':
        /* Numeric entity: &#DDD; or &#xHH; */
        {
            int digits = 0;
            val = 0;
            if (p < end && *p == 'x') {
                /* Hex */
                for (++p; val < 256 && p < end && c_isxdigit(*p); ++p, ++digits) {
                    int h = c_isdigit(*p) ? (*p - '0')
                                          : (c_tolower(*p) - 'a' + 10);
                    val = (val << 4) | h;
                }
            } else {
                /* Decimal */
                for (; val < 256 && p < end && c_isdigit(*p); ++p, ++digits)
                    val = val * 10 + (*p - '0');
            }
            if (!digits || !val || (val & ~0x7f)) return -1;
            *pp = SKIP_SEMI(p, 0, end);
            return val;
        }

    case 'l':
        if (ENTITY_FITS(p, 1, end) && p[0] == 't')
            { val = '<';  *pp = SKIP_SEMI(p, 1, end); }
        break;
    case 'g':
        if (ENTITY_FITS(p, 1, end) && p[0] == 't')
            { val = '>';  *pp = SKIP_SEMI(p, 1, end); }
        break;
    case 'a':
        if (ENTITY_FITS(p, 2, end) && p[0] == 'm' && p[1] == 'p')
            { val = '&';  *pp = SKIP_SEMI(p, 2, end); }
        else if (ENTITY_FITS(p, 3, end) && p[0]=='p' && p[1]=='o' && p[2]=='s')
            { val = '\''; *pp = SKIP_SEMI(p, 3, end); }
        break;
    case 'q':
        if (ENTITY_FITS(p, 3, end) && p[0]=='u' && p[1]=='o' && p[2]=='t')
            { val = '"';  *pp = SKIP_SEMI(p, 3, end); }
        break;
    }
    return val;
}

#undef ENTITY_FITS
#undef SKIP_SEMI

/*
 * String copy with optional transforms
 *
 * Copies [beg, end) into POOL, then NUL-terminates.  One or more of the
 * following transforms may be applied (bit-OR of the flags below):
 *   COPY_DOWNCASE         - lower-case every letter.
 *   COPY_DECODE_ENTITIES  - decode HTML character entities (ASCII range).
 *   COPY_TRIM             - strip leading/trailing whitespace; collapse
 *                           embedded CR/LF characters.
 */

enum {
    COPY_DOWNCASE        = 1,
    COPY_DECODE_ENTITIES = 2,
    COPY_TRIM            = 4,
};

static void copy_to_pool(Pool *pool, const char *beg, const char *end, int flags)
{
    int start_pos = pool->len;

    /* Trim whitespace from the edges before we touch anything else, so that
     * &amp;-encoded spaces are still reachable via &#32;. */
    if (flags & COPY_TRIM) {
        while (beg < end && c_isspace(*beg)) ++beg;
        while (end > beg && c_isspace(end[-1])) --end;
    }

    if (flags & COPY_DECODE_ENTITIES) {
        /* Pre-reserve the maximum possible space (entity decoding can only
         * shrink the string, never grow it). */
        pool_reserve(pool, (int)(end - beg) + 1);

        const char *src = beg;
        char       *dst = pool->buf + pool->len;
        bool squash_newlines = !!(flags & COPY_TRIM);

        while (src < end) {
            if (*src == '&') {
                int ch = decode_entity(&src, end);
                if (ch != -1)
                    *dst++ = (char)ch;
                else
                    *dst++ = *src++;
            } else if (squash_newlines && (*src == '\n' || *src == '\r')) {
                ++src;
            } else {
                *dst++ = *src++;
            }
        }

        /* Sanity: we must not have written more bytes than we reserved. */
        assert(dst - (pool->buf + pool->len) <= end - beg);
        pool->len = (int)(dst - pool->buf);
        pool_append_char(pool, '\0');
    } else {
        pool_append(pool, beg, end);
        pool_append_char(pool, '\0');
    }

    if (flags & COPY_DOWNCASE) {
        for (char *p = pool->buf + start_pos; *p; ++p)
            *p = c_tolower(*p);
    }
}

/*
 * Tag-content tracking stack
 *
 * To fill in html_tag.contents_begin/end we keep a doubly-linked list of
 * open tags.  Each node stores the source-text pointers to the tag name
 * and the position right after the tag's closing '>'.  When a matching
 * end-tag is found, we walk the list backwards to find the entry.
 */

typedef struct TagStackNode {
    const char         *name_begin;
    const char         *name_end;
    const char         *contents_begin;   /* NULL until '>' of start-tag seen */
    struct TagStackNode *prev;
    struct TagStackNode *next;
} TagStackNode;

/* Push a new node onto the tail of the list.  Returns the new node. */
static TagStackNode *ts_push(TagStackNode **head, TagStackNode **tail)
{
    TagStackNode *n = xmalloc(sizeof *n);
    n->name_begin = n->name_end = n->contents_begin = NULL;
    n->next = NULL;
    n->prev = *tail;

    if (*tail)
        (*tail)->next = n;
    else
        *head = n;
    *tail = n;
    return n;
}

/* Pop TS and every node after it (i.e. nested unclosed tags).
 * Passing head itself clears the whole list. */
static void ts_pop(TagStackNode **head, TagStackNode **tail, TagStackNode *ts)
{
    if (!*head || !ts) return;

    /* Detach ts from the node before it. */
    if (ts->prev)
        ts->prev->next = NULL;
    else
        *head = NULL;

    *tail = ts->prev;

    /* Free ts and everything after it. */
    while (ts) {
        TagStackNode *next = ts->next;
        xfree(ts);
        ts = next;
    }
}

/* Search backwards from TAIL for the most recent open tag whose name matches
 * [name_begin, name_end) (case-insensitive). */
static TagStackNode *ts_find(TagStackNode *tail,
                             const char *name_begin, const char *name_end)
{
    int len = (int)(name_end - name_begin);
    for (TagStackNode *n = tail; n; n = n->prev) {
        if ((int)(n->name_end - n->name_begin) == len
            && strncasecmp(n->name_begin, name_begin, len) == 0)
            return n;
    }
    return NULL;
}

/*
 * SGML declaration / strict-comment skipper
 *
 * Advances past an SGML declaration that starts just after '<' (p[0]
 * should be '!').  Returns the new position (past '>') on success, or
 * beg+1 if the declaration could not be parsed (causing the caller to
 * treat '<' as a literal character and resume scanning).
 *
 * The state machine handles:
 *   - Simple declarations:   <!DOCTYPE ...>
 *   - Declarations w/ comment: <!DECL -- comment -- ...>
 *   - Plain HTML comments:   <!-- comment -->
 *   - Multiple comments:     <!-- a -- -- b -->
 *   - Quoted strings inside: <!DECL "foo" ...>
 */

static const char *skip_declaration(const char *beg, const char *end)
{
    typedef enum {
        SD_BANG,       /* expecting '!'                              */
        SD_BODY,       /* inside declaration body                    */
        SD_NAME,       /* inside a name token                        */
        SD_DASH1,      /* saw '-', expecting second '-' to open cmt  */
        SD_DASH2,      /* saw '--', entering comment                 */
        SD_COMMENT,    /* inside -- comment --                       */
        SD_CMT_DASH1,  /* saw '-' inside comment                     */
        SD_CMT_DASH2,  /* saw '--' inside comment, looking for end   */
        SD_QUOTE,      /* inside a quoted string                     */
        SD_DONE,
        SD_BACKOUT,
    } State;

    const char *p     = beg;
    State       state = SD_BANG;
    char        qchar = 0;

    if (p == end) return beg;

    while (state != SD_DONE && state != SD_BACKOUT) {
        if (p == end) { state = SD_BACKOUT; break; }

        char c = *p;

        switch (state) {
        case SD_BANG:
            if (c == '!') { ++p; state = SD_BODY; }
            else            state = SD_BACKOUT;
            break;

        case SD_BODY:
            if (c == '>') { ++p; state = SD_DONE; }
            else if (c == '<') { state = SD_DONE; }   /* leave '<' for caller */
            else if (c == '-') { ++p; state = SD_DASH1; }
            else if (c == '\'' || c == '"') { qchar = c; ++p; state = SD_QUOTE; }
            else if (NAME_CH(c)) { ++p; state = SD_NAME; }
            else if (c_isspace(c)) ++p;
            else state = SD_BACKOUT;
            break;

        case SD_NAME:
            if (NAME_CH(c))   ++p;
            else if (c == '-') { ++p; state = SD_DASH1; }
            else               state = SD_BODY;
            break;

        case SD_DASH1:
            if (c == '-') { ++p; state = SD_DASH2; }
            else            state = SD_BACKOUT;
            break;

        case SD_DASH2:
            /* We now have "--": enter a comment. */
            state = SD_COMMENT;
            break;

        case SD_COMMENT:
            if (c == '-') { ++p; state = SD_CMT_DASH1; }
            else            ++p;
            break;

        case SD_CMT_DASH1:
            if (c == '-') { ++p; state = SD_CMT_DASH2; }
            else { ++p; state = SD_COMMENT; }
            break;

        case SD_CMT_DASH2:
            /* "--" seen inside comment: end the comment and return to body. */
            state = SD_BODY;
            break;

        case SD_QUOTE:
            if (c == qchar) { ++p; state = SD_BODY; }
            else              ++p;
            break;

        default:
            break;
        }
    }

    return (state == SD_BACKOUT) ? beg + 1 : p;
}

/*
 * Fast Boyer-Moore-ish search for "-->"
 *
 * Examines the third character of each potential match, advancing by 3 when
 * the character cannot possibly be part of "-->".
 */

static const char *find_comment_end(const char *beg, const char *end)
{
    const char *p = beg - 1;

    while ((p += 3) < end) {
        switch (p[0]) {
        case '>':
            if (p[-1] == '-' && p[-2] == '-')
                return p + 1;
            break;

        case '-':
        at_dash:
            if (p[-1] == '-') {
            at_dash_dash:
                if (++p == end) return NULL;
                switch (p[0]) {
                case '>': return p + 1;
                case '-': goto at_dash_dash;
                }
            } else {
                if ((p += 2) >= end) return NULL;
                switch (p[0]) {
                case '>':
                    if (p[-1] == '-') return p + 1;
                    break;
                case '-':
                    goto at_dash;
                }
            }
            break;
        }
    }
    return NULL;
}

/*
 * Name-filter helper
 *
 * Returns true when HT is NULL (i.e. all names are allowed) or when the
 * lower-cased name [b, e) is a key in HT.
 */

static bool name_allowed(const struct hash_table *ht,
                         const char *b, const char *e)
{
    if (!ht) return true;

    /* Use a stack buffer for the common case; fall back to heap for long names. */
    char   stack[256];
    char  *name;
    size_t len = (size_t)(e - b);
    bool   heap_alloc = false;

    if (len < sizeof stack) {
        name = stack;
    } else {
        name = xmalloc(len + 1);
        heap_alloc = true;
    }

    for (size_t i = 0; i < len; ++i)
        name[i] = c_tolower((unsigned char)b[i]);
    name[len] = '\0';

    bool allowed = hash_table_get(ht, name) != NULL;

    if (heap_alloc) xfree(name);
    return allowed;
}

/*
 * Cursor macros used inside html_scan_tags()
 */

/* Advance the cursor by one byte; jump to `finish' if we hit end-of-input. */
#define ADVANCE(p) do { if (++(p) >= end) goto finish; } while (0)

/* Skip runs of ASCII whitespace, jumping to `finish' on end-of-input. */
#define SKIP_WS(p) do { while (c_isspace(*(p))) { ADVANCE(p); } } while (0)

/*
 * html_scan_tags() (public entry point)
 */

void html_scan_tags(const char *text, int len,
                    void (*cb)(struct html_tag *, void *), void *user_data,
                    int flags,
                    const struct hash_table *allowed_tags,
                    const struct hash_table *allowed_attributes)
{
    if (!len) return;

    const char *p   = text;
    const char *end = text + len;

    /* String pool (starts on the stack) */
    char pool_stack[256];
    Pool pool;
    POOL_INIT(&pool, pool_stack, (int)sizeof pool_stack);

    /* Attribute pair array (starts on the stack) */
    struct html_attr attr_stack[8];
    int              attr_cap      = 8;
    bool             attr_heap     = false;
    struct html_attr *attrs        = attr_stack;

    /* Tag-content tracking list */
    TagStackNode *ts_head = NULL, *ts_tail = NULL;

    /* Variables reused each iteration (declared here to keep the goto-heavy
     * code below readable). */
    int         nattrs;
    bool        is_end_tag;
    bool        skip_tag;           /* tag not in allowed_tags filter */
    const char *tag_name_beg;
    const char *tag_name_end;
    const char *tag_start;

/* main scan loop */
look_for_tag:
    POOL_REWIND(&pool);
    nattrs     = 0;
    is_end_tag = false;

    /* Zip to the next '<'.  memchr is faster than a byte-by-byte loop. */
    p = memchr(p, '<', (size_t)(end - p));
    if (!p) goto finish;

    tag_start = p;
    ADVANCE(p);

    /*
     * Classify what follows '<'.
     */

    if (*p == '!') {
        /*
         * Either a comment or an SGML declaration.
         * Non-strict mode (default): scan for "-->" without caring about
         * SGML comment syntax — matches what real browsers do.
         * Strict mode: delegate to skip_declaration().
         */
        if (!(flags & HTML_SCAN_STRICT_COMMENTS)
            && p + 3 < end && p[1] == '-' && p[2] == '-') {
            const char *ce = find_comment_end(p + 3, end);
            if (ce) p = ce;
        } else {
            p = skip_declaration(p, end);
        }
        if (p == end) goto finish;
        goto look_for_tag;
    }

    if (*p == '/') {
        is_end_tag = true;
        ADVANCE(p);
    }

    /*
     * Tag name
     */
    tag_name_beg = p;
    while (NAME_CH(*p)) ADVANCE(p);
    tag_name_end = p;

    if (tag_name_beg == tag_name_end)
        /* '<' not followed by a name character: treat '<' as literal text. */
        goto look_for_tag;

    SKIP_WS(p);

    /* Push onto the contents-tracking stack for every start-tag we see,
     * regardless of whether it matches the allowed-tags filter, so that
     * end-tags can be matched correctly even when their start-tags were
     * filtered out. */
    if (!is_end_tag) {
        TagStackNode *n = ts_push(&ts_head, &ts_tail);
        n->name_begin = tag_name_beg;
        n->name_end   = tag_name_end;
    }

    /* After skipping whitespace, end-tags must reach '>' or '<' directly. */
    if (is_end_tag && *p != '>' && *p != '<')
        goto backout_tag;

    /* Apply the tag-name filter. */
    if (!name_allowed(allowed_tags, tag_name_beg, tag_name_end)) {
        skip_tag = true;
    } else {
        skip_tag = false;
        copy_to_pool(&pool, tag_name_beg, tag_name_end, COPY_DOWNCASE);
    }

    /*
     * Attribute parsing loop
     */
    while (1) {
        const char *aname_beg, *aname_end;
        const char *aval_beg,  *aval_end;
        const char *araw_beg,  *araw_end;
        int         copy_flags;

        SKIP_WS(p);

        /* XML/XHTML self-closing slash: <br /> */
        if (*p == '/') {
            ADVANCE(p);
            SKIP_WS(p);
            if (*p != '<' && *p != '>')
                goto backout_tag;
        }

        /* End of tag? */
        if (*p == '<' || *p == '>') break;

        /* Attribute name */
        aname_beg = p;
        while (NAME_CH(*p)) ADVANCE(p);
        aname_end = p;

        if (aname_beg == aname_end)
            goto backout_tag;

        SKIP_WS(p);

        /* Attribute value */
        if (NAME_CH(*p) || *p == '/' || *p == '<' || *p == '>') {
            /*
             * Minimised attribute (no '=').  The attribute value is
             * treated as equal to the attribute name, e.g. <ul compact>.
             */
            araw_beg = aval_beg = aname_beg;
            araw_end = aval_end = aname_end;
            copy_flags = COPY_DOWNCASE;

        } else if (*p == '=') {
            ADVANCE(p);
            SKIP_WS(p);

            if (*p == '"' || *p == '\'') {
                /* Quoted value */
                char   qch       = *p;
                bool   saw_nl    = false;
                araw_beg = p;
                ADVANCE(p);
                aval_beg = p;

                while (*p != qch) {
                    if (!saw_nl && *p == '\n') {
                        /*
                         * Newline inside a quoted value almost certainly
                         * means a missing closing quote.  Back up and let
                         * the tag terminate on '<' or '>'.
                         */
                        p       = aval_beg;
                        saw_nl  = true;
                        continue;
                    }
                    if (saw_nl && (*p == '<' || *p == '>'))
                        break;
                    ADVANCE(p);
                }
                aval_end = p;
                if (*p == qch) ADVANCE(p);
                else            goto look_for_tag;   /* unclosed quote */
                araw_end   = p;
                copy_flags = COPY_DECODE_ENTITIES;
                if (flags & HTML_SCAN_TRIM_VALUES)
                    copy_flags |= COPY_TRIM;

            } else {
                /* Unquoted value: read until whitespace or tag delimiter. */
                aval_beg = p;
                while (!c_isspace(*p) && *p != '<' && *p != '>') ADVANCE(p);
                aval_end = p;

                if (aval_beg == aval_end)
                    /* Empty unquoted value: <foo bar=> — malformed. */
                    goto backout_tag;

                araw_beg   = aval_beg;
                araw_end   = aval_end;
                copy_flags = COPY_DECODE_ENTITIES;
            }

        } else {
            /* Something unexpected after the attribute name. */
            goto backout_tag;
        }

        /* If the tag is filtered out, we still need to advance the cursor
         * correctly, but we can skip recording the attribute. */
        if (skip_tag) continue;

        /* If this attribute is filtered out, skip it too. */
        if (!name_allowed(allowed_attributes, aname_beg, aname_end)) continue;

        /* Grow the attribute array if needed. */
        GROW_ARRAY(attrs, attr_cap, nattrs + 1, attr_heap, struct html_attr);

        attrs[nattrs]._name_pool_off  = pool.len;
        copy_to_pool(&pool, aname_beg, aname_end, COPY_DOWNCASE);

        attrs[nattrs]._value_pool_off = pool.len;
        copy_to_pool(&pool, aval_beg, aval_end, copy_flags);

        attrs[nattrs].raw_value_begin = araw_beg;
        attrs[nattrs].raw_value_len   = (int)(araw_end - araw_beg);
        ++nattrs;
    }

    /* Record where this start-tag's content begins (right after '>'). */
    if (!is_end_tag && ts_tail
        && ts_tail->name_begin == tag_name_beg) {
        ts_tail->contents_begin = p + 1;
    }

    if (skip_tag) {
        ADVANCE(p);
        goto look_for_tag;
    }

    /*
     * All data collected so resolve pool pointers and call the user back.
     */
    {
        struct html_tag tag;

        tag.name       = pool.buf;
        tag.is_end_tag = is_end_tag;
        tag.nattrs     = nattrs;

        /* Resolve pool-relative offsets to real pointers.  We defer this
         * until here because pool_reserve() may realloc the buffer. */
        for (int i = 0; i < nattrs; ++i) {
            attrs[i].name  = pool.buf + attrs[i]._name_pool_off;
            attrs[i].value = pool.buf + attrs[i]._value_pool_off;
        }
        tag.attrs          = (nattrs > 0) ? attrs : NULL;
        tag.start          = tag_start;
        tag.end            = p + 1;
        tag.contents_begin = NULL;
        tag.contents_end   = NULL;

        if (is_end_tag) {
            TagStackNode *match = ts_find(ts_tail, tag_name_beg, tag_name_end);
            if (match) {
                if (match->contents_begin) {
                    tag.contents_begin = match->contents_begin;
                    tag.contents_end   = tag_start;
                }
                ts_pop(&ts_head, &ts_tail, match);
            }
        }

        cb(&tag, user_data);

        if (*p != '<') ADVANCE(p);
    }
    goto look_for_tag;

backout_tag:
    /* Not a real tag so treat '<' as literal text and resume from p+1. */
    p = tag_start + 1;
    goto look_for_tag;

finish:
    POOL_FREE(&pool);
    if (attr_heap) xfree(attrs);
    ts_pop(&ts_head, &ts_tail, ts_head);   /* drain any unclosed tags */
}

#undef ADVANCE
#undef SKIP_WS

/*
 * Standalone test driver
 *
 *   cc -DHTML_PARSER_STANDALONE -o test html_parser.c && echo "<b>hi</b>" | ./test
 */
#ifdef HTML_PARSER_STANDALONE

static int tag_count;

static void print_tag(struct html_tag *tag, void *arg)
{
    (void)arg;
    printf("%s%s", tag->is_end_tag ? "/" : "", tag->name);
    for (int i = 0; i < tag->nattrs; ++i)
        printf(" %s=%s", tag->attrs[i].name, tag->attrs[i].value);
    if (tag->contents_begin && tag->contents_end) {
        int clen = (int)(tag->contents_end - tag->contents_begin);
        printf(" [contents: %.*s]", clen, tag->contents_begin);
    }
    putchar('\n');
    ++tag_count;
}

int main(void)
{
    int   cap  = 256;
    char *buf  = malloc(cap);
    int   used = 0;
    int   n;

    while ((n = (int)fread(buf + used, 1, cap - used, stdin)) > 0) {
        used += n;
        if (used == cap) { cap <<= 1; buf = realloc(buf, cap); }
    }

    html_scan_tags(buf, used, print_tag, NULL, 0, NULL, NULL);
    printf("Tags: %d\n", tag_count);
    free(buf);
    return 0;
}
#endif /* HTML_PARSER_STANDALONE */
