# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Top-level presubmit script for testing.

See http://dev.ch40m1um.qjz9zk/developers/how-tos/depottools/presubmit-scripts
for more details on the presubmit API built into depot_tools.
"""


def CommonChecks(input_api, output_api):
  output = []
  output.extend(input_api.canned_checks.RunUnitTestsInDirectory(
      input_api, output_api, '.', [r'^.+_unittest\.py$']))
  output.extend(input_api.canned_checks.RunPylint(
      input_api, output_api, files_to_skip=[r'gmock.*', r'gtest.*']))
  return output


def CheckChangeOnUpload(input_api, output_api):
  return CommonChecks(input_api, output_api)


def CheckChangeOnCommit(input_api, output_api):
  return CommonChecks(input_api, output_api)
