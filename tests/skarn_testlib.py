# skarn_testlib.py -- what the test runners under tests/ share.
#
# Finding the driver, running it, colored PASS/FAIL, and the command-line options every runner takes.
# Python 3.9 or newer, standard library only, so the runners work the same on Windows and macOS.
#
# The driver is `skarnvm` (`skarnvm.exe` on Windows) in both builds. Without --exe a runner looks, in this
# order, at
#     x64/<config>/skarnvm.exe            (the Visual Studio solution)
#     build/skarnvm                       (CMake, single-configuration generator)
#     build/<config>/skarnvm              (CMake, multi-configuration generator)
# the build/ ones each also with `.exe`, and takes the first that exists.

import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def repo_root():
    return REPO_ROOT


def rel(path):
    """The path relative to the repository root, with forward slashes; the path itself if outside it."""
    try:
        return Path(path).resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(path)


# ---- colored output --------------------------------------------------------------------------------

def _use_color():
    if os.environ.get("NO_COLOR") or not sys.stdout.isatty():
        return False
    if os.name == "nt":
        os.system("")  # switches the Windows console to ANSI escape sequences
    return True


_COLOR = _use_color()


def _paint(code, text):
    return "\033[{}m{}\033[0m".format(code, text) if _COLOR else text


def green(text):
    return _paint("32", text)


def red(text):
    return _paint("31", text)


def yellow(text):
    return _paint("33", text)


def say(text=""):
    print(text, flush=True)


# ---- the driver -------------------------------------------------------------------------------------

def add_driver_options(parser, timeout=60):
    parser.add_argument("--exe", default="", help="the driver (skarnvm)")
    parser.add_argument("--config", default="Release", help="build configuration to look in (default Release)")
    parser.add_argument("--timeout", type=int, default=timeout, help="seconds per program (default %(default)s)")


def find_driver(exe, config):
    """The driver's path, or exits with 1 after naming every place it looked."""
    if exe:
        p = Path(exe)
        if p.is_file():
            return p.resolve()
        say(red("ERROR: the driver '{}' does not exist.".format(exe)))
        sys.exit(1)
    candidates = [
        REPO_ROOT / "x64" / config / "skarnvm.exe",
        REPO_ROOT / "build" / "skarnvm",
        REPO_ROOT / "build" / "skarnvm.exe",
        REPO_ROOT / "build" / config / "skarnvm",
        REPO_ROOT / "build" / config / "skarnvm.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c
    say(red("ERROR: no driver found. Build the project ({}) first, or pass --exe. Looked at:".format(config)))
    for c in candidates:
        say(red("  " + str(c)))
    sys.exit(1)


class Result:
    def __init__(self, code, out, err, timed_out=False):
        self.code = code
        self.out = out
        self.err = err
        self.timed_out = timed_out


def _text(data):
    if data is None:
        return ""
    if isinstance(data, bytes):
        data = data.decode("utf-8", errors="replace")
    return data.replace("\r\n", "\n")


def run_driver(exe, args, cwd=None, timeout=60):
    """Runs the driver with an empty standard input; stdout and stderr decoded as UTF-8, LF line ends."""
    try:
        p = subprocess.run([str(exe)] + [str(a) for a in args], cwd=None if cwd is None else str(cwd),
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return Result(-1, "", "timed out after {} s".format(timeout), timed_out=True)
    return Result(p.returncode, _text(p.stdout), _text(p.stderr))
