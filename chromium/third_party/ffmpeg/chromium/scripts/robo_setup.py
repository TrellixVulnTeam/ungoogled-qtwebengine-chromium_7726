#!/usr/bin/python
#
# Copyright 2018 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Set up everything for the roll.

import io
import os
from robo_lib import UserInstructions
import subprocess
from robo_lib import log
import shutil


def InstallUbuntuPackage(robo_configuration, package):
  """Install |package|.

    Args:
      robo_configuration: current RoboConfiguration.
      package: package name.
  """

  log("Installing package %s" % package)
  if robo_configuration.Call(["sudo", "apt-get", "install", package]):
    raise Exception("Could not install %s" % package)


def InstallPrereqs(robo_configuration):
  """Install prereqs needed to build ffmpeg.

    Args:
      robo_configuration: current RoboConfiguration.
  """
  robo_configuration.chdir_to_chrome_src()

  # Check out data for ffmpeg regression tests.
  media_directory = os.path.join("media", "test", "data", "internal")
  if not os.path.exists(media_directory):
    log("Checking out media internal test data")
    if robo_configuration.Call([
        "git", "clone",
        "https://chrome-internal.9oo91esource.qjz9zk/chrome/data/media",
        media_directory
    ]):
      raise Exception("Could not check out chrome media internal test data %s" %
                      media_directory)

  if robo_configuration.host_operating_system() == "linux":
    InstallUbuntuPackage(robo_configuration, "nasm")
    InstallUbuntuPackage(robo_configuration, "gcc-aarch64-linux-gnu")
  else:
    raise Exception("I don't know how to install deps for host os %s" %
                    robo_configuration.host_operating_system())


def EnsureASANDirWorks(robo_configuration):
  """Create the asan out dir and config for ninja builds.

    Args:
      robo_configuration: current RoboConfiguration.
  """

  robo_configuration.chdir_to_chrome_src()

  directory_name = robo_configuration.absolute_asan_directory()
  if os.path.exists(directory_name):
    return

  # Dir doesn't exist, so make it and generate the gn files.  Note that we
  # have no idea what state the ffmpeg config is, but that's okay.  gn will
  # re-build it when we run ninja later (after importing the ffmpeg config)
  # if it's changed.

  log("Creating asan build dir %s" % directory_name)
  os.mkdir(directory_name)

  # TODO(liberato): ffmpeg_branding Chrome vs ChromeOS.  also add arch
  # flags, etc.  Step 28.iii, and elsewhere.
  opts = ("is_debug=false", "is_clang=true", "proprietary_codecs=true",
          "media_use_libvpx=true", "media_use_ffmpeg=true",
          'ffmpeg_branding="Chrome"', "use_goma=true", "is_asan=true",
          "dcheck_always_on=true")
  print opts
  with open(os.path.join(directory_name, "args.gn"), "w") as f:
    for opt in opts:
      print opt
      f.write("%s\n" % opt)

  # Ask gn to generate build files.
  log("Running gn on %s" % directory_name)
  if robo_configuration.Call(
      ["gn", "gen", robo_configuration.relative_asan_directory()]):
    raise Exception(
        "Unable to gn gen %s" % robo_configuration.local_asan_directory())


def FileRead(filename):
  with io.open(filename, encoding='utf-8') as f:
    return f.read()


def EnsureGClientTargets(robo_configuration):
  """Make sure that we've got the right sdks if we're on a linux host."""
  if not robo_configuration.host_operating_system() == "linux":
    log("Not changing gclient target_os list on a non-linux host")
    return
  log("Checking gclient target_os list")
  gclient_filename = os.path.join(robo_configuration.chrome_src(), "..",
                                  ".gclient")

  # Ensure that target_os include 'android' and 'win'
  scope = {}
  try:
    exec (FileRead(gclient_filename), scope)
  except SyntaxError, e:
    raise Exception("Unable to read %s" % gclient_filename)

  if 'target_os' not in scope:
    log("Missing 'target_os', which goes at the end of .gclient,")
    log("OUTSIDE of the solutions = [] section")
    log("Example line:")
    log("target_os = [ 'android', 'win' ]")
    raise UserInstructions("Please add target_os to %s" % gclient_filename)

  if ('android' not in scope['target_os']) or ('win' not in scope['target_os']):
    log("Missing 'android' and/or 'win' in target_os, which goes at the")
    log("end of .gclient, OUTSIDE of the solutions = [] section")
    log("Example line:")
    log("target_os = [ 'android', 'win' ]")
    raise UserInstructions(
        "Please add 'android' and 'win' to target_os in %s" % gclient_filename)

  # Sync regardless of whether we changed the config.
  log("Running gclient sync")
  robo_configuration.chdir_to_chrome_src()
  if robo_configuration.Call(["gclient", "sync"]):
    raise Exception("gclient sync failed")


def FetchAdditionalWindowsBinaries(robo_configuration):
  """Download some additional binaries needed by ffmpeg.  gclient sync can
  sometimes remove these.  Re-run this if you're missing llvm-nm or llvm-ar."""
  robo_configuration.chdir_to_chrome_src()
  log("Downloading some additional compiler tools")
  if robo_configuration.Call(["tools/clang/scripts/update.py",
                              "--package=objdump"]):
    raise Exception("update.py --package=objdump failed")


def FetchMacSDKs(robo_configuration):
  """Download the MacOSX SDKs."""
  log("Installing Mac OSX sdk")
  robo_configuration.chdir_to_chrome_src()
  if robo_configuration.Call(
      "FORCE_MAC_TOOLCHAIN=1 build/mac_toolchain.py",
      shell=True):
    raise Exception("Cannot download and extract Mac beta SDK")

  # TODO: Once the 11.0 SDK is out of beta, it should be used for both
  # arm64 and intel builds.
  log("Installing Mac OSX 11.0 beta sdk for arm64")
  robo_configuration.chdir_to_chrome_src()
  if robo_configuration.Call(
      "FORCE_MAC_TOOLCHAIN=1 build/mac_toolchain.py "
      "--xcode-version xcode_12_beta",
      shell=True):
    raise Exception("Cannot download and extract Mac beta SDK")


def EnsureLLVMSymlinks(robo_configuration):
  """Create some symlinks to clang and friends, since that changes their
  behavior somewhat."""
  log("Creating symlinks to compiler tools if needed")
  os.chdir(
      os.path.join(robo_configuration.chrome_src(), "third_party", "llvm-build",
                   "Release+Asserts", "bin"))

  def EnsureSymlink(source, link_name):
    if not os.path.exists(link_name):
      os.symlink(source, link_name)

  # For windows.
  EnsureSymlink("clang", "clang-cl")
  EnsureSymlink("clang", "clang++")
  EnsureSymlink("lld", "ld.lld")
  EnsureSymlink("lld", "lld-link")
  # For mac. Only used at configure time to check if symbols are present.
  EnsureSymlink("lld", "ld64.lld")


def EnsureSysroots(robo_configuration):
  """Install arm/arm64/mips/mips64 sysroots."""
  robo_configuration.chdir_to_chrome_src()
  for arch in ["arm", "arm64", "mips", "mips64el"]:
    if robo_configuration.Call(
        ["build/linux/sysroot_scripts/install-sysroot.py", "--arch=" + arch]):
      raise Exception("Failed to install sysroot for " + arch)


def EnsureChromiumNasm(robo_configuration):
  """Make sure that chromium's nasm is built, so we can use it.  apt-get's is
  too old."""
  robo_configuration.chdir_to_chrome_src()

  # nasm in the LLVM bin directory that we already added to $PATH.  Note that we
  # put it there so that configure can find is as "nasm", rather than us having
  # to give it the full path.  I think the full path would affect the real
  # build.  That's not good.
  llvm_nasm_path = os.path.join(robo_configuration.llvm_path(), "nasm")
  if os.path.exists(llvm_nasm_path):
    log("nasm already installed in llvm bin directory")
    return

  # Make sure nasm is built, and copy it to the llvm bin directory.
  chromium_nasm_path = os.path.join(
      robo_configuration.absolute_asan_directory(), "nasm")
  if not os.path.exists(chromium_nasm_path):
    log("Building Chromium's nasm")
    if robo_configuration.Call([
        "ninja", "-j5000", "-C",
        robo_configuration.relative_asan_directory(), "third_party/nasm"
    ]):
      raise Exception("Failed to build nasm")
    # Verify that it exists now, for sanity.
    if not os.path.exists(chromium_nasm_path):
      raise Exception("Failed to find nasm even after building it")

  # Copy it
  log("Copying Chromium's nasm to llvm bin directory")
  if shutil.copy(chromium_nasm_path, llvm_nasm_path):
    raise Exception(
        "Could not copy %s into %s" % (chromium_nasm_path, llvm_nasm_path))


def EnsureToolchains(robo_configuration):
  """Make sure that we have all the toolchains for cross-compilation"""
  EnsureGClientTargets(robo_configuration)
  FetchAdditionalWindowsBinaries(robo_configuration)
  FetchMacSDKs(robo_configuration)
  EnsureLLVMSymlinks(robo_configuration)
  EnsureSysroots(robo_configuration)


def EnsureUpstreamRemote(robo_configuration):
  """Make sure that the upstream remote is defined."""
  robo_configuration.chdir_to_ffmpeg_home()
  remotes = subprocess.check_output(["git", "remote", "-v"]).split()
  if "upstream" in remotes:
    log("Upstream remote found")
    return
  log("Adding upstream remote")
  if robo_configuration.Call([
      "git", "remote", "add", "upstream", "git://source.ffmpeg.org/ffmpeg.git"
  ]):
    raise Exception("Failed to add git remote")
