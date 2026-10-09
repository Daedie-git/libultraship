#!/usr/bin/env python3
"""Compile/run real keyboard controller implementations without launching the game.

Usage: python3 libultraship/tests/keyboard_controller_headless_tests.py --build-dir build-cmake
Requires an existing configured Ninja build for dependency include paths; does not
run Ninja build commands, initialize SDL, create windows, or touch build outputs.
Application-service substitutes and their limits are in the C++ test file.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

LUS = Path(__file__).resolve().parents[1]
CONTROLLER = LUS / "src/ship/controller/controldevice/controller"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    target = "libultraship/src/CMakeFiles/libultraship.dir/ship/controller/controldevice/controller/ControllerStick.cpp.o"
    commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", target], text=True)
    compile_line = next(line for line in commands.splitlines() if line.endswith(" -c " + str(CONTROLLER / "ControllerStick.cpp")))
    words = shlex.split(compile_line)
    flags = []
    iterator = iter(words[1:])
    for word in iterator:
        if word in ("-I", "-isystem"):
            flags.extend([word, next(iterator)])
        elif word.startswith(("-I", "-D")) and not word.startswith(("-DSPDLOG_", "-DNDEBUG")):
            flags.append(word)
    sources = [CONTROLLER / "ControllerStick.cpp", CONTROLLER / "ControllerButton.cpp"]
    mapping = CONTROLLER / "mapping"
    sources += [mapping / (name + ".cpp") for name in
                ("ControllerMapping", "ControllerInputMapping", "ControllerAxisDirectionMapping", "ControllerButtonMapping")]
    sources += [mapping / "keyboard" / (name + ".cpp") for name in
                ("KeyboardKeyToAnyMapping", "KeyboardKeyToAxisDirectionMapping", "KeyboardKeyToButtonMapping")]
    sources += [LUS / "src/ship/utils/StringHelper.cpp", Path(__file__).with_suffix(".cpp")]
    with tempfile.TemporaryDirectory(prefix="keyboard-controller-headless-") as temporary:
        executable = Path(temporary) / "tests"
        command = [os.environ.get("CXX", words[0]), *flags, "-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_OFF",
                   "-std=c++20", "-O0", "-g", "-ffunction-sections", "-fdata-sections",
                   *map(str, sources), "-Wl,--gc-sections", "-o", str(executable)]
        subprocess.run(command, cwd=build, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
