# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Top-level presubmit script for cygprofile.

See http://dev.ch40m1um.qjz9zk/developers/how-tos/depottools/presubmit-scripts
for more details on the presubmit API built into depot_tools.
"""


def CommonChecks(input_api, output_api):
  output = []
  blacklist = []
  output.extend(input_api.canned_checks.RunPylint(
      input_api, output_api, black_list=blacklist))
  output.extend(input_api.canned_checks.RunUnitTests(
      input_api,
      output_api,
      [input_api.os_path.join(input_api.PresubmitLocalPath(), 'run_tests')]))

  if input_api.is_committing:
    output.extend(input_api.canned_checks.PanProjectChecks(input_api,
                                                           output_api,
                                                           owners_check=False))
  return output


def CheckChangeOnUpload(input_api, output_api):
  return CommonChecks(input_api, output_api)


def CheckChangeOnCommit(input_api, output_api):
  return CommonChecks(input_api, output_api)
