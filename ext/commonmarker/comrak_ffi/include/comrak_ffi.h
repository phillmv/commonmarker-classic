#ifndef COMRAK_FFI_H
#define COMRAK_FFI_H

/*
 * A small C ABI over comrak, purpose-built for commonmarker's Ruby
 * extension. It exposes comrak's model; translating cmark-gfm semantics
 * (option bits, node type names, error behaviour) is the caller's job.
 *
 * Memory model
 * ------------
 * comrak allocates nodes in arenas that are only freed as a whole. A
 * `comrak_group` owns one or more arenas and is reference counted. Every
 * `comrak_node *` lives in exactly one group and stays valid for as long as
 * a reference to that group, or to any group merged with it, is held.
 *
 * Linking two nodes (append_child and friends) merges their groups, so a
 * tree can never outlive an arena that one of its nodes was allocated in.
 * Unlinking a node does not free it; its memory is reclaimed together with
 * its group.
 *
 * Strings
 * -------
 * Input strings are (pointer, length) pairs and need not be NUL-terminated.
 * Invalid UTF-8 is replaced with U+FFFD. Getters borrow: the returned
 * pointer is valid until the node is next mutated or its group is freed.
 * Rendered output is owned by the caller and must be released with
 * comrak_string_free().
 *
 * Threading
 * ---------
 * Nothing here is thread-safe. Callers must serialise every call, including
 * group retain/release (the Ruby extension relies on the GVL).
 *
 * Every #define and enum value below is cross-checked against the Rust
 * definitions by `cargo test` in comrak_ffi.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct comrak_node comrak_node;
typedef struct comrak_group comrak_group;

typedef enum {
  COMRAK_OK = 0,
  /* comrak panicked; the operation had no effect. */
  COMRAK_ERR_PANIC = 1,
  /* `flags` contained bits this library does not know about. */
  COMRAK_ERR_INVALID_FLAGS = 2,
  /* A renderer reported an error. */
  COMRAK_ERR_RENDER = 3,
} comrak_status;

/* Option flags, accepted by parse and render functions alike. */
#define COMRAK_OPT_EXT_STRIKETHROUGH (1u << 0)
#define COMRAK_OPT_EXT_TABLE (1u << 1)
#define COMRAK_OPT_EXT_AUTOLINK (1u << 2)
#define COMRAK_OPT_EXT_TASKLIST (1u << 3)
#define COMRAK_OPT_EXT_FOOTNOTES (1u << 4)
#define COMRAK_OPT_PARSE_SMART (1u << 8)
#define COMRAK_OPT_RENDER_HARDBREAKS (1u << 16)
#define COMRAK_OPT_RENDER_GITHUB_PRE_LANG (1u << 17)
#define COMRAK_OPT_RENDER_FULL_INFO_STRING (1u << 18)
#define COMRAK_OPT_RENDER_UNSAFE (1u << 19)
#define COMRAK_OPT_RENDER_SOURCEPOS (1u << 20)
#define COMRAK_OPT_RENDER_GFM_QUIRKS (1u << 21)

typedef enum {
  COMRAK_NODE_DOCUMENT = 1,
  COMRAK_NODE_BLOCK_QUOTE = 2,
  COMRAK_NODE_LIST = 3,
  COMRAK_NODE_ITEM = 4,
  COMRAK_NODE_CODE_BLOCK = 5,
  COMRAK_NODE_HTML_BLOCK = 6,
  COMRAK_NODE_PARAGRAPH = 7,
  COMRAK_NODE_HEADING = 8,
  COMRAK_NODE_THEMATIC_BREAK = 9,
  COMRAK_NODE_FOOTNOTE_DEFINITION = 10,
  COMRAK_NODE_TABLE = 11,
  COMRAK_NODE_TABLE_ROW = 12,
  COMRAK_NODE_TABLE_CELL = 13,
  COMRAK_NODE_TASK_ITEM = 14,
  COMRAK_NODE_TEXT = 15,
  COMRAK_NODE_SOFTBREAK = 16,
  COMRAK_NODE_LINEBREAK = 17,
  COMRAK_NODE_CODE = 18,
  COMRAK_NODE_HTML_INLINE = 19,
  COMRAK_NODE_EMPH = 20,
  COMRAK_NODE_STRONG = 21,
  COMRAK_NODE_LINK = 22,
  COMRAK_NODE_IMAGE = 23,
  COMRAK_NODE_FOOTNOTE_REFERENCE = 24,
  COMRAK_NODE_STRIKETHROUGH = 25,
  /* A comrak node type this library does not expose. */
  COMRAK_NODE_OTHER = 255,
} comrak_node_type;

typedef enum {
  COMRAK_LIST_BULLET = 1,
  COMRAK_LIST_ORDERED = 2,
} comrak_list_type;

typedef enum {
  COMRAK_ALIGN_NONE = 0,
  COMRAK_ALIGN_LEFT = 1,
  COMRAK_ALIGN_CENTER = 2,
  COMRAK_ALIGN_RIGHT = 3,
} comrak_table_alignment;

/* Groups */

/* Returns a new reference to the group `group` currently belongs to. */
comrak_group *comrak_group_retain(comrak_group *group);
void comrak_group_release(comrak_group *group);

/* Parsing and construction */

/* On COMRAK_OK, stores the document node and a new group reference. */
comrak_status comrak_parse_document(const char *text, size_t len,
                                    uint32_t flags, comrak_group **group_out,
                                    comrak_node **root_out);

/*
 * Creates a detached node in a new group. Returns NULL for types that cannot
 * be created standalone (tables, table rows and cells, task items, OTHER).
 */
comrak_node *comrak_node_new(comrak_node_type type, comrak_group **group_out);

/* Inspection */

comrak_node_type comrak_node_get_type(const comrak_node *node);
/* True for a table row that is the table's header row. */
bool comrak_node_is_table_header(const comrak_node *node);
void comrak_node_get_sourcepos(const comrak_node *node, size_t *start_line,
                               size_t *start_column, size_t *end_line,
                               size_t *end_column);

/* Navigation; each returns NULL if there is no such node. */

comrak_node *comrak_node_parent(const comrak_node *node);
comrak_node *comrak_node_first_child(const comrak_node *node);
comrak_node *comrak_node_last_child(const comrak_node *node);
comrak_node *comrak_node_next(const comrak_node *node);
comrak_node *comrak_node_previous(const comrak_node *node);

/*
 * Tree mutation. Each returns false, changing nothing, if the result would be
 * ill-formed: the parent cannot contain the child's type (per comrak), the
 * child is the parent or one of its ancestors, or (for siblings) the node has
 * no parent. On success the two groups are merged.
 */

bool comrak_node_append_child(comrak_node *node, comrak_group *node_group,
                              comrak_node *child, comrak_group *child_group);
bool comrak_node_prepend_child(comrak_node *node, comrak_group *node_group,
                               comrak_node *child, comrak_group *child_group);
bool comrak_node_insert_before(comrak_node *node, comrak_group *node_group,
                               comrak_node *sibling,
                               comrak_group *sibling_group);
bool comrak_node_insert_after(comrak_node *node, comrak_group *node_group,
                              comrak_node *sibling,
                              comrak_group *sibling_group);
void comrak_node_unlink(comrak_node *node);

/*
 * Fields. Getters return false if the node has no such field; setters return
 * false, changing nothing, if the node has no such field or the value is out
 * of range.
 */

/* Text, code, code block, HTML block and inline HTML content. */
bool comrak_node_get_literal(const comrak_node *node, const char **ptr,
                             size_t *len);
bool comrak_node_set_literal(comrak_node *node, const char *ptr, size_t len);

/* Links and images. */
bool comrak_node_get_url(const comrak_node *node, const char **ptr,
                         size_t *len);
bool comrak_node_set_url(comrak_node *node, const char *ptr, size_t len);
bool comrak_node_get_title(const comrak_node *node, const char **ptr,
                           size_t *len);
bool comrak_node_set_title(comrak_node *node, const char *ptr, size_t len);

/* Code blocks. */
bool comrak_node_get_fence_info(const comrak_node *node, const char **ptr,
                                size_t *len);
bool comrak_node_set_fence_info(comrak_node *node, const char *ptr,
                                size_t len);

/* Footnote definitions and references. */
bool comrak_node_get_footnote_name(const comrak_node *node, const char **ptr,
                                   size_t *len);
/* Footnote references: 1-based index of the footnote; 0 until resolved. */
bool comrak_node_get_footnote_ix(const comrak_node *node, uint32_t *ix);

/* Headings; levels are 1 to 6. */
bool comrak_node_get_heading_level(const comrak_node *node, int *level);
bool comrak_node_set_heading_level(comrak_node *node, int level);

/* Lists. */
bool comrak_node_get_list_type(const comrak_node *node,
                               comrak_list_type *type);
bool comrak_node_set_list_type(comrak_node *node, comrak_list_type type);
bool comrak_node_get_list_start(const comrak_node *node, size_t *start);
bool comrak_node_set_list_start(comrak_node *node, size_t start);
bool comrak_node_get_list_tight(const comrak_node *node, bool *tight);
bool comrak_node_set_list_tight(comrak_node *node, bool tight);

/* Tables: the column count, or 0 if `node` is not a table. */
size_t comrak_node_get_table_columns(const comrak_node *node);
comrak_table_alignment comrak_node_get_table_alignment(const comrak_node *node,
                                                       size_t column);

/* Task items. */
bool comrak_node_get_task_checked(const comrak_node *node, bool *checked);
bool comrak_node_set_task_checked(comrak_node *node, bool checked);

/* Rendering; renders `node` and its descendants. */

comrak_status comrak_render_html(const comrak_node *node, uint32_t flags,
                                 char **out, size_t *out_len);
comrak_status comrak_render_xml(const comrak_node *node, uint32_t flags,
                                char **out, size_t *out_len);
/* `width` is the column to wrap at; 0 disables wrapping. */
comrak_status comrak_render_commonmark(const comrak_node *node, uint32_t flags,
                                       size_t width, char **out,
                                       size_t *out_len);

/* Escaping, as comrak's HTML renderer does it. */

void comrak_escape_html(const char *ptr, size_t len, char **out,
                        size_t *out_len);
void comrak_escape_href(const char *ptr, size_t len, char **out,
                        size_t *out_len);

void comrak_string_free(char *ptr, size_t len);

#ifdef __cplusplus
}
#endif

#endif
