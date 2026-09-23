# frozen_string_literal: true

require "test_helper"

# Behaviour specific to the comrak backend: what it deliberately does not
# support, and where it is stricter than cmark-gfm was. See COMRAK.md.
class TestComrak < Minitest::Test
  def test_tagfilter_is_not_offered
    refute_includes(CommonMarker.extensions, "tagfilter")
    assert_equal(["table", "strikethrough", "autolink", "tasklist"], CommonMarker.extensions)
  end

  def test_tagfilter_is_not_implemented
    assert_raises(NotImplementedError) { CommonMarker.render_html("<xmp>", :DEFAULT, [:tagfilter]) }
    assert_raises(NotImplementedError) { CommonMarker.render_doc("<xmp>", :DEFAULT, [:tagfilter]) }
    assert_raises(NotImplementedError) { CommonMarker.render_doc("<xmp>").to_html(:DEFAULT, [:tagfilter]) }
  end

  def test_the_ruby_renderer_still_filters_tags
    doc = CommonMarker.render_doc("<xmp>")

    assert_equal("&lt;xmp>\n", HtmlRenderer.new(options: :UNSAFE, extensions: [:tagfilter]).render(doc))
  end

  def test_plaintext_is_not_implemented
    assert_raises(NotImplementedError) { CommonMarker.render_doc("hi").to_plaintext }
  end

  def test_unsupported_parse_options_are_not_implemented
    [:LIBERAL_HTML_TAG, :STRIKETHROUGH_DOUBLE_TILDE].each do |option|
      assert_raises(NotImplementedError, option) { CommonMarker.render_doc("hi", option) }
    end
  end

  def test_unsupported_render_options_are_not_implemented_where_cmark_read_them
    doc = CommonMarker.render_doc("a\nb")

    assert_raises(NotImplementedError) { doc.to_html(:NOBREAKS) }
    assert_raises(NotImplementedError) { doc.to_commonmark(:NOBREAKS) }
    assert_raises(NotImplementedError) { doc.to_html(:TABLE_PREFER_STYLE_ATTRIBUTES) }
    assert_raises(NotImplementedError) { CommonMarker.render_html("a", :NOBREAKS) }
    # cmark-gfm's XML and CommonMark renderers ignored these, so they still may.
    assert_match(/<paragraph>/, doc.to_xml([:NOBREAKS, :TABLE_PREFER_STYLE_ATTRIBUTES]))
    assert_equal(doc.to_commonmark, doc.to_commonmark(:TABLE_PREFER_STYLE_ATTRIBUTES))
  end

  def test_parse_only_options_are_accepted_when_rendering
    doc = CommonMarker.render_doc("hi")

    [:VALIDATE_UTF8, :SMART, :LIBERAL_HTML_TAG, :FOOTNOTES, :STRIKETHROUGH_DOUBLE_TILDE].each do |option|
      assert_equal("<p>hi</p>\n", doc.to_html(option), option)
    end
  end

  def test_invalid_utf8_is_always_replaced
    assert_equal("<p>a\u{fffd}b</p>\n", CommonMarker.render_html(+"a\xffb"))
  end

  def test_a_node_cannot_be_its_own_sibling
    doc = CommonMarker.render_doc("*a*")
    emph = doc.first_child.first_child

    assert_raises(NodeError) { emph.insert_before(emph) }
    assert_raises(NodeError) { emph.insert_after(emph) }
    assert_equal("<p><em>a</em></p>\n", doc.to_html)
  end

  def test_a_node_cannot_contain_its_ancestor
    doc = CommonMarker.render_doc("> > a")
    outer = doc.first_child
    inner = outer.first_child

    assert_raises(NodeError) { inner.append_child(outer) }
    assert_raises(NodeError) { outer.append_child(outer) }
    assert_equal("<blockquote>\n<blockquote>\n<p>a</p>\n</blockquote>\n</blockquote>\n", doc.to_html)
  end

  def test_moving_nodes_between_documents
    doc = CommonMarker.render_doc("para")
    other = CommonMarker.render_doc("*moved*")
    emph = other.first_child.first_child
    doc.first_child.append_child(emph)

    assert_equal("<p>para<em>moved</em></p>\n", doc.to_html)
    assert_equal("<p></p>\n", other.to_html)
    assert_same(doc.first_child, emph.parent)
  end

  # This relies on comrak panicking when rendering a detached list item as
  # CommonMark. If comrak stops panicking there, find another trigger or drop
  # the test; what matters is that a panic does not abort the process.
  def test_comrak_panics_become_node_errors
    error = assert_raises(NodeError) { Node.new(:list_item).to_commonmark }

    assert_match(/comrak panicked/, error.message)
  end
end
