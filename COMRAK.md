# The comrak backend

On this branch, commonmarker's C extension no longer links libcmark-gfm.
It talks to [comrak](https://github.com/kivikakk/comrak), a Rust port of
cmark-gfm, through a small purpose-built C ABI. The Ruby API is meant to be
the same. This document covers how that works, what is not supported, and
where behaviour differs.

This is a prototype. The cmark-gfm sources still sit in `ext/commonmarker/`,
but they are no longer compiled or packaged.

## Layout

```
lib/                          Ruby API (unchanged)
ext/commonmarker/
  commonmarker.c              Ruby bindings; translates cmark-gfm semantics
  extconf.rb                  builds comrak_ffi with cargo, links it statically
  comrak_ffi/                 Rust crate exposing a C ABI over comrak
    include/comrak_ffi.h        the contract, including the memory model
    src/lib.rs, src/tests.rs
  comrak/                     vendored comrak (see comrak/VENDORED)
script/vendor-comrak          re-vendors comrak from a checkout
```

`comrak_ffi` only exposes comrak's model. The cmark-gfm compatibility layer
lives in `commonmarker.c`: option bits, node type names and symbols, which
accessors raise, and string encodings. `cargo test` in `comrak_ffi`
cross-checks every constant in the header against the Rust definitions.

## Building

You need a Rust toolchain: cargo, and rustc 1.85 or newer. `extconf.rb` runs
`cargo rustc --release --locked`, and the generated Makefile reruns it, so
edits to the Rust sources are picked up by `rake compile`. Set `$CARGO` to
use a different cargo.

comrak's own dependencies (typed-arena, phf, and so on) are not vendored yet,
so building fetches them from crates.io.

To update comrak, run `script/vendor-comrak PATH_TO_COMRAK_CHECKOUT`. It
refuses to run on a dirty checkout and records the upstream commit.

## Memory model

comrak allocates nodes in arenas that are only freed as a whole.
`comrak_ffi` wraps these arenas in reference-counted *groups*. Every Ruby
`Node` holds a reference to its group. Linking nodes from different groups
(for example, appending a node from one document into another) merges the
groups, so a tree can never outlive an arena that one of its nodes lives in.

Ruby `Node` objects are cached per node, so navigating to the same node twice
returns the same object, as it did with cmark-gfm. Wrappers mark their
parent's and children's wrappers, which keeps that cache valid. The cache
also survives GC compaction; see `test_cached_nodes_survive_compaction`. The
cmark-gfm binding did not: it segfaults under
`GC.verify_compaction_references` when a document's root is only held from
the heap.

One consequence: a deleted node's memory is reclaimed only once nothing
reachable refers to any node in its group. cmark-gfm freed it as soon as the
node's Ruby object was collected.

## Not supported

These raise `NotImplementedError`. Note that this is a `ScriptError`, not a
`StandardError`, so a bare `rescue` does not catch it.

| Feature | Why |
| --- | --- |
| `:tagfilter` extension (parse or render) | Removed from comrak in kivikakk/comrak#845. `CommonMarker.extensions` no longer lists it. `HtmlRenderer` still filters tags itself, in Ruby. |
| `Node#to_plaintext`, `commonmarker --to=plaintext` | comrak has no plain text renderer. |
| `:LIBERAL_HTML_TAG`, `:STRIKETHROUGH_DOUBLE_TILDE` when parsing | No comrak equivalent. |
| `:NOBREAKS` when rendering HTML or CommonMark | No comrak equivalent. |
| `:TABLE_PREFER_STYLE_ATTRIBUTES` when rendering HTML | No comrak equivalent. |
| `Node#string_content=` on a `:footnote_reference` | cmark-gfm stored the footnote's number there; comrak has no such field. |

An option only raises at a stage where cmark-gfm actually read it. Anywhere
cmark-gfm ignored it, it is still ignored. For example, `to_xml(:NOBREAKS)`
works, as do parse-only options like `:SMART` passed to `to_html`. comrak
always tracks source positions and always validates UTF-8, so `:SOURCEPOS` at
parse time and `:VALIDATE_UTF8` are accepted and need no translation.

## Divergences

These were found by running the existing test suite, and by diffing a
behavioural probe across both backends. The probe covers every Node method,
option, and error path. On the probe, setters, tree manipulation, escaping,
error messages, and string encodings match cmark-gfm exactly.

### Nondeterministic HTML attribute order (comrak bug)

comrak collects `<pre>` and `<code>` attributes in a `std::collections::HashMap`,
so when a code block has more than one attribute, their order varies from
process to process. That happens with `:FULL_INFO_STRING`, or with
`:GITHUB_PRE_LANG` plus `:SOURCEPOS`. Across 20 runs of the same input, each
order appeared about half the time. As a result `test_full_info_string` is
flaky. This should be fixed upstream by keeping insertion order.

### Source positions

- **HTML `:SOURCEPOS`**: comrak also puts `data-sourcepos` on inline elements
  (`em`, `strong`, `code`, `a`, `img`, `br`, …). On `<ol start>`, `<th align>`
  and `<td align>` it writes `data-sourcepos` first. This accounts for all 285
  failing `TestSpec#test_sourcepos_example_*` tests, which compare the C
  renderer against the Ruby `HtmlRenderer`. After normalising those two
  things, block positions agree on every spec example.
- **Values**: comrak's positions are generally more accurate. Blocks followed
  by a blank line end where their content ends, rather than at column 0 of the
  following line. Softbreaks and linebreaks have positions, where cmark-gfm
  reported zeros. Code spans include their backticks. Text after a hard line
  break is on the right line, whereas cmark-gfm put it on the previous one.
  (`test_attributes`, `test_xml`)

### XML

- Table header rows are `<table_row>` rather than `<table_header>`.
- Task items are `<taskitem completed="…">`, and their list has
  `tasklist="true"`. cmark-gfm used `<tasklist completed="…">`.
- Footnote references are `<footnote_reference label="1" />`. cmark-gfm
  emitted `<<unknown> />`, which is not well-formed XML.

### CommonMark output

- Hard line breaks are written as `\` plus a newline, rather than two spaces.
- List markers are followed by one space (`1. one`, `- a`). cmark-gfm wrote
  `1.  one` and `  - a`.
- Fence info strings follow the fence directly (`` ```ruby ``), with no space.
- A standalone empty node renders as `""` rather than `"\n"`.
- A plain item after task items is no longer wrongly nested under them.
- Wrapping with `:SMART` breaks lines in slightly different places.

### Strings and encodings

- Invalid UTF-8 is always replaced with U+FFFD. Without `:VALIDATE_UTF8`,
  cmark-gfm passed the bytes through.
- `html_escape_html` and `html_escape_href` replace invalid UTF-8 the same way.
  The escaping rules themselves match.
- Some cmark-gfm quirks are kept deliberately, as candidates for later
  cleanup. `url`, `title`, `fence_info`, `type_string`, and the `_render_*`
  results are BINARY strings. Footnote nodes' `type_string` is `"<unknown>"`.
  A footnote reference's `string_content` is its number.

### Safety

- Inserting a node as its own sibling, or under itself or one of its
  descendants, raises `NodeError`. cmark-gfm has these checks disabled, so it
  would corrupt the tree.
- A comrak panic surfaces as `NodeError` ("comrak panicked while …"), rather
  than aborting the process, and the panic message is printed to stderr. For
  example, comrak panics rendering a detached `Node.new(:list_item)` as
  CommonMark. cmark-gfm segfaulted rendering `Node.new(:paragraph)` to HTML,
  and on detached footnote references. comrak renders those fine.
- `Node.parse_document`'s length argument is clamped to the string's length.
- Type errors for non-Node arguments now say `expected CommonMarker::Node`.

## Test suite status

`rake test:unit` on Ruby 4.0.7: 2359 runs, 288 or 289 failures (depending on
the flaky test below), and 11 errors. There
are 13 new tests, in `test_comrak.rb` and `test_gc.rb`. The other 11 new runs
come from `test_pathological_inputs.rb`, which defines its tests on
`Minitest::Test`, so every test class, including `TestComrak`, inherits them.

| Cause | Tests |
| --- | --- |
| HTML `:SOURCEPOS` on inlines / attribute order | 285 `TestSpec#test_sourcepos_example_*` |
| `:tagfilter` not implemented | spec example 652 (×3), `test_comments_are_kept_as_expected`, `test_multiple_tables` |
| Plain text not implemented | `test_plaintext`, `TestPlaintext#test_to_commonmark`, `test_understands_plaintext_format` |
| Unsupported options | `test_uses_specified_extensions`, `test_extensions_with_renderers`, `test_accept_every_option`, `test_table_prefer_style_attributes` |
| Source position values | `test_attributes#test_sourcepos`, `test_xml#test_to_xml` (also `<table_row>`) |
| Nondeterministic attribute order (flaky) | `test_full_info_string` |

Every other spec example renders identically through `to_html` and through
`HtmlRenderer`.

`test_spec.rb` now parses each example inside its test, so that an example
that raises (`:tagfilter`) fails on its own instead of aborting the file.
