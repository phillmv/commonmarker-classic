use std::collections::BTreeMap;
use std::rc::{Rc, Weak};

use super::*;

const HEADER: &str = include_str!("../include/comrak_ffi.h");

/// Every constant the header defines, with the value the Rust side uses.
fn rust_constants() -> BTreeMap<&'static str, i64> {
    macro_rules! table {
        ($($name:ident),* $(,)?) => {
            BTreeMap::from([$((stringify!($name), $name as i64)),*])
        };
    }
    table![
        COMRAK_OK,
        COMRAK_ERR_PANIC,
        COMRAK_ERR_INVALID_FLAGS,
        COMRAK_ERR_RENDER,
        COMRAK_OPT_EXT_STRIKETHROUGH,
        COMRAK_OPT_EXT_TABLE,
        COMRAK_OPT_EXT_AUTOLINK,
        COMRAK_OPT_EXT_TASKLIST,
        COMRAK_OPT_EXT_FOOTNOTES,
        COMRAK_OPT_PARSE_SMART,
        COMRAK_OPT_RENDER_HARDBREAKS,
        COMRAK_OPT_RENDER_GITHUB_PRE_LANG,
        COMRAK_OPT_RENDER_FULL_INFO_STRING,
        COMRAK_OPT_RENDER_UNSAFE,
        COMRAK_OPT_RENDER_SOURCEPOS,
        COMRAK_OPT_RENDER_GFM_QUIRKS,
        COMRAK_NODE_DOCUMENT,
        COMRAK_NODE_BLOCK_QUOTE,
        COMRAK_NODE_LIST,
        COMRAK_NODE_ITEM,
        COMRAK_NODE_CODE_BLOCK,
        COMRAK_NODE_HTML_BLOCK,
        COMRAK_NODE_PARAGRAPH,
        COMRAK_NODE_HEADING,
        COMRAK_NODE_THEMATIC_BREAK,
        COMRAK_NODE_FOOTNOTE_DEFINITION,
        COMRAK_NODE_TABLE,
        COMRAK_NODE_TABLE_ROW,
        COMRAK_NODE_TABLE_CELL,
        COMRAK_NODE_TASK_ITEM,
        COMRAK_NODE_TEXT,
        COMRAK_NODE_SOFTBREAK,
        COMRAK_NODE_LINEBREAK,
        COMRAK_NODE_CODE,
        COMRAK_NODE_HTML_INLINE,
        COMRAK_NODE_EMPH,
        COMRAK_NODE_STRONG,
        COMRAK_NODE_LINK,
        COMRAK_NODE_IMAGE,
        COMRAK_NODE_FOOTNOTE_REFERENCE,
        COMRAK_NODE_STRIKETHROUGH,
        COMRAK_NODE_OTHER,
        COMRAK_LIST_BULLET,
        COMRAK_LIST_ORDERED,
        COMRAK_ALIGN_NONE,
        COMRAK_ALIGN_LEFT,
        COMRAK_ALIGN_CENTER,
        COMRAK_ALIGN_RIGHT,
    ]
}

/// Constants as written in the header: `#define X (1u << n)` and `X = n,`.
fn header_constants() -> BTreeMap<&'static str, i64> {
    let mut found = BTreeMap::new();
    for line in HEADER.lines().map(str::trim) {
        if let Some(rest) = line.strip_prefix("#define ") {
            // The include guard has no value.
            let Some((name, value)) = rest.split_once(' ') else {
                continue;
            };
            let shift = value
                .strip_prefix("(1u << ")
                .and_then(|v| v.strip_suffix(')'))
                .unwrap_or_else(|| panic!("unexpected #define value: {value}"));
            found.insert(name, 1i64 << shift.parse::<u32>().unwrap());
        } else if line.starts_with("COMRAK_") && line.contains(" = ") {
            let (name, value) = line.split_once(" = ").unwrap();
            let value = value.trim_end_matches(',');
            found.insert(name, value.parse().unwrap());
        }
    }
    found
}

#[test]
fn header_matches_rust_constants() {
    assert_eq!(header_constants(), rust_constants());
}

#[test]
fn every_rust_flag_is_known() {
    let flags: u32 = rust_constants()
        .iter()
        .filter(|(name, _)| name.starts_with("COMRAK_OPT_"))
        .map(|(_, &v)| v as u32)
        .fold(0, |a, b| a | b);
    assert_eq!(flags, KNOWN_FLAGS);
}

// Helpers over the C ABI, so the tests exercise exactly what C calls.

fn parse(md: &str) -> (*mut Group, *mut Node) {
    let (mut g, mut n) = (ptr::null_mut(), ptr::null_mut());
    let status = unsafe { comrak_parse_document(md.as_ptr().cast(), md.len(), 0, &mut g, &mut n) };
    assert_eq!(status, COMRAK_OK);
    (g, n)
}

fn new_node(t: c_int) -> (*mut Group, *mut Node) {
    let mut g = ptr::null_mut();
    let n = unsafe { comrak_node_new(t, &mut g) };
    assert!(!n.is_null());
    (g, n)
}

fn html(n: *const Node) -> String {
    let (mut p, mut len) = (ptr::null_mut(), 0);
    assert_eq!(
        unsafe { comrak_render_html(n, 0, &mut p, &mut len) },
        COMRAK_OK
    );
    let s = unsafe { std::str::from_utf8(std::slice::from_raw_parts(p.cast(), len)) }
        .unwrap()
        .to_owned();
    unsafe { comrak_string_free(p, len) };
    s
}

fn set_text(n: *mut Node, s: &str) {
    assert!(unsafe { comrak_node_set_literal(n, s.as_ptr().cast(), s.len()) });
}

/// A weak handle on the root of `g`, to observe when its arenas are freed.
fn watch(g: *mut Group) -> Weak<Group> {
    Rc::downgrade(&unsafe { group(g) }.root())
}

#[test]
fn a_group_is_freed_when_its_last_reference_is_released() {
    let (g, _) = parse("hello");
    let extra = unsafe { comrak_group_retain(g) };
    let watched = watch(g);
    unsafe { comrak_group_release(g) };
    assert!(watched.upgrade().is_some());
    unsafe { comrak_group_release(extra) };
    assert!(watched.upgrade().is_none());
}

#[test]
fn linking_keeps_both_arenas_alive_through_either_reference() {
    for release_doc_first in [true, false] {
        let (doc_group, doc) = parse("para\n");
        let (text_group, text) = new_node(COMRAK_NODE_TEXT);
        set_text(text, " added");
        let para = unsafe { comrak_node_first_child(doc) };
        assert!(unsafe { comrak_node_append_child(para, doc_group, text, text_group) });

        let doc_arenas = watch(doc_group);
        let text_arenas = watch(text_group);
        let (first, second) = if release_doc_first {
            (doc_group, text_group)
        } else {
            (text_group, doc_group)
        };
        unsafe { comrak_group_release(first) };
        assert!(doc_arenas.upgrade().is_some() && text_arenas.upgrade().is_some());
        assert_eq!(html(doc), "<p>para added</p>\n");
        unsafe { comrak_group_release(second) };
        assert!(doc_arenas.upgrade().is_none() && text_arenas.upgrade().is_none());
    }
}

#[test]
fn a_failed_link_does_not_merge_groups() {
    let (doc_group, doc) = parse("para\n");
    let (other_group, other) = new_node(COMRAK_NODE_DOCUMENT);
    assert!(!unsafe { comrak_node_append_child(doc, doc_group, other, other_group) });
    let doc_arenas = watch(doc_group);
    unsafe { comrak_group_release(doc_group) };
    assert!(doc_arenas.upgrade().is_none());
    unsafe { comrak_group_release(other_group) };
}

#[test]
fn a_node_cannot_become_its_own_ancestor_or_sibling() {
    let (g, doc) = parse("para *emph*\n");
    let para = unsafe { comrak_node_first_child(doc) };
    let emph = unsafe { comrak_node_last_child(para) };
    unsafe {
        assert!(!comrak_node_append_child(emph, g, para, g));
        assert!(!comrak_node_append_child(para, g, para, g));
        assert!(!comrak_node_insert_before(emph, g, emph, g));
        assert!(!comrak_node_insert_after(emph, g, para, g));
        assert!(!comrak_node_insert_after(doc, g, emph, g));
    }
    assert_eq!(html(doc), "<p>para <em>emph</em></p>\n");
    unsafe { comrak_group_release(g) };

    // Types alone would allow these; only ancestry forbids them.
    let (g, doc) = parse("> > quote\n");
    let outer = unsafe { comrak_node_first_child(doc) };
    let inner = unsafe { comrak_node_first_child(outer) };
    unsafe {
        assert!(!comrak_node_append_child(inner, g, outer, g));
        assert!(!comrak_node_prepend_child(inner, g, outer, g));
        assert!(!comrak_node_insert_before(inner, g, outer, g));
        assert!(!comrak_node_append_child(outer, g, outer, g));
    }
    assert_eq!(
        html(doc),
        "<blockquote>\n<blockquote>\n<p>quote</p>\n</blockquote>\n</blockquote>\n"
    );
    unsafe { comrak_group_release(g) };
}

#[test]
fn unknown_flags_are_rejected() {
    let (mut g, mut n) = (ptr::null_mut(), ptr::null_mut());
    let status = unsafe { comrak_parse_document(ptr::null(), 0, 1 << 31, &mut g, &mut n) };
    assert_eq!(status, COMRAK_ERR_INVALID_FLAGS);
}

#[test]
fn invalid_utf8_is_replaced() {
    let bytes = b"a\xffb";
    let (mut g, mut n) = (ptr::null_mut(), ptr::null_mut());
    let status = unsafe { comrak_parse_document(bytes.as_ptr().cast(), 3, 0, &mut g, &mut n) };
    assert_eq!(status, COMRAK_OK);
    assert_eq!(html(n), "<p>a\u{fffd}b</p>\n");
    unsafe { comrak_group_release(g) };
}
