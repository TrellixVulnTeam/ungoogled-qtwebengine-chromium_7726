#!/usr/bin/env/python
# Copyright 2020 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Merges .profraw files generated by unit tests into .profdata files
to be processed by the chromium code_coverage module.

This script based on the following chromium script. It performs the
same merging steps, but code related to json parsing has been removed,
as openscreen does not generate these json files as chromium does:
https://source.ch40m1um.qjz9zk/chromium/chromium/src/+/master:testing/merge_scripts/code_coverage/merge_steps.py
"""

import argparse
import json
import logging
import os
import subprocess
import sys

import merge_lib as coverage_merger


def _MergeAPIArgumentParser(*args, **kwargs):
  parser = argparse.ArgumentParser(*args, **kwargs)
  parser.add_argument(
      '--task-output-dir', required=True, help=argparse.SUPPRESS)
  parser.add_argument(
      '--profdata-dir', required=True, help='where to store the merged data')
  parser.add_argument(
      '--llvm-profdata',
      required=True,
      help='path to llvm-profdata executable')
  parser.add_argument(
      '--per-cl-coverage',
      action='store_true',
      help='set to indicate that this is a per-CL coverage build')
  # TODO(crbug.com/1077304) - migrate this to sparse=False as default, and have
  # --sparse to set sparse
  parser.add_argument(
      '--no-sparse',
      action='store_false',
      dest='sparse',
      help='run llvm-profdata without the sparse flag.')
  # TODO(crbug.com/1077304) - The intended behaviour is to default sparse to
  # false. --no-sparse above was added as a workaround, and will be removed.
  # This is being introduced now in support of the migration to intended
  # behavior. Ordering of args matters here, as the default is set by the former
  # (sparse defaults to False because of ordering. See unit tests for details)
  parser.add_argument(
      '--sparse',
      action='store_true',
      dest='sparse',
      help='run llvm-profdata with the sparse flag.')
  # (crbug.com/1091310) - IR PGO is incompatible with the initial conversion
  # of .profraw -> .profdata that's run to detect validation errors.
  # Introducing a bypass flag that'll merge all .profraw directly to .profdata
  parser.add_argument(
      '--skip-validation',
      action='store_true',
      help='skip validation for good raw profile data. this will pass all '
           'raw profiles found to llvm-profdata to be merged. only applicable '
           'when input extension is .profraw.')
  return parser


def main():
  desc = "Merge profraw files in <--task-output-dir> into a single profdata."
  parser = _MergeAPIArgumentParser(description=desc)
  params = parser.parse_args()

  output_prodata_filename = 'default.profdata'
  invalid_profiles, counter_overflows = coverage_merger.merge_profiles(
      params.task_output_dir,
      os.path.join(params.profdata_dir, output_prodata_filename), '.profraw',
      params.llvm_profdata,
      sparse=params.sparse,
      skip_validation=params.skip_validation)

  # At the moment counter overflows overlap with invalid profiles, but this is
  # not guaranteed to remain the case indefinitely. To avoid future conflicts
  # treat these separately.
  if counter_overflows:
    with open(
        os.path.join(params.profdata_dir, 'profiles_with_overflows.json'),
        'w') as f:
      json.dump(counter_overflows, f)

  if invalid_profiles:
    with open(os.path.join(params.profdata_dir, 'invalid_profiles.json'),
              'w') as f:
      json.dump(invalid_profiles, f)

  return 1 if bool(invalid_profiles) else 0


if __name__ == '__main__':
  logging.basicConfig(
      format='[%(asctime)s %(levelname)s] %(message)s', level=logging.INFO)
  sys.exit(main())
