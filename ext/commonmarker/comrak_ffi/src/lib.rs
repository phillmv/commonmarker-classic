//! A small C ABI over comrak. See `include/comrak_ffi.h` for the contract;
//! this file implements it.
//!
//! Safety: every `extern "C"` function trusts its pointer arguments to be
//! ones this library handed out and that are still alive per the header's
//! memory model. Callers serialise all calls (the Ruby extension holds the
//! GVL), so `Rc` and `RefCell` are sufficient.

// The safety contract is the same for every function and is documented once,
// above and in the header, rather than on each function.
#![allow(clippy::missing_safety_doc)]

use std::borrow::Cow;
use std::cell::RefCell;
use std::ffi::{c_char, c_int};
use std::mem::ManuallyDrop;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::rc::Rc;

use comrak::nodes::{
    AstNode, ListType, NodeCode, NodeCodeBlock, NodeFootnoteDefinition, NodeHeading, NodeHtmlBlock,
    NodeLink, NodeList, NodeValue, TableAlignment,
};
use comrak::{Arena, Options};

type Node = AstNode<'static>;

// Status codes.
pub const COMRAK_OK: c_int = 0;
pub const COMRAK_ERR_PANIC: c_int = 1;
pub const COMRAK_ERR_INVALID_FLAGS: c_int = 2;
pub const COMRAK_ERR_RENDER: c_int = 3;

// Option flags.
pub const COMRAK_OPT_EXT_STRIKETHROUGH: u32 = 1 << 0;
pub const COMRAK_OPT_EXT_TABLE: u32 = 1 << 1;
pub const COMRAK_OPT_EXT_AUTOLINK: u32 = 1 << 2;
pub const COMRAK_OPT_EXT_TASKLIST: u32 = 1 << 3;
pub const COMRAK_OPT_EXT_FOOTNOTES: u32 = 1 << 4;
pub const COMRAK_OPT_PARSE_SMART: u32 = 1 << 8;
pub const COMRAK_OPT_RENDER_HARDBREAKS: u32 = 1 << 16;
pub const COMRAK_OPT_RENDER_GITHUB_PRE_LANG: u32 = 1 << 17;
pub const COMRAK_OPT_RENDER_FULL_INFO_STRING: u32 = 1 << 18;
pub const COMRAK_OPT_RENDER_UNSAFE: u32 = 1 << 19;
pub const COMRAK_OPT_RENDER_SOURCEPOS: u32 = 1 << 20;
pub const COMRAK_OPT_RENDER_GFM_QUIRKS: u32 = 1 << 21;

const KNOWN_FLAGS: u32 = COMRAK_OPT_EXT_STRIKETHROUGH
    | COMRAK_OPT_EXT_TABLE
    | COMRAK_OPT_EXT_AUTOLINK
    | COMRAK_OPT_EXT_TASKLIST
    | COMRAK_OPT_EXT_FOOTNOTES
    | COMRAK_OPT_PARSE_SMART
    | COMRAK_OPT_RENDER_HARDBREAKS
    | COMRAK_OPT_RENDER_GITHUB_PRE_LANG
    | COMRAK_OPT_RENDER_FULL_INFO_STRING
    | COMRAK_OPT_RENDER_UNSAFE
    | COMRAK_OPT_RENDER_SOURCEPOS
    | COMRAK_OPT_RENDER_GFM_QUIRKS;

// Node types.
pub const COMRAK_NODE_DOCUMENT: c_int = 1;
pub const COMRAK_NODE_BLOCK_QUOTE: c_int = 2;
pub const COMRAK_NODE_LIST: c_int = 3;
pub const COMRAK_NODE_ITEM: c_int = 4;
pub const COMRAK_NODE_CODE_BLOCK: c_int = 5;
pub const COMRAK_NODE_HTML_BLOCK: c_int = 6;
pub const COMRAK_NODE_PARAGRAPH: c_int = 7;
pub const COMRAK_NODE_HEADING: c_int = 8;
pub const COMRAK_NODE_THEMATIC_BREAK: c_int = 9;
pub const COMRAK_NODE_FOOTNOTE_DEFINITION: c_int = 10;
pub const COMRAK_NODE_TABLE: c_int = 11;
pub const COMRAK_NODE_TABLE_ROW: c_int = 12;
pub const COMRAK_NODE_TABLE_CELL: c_int = 13;
pub const COMRAK_NODE_TASK_ITEM: c_int = 14;
pub const COMRAK_NODE_TEXT: c_int = 15;
pub const COMRAK_NODE_SOFTBREAK: c_int = 16;
pub const COMRAK_NODE_LINEBREAK: c_int = 17;
pub const COMRAK_NODE_CODE: c_int = 18;
pub const COMRAK_NODE_HTML_INLINE: c_int = 19;
pub const COMRAK_NODE_EMPH: c_int = 20;
pub const COMRAK_NODE_STRONG: c_int = 21;
pub const COMRAK_NODE_LINK: c_int = 22;
pub const COMRAK_NODE_IMAGE: c_int = 23;
pub const COMRAK_NODE_FOOTNOTE_REFERENCE: c_int = 24;
pub const COMRAK_NODE_STRIKETHROUGH: c_int = 25;
pub const COMRAK_NODE_OTHER: c_int = 255;

pub const COMRAK_LIST_BULLET: c_int = 1;
pub const COMRAK_LIST_ORDERED: c_int = 2;

pub const COMRAK_ALIGN_NONE: c_int = 0;
pub const COMRAK_ALIGN_LEFT: c_int = 1;
pub const COMRAK_ALIGN_CENTER: c_int = 2;
pub const COMRAK_ALIGN_RIGHT: c_int = 3;

// ---------------------------------------------------------------------------
// Groups
//
// A union-find forest of arena owners. Only a root group owns arenas; a
// merged group forwards to the group that absorbed its arenas, and that
// forward reference keeps the absorbing group alive. There are never cycles.

pub struct Group {
    forward: RefCell<Option<Rc<Group>>>,
    // Boxed so that an arena never moves: `Group::new` hands out a reference
    // to the arena itself, which must survive the Vec growing or merging.
    #[allow(clippy::vec_box)]
    arenas: RefCell<Vec<Box<Arena<'static>>>>,
}

impl Group {
    /// A new group with one empty arena, plus a reference to that arena
    /// which is valid for as long as the group (or its successor) lives.
    fn new() -> (Rc<Group>, &'static Arena<'static>) {
        let arena = Box::new(Arena::new());
        // Moving the Box into the Vec does not move the arena itself.
        let arena_ref: &'static Arena<'static> = unsafe { &*ptr::from_ref(&*arena) };
        let group = Rc::new(Group {
            forward: RefCell::new(None),
            arenas: RefCell::new(vec![arena]),
        });
        (group, arena_ref)
    }

    fn root(self: &Rc<Group>) -> Rc<Group> {
        let mut cur = self.clone();
        loop {
            let next = cur.forward.borrow().clone();
            match next {
                Some(next) => cur = next,
                None => return cur,
            }
        }
    }

    fn merge(a: &Rc<Group>, b: &Rc<Group>) {
        let (a, b) = (a.root(), b.root());
        if Rc::ptr_eq(&a, &b) {
            return;
        }
        // Union by arena count keeps forwarding chains short.
        let (big, small) = if a.arenas.borrow().len() >= b.arenas.borrow().len() {
            (a, b)
        } else {
            (b, a)
        };
        let moved = std::mem::take(&mut *small.arenas.borrow_mut());
        big.arenas.borrow_mut().extend(moved);
        *small.forward.borrow_mut() = Some(big);
    }
}

unsafe fn group(g: *mut Group) -> ManuallyDrop<Rc<Group>> {
    ManuallyDrop::new(unsafe { Rc::from_raw(g) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_group_retain(g: *mut Group) -> *mut Group {
    let g = unsafe { group(g) };
    Rc::into_raw(g.root()).cast_mut()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_group_release(g: *mut Group) {
    drop(unsafe { Rc::from_raw(g) });
}

// ---------------------------------------------------------------------------
// Helpers

unsafe fn node(n: *const Node) -> &'static Node {
    unsafe { &*n }
}

fn raw(n: Option<&'static Node>) -> *mut Node {
    n.map_or(ptr::null_mut(), |n| ptr::from_ref(n).cast_mut())
}

unsafe fn input<'x>(p: *const c_char, len: usize) -> Cow<'x, str> {
    if len == 0 {
        return Cow::Borrowed("");
    }
    let bytes = unsafe { std::slice::from_raw_parts(p.cast::<u8>(), len) };
    String::from_utf8_lossy(bytes)
}

unsafe fn lend(s: &str, p: *mut *const c_char, len: *mut usize) -> bool {
    unsafe {
        *p = s.as_ptr().cast();
        *len = s.len();
    }
    true
}

unsafe fn give(s: String, out: *mut *mut c_char, out_len: *mut usize) {
    let bytes = s.into_bytes().into_boxed_slice();
    let len = bytes.len();
    unsafe {
        *out = Box::into_raw(bytes).cast::<c_char>();
        *out_len = len;
    }
}

fn options(flags: u32) -> Option<Options<'static>> {
    if flags & !KNOWN_FLAGS != 0 {
        return None;
    }
    let on = |f: u32| flags & f != 0;
    let mut o = Options::default();
    o.extension.strikethrough = on(COMRAK_OPT_EXT_STRIKETHROUGH);
    o.extension.table = on(COMRAK_OPT_EXT_TABLE);
    o.extension.autolink = on(COMRAK_OPT_EXT_AUTOLINK);
    o.extension.tasklist = on(COMRAK_OPT_EXT_TASKLIST);
    o.extension.footnotes = on(COMRAK_OPT_EXT_FOOTNOTES);
    o.parse.smart = on(COMRAK_OPT_PARSE_SMART);
    o.render.hardbreaks = on(COMRAK_OPT_RENDER_HARDBREAKS);
    o.render.github_pre_lang = on(COMRAK_OPT_RENDER_GITHUB_PRE_LANG);
    o.render.full_info_string = on(COMRAK_OPT_RENDER_FULL_INFO_STRING);
    o.render.r#unsafe = on(COMRAK_OPT_RENDER_UNSAFE);
    o.render.sourcepos = on(COMRAK_OPT_RENDER_SOURCEPOS);
    o.render.gfm_quirks = on(COMRAK_OPT_RENDER_GFM_QUIRKS);
    Some(o)
}

// ---------------------------------------------------------------------------
// Parsing and construction

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_parse_document(
    text: *const c_char,
    len: usize,
    flags: u32,
    group_out: *mut *mut Group,
    root_out: *mut *mut Node,
) -> c_int {
    let Some(opts) = options(flags) else {
        return COMRAK_ERR_INVALID_FLAGS;
    };
    let text = unsafe { input(text, len) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let (group, arena) = Group::new();
        let root = comrak::parse_document(arena, &text, &opts);
        (group, root)
    }));
    match result {
        Ok((group, root)) => {
            unsafe {
                *group_out = Rc::into_raw(group).cast_mut();
                *root_out = raw(Some(root));
            }
            COMRAK_OK
        }
        Err(_) => COMRAK_ERR_PANIC,
    }
}

fn new_value(t: c_int) -> Option<NodeValue> {
    Some(match t {
        COMRAK_NODE_DOCUMENT => NodeValue::Document,
        COMRAK_NODE_BLOCK_QUOTE => NodeValue::BlockQuote,
        COMRAK_NODE_LIST => NodeValue::List(NodeList {
            bullet_char: b'-',
            ..NodeList::default()
        }),
        COMRAK_NODE_ITEM => NodeValue::Item(NodeList {
            bullet_char: b'-',
            ..NodeList::default()
        }),
        COMRAK_NODE_CODE_BLOCK => NodeValue::CodeBlock(Box::new(NodeCodeBlock {
            closed: true,
            ..NodeCodeBlock::default()
        })),
        COMRAK_NODE_HTML_BLOCK => NodeValue::HtmlBlock(NodeHtmlBlock::default()),
        COMRAK_NODE_PARAGRAPH => NodeValue::Paragraph,
        COMRAK_NODE_HEADING => NodeValue::Heading(NodeHeading {
            level: 1,
            ..NodeHeading::default()
        }),
        COMRAK_NODE_THEMATIC_BREAK => NodeValue::ThematicBreak,
        COMRAK_NODE_FOOTNOTE_DEFINITION => {
            NodeValue::FootnoteDefinition(NodeFootnoteDefinition::default())
        }
        COMRAK_NODE_TEXT => NodeValue::Text(Cow::Borrowed("")),
        COMRAK_NODE_SOFTBREAK => NodeValue::SoftBreak,
        COMRAK_NODE_LINEBREAK => NodeValue::LineBreak,
        COMRAK_NODE_CODE => NodeValue::Code(NodeCode::default()),
        COMRAK_NODE_HTML_INLINE => NodeValue::HtmlInline(String::new()),
        COMRAK_NODE_EMPH => NodeValue::Emph,
        COMRAK_NODE_STRONG => NodeValue::Strong,
        COMRAK_NODE_LINK => NodeValue::Link(Box::default()),
        COMRAK_NODE_IMAGE => NodeValue::Image(Box::default()),
        COMRAK_NODE_FOOTNOTE_REFERENCE => NodeValue::FootnoteReference(Box::default()),
        COMRAK_NODE_STRIKETHROUGH => NodeValue::Strikethrough,
        _ => return None,
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_new(t: c_int, group_out: *mut *mut Group) -> *mut Node {
    let Some(value) = new_value(t) else {
        return ptr::null_mut();
    };
    let (group, arena) = Group::new();
    let n: &'static Node = arena.alloc(AstNode::from(value));
    unsafe { *group_out = Rc::into_raw(group).cast_mut() };
    raw(Some(n))
}

// ---------------------------------------------------------------------------
// Inspection and navigation

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_type(n: *const Node) -> c_int {
    match unsafe { node(n) }.data.borrow().value {
        NodeValue::Document => COMRAK_NODE_DOCUMENT,
        NodeValue::BlockQuote => COMRAK_NODE_BLOCK_QUOTE,
        NodeValue::List(_) => COMRAK_NODE_LIST,
        NodeValue::Item(_) => COMRAK_NODE_ITEM,
        NodeValue::CodeBlock(_) => COMRAK_NODE_CODE_BLOCK,
        NodeValue::HtmlBlock(_) => COMRAK_NODE_HTML_BLOCK,
        NodeValue::Paragraph => COMRAK_NODE_PARAGRAPH,
        NodeValue::Heading(_) => COMRAK_NODE_HEADING,
        NodeValue::ThematicBreak => COMRAK_NODE_THEMATIC_BREAK,
        NodeValue::FootnoteDefinition(_) => COMRAK_NODE_FOOTNOTE_DEFINITION,
        NodeValue::Table(_) => COMRAK_NODE_TABLE,
        NodeValue::TableRow(_) => COMRAK_NODE_TABLE_ROW,
        NodeValue::TableCell => COMRAK_NODE_TABLE_CELL,
        NodeValue::TaskItem(_) => COMRAK_NODE_TASK_ITEM,
        NodeValue::Text(_) => COMRAK_NODE_TEXT,
        NodeValue::SoftBreak => COMRAK_NODE_SOFTBREAK,
        NodeValue::LineBreak => COMRAK_NODE_LINEBREAK,
        NodeValue::Code(_) => COMRAK_NODE_CODE,
        NodeValue::HtmlInline(_) => COMRAK_NODE_HTML_INLINE,
        NodeValue::Emph => COMRAK_NODE_EMPH,
        NodeValue::Strong => COMRAK_NODE_STRONG,
        NodeValue::Link(_) => COMRAK_NODE_LINK,
        NodeValue::Image(_) => COMRAK_NODE_IMAGE,
        NodeValue::FootnoteReference(_) => COMRAK_NODE_FOOTNOTE_REFERENCE,
        NodeValue::Strikethrough => COMRAK_NODE_STRIKETHROUGH,
        _ => COMRAK_NODE_OTHER,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_is_table_header(n: *const Node) -> bool {
    matches!(
        unsafe { node(n) }.data.borrow().value,
        NodeValue::TableRow(true)
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_sourcepos(
    n: *const Node,
    start_line: *mut usize,
    start_column: *mut usize,
    end_line: *mut usize,
    end_column: *mut usize,
) {
    let sp = unsafe { node(n) }.data.borrow().sourcepos;
    unsafe {
        *start_line = sp.start.line;
        *start_column = sp.start.column;
        *end_line = sp.end.line;
        *end_column = sp.end.column;
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_parent(n: *const Node) -> *mut Node {
    raw(unsafe { node(n) }.parent())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_first_child(n: *const Node) -> *mut Node {
    raw(unsafe { node(n) }.first_child())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_last_child(n: *const Node) -> *mut Node {
    raw(unsafe { node(n) }.last_child())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_next(n: *const Node) -> *mut Node {
    raw(unsafe { node(n) }.next_sibling())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_previous(n: *const Node) -> *mut Node {
    raw(unsafe { node(n) }.previous_sibling())
}

// ---------------------------------------------------------------------------
// Tree mutation

fn is_ancestor_or_self(candidate: &Node, of: &'static Node) -> bool {
    let mut cur = Some(of);
    while let Some(n) = cur {
        if ptr::eq(n, candidate) {
            return true;
        }
        cur = n.parent();
    }
    false
}

fn can_adopt(parent: &'static Node, child: &'static Node) -> bool {
    // The ancestor check comes first: can_contain_type borrows both nodes.
    !is_ancestor_or_self(child, parent) && parent.can_contain_type(&child.data.borrow().value)
}

unsafe fn link(
    anchor: *mut Node,
    anchor_group: *mut Group,
    other: *mut Node,
    other_group: *mut Group,
    op: fn(&'static Node, &'static Node) -> bool,
) -> bool {
    let (anchor, other) = unsafe { (node(anchor), node(other)) };
    if !op(anchor, other) {
        return false;
    }
    unsafe { Group::merge(&group(anchor_group), &group(other_group)) };
    true
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_append_child(
    n: *mut Node,
    ng: *mut Group,
    child: *mut Node,
    cg: *mut Group,
) -> bool {
    unsafe {
        link(n, ng, child, cg, |n, child| {
            can_adopt(n, child) && {
                n.append(child);
                true
            }
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_prepend_child(
    n: *mut Node,
    ng: *mut Group,
    child: *mut Node,
    cg: *mut Group,
) -> bool {
    unsafe {
        link(n, ng, child, cg, |n, child| {
            can_adopt(n, child) && {
                n.prepend(child);
                true
            }
        })
    }
}

fn can_be_sibling(n: &'static Node, sibling: &'static Node) -> bool {
    match n.parent() {
        Some(parent) => !ptr::eq(n, sibling) && can_adopt(parent, sibling),
        None => false,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_insert_before(
    n: *mut Node,
    ng: *mut Group,
    sibling: *mut Node,
    sg: *mut Group,
) -> bool {
    unsafe {
        link(n, ng, sibling, sg, |n, sibling| {
            can_be_sibling(n, sibling) && {
                n.insert_before(sibling);
                true
            }
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_insert_after(
    n: *mut Node,
    ng: *mut Group,
    sibling: *mut Node,
    sg: *mut Group,
) -> bool {
    unsafe {
        link(n, ng, sibling, sg, |n, sibling| {
            can_be_sibling(n, sibling) && {
                n.insert_after(sibling);
                true
            }
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_unlink(n: *mut Node) {
    unsafe { node(n) }.detach();
}

// ---------------------------------------------------------------------------
// Fields

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_literal(
    n: *const Node,
    p: *mut *const c_char,
    len: *mut usize,
) -> bool {
    let data = unsafe { node(n) }.data.borrow();
    let s: &str = match data.value {
        NodeValue::Text(ref t) => t,
        NodeValue::Code(ref c) => &c.literal,
        NodeValue::CodeBlock(ref cb) => &cb.literal,
        NodeValue::HtmlBlock(ref h) => &h.literal,
        NodeValue::HtmlInline(ref s) => s,
        _ => return false,
    };
    unsafe { lend(s, p, len) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_set_literal(
    n: *mut Node,
    p: *const c_char,
    len: usize,
) -> bool {
    let s = unsafe { input(p, len) }.into_owned();
    let mut data = unsafe { node(n) }.data.borrow_mut();
    match data.value {
        NodeValue::Text(ref mut t) => *t = Cow::Owned(s),
        NodeValue::Code(ref mut c) => c.literal = s,
        NodeValue::CodeBlock(ref mut cb) => cb.literal = s,
        NodeValue::HtmlBlock(ref mut h) => h.literal = s,
        NodeValue::HtmlInline(ref mut h) => *h = s,
        _ => return false,
    }
    true
}

fn with_link<R>(n: &Node, f: impl FnOnce(&mut NodeLink) -> R) -> Option<R> {
    match n.data.borrow_mut().value {
        NodeValue::Link(ref mut l) | NodeValue::Image(ref mut l) => Some(f(l)),
        _ => None,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_url(
    n: *const Node,
    p: *mut *const c_char,
    len: *mut usize,
) -> bool {
    with_link(unsafe { node(n) }, |l| unsafe { lend(&l.url, p, len) }).unwrap_or(false)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_set_url(n: *mut Node, p: *const c_char, len: usize) -> bool {
    let s = unsafe { input(p, len) }.into_owned();
    with_link(unsafe { node(n) }, |l| l.url = s).is_some()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_title(
    n: *const Node,
    p: *mut *const c_char,
    len: *mut usize,
) -> bool {
    with_link(unsafe { node(n) }, |l| unsafe { lend(&l.title, p, len) }).unwrap_or(false)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_set_title(n: *mut Node, p: *const c_char, len: usize) -> bool {
    let s = unsafe { input(p, len) }.into_owned();
    with_link(unsafe { node(n) }, |l| l.title = s).is_some()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_fence_info(
    n: *const Node,
    p: *mut *const c_char,
    len: *mut usize,
) -> bool {
    match unsafe { node(n) }.data.borrow().value {
        NodeValue::CodeBlock(ref cb) => unsafe { lend(&cb.info, p, len) },
        _ => false,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_set_fence_info(
    n: *mut Node,
    p: *const c_char,
    len: usize,
) -> bool {
    let s = unsafe { input(p, len) }.into_owned();
    match unsafe { node(n) }.data.borrow_mut().value {
        NodeValue::CodeBlock(ref mut cb) => {
            cb.info = s;
            true
        }
        _ => false,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_footnote_name(
    n: *const Node,
    p: *mut *const c_char,
    len: *mut usize,
) -> bool {
    match unsafe { node(n) }.data.borrow().value {
        NodeValue::FootnoteDefinition(ref d) => unsafe { lend(&d.name, p, len) },
        NodeValue::FootnoteReference(ref r) => unsafe { lend(&r.name, p, len) },
        _ => false,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_footnote_ix(n: *const Node, ix: *mut u32) -> bool {
    match unsafe { node(n) }.data.borrow().value {
        NodeValue::FootnoteReference(ref r) => {
            unsafe { *ix = r.ix };
            true
        }
        _ => false,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_heading_level(n: *const Node, level: *mut c_int) -> bool {
    match unsafe { node(n) }.data.borrow().value {
        NodeValue::Heading(ref h) => {
            unsafe { *level = c_int::from(h.level) };
            true
        }
        _ => false,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_set_heading_level(n: *mut Node, level: c_int) -> bool {
    let Ok(level @ 1..=6) = u8::try_from(level) else {
        return false;
    };
    match unsafe { node(n) }.data.borrow_mut().value {
        NodeValue::Heading(ref mut h) => {
            h.level = level;
            true
        }
        _ => false,
    }
}

fn with_list<R>(n: &Node, f: impl FnOnce(&mut NodeList) -> R) -> Option<R> {
    match n.data.borrow_mut().value {
        NodeValue::List(ref mut l) => Some(f(l)),
        _ => None,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_list_type(n: *const Node, t: *mut c_int) -> bool {
    with_list(unsafe { node(n) }, |l| unsafe {
        *t = match l.list_type {
            ListType::Bullet => COMRAK_LIST_BULLET,
            ListType::Ordered => COMRAK_LIST_ORDERED,
        };
    })
    .is_some()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_set_list_type(n: *mut Node, t: c_int) -> bool {
    let list_type = match t {
        COMRAK_LIST_BULLET => ListType::Bullet,
        COMRAK_LIST_ORDERED => ListType::Ordered,
        _ => return false,
    };
    with_list(unsafe { node(n) }, |l| l.list_type = list_type).is_some()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_list_start(n: *const Node, start: *mut usize) -> bool {
    with_list(unsafe { node(n) }, |l| unsafe { *start = l.start }).is_some()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_set_list_start(n: *mut Node, start: usize) -> bool {
    with_list(unsafe { node(n) }, |l| l.start = start).is_some()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_list_tight(n: *const Node, tight: *mut bool) -> bool {
    with_list(unsafe { node(n) }, |l| unsafe { *tight = l.tight }).is_some()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_set_list_tight(n: *mut Node, tight: bool) -> bool {
    with_list(unsafe { node(n) }, |l| l.tight = tight).is_some()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_table_columns(n: *const Node) -> usize {
    match unsafe { node(n) }.data.borrow().value {
        NodeValue::Table(ref t) => t.num_columns,
        _ => 0,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_table_alignment(n: *const Node, column: usize) -> c_int {
    match unsafe { node(n) }.data.borrow().value {
        NodeValue::Table(ref t) => match t.alignments.get(column) {
            Some(TableAlignment::Left) => COMRAK_ALIGN_LEFT,
            Some(TableAlignment::Center) => COMRAK_ALIGN_CENTER,
            Some(TableAlignment::Right) => COMRAK_ALIGN_RIGHT,
            Some(TableAlignment::None) | None => COMRAK_ALIGN_NONE,
        },
        _ => COMRAK_ALIGN_NONE,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_get_task_checked(n: *const Node, checked: *mut bool) -> bool {
    match unsafe { node(n) }.data.borrow().value {
        NodeValue::TaskItem(ref t) => {
            unsafe { *checked = t.symbol.is_some() };
            true
        }
        _ => false,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_node_set_task_checked(n: *mut Node, checked: bool) -> bool {
    match unsafe { node(n) }.data.borrow_mut().value {
        NodeValue::TaskItem(ref mut t) => {
            t.symbol = checked.then_some('x');
            true
        }
        _ => false,
    }
}

// ---------------------------------------------------------------------------
// Rendering and escaping

type Formatter = fn(&'static Node, &Options, &mut String) -> std::fmt::Result;

unsafe fn render(
    n: *const Node,
    flags: u32,
    width: usize,
    out: *mut *mut c_char,
    out_len: *mut usize,
    format: Formatter,
) -> c_int {
    let Some(mut opts) = options(flags) else {
        return COMRAK_ERR_INVALID_FLAGS;
    };
    opts.render.width = width;
    let n = unsafe { node(n) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut s = String::new();
        format(n, &opts, &mut s).map(|()| s)
    }));
    match result {
        Ok(Ok(s)) => {
            unsafe { give(s, out, out_len) };
            COMRAK_OK
        }
        Ok(Err(_)) => COMRAK_ERR_RENDER,
        Err(_) => COMRAK_ERR_PANIC,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_render_html(
    n: *const Node,
    flags: u32,
    out: *mut *mut c_char,
    out_len: *mut usize,
) -> c_int {
    unsafe {
        render(n, flags, 0, out, out_len, |n, o, s| {
            comrak::format_html(n, o, s)
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_render_xml(
    n: *const Node,
    flags: u32,
    out: *mut *mut c_char,
    out_len: *mut usize,
) -> c_int {
    unsafe {
        render(n, flags, 0, out, out_len, |n, o, s| {
            comrak::format_xml(n, o, s)
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_render_commonmark(
    n: *const Node,
    flags: u32,
    width: usize,
    out: *mut *mut c_char,
    out_len: *mut usize,
) -> c_int {
    unsafe {
        render(n, flags, width, out, out_len, |n, o, s| {
            comrak::format_commonmark(n, o, s)
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_escape_html(
    p: *const c_char,
    len: usize,
    out: *mut *mut c_char,
    out_len: *mut usize,
) {
    let s = unsafe { input(p, len) };
    let mut escaped = String::with_capacity(s.len());
    comrak::html::escape(&mut escaped, &s).expect("writing to a String cannot fail");
    unsafe { give(escaped, out, out_len) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_escape_href(
    p: *const c_char,
    len: usize,
    out: *mut *mut c_char,
    out_len: *mut usize,
) {
    let s = unsafe { input(p, len) };
    let mut escaped = String::with_capacity(s.len());
    comrak::html::escape_href(&mut escaped, &s, false).expect("writing to a String cannot fail");
    unsafe { give(escaped, out, out_len) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn comrak_string_free(p: *mut c_char, len: usize) {
    drop(unsafe { Box::from_raw(ptr::slice_from_raw_parts_mut(p.cast::<u8>(), len)) });
}

#[cfg(test)]
mod tests;
