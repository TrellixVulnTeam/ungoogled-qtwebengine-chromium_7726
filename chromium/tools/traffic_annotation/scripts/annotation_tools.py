# Copyright 2018 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Tools for annotation test scripts."""

from __future__ import print_function

import json
import os
import subprocess
import sys

script_dir = os.path.dirname(os.path.realpath(__file__))
tool_dir = os.path.abspath(os.path.join(script_dir, '../../clang/pylib'))
sys.path.insert(0, tool_dir)

#TODO(https://crbug.com/1119417): To be replaced with a commandline argument or
# made to run in addition to the preexisting C++ auditor executable.
PYTHON_AUDITOR = False

from clang import compile_db

class NetworkTrafficAnnotationTools():
  def __init__(self, build_path=None):
    """Initializes a NetworkTrafficAnnotationTools object.

    Args:
      build_path: str Absolute or relative path to a fully compiled build
          directory. If not specified, the script tries to find it based on
          relative position of this file (src/tools/traffic_annotation).
    """
    self.this_dir = os.path.dirname(os.path.abspath(__file__))

    if not build_path:
      build_path = self._FindPossibleBuildPath()
    if build_path:
      self.build_path = os.path.abspath(build_path)

    self.auditor_path = None

    # For each platform, map the returned platform name from python sys, to
    # directory name of traffic_annotation_auditor executable.
    platform = {
      'linux2': 'linux64',
      'darwin': 'mac',
      'win32': 'win32',
    }[sys.platform]

    if PYTHON_AUDITOR:
      path = os.path.join(self.this_dir, "../scripts/auditor.py")
    else:
      path = os.path.join(self.this_dir, '..', 'bin', platform,
                        'traffic_annotation_auditor')
      if sys.platform == 'win32':
        path += '.exe'

    if os.path.exists(path):
      self.auditor_path = path

  def _FindPossibleBuildPath(self):
    """Returns the first folder in //out that looks like a build dir."""
    # Assuming this file is in 'tools/traffic_annotation/scripts', three
    # directories deeper is 'src' and hopefully there is an 'out' in it.
    out = os.path.abspath(os.path.join(self.this_dir, '..', '..', '..', 'out'))
    if os.path.exists(out):
      for folder in os.listdir(out):
        candidate = os.path.join(out, folder)
        if (os.path.isdir(candidate) and
            self._CheckIfDirectorySeemsAsBuild(candidate)):
          return candidate
    return None

  def _CheckIfDirectorySeemsAsBuild(self, path):
    """Checks to see if a directory seems to be a compiled build directory by
    searching for 'gen' folder and 'build.ninja' file in it.
    """
    return all(os.path.exists(
        os.path.join(path, item)) for item in ('gen', 'build.ninja'))

  def GetCompDBFiles(self, generate_compdb):
    """Gets the list of files.

    Args:
      generate_compdb: if true, generate a new compdb and write it to
                       compile_commands.json.

    Returns:
      A set of absolute filepaths, with all compile-able C++ files (based on the
      compilation database).
    """
    if generate_compdb:
      compile_commands = compile_db.GenerateWithNinja(self.build_path)
      compdb_path = os.path.join(self.build_path, 'compile_commands.json')
      with open(compdb_path, 'w') as f:
        f.write(json.dumps(compile_commands, indent=2))

    compdb = compile_db.Read(self.build_path)
    return set(
        os.path.abspath(os.path.join(self.build_path, e['file']))
        for e in compdb)

  def GetModifiedFiles(self):
    """Gets the list of modified files from git. Returns None if any error
    happens.

    Returns:
      list of str List of modified files. Returns None on errors.
    """

    # List of files is extracted almost the same way as the following test
    # recipe: https://cs.ch40m1um.qjz9zk/chromium/tools/depot_tools/recipes/
    # recipe_modules/tryserver/api.py
    # '--no-renames' switch is added so that if a file is renamed, both old and
    # new name would be given. Old name is needed to discard its data in
    # annotations.xml and new name is needed for updating the XML and checking
    # its content for possible changes.
    args = ["git.bat"] if sys.platform == "win32" else ["git"]
    args += ["diff", "--cached", "--name-only", "--no-renames"]

    original_path = os.getcwd()

    # Change directory to src (two levels upper than build path).
    os.chdir(os.path.join(self.build_path, "..", ".."))
    command = subprocess.Popen(args, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    stdout_text, stderr_text = command.communicate()

    if stderr_text:
      print("Could not run '%s' to get the list of changed files "
            "beacuse: %s" % (" ".join(args), stderr_text))
      os.chdir(original_path)
      return None

    os.chdir(original_path)
    return stdout_text.splitlines()

  def CanRunAuditor(self):
    """Retruns true if all required paths to run auditor are known."""
    return self.build_path and self.auditor_path

  def RunAuditor(self, args):
    """Runs traffic annotation auditor and returns the results.

    Args:
      args: list of str Arguments to be passed to traffic annotation auditor.

    Returns:
      stdout_text: str Auditor's runtime outputs.
      stderr_text: str Auditor's returned errors.
      return_code: int Auditor's exit code.
    """

    if PYTHON_AUDITOR:
      command_line = [
        "vpython", self.auditor_path, "--build-path=" + self.build_path] + args
    else:
      command_line = [self.auditor_path, "--build-path=" + self.build_path] + \
      args

    command = subprocess.Popen(
        command_line, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout_text, stderr_text = command.communicate()
    return_code = command.returncode

    return stdout_text, stderr_text, return_code
