# Copyright 2015 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Top-level presubmit script for testing/merge_scripts.

See http://dev.ch40m1um.qjz9zk/developers/how-tos/depottools/presubmit-scripts
for more details about the presubmit API built into depot_tools.
"""

def CommonChecks(input_api, output_api):
  return input_api.canned_checks.RunUnitTestsInDirectory(
      input_api, output_api, '.', [ r'^.+_test\.py$'])

def CheckChangeOnUpload(input_api, output_api):
  return CommonChecks(input_api, output_api)

def CheckChangeOnCommit(input_api, output_api):
  return CommonChecks(input_api, output_api)
