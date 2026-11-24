# PlatformIO post script: bake the firmware version into the project sources as
# PLANE_RADAR_VERSION (shown on /info, /debug and in the serial log).
#
#   tagged release build (GitHub Actions)  -> "2.0.0"
#   local build on the tagged commit        -> "2.0.0"
#   local build three commits later         -> "2.0.0-3-gabc1234"
#   ... with uncommitted changes            -> "2.0.0-3-gabc1234-dirty"
#   no git / no tags                        -> "unknown" (or the commit hash)
#
# Only the project sources get the define (projenv), so a new version rebuilds
# them but not the framework or the libraries.

Import("env", "projenv")

import os
import subprocess


def detect_version():
    if os.environ.get("GITHUB_REF_TYPE") == "tag" and os.environ.get("GITHUB_REF_NAME"):
        version = os.environ["GITHUB_REF_NAME"]
    else:
        try:
            version = subprocess.run(
                ["git", "describe", "--tags", "--match", "v[0-9]*", "--always", "--dirty"],
                cwd=env.subst("$PROJECT_DIR"),
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            version = ""
    if version.startswith("v"):
        version = version[1:]
    return version or "unknown"


version = detect_version()
print(f"Plane Radar firmware version: {version}")
projenv.Append(CPPDEFINES=[("PLANE_RADAR_VERSION", env.StringifyMacro(version))])
