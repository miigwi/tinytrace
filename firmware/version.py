# Stamp the build with the git commit it came from.
#
# Five of these boards look identical and routinely carry different firmware.
# Without a stamp the only way to answer "is this the current build?" is to drop
# into the ROM bootloader and byte-compare the flash — which is exactly the
# detour the answer was supposed to save.
#
# A trailing "+" means the working tree had uncommitted changes, so the flashed
# image does not correspond to any commit. That is the case worth flagging: a
# clean hash can be checked out and rebuilt, a dirty one cannot.
import subprocess

Import("env")  # noqa: F821 — injected by PlatformIO


def git(*args):
    try:
        out = subprocess.check_output(["git"] + list(args), stderr=subprocess.DEVNULL)
        return out.decode().strip()
    except Exception:
        return ""  # no git, no repo, or a source tarball — fall back below


sha = git("rev-parse", "--short=7", "HEAD")
if not sha:
    sha = "nogit"
elif git("status", "--porcelain"):
    sha += "+"

env.Append(CPPDEFINES=[("TT_BUILD", env.StringifyMacro(sha))])  # noqa: F821
print("build stamp: %s" % sha)
