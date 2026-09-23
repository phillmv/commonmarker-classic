#include "commonmarker.h"
#include "comrak_ffi.h"

#include <stdio.h>
#include <string.h>

/*
 * Ruby bindings for comrak, via comrak_ffi.
 *
 * This file preserves the Ruby API that commonmarker exposed over cmark-gfm:
 * option bits, node type names, which accessors raise, and string encodings.
 * Where comrak cannot do what cmark-gfm did, we raise NotImplementedError.
 * See COMRAK.md at the root of the repository.
 *
 * Node objects
 * ------------
 * Each Ruby Node wraps one comrak node and holds a reference to its group,
 * which keeps the node's memory alive (see comrak_ffi.h). Wrappers are cached
 * per node in `node_cache`, so navigating to the same node twice returns the
 * same Ruby object. To keep those cached objects valid, a wrapper marks the
 * wrappers of its parent and children, exactly as the cmark-gfm binding did:
 * any Ruby object reachable by navigating from a live wrapper is itself live.
 */

static VALUE rb_eNodeError;
static VALUE rb_cNode;

static VALUE sym_document;
static VALUE sym_blockquote;
static VALUE sym_list;
static VALUE sym_list_item;
static VALUE sym_code_block;
static VALUE sym_html;
static VALUE sym_paragraph;
static VALUE sym_header;
static VALUE sym_hrule;
static VALUE sym_text;
static VALUE sym_softbreak;
static VALUE sym_linebreak;
static VALUE sym_code;
static VALUE sym_inline_html;
static VALUE sym_emph;
static VALUE sym_strong;
static VALUE sym_link;
static VALUE sym_image;
static VALUE sym_footnote_reference;
static VALUE sym_footnote_definition;
static VALUE sym_table;
static VALUE sym_table_header;
static VALUE sym_table_row;
static VALUE sym_table_cell;
static VALUE sym_strikethrough;

static VALUE sym_bullet_list;
static VALUE sym_ordered_list;

static VALUE sym_left;
static VALUE sym_right;
static VALUE sym_center;

/* cmark-gfm's option bits, as used by lib/commonmarker/config.rb. */
#define CMARK_OPT_SOURCEPOS (1 << 1)
#define CMARK_OPT_HARDBREAKS (1 << 2)
#define CMARK_OPT_NOBREAKS (1 << 4)
#define CMARK_OPT_VALIDATE_UTF8 (1 << 9)
#define CMARK_OPT_SMART (1 << 10)
#define CMARK_OPT_GITHUB_PRE_LANG (1 << 11)
#define CMARK_OPT_LIBERAL_HTML_TAG (1 << 12)
#define CMARK_OPT_FOOTNOTES (1 << 13)
#define CMARK_OPT_STRIKETHROUGH_DOUBLE_TILDE (1 << 14)
#define CMARK_OPT_TABLE_PREFER_STYLE_ATTRIBUTES (1 << 15)
#define CMARK_OPT_FULL_INFO_STRING (1 << 16)
#define CMARK_OPT_UNSAFE (1 << 17)

/* The stages an option can affect, to decide whether it is honoured. */
#define STAGE_PARSE (1 << 0)
#define STAGE_HTML (1 << 1)
#define STAGE_XML (1 << 2)
#define STAGE_COMMONMARK (1 << 3)

/*
 * Options cmark-gfm honoured that comrak has no equivalent for, and the
 * stages at which cmark-gfm read them. Passing one to a stage that ignored it
 * anyway (e.g. NOBREAKS to the XML renderer) stays a no-op, as before.
 */
static const struct {
  int bit;
  const char *name;
  int stages;
} unsupported_options[] = {
    {CMARK_OPT_LIBERAL_HTML_TAG, "LIBERAL_HTML_TAG", STAGE_PARSE},
    {CMARK_OPT_STRIKETHROUGH_DOUBLE_TILDE, "STRIKETHROUGH_DOUBLE_TILDE",
     STAGE_PARSE},
    {CMARK_OPT_NOBREAKS, "NOBREAKS", STAGE_HTML | STAGE_COMMONMARK},
    {CMARK_OPT_TABLE_PREFER_STYLE_ATTRIBUTES, "TABLE_PREFER_STYLE_ATTRIBUTES",
     STAGE_HTML},
};

static const struct {
  const char *name;
  uint32_t flag; /* 0: known to cmark-gfm, but not supported by comrak */
} extensions[] = {
    {"table", COMRAK_OPT_EXT_TABLE},
    {"strikethrough", COMRAK_OPT_EXT_STRIKETHROUGH},
    {"autolink", COMRAK_OPT_EXT_AUTOLINK},
    {"tagfilter", 0},
    {"tasklist", COMRAK_OPT_EXT_TASKLIST},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Translates cmark-gfm option bits into comrak_ffi flags for the given
 * stages. Unknown bits are ignored, as cmark-gfm ignored them.
 */
static uint32_t translate_options(VALUE rb_options, int stages) {
  size_t i;
  int options;
  uint32_t flags = COMRAK_OPT_RENDER_GFM_QUIRKS;

  Check_Type(rb_options, T_FIXNUM);
  options = FIX2INT(rb_options);

  for (i = 0; i < ARRAY_LEN(unsupported_options); ++i) {
    if ((options & unsupported_options[i].bit) &&
        (stages & unsupported_options[i].stages)) {
      rb_raise(rb_eNotImpError,
               "the %s option is not supported by the comrak backend",
               unsupported_options[i].name);
    }
  }

  /* comrak always tracks source positions and validates UTF-8, so
     SOURCEPOS at parse time and VALIDATE_UTF8 need no translation. */
  if (options & CMARK_OPT_SMART)
    flags |= COMRAK_OPT_PARSE_SMART;
  if (options & CMARK_OPT_FOOTNOTES)
    flags |= COMRAK_OPT_EXT_FOOTNOTES;
  if (options & CMARK_OPT_HARDBREAKS)
    flags |= COMRAK_OPT_RENDER_HARDBREAKS;
  if (options & CMARK_OPT_GITHUB_PRE_LANG)
    flags |= COMRAK_OPT_RENDER_GITHUB_PRE_LANG;
  if (options & CMARK_OPT_FULL_INFO_STRING)
    flags |= COMRAK_OPT_RENDER_FULL_INFO_STRING;
  if (options & CMARK_OPT_UNSAFE)
    flags |= COMRAK_OPT_RENDER_UNSAFE;
  if (options & CMARK_OPT_SOURCEPOS)
    flags |= COMRAK_OPT_RENDER_SOURCEPOS;

  return flags;
}

/* `not_found_format` differs between parsing and rendering, as it did. */
static uint32_t translate_extensions(VALUE rb_extensions,
                                     const char *not_found_format) {
  long i;
  size_t j;
  uint32_t flags = 0;

  Check_Type(rb_extensions, T_ARRAY);

  for (i = 0; i < RARRAY_LEN(rb_extensions); ++i) {
    VALUE rb_ext_name = rb_ary_entry(rb_extensions, i);
    const char *name;

    if (!SYMBOL_P(rb_ext_name)) {
      rb_raise(rb_eTypeError,
               "extension names should be Symbols; got a %" PRIsVALUE "",
               rb_obj_class(rb_ext_name));
    }

    name = rb_id2name(SYM2ID(rb_ext_name));
    for (j = 0; j < ARRAY_LEN(extensions); ++j) {
      if (strcmp(name, extensions[j].name) == 0)
        break;
    }

    if (j == ARRAY_LEN(extensions))
      rb_raise(rb_eArgError, not_found_format, name);
    if (!extensions[j].flag)
      rb_raise(rb_eNotImpError,
               "the %s extension is not supported by the comrak backend",
               name);

    flags |= extensions[j].flag;
  }

  return flags;
}

static void check_status(comrak_status status, const char *what) {
  switch (status) {
  case COMRAK_OK:
    return;
  case COMRAK_ERR_PANIC:
    rb_raise(rb_eNodeError, "comrak panicked while %s", what);
  case COMRAK_ERR_RENDER:
    rb_raise(rb_eNodeError, "comrak failed while %s", what);
  default:
    rb_raise(rb_eRuntimeError, "internal error (status %d) while %s",
             (int)status, what);
  }
}

/* Copies a string comrak_ffi allocated into a Ruby string, then frees it. */
static VALUE adopt_string(char *str, size_t len, rb_encoding *enc) {
  VALUE ret = rb_enc_str_new(str, len, enc);
  comrak_string_free(str, len);
  return ret;
}

/* ------------------------------------------------------------------------ */
/* Node objects */

typedef struct {
  VALUE self;
  comrak_node *node;
  comrak_group *group;
} node_wrapper;

static st_table *node_cache;

static node_wrapper *cached_wrapper(const comrak_node *node) {
  st_data_t found;
  if (node && st_lookup(node_cache, (st_data_t)node, &found))
    return (node_wrapper *)found;
  return NULL;
}

static void mark_cached(const comrak_node *node) {
  node_wrapper *w = cached_wrapper(node);
  if (w)
#ifdef HAVE_RB_GC_MARK_MOVABLE
    rb_gc_mark_movable(w->self);
#else
    rb_gc_mark(w->self);
#endif
}

static void node_mark(void *data) {
  node_wrapper *w = data;
  comrak_node *child;

  if (!w->node)
    return;

  /* The parent keeps the tree's wrappers alive while a child is held. */
  mark_cached(comrak_node_parent(w->node));

  /* Children keep their cached wrappers valid. */
  for (child = comrak_node_first_child(w->node); child;
       child = comrak_node_next(child))
    mark_cached(child);
}

static void node_free(void *data) {
  node_wrapper *w = data;

  if (w->node) {
    st_data_t key = (st_data_t)w->node;
    if (cached_wrapper(w->node) == w)
      st_delete(node_cache, &key, NULL);
    comrak_group_release(w->group);
  }
  xfree(w);
}

static size_t node_memsize(const void *data) { return sizeof(node_wrapper); }

#ifdef HAVE_RB_GC_MARK_MOVABLE
static void node_compact(void *data) {
  node_wrapper *w = data;
  w->self = rb_gc_location(w->self);
}
#endif

static const rb_data_type_t node_data_type = {
    "CommonMarker::Node",
    {
        node_mark,
        node_free,
        node_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
        node_compact,
#endif
    },
    0,
    0,
    RUBY_TYPED_FREE_IMMEDIATELY,
};

/*
 * Returns the Ruby object for `node`, creating it if necessary. `group` is
 * any group reference that keeps `node` alive; it is not consumed.
 */
static VALUE rb_node_to_value(comrak_node *node, comrak_group *group) {
  node_wrapper *w;
  VALUE val;

  if (node == NULL)
    return Qnil;

  w = cached_wrapper(node);
  if (w)
    return w->self;

  /* w is zeroed until filled in, which node_mark and node_free allow for. */
  val = TypedData_Make_Struct(rb_cNode, node_wrapper, &node_data_type, w);
  w->self = val;
  w->group = comrak_group_retain(group);
  w->node = node;
  st_insert(node_cache, (st_data_t)node, (st_data_t)w);

  return val;
}

/* As rb_node_to_value, but consumes the group reference. */
static VALUE rb_node_adopt(comrak_node *node, comrak_group *group) {
  VALUE val = rb_node_to_value(node, group);
  comrak_group_release(group);
  return val;
}

static node_wrapper *unwrap(VALUE self) {
  node_wrapper *w;
  TypedData_Get_Struct(self, node_wrapper, &node_data_type, w);
  return w;
}

static comrak_node *get_node(VALUE self) { return unwrap(self)->node; }

/* A neighbour of `self`, which lives in the same group. */
static VALUE neighbour(VALUE self, comrak_node *node) {
  return rb_node_to_value(node, unwrap(self)->group);
}

/* ------------------------------------------------------------------------ */
/* Node types */

static const struct {
  comrak_node_type type;
  VALUE *symbol;
  const char *type_string; /* cmark-gfm's cmark_node_get_type_string() */
} node_types[] = {
    {COMRAK_NODE_DOCUMENT, &sym_document, "document"},
    {COMRAK_NODE_BLOCK_QUOTE, &sym_blockquote, "block_quote"},
    {COMRAK_NODE_LIST, &sym_list, "list"},
    {COMRAK_NODE_ITEM, &sym_list_item, "item"},
    {COMRAK_NODE_TASK_ITEM, &sym_list_item, "tasklist"},
    {COMRAK_NODE_CODE_BLOCK, &sym_code_block, "code_block"},
    {COMRAK_NODE_HTML_BLOCK, &sym_html, "html_block"},
    {COMRAK_NODE_PARAGRAPH, &sym_paragraph, "paragraph"},
    {COMRAK_NODE_HEADING, &sym_header, "heading"},
    {COMRAK_NODE_THEMATIC_BREAK, &sym_hrule, "thematic_break"},
    {COMRAK_NODE_TEXT, &sym_text, "text"},
    {COMRAK_NODE_SOFTBREAK, &sym_softbreak, "softbreak"},
    {COMRAK_NODE_LINEBREAK, &sym_linebreak, "linebreak"},
    {COMRAK_NODE_CODE, &sym_code, "code"},
    {COMRAK_NODE_HTML_INLINE, &sym_inline_html, "html_inline"},
    {COMRAK_NODE_EMPH, &sym_emph, "emph"},
    {COMRAK_NODE_STRONG, &sym_strong, "strong"},
    {COMRAK_NODE_LINK, &sym_link, "link"},
    {COMRAK_NODE_IMAGE, &sym_image, "image"},
    /* cmark-gfm had no type string for footnotes. */
    {COMRAK_NODE_FOOTNOTE_REFERENCE, &sym_footnote_reference, "<unknown>"},
    {COMRAK_NODE_FOOTNOTE_DEFINITION, &sym_footnote_definition, "<unknown>"},
    {COMRAK_NODE_TABLE, &sym_table, "table"},
    {COMRAK_NODE_TABLE_ROW, &sym_table_row, "table_row"},
    {COMRAK_NODE_TABLE_CELL, &sym_table_cell, "table_cell"},
    {COMRAK_NODE_STRIKETHROUGH, &sym_strikethrough, "strikethrough"},
};

/* Returns the node_types index for `node`, or -1. */
static int node_type_index(comrak_node *node) {
  comrak_node_type type = comrak_node_get_type(node);
  size_t i;
  for (i = 0; i < ARRAY_LEN(node_types); ++i) {
    if (node_types[i].type == type)
      return (int)i;
  }
  return -1;
}

/* The types Node.new accepts, as before. */
static const struct {
  VALUE *symbol;
  comrak_node_type type;
} creatable_types[] = {
    {&sym_document, COMRAK_NODE_DOCUMENT},
    {&sym_blockquote, COMRAK_NODE_BLOCK_QUOTE},
    {&sym_list, COMRAK_NODE_LIST},
    {&sym_list_item, COMRAK_NODE_ITEM},
    {&sym_code_block, COMRAK_NODE_CODE_BLOCK},
    {&sym_html, COMRAK_NODE_HTML_BLOCK},
    {&sym_paragraph, COMRAK_NODE_PARAGRAPH},
    {&sym_header, COMRAK_NODE_HEADING},
    {&sym_hrule, COMRAK_NODE_THEMATIC_BREAK},
    {&sym_text, COMRAK_NODE_TEXT},
    {&sym_softbreak, COMRAK_NODE_SOFTBREAK},
    {&sym_linebreak, COMRAK_NODE_LINEBREAK},
    {&sym_code, COMRAK_NODE_CODE},
    {&sym_inline_html, COMRAK_NODE_HTML_INLINE},
    {&sym_emph, COMRAK_NODE_EMPH},
    {&sym_strong, COMRAK_NODE_STRONG},
    {&sym_link, COMRAK_NODE_LINK},
    {&sym_image, COMRAK_NODE_IMAGE},
    {&sym_footnote_reference, COMRAK_NODE_FOOTNOTE_REFERENCE},
    {&sym_footnote_definition, COMRAK_NODE_FOOTNOTE_DEFINITION},
};

/* ------------------------------------------------------------------------ */
/* Parsing */

static comrak_node *parse(VALUE rb_text, long len, uint32_t flags,
                          comrak_group **group) {
  comrak_node *doc;
  comrak_status status;

  if (len < 0)
    len = 0;
  if (len > RSTRING_LEN(rb_text))
    len = RSTRING_LEN(rb_text);

  status = comrak_parse_document(RSTRING_PTR(rb_text), (size_t)len, flags,
                                 group, &doc);
  check_status(status, "parsing");
  return doc;
}

/*
 * Internal: Parses a Markdown string into an HTML string.
 *
 */
static VALUE rb_markdown_to_html(VALUE self, VALUE rb_text, VALUE rb_options,
                                 VALUE rb_extensions) {
  uint32_t flags;
  comrak_group *group;
  comrak_node *doc;
  comrak_status status;
  char *html;
  size_t len;

  Check_Type(rb_text, T_STRING);

  flags = translate_options(rb_options, STAGE_PARSE | STAGE_HTML) |
          translate_extensions(rb_extensions, "extension %s not found");

  doc = parse(rb_text, RSTRING_LEN(rb_text), flags, &group);
  status = comrak_render_html(doc, flags, &html, &len);
  comrak_group_release(group);
  check_status(status, "rendering HTML");

  return adopt_string(html, len, rb_utf8_encoding());
}

/*
 * Internal: Parses a Markdown string into an HTML string.
 *
 */
static VALUE rb_markdown_to_xml(VALUE self, VALUE rb_text, VALUE rb_options,
                                VALUE rb_extensions) {
  uint32_t flags;
  comrak_group *group;
  comrak_node *doc;
  comrak_status status;
  char *xml;
  size_t len;

  Check_Type(rb_text, T_STRING);

  flags = translate_options(rb_options, STAGE_PARSE | STAGE_XML) |
          translate_extensions(rb_extensions, "extension %s not found");

  doc = parse(rb_text, RSTRING_LEN(rb_text), flags, &group);
  status = comrak_render_xml(doc, flags, &xml, &len);
  comrak_group_release(group);
  check_status(status, "rendering XML");

  return adopt_string(xml, len, rb_utf8_encoding());
}

/*
 * Internal: Creates a node based on a node type.
 *
 * type -  A {Symbol} representing the node to be created. Must be one of the
 * following:
 * - `:document`
 * - `:blockquote`
 * - `:list`
 * - `:list_item`
 * - `:code_block`
 * - `:html`
 * - `:paragraph`
 * - `:header`
 * - `:hrule`
 * - `:text`
 * - `:softbreak`
 * - `:linebreak`
 * - `:code`
 * - `:inline_html`
 * - `:emph`
 * - `:strong`
 * - `:link`
 * - `:image`
 */
static VALUE rb_node_new(VALUE self, VALUE type) {
  size_t i;
  comrak_group *group;
  comrak_node *node;

  Check_Type(type, T_SYMBOL);

  for (i = 0; i < ARRAY_LEN(creatable_types); ++i) {
    if (*creatable_types[i].symbol == type)
      break;
  }
  if (i == ARRAY_LEN(creatable_types))
    rb_raise(rb_eNodeError, "invalid node of type %d", 0);

  node = comrak_node_new(creatable_types[i].type, &group);
  if (node == NULL) {
    rb_raise(rb_eNodeError, "could not create node of type %d",
             (int)creatable_types[i].type);
  }

  return rb_node_adopt(node, group);
}

/*
 * Internal: Parses a Markdown string into a document.
 *
 */
static VALUE rb_parse_document(VALUE self, VALUE rb_text, VALUE rb_len,
                               VALUE rb_options, VALUE rb_extensions) {
  uint32_t flags;
  comrak_group *group;
  comrak_node *doc;

  Check_Type(rb_text, T_STRING);
  Check_Type(rb_len, T_FIXNUM);
  Check_Type(rb_options, T_FIXNUM);

  flags = translate_options(rb_options, STAGE_PARSE) |
          translate_extensions(rb_extensions, "extension %s not found");

  doc = parse(rb_text, FIX2LONG(rb_len), flags, &group);

  return rb_node_adopt(doc, group);
}

/* ------------------------------------------------------------------------ */
/* Accessors */

typedef bool (*string_getter)(const comrak_node *, const char **, size_t *);
typedef bool (*string_setter)(comrak_node *, const char *, size_t);

/* cmark-gfm returned these as binary strings; so do we. */
static VALUE get_binary_string(VALUE self, string_getter get,
                               const char *error) {
  const char *ptr;
  size_t len;

  if (!get(get_node(self), &ptr, &len))
    rb_raise(rb_eNodeError, "%s", error);

  return rb_str_new(ptr, len);
}

static VALUE set_string(VALUE self, VALUE s, string_setter set,
                        const char *error) {
  char *text;
  Check_Type(s, T_STRING);

  text = StringValueCStr(s);
  if (!set(get_node(self), text, strlen(text)))
    rb_raise(rb_eNodeError, "%s", error);

  return Qnil;
}

/*
 * Public: Fetch the string contents of the node.
 *
 * Returns a {String}.
 */
static VALUE rb_node_get_string_content(VALUE self) {
  comrak_node *node = get_node(self);
  const char *ptr;
  size_t len;
  uint32_t ix;

  /* cmark-gfm stored a reference's footnote number as its literal. */
  if (comrak_node_get_footnote_ix(node, &ix)) {
    char buf[16] = "";
    if (ix)
      snprintf(buf, sizeof(buf), "%u", (unsigned)ix);
    return rb_utf8_str_new_cstr(buf);
  }

  if (comrak_node_get_literal(node, &ptr, &len) ||
      (comrak_node_get_type(node) == COMRAK_NODE_FOOTNOTE_DEFINITION &&
       comrak_node_get_footnote_name(node, &ptr, &len)))
    return rb_utf8_str_new(ptr, len);

  rb_raise(rb_eNodeError, "could not get string content");
}

/*
 * Public: Sets the string content of the node.
 *
 * string - A {String} containing new content.
 *
 * Raises NodeError if the string content can't be set.
 */
static VALUE rb_node_set_string_content(VALUE self, VALUE s) {
  if (comrak_node_get_type(get_node(self)) == COMRAK_NODE_FOOTNOTE_REFERENCE) {
    rb_raise(rb_eNotImpError, "setting the string content of a "
                              "footnote_reference is not supported by the "
                              "comrak backend");
  }

  return set_string(self, s, comrak_node_set_literal,
                    "could not set string content");
}

/*
 * Public: Fetches the list type of the node.
 *
 * Returns a {Symbol} representing the node's type.
 */
static VALUE rb_node_get_type(VALUE self) {
  comrak_node *node = get_node(self);
  int i = node_type_index(node);

  if (i < 0)
    rb_raise(rb_eNodeError, "invalid node type %d",
             (int)comrak_node_get_type(node));

  if (node_types[i].type == COMRAK_NODE_TABLE_ROW &&
      comrak_node_is_table_header(node))
    return sym_table_header;

  return *node_types[i].symbol;
}

/*
 * Public: Fetches the sourcepos of the node.
 *
 * Returns a {Hash} containing {Symbol} keys of the positions.
 */
static VALUE rb_node_get_sourcepos(VALUE self) {
  size_t start_line, start_column, end_line, end_column;
  VALUE result;

  comrak_node_get_sourcepos(get_node(self), &start_line, &start_column,
                            &end_line, &end_column);

  result = rb_hash_new();
  rb_hash_aset(result, CSTR2SYM("start_line"), SIZET2NUM(start_line));
  rb_hash_aset(result, CSTR2SYM("start_column"), SIZET2NUM(start_column));
  rb_hash_aset(result, CSTR2SYM("end_line"), SIZET2NUM(end_line));
  rb_hash_aset(result, CSTR2SYM("end_column"), SIZET2NUM(end_column));

  return result;
}

/*
 * Public: Returns the type of the current pointer as a string.
 *
 * Returns a {String}.
 */
static VALUE rb_node_get_type_string(VALUE self) {
  comrak_node *node = get_node(self);
  int i = node_type_index(node);

  if (i < 0)
    return rb_str_new2("<unknown>");

  if (node_types[i].type == COMRAK_NODE_TABLE_ROW &&
      comrak_node_is_table_header(node))
    return rb_str_new2("table_header");

  return rb_str_new2(node_types[i].type_string);
}

/*
 * Internal: Unlinks the node from the tree (fixing pointers in
 * parents and siblings appropriately).
 */
static VALUE rb_node_unlink(VALUE self) {
  comrak_node_unlink(get_node(self));

  return Qnil;
}

/* Public: Fetches the first child of the node.
 *
 * Returns a {Node} if a child exists, `nil` otherise.
 */
static VALUE rb_node_first_child(VALUE self) {
  return neighbour(self, comrak_node_first_child(get_node(self)));
}

/* Public: Fetches the next sibling of the node.
 *
 * Returns a {Node} if a sibling exists, `nil` otherwise.
 */
static VALUE rb_node_next(VALUE self) {
  return neighbour(self, comrak_node_next(get_node(self)));
}

typedef bool (*linker)(comrak_node *, comrak_group *, comrak_node *,
                       comrak_group *);

static VALUE link_nodes(VALUE self, VALUE other, linker link,
                        const char *error) {
  node_wrapper *w1 = unwrap(self);
  node_wrapper *w2 = unwrap(other);

  if (!link(w1->node, w1->group, w2->node, w2->group))
    rb_raise(rb_eNodeError, "%s", error);

  return Qtrue;
}

/*
 * Public: Inserts a node as a sibling before the current node.
 *
 * sibling - A sibling {Node} to insert.
 *
 * Returns `true` if successful.
 * Raises NodeError if the node can't be inserted.
 */
static VALUE rb_node_insert_before(VALUE self, VALUE sibling) {
  return link_nodes(self, sibling, comrak_node_insert_before,
                    "could not insert before");
}

/* Internal: Convert the node to an HTML string.
 *
 * Returns a {String}.
 */
static VALUE rb_render_html(VALUE self, VALUE rb_options, VALUE rb_extensions) {
  uint32_t flags;
  char *html;
  size_t len;

  flags = translate_options(rb_options, STAGE_HTML) |
          translate_extensions(rb_extensions, "extension %s not found\n");

  check_status(comrak_render_html(get_node(self), flags, &html, &len),
               "rendering HTML");

  return adopt_string(html, len, rb_ascii8bit_encoding());
}

/* Internal: Convert the node to an XML string.
 *
 * Returns a {String}.
 */
static VALUE rb_render_xml(VALUE self, VALUE rb_options) {
  uint32_t flags = translate_options(rb_options, STAGE_XML);
  char *xml;
  size_t len;

  check_status(comrak_render_xml(get_node(self), flags, &xml, &len),
               "rendering XML");

  return adopt_string(xml, len, rb_ascii8bit_encoding());
}

static int get_width(VALUE rb_width) {
  int width = 120;
  if (!NIL_P(rb_width)) {
    Check_Type(rb_width, T_FIXNUM);
    width = FIX2INT(rb_width);
  }
  /* cmark-gfm wrapped only for positive widths. */
  return width > 0 ? width : 0;
}

/* Internal: Convert the node to a CommonMark string.
 *
 * Returns a {String}.
 */
static VALUE rb_render_commonmark(int argc, VALUE *argv, VALUE self) {
  VALUE rb_options, rb_width;
  uint32_t flags;
  int width;
  char *cmark;
  size_t len;

  rb_scan_args(argc, argv, "11", &rb_options, &rb_width);
  width = get_width(rb_width);
  flags = translate_options(rb_options, STAGE_COMMONMARK);

  check_status(comrak_render_commonmark(get_node(self), flags, (size_t)width,
                                        &cmark, &len),
               "rendering CommonMark");

  return adopt_string(cmark, len, rb_ascii8bit_encoding());
}

/* Internal: Convert the node to a plain textstring.
 *
 * comrak has no plain text renderer.
 */
static VALUE rb_render_plaintext(int argc, VALUE *argv, VALUE self) {
  VALUE rb_options, rb_width;

  rb_scan_args(argc, argv, "11", &rb_options, &rb_width);
  get_width(rb_width);
  Check_Type(rb_options, T_FIXNUM);

  rb_raise(rb_eNotImpError,
           "plaintext rendering is not supported by the comrak backend");
}

/*
 * Public: Inserts a node as a sibling after the current node.
 *
 * sibling - A sibling {Node} to insert.
 *
 * Returns `true` if successful.
 * Raises NodeError if the node can't be inserted.
 */
static VALUE rb_node_insert_after(VALUE self, VALUE sibling) {
  return link_nodes(self, sibling, comrak_node_insert_after,
                    "could not insert after");
}

/*
 * Public: Inserts a node as the first child of the current node.
 *
 * child - A child {Node} to insert.
 *
 * Returns `true` if successful.
 * Raises NodeError if the node can't be inserted.
 */
static VALUE rb_node_prepend_child(VALUE self, VALUE child) {
  return link_nodes(self, child, comrak_node_prepend_child,
                    "could not prepend child");
}

/*
 * Public: Inserts a node as the last child of the current node.
 *
 * child - A child {Node} to insert.
 *
 * Returns `true` if successful.
 * Raises NodeError if the node can't be inserted.
 */
static VALUE rb_node_append_child(VALUE self, VALUE child) {
  return link_nodes(self, child, comrak_node_append_child,
                    "could not append child");
}

/* Public: Fetches the first child of the current node.
 *
 * Returns a {Node} if a child exists, `nil` otherise.
 */
static VALUE rb_node_last_child(VALUE self) {
  return neighbour(self, comrak_node_last_child(get_node(self)));
}

/* Public: Fetches the parent of the current node.
 *
 * Returns a {Node} if a parent exists, `nil` otherise.
 */
static VALUE rb_node_parent(VALUE self) {
  return neighbour(self, comrak_node_parent(get_node(self)));
}

/* Public: Fetches the previous sibling of the current node.
 *
 * Returns a {Node} if a parent exists, `nil` otherise.
 */
static VALUE rb_node_previous(VALUE self) {
  return neighbour(self, comrak_node_previous(get_node(self)));
}

/*
 * Public: Gets the URL of the current node (must be a `:link` or `:image`).
 *
 * Returns a {String}.
 * Raises a NodeError if the URL can't be retrieved.
 */
static VALUE rb_node_get_url(VALUE self) {
  return get_binary_string(self, comrak_node_get_url, "could not get url");
}

/*
 * Public: Sets the URL of the current node (must be a `:link` or `:image`).
 *
 * url - A {String} representing the new URL
 *
 * Raises a NodeError if the URL can't be set.
 */
static VALUE rb_node_set_url(VALUE self, VALUE url) {
  return set_string(self, url, comrak_node_set_url, "could not set url");
}

/*
 * Public: Gets the title of the current node (must be a `:link` or `:image`).
 *
 * Returns a {String}.
 * Raises a NodeError if the title can't be retrieved.
 */
static VALUE rb_node_get_title(VALUE self) {
  return get_binary_string(self, comrak_node_get_title, "could not get title");
}

/*
 * Public: Sets the title of the current node (must be a `:link` or `:image`).
 *
 * title - A {String} representing the new title
 *
 * Raises a NodeError if the title can't be set.
 */
static VALUE rb_node_set_title(VALUE self, VALUE title) {
  return set_string(self, title, comrak_node_set_title, "could not set title");
}

/*
 * Public: Gets the header level of the current node (must be a `:header`).
 *
 * Returns a {Number} representing the header level.
 * Raises a NodeError if the header level can't be retrieved.
 */
static VALUE rb_node_get_header_level(VALUE self) {
  int header_level;

  if (!comrak_node_get_heading_level(get_node(self), &header_level))
    rb_raise(rb_eNodeError, "could not get header_level");

  return INT2NUM(header_level);
}

/*
 * Public: Sets the header level of the current node (must be a `:header`).
 *
 * level - A {Number} representing the new header level
 *
 * Raises a NodeError if the header level can't be set.
 */
static VALUE rb_node_set_header_level(VALUE self, VALUE level) {
  Check_Type(level, T_FIXNUM);

  if (!comrak_node_set_heading_level(get_node(self), FIX2INT(level)))
    rb_raise(rb_eNodeError, "could not set header_level");

  return Qnil;
}

/*
 * Public: Gets the list type of the current node (must be a `:list`).
 *
 * Returns a {Symbol}.
 * Raises a NodeError if the title can't be retrieved.
 */
static VALUE rb_node_get_list_type(VALUE self) {
  comrak_list_type list_type;

  if (!comrak_node_get_list_type(get_node(self), &list_type))
    rb_raise(rb_eNodeError, "could not get list_type");

  return list_type == COMRAK_LIST_ORDERED ? sym_ordered_list : sym_bullet_list;
}

/*
 * Public: Sets the list type of the current node (must be a `:list`).
 *
 * level - A {Symbol} representing the new list type
 *
 * Raises a NodeError if the list type can't be set.
 */
static VALUE rb_node_set_list_type(VALUE self, VALUE list_type) {
  comrak_list_type type;
  Check_Type(list_type, T_SYMBOL);

  if (list_type == sym_bullet_list) {
    type = COMRAK_LIST_BULLET;
  } else if (list_type == sym_ordered_list) {
    type = COMRAK_LIST_ORDERED;
  } else {
    rb_raise(rb_eNodeError, "invalid list_type");
  }

  if (!comrak_node_set_list_type(get_node(self), type))
    rb_raise(rb_eNodeError, "could not set list_type");

  return Qnil;
}

/*
 * Public: Gets the starting number the current node (must be an
 * `:ordered_list`).
 *
 * Returns a {Number} representing the starting number.
 * Raises a NodeError if the starting number can't be retrieved.
 */
static VALUE rb_node_get_list_start(VALUE self) {
  comrak_node *node = get_node(self);
  comrak_list_type list_type;
  size_t start;

  /* cmark-gfm's numbering: 0 for no list, then bullet and ordered. */
  if (!comrak_node_get_list_type(node, &list_type))
    list_type = 0;

  if (list_type != COMRAK_LIST_ORDERED ||
      !comrak_node_get_list_start(node, &start)) {
    rb_raise(rb_eNodeError, "can't get list_start for non-ordered list %d",
             (int)list_type);
  }

  return SIZET2NUM(start);
}

/*
 * Public: Sets the starting number of the current node (must be an
 * `:ordered_list`).
 *
 * level - A {Number} representing the new starting number
 *
 * Raises a NodeError if the starting number can't be set.
 */
static VALUE rb_node_set_list_start(VALUE self, VALUE start) {
  int s;
  Check_Type(start, T_FIXNUM);

  s = FIX2INT(start);
  if (s < 0 || !comrak_node_set_list_start(get_node(self), (size_t)s))
    rb_raise(rb_eNodeError, "could not set list_start");

  return Qnil;
}

/*
 * Public: Gets the tight status the current node (must be a `:list`).
 *
 * Returns a `true` if the list is tight, `false` otherwise.
 * Raises a NodeError if the starting number can't be retrieved.
 */
static VALUE rb_node_get_list_tight(VALUE self) {
  bool tight;

  if (!comrak_node_get_list_tight(get_node(self), &tight))
    rb_raise(rb_eNodeError, "can't get list_tight for non-list");

  return tight ? Qtrue : Qfalse;
}

/*
 * Public: Sets the tight status of the current node (must be a `:list`).
 *
 * tight - A {Boolean} representing the new tightness
 *
 * Raises a NodeError if the tightness can't be set.
 */
static VALUE rb_node_set_list_tight(VALUE self, VALUE tight) {
  if (!comrak_node_set_list_tight(get_node(self), RTEST(tight)))
    rb_raise(rb_eNodeError, "could not set list_tight");

  return Qnil;
}

/*
 * Public: Gets the fence info of the current node (must be a `:code_block`).
 *
 * Returns a {String} representing the fence info.
 * Raises a NodeError if the fence info can't be retrieved.
 */
static VALUE rb_node_get_fence_info(VALUE self) {
  return get_binary_string(self, comrak_node_get_fence_info,
                           "could not get fence_info");
}

/*
 * Public: Sets the fence info of the current node (must be a `:code_block`).
 *
 * info - A {String} representing the new fence info
 *
 * Raises a NodeError if the fence info can't be set.
 */
static VALUE rb_node_set_fence_info(VALUE self, VALUE info) {
  return set_string(self, info, comrak_node_set_fence_info,
                    "could not set fence_info");
}

/* Non-task-list nodes read as unchecked, as they did. */
static bool task_checked(VALUE self) {
  bool checked;
  return comrak_node_get_task_checked(get_node(self), &checked) && checked;
}

static VALUE rb_node_get_tasklist_item_checked(VALUE self) {
  return task_checked(self) ? Qtrue : Qfalse;
}

/*
 * Public: Sets the checkbox state of the current node (must be a `:tasklist`).
 *
 * item_checked - A {Boolean} representing the new checkbox state
 *
 * Returns a {Boolean} representing the new checkbox state.
 * Raises a NodeError if the checkbox state can't be set.
 */
static VALUE rb_node_set_tasklist_item_checked(VALUE self, VALUE item_checked) {
  bool checked = RTEST(item_checked);

  if (!comrak_node_set_task_checked(get_node(self), checked))
    rb_raise(rb_eNodeError, "could not set tasklist_item_checked");

  return checked ? Qtrue : Qfalse;
}

// TODO: remove this, superseded by the above method
static VALUE rb_node_get_tasklist_state(VALUE self) {
  return rb_str_new2(task_checked(self) ? "checked" : "unchecked");
}

static VALUE rb_node_get_table_alignments(VALUE self) {
  comrak_node *node = get_node(self);
  size_t column_count = comrak_node_get_table_columns(node), i;
  VALUE ary;

  if (!column_count)
    rb_raise(rb_eNodeError, "could not get column_count or alignments");

  ary = rb_ary_new();
  for (i = 0; i < column_count; ++i) {
    switch (comrak_node_get_table_alignment(node, i)) {
    case COMRAK_ALIGN_LEFT:
      rb_ary_push(ary, sym_left);
      break;
    case COMRAK_ALIGN_CENTER:
      rb_ary_push(ary, sym_center);
      break;
    case COMRAK_ALIGN_RIGHT:
      rb_ary_push(ary, sym_right);
      break;
    default:
      rb_ary_push(ary, Qnil);
    }
  }
  return ary;
}

typedef void (*escaper)(const char *, size_t, char **, size_t *);

/* Returns `rb_text` itself if nothing needed escaping, as houdini did. */
static VALUE escape(VALUE self, VALUE rb_text, escaper esc) {
  char *result;
  size_t len;
  Check_Type(rb_text, T_STRING);

  unwrap(self);

  esc(RSTRING_PTR(rb_text), RSTRING_LEN(rb_text), &result, &len);
  if (len == (size_t)RSTRING_LEN(rb_text) &&
      memcmp(result, RSTRING_PTR(rb_text), len) == 0) {
    comrak_string_free(result, len);
    return rb_text;
  }

  return adopt_string(result, len, rb_enc_get(rb_text));
}

/* Internal: Escapes href URLs safely. */
static VALUE rb_html_escape_href(VALUE self, VALUE rb_text) {
  return escape(self, rb_text, comrak_escape_href);
}

/* Internal: Escapes HTML content safely. */
static VALUE rb_html_escape_html(VALUE self, VALUE rb_text) {
  return escape(self, rb_text, comrak_escape_html);
}

VALUE rb_extensions(VALUE self) {
  VALUE ary = rb_ary_new();
  size_t i;

  for (i = 0; i < ARRAY_LEN(extensions); ++i) {
    if (extensions[i].flag)
      rb_ary_push(ary, rb_str_new2(extensions[i].name));
  }

  return ary;
}

__attribute__((visibility("default"))) void Init_commonmarker() {
  VALUE module;
  sym_document = ID2SYM(rb_intern("document"));
  sym_blockquote = ID2SYM(rb_intern("blockquote"));
  sym_list = ID2SYM(rb_intern("list"));
  sym_list_item = ID2SYM(rb_intern("list_item"));
  sym_code_block = ID2SYM(rb_intern("code_block"));
  sym_html = ID2SYM(rb_intern("html"));
  sym_paragraph = ID2SYM(rb_intern("paragraph"));
  sym_header = ID2SYM(rb_intern("header"));
  sym_hrule = ID2SYM(rb_intern("hrule"));
  sym_text = ID2SYM(rb_intern("text"));
  sym_softbreak = ID2SYM(rb_intern("softbreak"));
  sym_linebreak = ID2SYM(rb_intern("linebreak"));
  sym_code = ID2SYM(rb_intern("code"));
  sym_inline_html = ID2SYM(rb_intern("inline_html"));
  sym_emph = ID2SYM(rb_intern("emph"));
  sym_strong = ID2SYM(rb_intern("strong"));
  sym_link = ID2SYM(rb_intern("link"));
  sym_image = ID2SYM(rb_intern("image"));
  sym_footnote_reference = ID2SYM(rb_intern("footnote_reference"));
  sym_footnote_definition = ID2SYM(rb_intern("footnote_definition"));
  sym_table = ID2SYM(rb_intern("table"));
  sym_table_header = ID2SYM(rb_intern("table_header"));
  sym_table_row = ID2SYM(rb_intern("table_row"));
  sym_table_cell = ID2SYM(rb_intern("table_cell"));
  sym_strikethrough = ID2SYM(rb_intern("strikethrough"));

  sym_bullet_list = ID2SYM(rb_intern("bullet_list"));
  sym_ordered_list = ID2SYM(rb_intern("ordered_list"));

  sym_left = ID2SYM(rb_intern("left"));
  sym_right = ID2SYM(rb_intern("right"));
  sym_center = ID2SYM(rb_intern("center"));

  node_cache = st_init_numtable();

  module = rb_define_module("CommonMarker");
  rb_define_singleton_method(module, "extensions", rb_extensions, 0);
  rb_eNodeError = rb_define_class_under(module, "NodeError", rb_eStandardError);
  rb_cNode = rb_define_class_under(module, "Node", rb_cObject);
  rb_undef_alloc_func(rb_cNode);
  rb_define_singleton_method(rb_cNode, "markdown_to_html", rb_markdown_to_html,
                             3);
  rb_define_singleton_method(rb_cNode, "markdown_to_xml", rb_markdown_to_xml,
                             3);
  rb_define_singleton_method(rb_cNode, "new", rb_node_new, 1);
  rb_define_singleton_method(rb_cNode, "parse_document", rb_parse_document, 4);
  rb_define_method(rb_cNode, "string_content", rb_node_get_string_content, 0);
  rb_define_method(rb_cNode, "string_content=", rb_node_set_string_content, 1);
  rb_define_method(rb_cNode, "type", rb_node_get_type, 0);
  rb_define_method(rb_cNode, "type_string", rb_node_get_type_string, 0);
  rb_define_method(rb_cNode, "sourcepos", rb_node_get_sourcepos, 0);
  rb_define_method(rb_cNode, "delete", rb_node_unlink, 0);
  rb_define_method(rb_cNode, "first_child", rb_node_first_child, 0);
  rb_define_method(rb_cNode, "next", rb_node_next, 0);
  rb_define_method(rb_cNode, "insert_before", rb_node_insert_before, 1);
  rb_define_method(rb_cNode, "_render_html", rb_render_html, 2);
  rb_define_method(rb_cNode, "_render_xml", rb_render_xml, 1);
  rb_define_method(rb_cNode, "_render_commonmark", rb_render_commonmark, -1);
  rb_define_method(rb_cNode, "_render_plaintext", rb_render_plaintext, -1);
  rb_define_method(rb_cNode, "insert_after", rb_node_insert_after, 1);
  rb_define_method(rb_cNode, "prepend_child", rb_node_prepend_child, 1);
  rb_define_method(rb_cNode, "append_child", rb_node_append_child, 1);
  rb_define_method(rb_cNode, "last_child", rb_node_last_child, 0);
  rb_define_method(rb_cNode, "parent", rb_node_parent, 0);
  rb_define_method(rb_cNode, "previous", rb_node_previous, 0);
  rb_define_method(rb_cNode, "url", rb_node_get_url, 0);
  rb_define_method(rb_cNode, "url=", rb_node_set_url, 1);
  rb_define_method(rb_cNode, "title", rb_node_get_title, 0);
  rb_define_method(rb_cNode, "title=", rb_node_set_title, 1);
  rb_define_method(rb_cNode, "header_level", rb_node_get_header_level, 0);
  rb_define_method(rb_cNode, "header_level=", rb_node_set_header_level, 1);
  rb_define_method(rb_cNode, "list_type", rb_node_get_list_type, 0);
  rb_define_method(rb_cNode, "list_type=", rb_node_set_list_type, 1);
  rb_define_method(rb_cNode, "list_start", rb_node_get_list_start, 0);
  rb_define_method(rb_cNode, "list_start=", rb_node_set_list_start, 1);
  rb_define_method(rb_cNode, "list_tight", rb_node_get_list_tight, 0);
  rb_define_method(rb_cNode, "list_tight=", rb_node_set_list_tight, 1);
  rb_define_method(rb_cNode, "fence_info", rb_node_get_fence_info, 0);
  rb_define_method(rb_cNode, "fence_info=", rb_node_set_fence_info, 1);
  rb_define_method(rb_cNode, "table_alignments", rb_node_get_table_alignments, 0);
  rb_define_method(rb_cNode, "tasklist_state", rb_node_get_tasklist_state, 0);
  rb_define_method(rb_cNode, "tasklist_item_checked?", rb_node_get_tasklist_item_checked, 0);
  rb_define_method(rb_cNode, "tasklist_item_checked=", rb_node_set_tasklist_item_checked, 1);

  rb_define_method(rb_cNode, "html_escape_href", rb_html_escape_href, 1);
  rb_define_method(rb_cNode, "html_escape_html", rb_html_escape_html, 1);
}
