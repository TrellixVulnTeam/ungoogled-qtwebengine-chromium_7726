#!/usr/bin/env python
# Copyright 2017 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""A script to merge multiple source xml files into a single histograms.xml."""

import argparse
import os
import sys
import xml.dom.minidom

import expand_owners
import extract_histograms
import histogram_configuration_model
import histogram_paths
import populate_enums


def GetElementsByTagName(trees, tag, depth=2):
  """Gets all elements with the specified tag from a set of DOM trees.

  Args:
    trees: A list of DOM trees.
    tag: The tag of the elements to find.
    depth: The depth in the trees by which a match should be found.

  Returns:
    A list of DOM nodes with the specified tag.
  """
  iterator = extract_histograms.IterElementsWithTag
  return list(e for t in trees for e in iterator(t, tag, depth))


def GetEnumsNodes(doc, trees):
  """Gets all enums from a set of DOM trees.

  If trees contain ukm events, populates a list of ints to the
  "UkmEventNameHash" enum where each value is a ukm event name hash truncated
  to 31 bits and each label is the corresponding event name.

  Args:
    doc: The document to create the node in.
    trees: A list of DOM trees.
  Returns:
    A list of enums DOM nodes.
  """
  enums_list = GetElementsByTagName(trees, 'enums')
  ukm_events = GetElementsByTagName(
      GetElementsByTagName(trees, 'ukm-configuration'), 'event')
  # Early return if there are no ukm events provided. MergeFiles have callers
  # that do not pass ukm events so, in that case, we don't need to iterate
  # through the enum list.
  if not ukm_events:
    return enums_list
  for enums in enums_list:
    populate_enums.PopulateEnumsWithUkmEvents(doc, enums, ukm_events)
  return enums_list


def CombineHistogramsSorted(doc, trees):
  """Sorts histograms related nodes by name and returns the combined nodes.

  This function sorts nodes including <histogram>, <variant> and
  <histogram_suffix>. Then it returns one <histograms> that contains the
  sorted <histogram> and <variant> nodes and the other <histogram_suffixes_list>
  node containing all <histogram_suffixes> nodes.

  Args:
    doc: The document to create the node in.
    trees: A list of DOM trees.

  Returns:
    A list containing the combined <histograms> node and the combined
    <histogram_suffix_list> node.
  """
  # Create the combined <histograms> tag.
  combined_histograms = doc.createElement('histograms')

  def SortByLowerCaseName(node):
    return node.getAttribute('name').lower()

  variants_nodes = GetElementsByTagName(trees, 'variants', depth=3)
  sorted_variants = sorted(variants_nodes, key=SortByLowerCaseName)

  histogram_nodes = GetElementsByTagName(trees, 'histogram', depth=3)
  sorted_histograms = sorted(histogram_nodes, key=SortByLowerCaseName)

  for variants in sorted_variants:
    # Use unsafe version of `appendChild` function here because the safe one
    # takes a lot longer (10000x) to append all children. The unsafe version
    # is ok here because:
    #   1. the node to be appended is a clean node.
    #   2. The unsafe version only does fewer checks but not changing any
    #     behavior and it's documented to be usable if performance matters.
    #     See https://github.com/python/cpython/blob/2.7/Lib/xml/dom/minidom.py#L276.
    xml.dom.minidom._append_child(combined_histograms, variants)

  for histogram in sorted_histograms:
    xml.dom.minidom._append_child(combined_histograms, histogram)

  # Create the combined <histogram_suffixes_list> tag.
  combined_histogram_suffixes_list = doc.createElement(
      'histogram_suffixes_list')

  histogram_suffixes_nodes = GetElementsByTagName(trees,
                                                  'histogram_suffixes',
                                                  depth=3)
  sorted_histogram_suffixes = sorted(histogram_suffixes_nodes,
                                     key=SortByLowerCaseName)

  for histogram_suffixes in sorted_histogram_suffixes:
    xml.dom.minidom._append_child(combined_histogram_suffixes_list,
                                  histogram_suffixes)

  return [combined_histograms, combined_histogram_suffixes_list]


def MakeNodeWithChildren(doc, tag, children):
  """Creates a DOM node with specified tag and child nodes.

  Args:
    doc: The document to create the node in.
    tag: The tag to create the node with.
    children: A list of DOM nodes to add as children.
  Returns:
    A DOM node.
  """
  node = doc.createElement(tag)
  for child in children:
    # if child.tagName == 'histograms':
    #   expand_owners.ExpandHistogramsOWNERS(child)
    node.appendChild(child)
  return node


def MergeTrees(trees):
  """Merges a list of histograms.xml DOM trees.

  Args:
    trees: A list of histograms.xml DOM trees.
  Returns:
    A merged DOM tree.
  """
  doc = xml.dom.minidom.Document()
  doc.appendChild(
      MakeNodeWithChildren(
          doc,
          'histogram-configuration',
          # This can result in the merged document having multiple <enums> and
          # similar sections, but scripts ignore these anyway.
          GetEnumsNodes(doc, trees) +
          # Sort the <histogram> and <histogram_suffixes> nodes by name and
          # return the combined nodes.
          CombineHistogramsSorted(doc, trees)))
  # After using the unsafe version of appendChild, we see a regression when
  # pretty-printing the merged |doc|. This might because the unsafe appendChild
  # doesn't build indexes for later lookup. And thus, we need to convert the
  # merged |doc| to a xml string and convert it back to force it to build
  # indexes for the merged |doc|.
  return xml.dom.minidom.parseString(doc.toxml())


def MergeFiles(filenames=[], files=[]):
  """Merges a list of histograms.xml files.

  Args:
    filenames: A list of histograms.xml filenames.
    files: A list of histograms.xml file-like objects.
  Returns:
    A merged DOM tree.
  """
  all_files = files + [open(f) for f in filenames]
  trees = [xml.dom.minidom.parse(f) for f in all_files]
  return MergeTrees(trees)


def PrettyPrintMergedFiles(filenames=[], files=[]):
  return histogram_configuration_model.PrettifyTree(
      MergeFiles(filenames=filenames, files=files))


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--output', required=True)
  args = parser.parse_args()
  with open(args.output, 'w') as f:
    # This is run by
    # https://source.ch40m1um.qjz9zk/chromium/chromium/src/+/master:tools/metrics/BUILD.gn;drc=573e48309695102dec2da1e8f806c18c3200d414;l=5
    # to send the merged histograms.xml to the server side. Providing |UKM_XML|
    # here is not to merge ukm.xml but to populate `UkmEventNameHash` enum
    # values.
    f.write(PrettyPrintMergedFiles(
      histogram_paths.ALL_XMLS + [histogram_paths.UKM_XML]))


if __name__ == '__main__':
  main()
