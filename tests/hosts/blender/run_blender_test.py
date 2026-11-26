# SPDX-License-Identifier: Apache-2.0
"""Runs one add-on test in a headless Blender with a throwaway profile:

    python run_blender_test.py <blender.exe> <extension.zip> <test.py>

Makes a fresh profile folder (BLENDER_USER_RESOURCES, _CONFIG, _SCRIPTS, _EXTENSIONS point into it, so
the user's own Blender settings and extensions are never touched), installs the GroomLab extension
from the build's zip the way a user does (blender --command extension install-file), then runs the
test with `blender --background --factory-startup --python-exit-code 1 --python <test.py>`.
A test named gui_*.py runs in a Blender with its window (no --background: it drives itself from
timers and quits with its exit code), stopped after GUI_TIMEOUT seconds.
The test enables the add-on itself (bl_ext.user_default.groomlab). Exit code: the test's.
"""

import os
import shutil
import subprocess
import sys
import tempfile

GUI_TIMEOUT = 600


def main():
    blender, zip_path, test = sys.argv[1:4]
    name = os.path.splitext(os.path.basename(test))[0]
    profile = tempfile.mkdtemp(prefix="groomlab_blender_%s_" % name)
    env = dict(os.environ)
    for var, sub in (("BLENDER_USER_RESOURCES", "resources"), ("BLENDER_USER_CONFIG", "config"),
                     ("BLENDER_USER_SCRIPTS", "scripts"), ("BLENDER_USER_EXTENSIONS", "extensions"),
                     ("BLENDER_USER_DATAFILES", "datafiles")):
        env[var] = os.path.join(profile, sub)
        os.makedirs(env[var], exist_ok=True)
    env["GROOMLAB_TEST_TMP"] = os.path.join(profile, "tmp")
    os.makedirs(env["GROOMLAB_TEST_TMP"])
    env.pop("GROOMLAB_HELPER", None)  # the extension's own bin/groomlab-helper
    flags = 0x08000000 if sys.platform == "win32" else 0  # no console windows
    try:
        install = subprocess.run([blender, "--factory-startup", "--command", "extension", "install-file", "-r",
                                  "user_default", zip_path], env=env, capture_output=True, text=True, creationflags=flags)
        sys.stdout.write(install.stdout + install.stderr)
        if install.returncode != 0:
            print("installing %s failed (%d)" % (zip_path, install.returncode))
            return 1
        installed = os.path.join(env["BLENDER_USER_EXTENSIONS"], "user_default", "groomlab", "blender_manifest.toml")
        if not os.path.isfile(installed):
            print("the extension is not where Blender installs it: %s" % installed)
            return 1
        gui = os.path.basename(test).startswith("gui_")
        command = [blender] + ([] if gui else ["--background"]) + ["--factory-startup", "--python-exit-code", "1",
                                                                  "--python", test]
        try:
            run = subprocess.run(command, env=env, creationflags=flags, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=GUI_TIMEOUT if gui else None)
        except subprocess.TimeoutExpired as e:
            sys.stdout.write((e.stdout or b"").decode("utf-8", "replace"))
            print("%s did not finish in %d s" % (name, GUI_TIMEOUT))
            return 1
        sys.stdout.write(run.stdout.decode("utf-8", "replace"))
        sys.stdout.flush()
        return run.returncode
    finally:
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
