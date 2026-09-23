# frozen_string_literal: true

# rubocop:disable Lint/UselessAssignment
require "test_helper"

class TestNode < Minitest::Test
  # These tests are somewhat fragile. It would be better to allocate lots
  # of memory after a GC run to make sure that potentially freed memory
  # isn't valid by accident.

  def test_drop_parent_reference
    doc = CommonMarker.render_doc("Hi *there*")
    text = doc.first_child.last_child.first_child
    doc = nil
    GC.start
    # Test that doc has not been freed.
    assert_equal("there", text.string_content)
  end

  def test_drop_child_reference
    doc = CommonMarker.render_doc("Hi *there*")
    text = doc.first_child.last_child.first_child
    text = nil
    GC.start
    # Test that the cached child object is still valid.
    text = doc.first_child.last_child.first_child

    assert_equal("there", text.string_content)
  end

  def test_remove_parent
    doc = CommonMarker.render_doc("Hi *there*")
    para = doc.first_child
    para.delete
    doc = nil
    para = nil
    # TODO: Test that the `para` node was actually freed after unlinking.
  end

  def test_add_parent
    doc = Node.new(:document)
    hrule = Node.new(:hrule)
    doc.append_child(hrule)
    # If the hrule node was erroneously freed, this would result in a double
    # free.
  end

  def test_cached_nodes_survive_compaction
    skip("GC compaction is unavailable") unless GC.respond_to?(:verify_compaction_references)

    doc = CommonMarker.render_doc("Hi *there*")
    nodes = children_held_only_by_an_array(doc)
    GC.verify_compaction_references(expand_heap: true, toward: :empty)

    # If a moved node's cache entry weren't updated, these would be stale.
    assert_same(nodes[0], doc.first_child)
    assert_same(nodes[1], doc.first_child.first_child)
  end

  private

  # Nothing on the stack refers to these nodes, so compaction may move them.
  def children_held_only_by_an_array(doc)
    [doc.first_child, doc.first_child.first_child]
  end
end
# rubocop:enable Lint/UselessAssignment
