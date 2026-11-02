#!/usr/bin/env python3
"""Windows-only settings must not be reachable from a non-Windows catalog.

`snow_shot/src/presentation/settings/settingscatalog.cpp` builds the settings
tree from `#if` branches, so "is this item offered on Linux" is decided by the
preprocessor and by nothing the compiler on Windows can see. Three of the four
capture settings used `#ifndef Q_OS_MACOS`, which reads like "not Apple" but is
also true on Linux: the port shipped a "Capture API" selector offering
DXGI/WGC/GDI above an X11/Wayland pipeline that never reads it, a window element
API for a selector whose Linux backend is a stub (port document C.12), and a
Windows colour-filter switch. None of that fails a test on the workstation.

This is the same blind spot as 9.6.19/9.6.24 and the reason
`check-linux-windows-only-platform-symbols.py` exists: the platform that can
compile the code is not the platform that consumes it.

The gate tracks `#if`/`#elif`/`#else`/`#endif` nesting and asks, for each of the
settings below, whether it sits inside a branch that *excludes* a non-Windows
build. Only an explicit Windows condition counts:

  * `#ifdef Q_OS_WIN`, `#if defined(Q_OS_WIN)`, `#if defined(_WIN32)`, and the
    same as `#elif` -- exclusive;
  * `#ifndef Q_OS_MACOS` -- not exclusive, which is the whole point;
  * `#else` / `#elif` of a Windows branch -- not exclusive either, that is the
    macOS-and-Linux side.

Usage: python scripts/check-linux-windows-only-settings.py [--root .]
Exit: 0 clean, 1 an item is reachable on Linux or no longer exists.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CATALOG = Path("snow_shot/src/presentation/settings/settingscatalog.cpp")

# Settings whose payload describes a Windows capture backend. Adding one here is
# how a new Windows-only setting gets the same check.
WINDOWS_ONLY_IDS = (
    "screenshot.api-mode",
    "screen-recording.api-mode",
    "screenshot.window-element-api",
    "screenshot.restore-original-screen-colors",
)

DIRECTIVE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")
WINDOWS_CONDITION = re.compile(r"\bQ_OS_WIN\b|\b_WIN32\b|\b_WIN64\b")
# `SettingsItemDefinition screenshotApiModeItem() {` -- the builder an id lives in.
DEFINITION = re.compile(r"^\s*SettingsItemDefinition\s+([A-Za-z_]\w*)\s*\(")


def describe(stack: list[tuple[str, str, bool]]) -> str:
    """Render the open guards for an error message."""
    if not stack:
        return "none"
    rendered = []
    for directive, condition, _ in stack:
        condition = condition.strip()
        if directive == "ifdef":
            rendered.append(f"defined({condition})")
        elif directive == "ifndef":
            rendered.append(f"!defined({condition})")
        elif directive == "else":
            rendered.append("#else")
        else:
            rendered.append(condition)
    return str(rendered)


def is_windows_branch(directive: str, condition: str) -> bool:
    """True when this branch is taken on Windows and never otherwise."""
    condition = condition.strip()
    if directive == "ifdef":
        return bool(WINDOWS_CONDITION.search(condition)) and "!" not in condition
    if directive == "ifndef":
        # `#ifndef Q_OS_WIN` is the opposite branch: taken everywhere but Windows.
        return False
    if directive == "if":
        # `#if defined(Q_OS_WIN)` only; a compound condition may also hold on
        # another platform, so require that nothing else can satisfy it.
        return bool(re.fullmatch(r"defined\s*\(?\s*(Q_OS_WIN|_WIN32|_WIN64)\s*\)?", condition))
    if directive == "elif":
        return bool(re.fullmatch(r"defined\s*\(?\s*(Q_OS_WIN|_WIN32|_WIN64)\s*\)?", condition))
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root (default: cwd)")
    args = parser.parse_args()
    root = Path(args.root).resolve()

    catalog = root / CATALOG
    if not catalog.is_file():
        print(f"missing {catalog}", file=sys.stderr)
        return 2

    lines = catalog.read_text(encoding="utf-8").splitlines()
    # One entry per open `#if`; `#elif`/`#else` replace the entry for the branch
    # that is currently active, because only that branch matters here.
    stack: list[tuple[str, str, bool]] = []
    stack_at: list[list[tuple[str, str, bool]]] = []
    seen: set[str] = set()
    builders: dict[str, int] = {}
    last_builder: str | None = None
    failures: list[str] = []

    for number, line in enumerate(lines, 1):
        definition_match = DEFINITION.match(line)
        if definition_match:
            last_builder = definition_match.group(1)
        directive_match = DIRECTIVE.match(line)
        if directive_match:
            directive, condition = directive_match.group(1), directive_match.group(2)
            if directive == "endif":
                if stack:
                    stack.pop()
                else:
                    failures.append(f"{CATALOG}:{number}: #endif without #if")
            elif directive == "else":
                if not stack:
                    failures.append(f"{CATALOG}:{number}: #else without #if")
                else:
                    stack[-1] = ("else", "", False)
            elif directive == "elif":
                if not stack:
                    failures.append(f"{CATALOG}:{number}: #elif without #if")
                else:
                    stack[-1] = ("elif", condition, is_windows_branch("elif", condition))
            else:
                stack.append((directive, condition, is_windows_branch(directive, condition)))
            stack_at.append(list(stack))
            continue
        stack_at.append(list(stack))

        for item_id in WINDOWS_ONLY_IDS:
            if f'QStringLiteral("{item_id}")' not in line:
                continue
            seen.add(item_id)
            if last_builder is None:
                failures.append(
                    f"{CATALOG}:{number}: '{item_id}' is not inside a "
                    f"`SettingsItemDefinition <name>(...)` body, so the call sites cannot be "
                    f"found"
                )
            else:
                builders.setdefault(last_builder, number)
            if not any(entry[2] for entry in stack):
                failures.append(
                    f"{CATALOG}:{number}: the definition of '{item_id}' is reachable when "
                    f"Q_OS_WIN is false (guards: {describe(stack)})"
                )

    # A guarded definition is not enough: reverting only the *call site* to
    # `#ifndef Q_OS_MACOS` puts the item back on Linux while the definition stays
    # inside its Windows branch. Every use of the builder has to be checked, and
    # that is the case this gate exists for.
    for number, line in enumerate(lines, 1):
        if DEFINITION.match(line):
            continue
        for builder in builders:
            if f"{builder}(" not in line:
                continue
            if not any(entry[2] for entry in stack_at[number - 1]):
                failures.append(
                    f"{CATALOG}:{number}: '{builder}()' is called where Q_OS_WIN is false "
                    f"(guards: {describe(stack_at[number - 1])})"
                )

    for item_id in WINDOWS_ONLY_IDS:
        if item_id not in seen:
            failures.append(
                f"{CATALOG}: no `QStringLiteral(\"{item_id}\")` -- the setting was renamed or "
                f"removed; update WINDOWS_ONLY_IDS so the check keeps covering it"
            )

    for failure in failures:
        print(f"FAIL {failure}")
    print(f"windows-only settings: {'OK' if not failures else f'{len(failures)} problem(s)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
