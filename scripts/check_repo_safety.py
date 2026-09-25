#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Amir Farhadi
"""
Repository Safety, Secret, Path Hygiene & Mod Metadata Checker for Caster Taskbar HUD
"""

import getpass
import os
import py_compile
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

IGNORED_DIRS = {
    ".git",
    ".vs",
    ".vscode",
    "bin",
    "obj",
    "build",
    "node_modules",
    ".ruff_cache",
    ".idea",
    "__pycache__",
    ".pytest_cache",
    ".venv",
    "venv",
    "env",
}

IGNORED_EXTENSIONS = {
    ".dll",
    ".exe",
    ".pdb",
    ".png",
    ".jpg",
    ".jpeg",
    ".gif",
    ".webp",
    ".mp4",
    ".ico",
    ".svg",
    ".pyc",
    ".pyo",
    ".pyd",
    ".woff",
    ".woff2",
    ".ttf",
}

# Regex patterns for private path hygiene
WINDOWS_USER_PATH_RE = re.compile(
    r"[A-Za-z]:(?:\\{1,4}|/)+(?:Users|Documents and Settings)(?:\\{1,4}|/)+",
    re.IGNORECASE,
)
USER_HOME_PATH_RE = re.compile(
    r"(?:[A-Za-z]:(?:\\{1,4}|/)+|/|\\{1,4})(?:Users|home|Documents and Settings)(?:\\{1,4}|/)+[A-Za-z0-9_.-]+(?:\\{1,4}|/)+",
    re.IGNORECASE,
)
UNIX_USER_PATH_RE = re.compile(r"^/(?:Users|home)/[A-Za-z0-9_.-]+(?:/|$)", re.IGNORECASE)
USER_FILE_URI_RE = re.compile(
    r"file:///(?:[A-Za-z]:/(?:Users|Documents and Settings|home)|(?:Users|home)/)",
    re.IGNORECASE,
)

try:
    CURRENT_USER = getpass.getuser()
    if CURRENT_USER and len(CURRENT_USER) > 1 and CURRENT_USER.lower() not in {"root", "runner", "github", "administrator", "system"}:
        ACTIVE_USER_PATH_RE = re.compile(
            rf"(?:\\{{1,4}}|/)+{re.escape(CURRENT_USER)}(?:\\{{1,4}}|/)+",
            re.IGNORECASE,
        )
    else:
        ACTIVE_USER_PATH_RE = None
except Exception:
    ACTIVE_USER_PATH_RE = None

# Secret & credential token patterns
SECRET_PATTERNS = [
    (re.compile(r"-----BEGIN (?:RSA|OPENSSH|EC|DSA|PGP)?\s?PRIVATE KEY-----"), "Private Key Header"),
    (re.compile(r"\b(?:sk|pk)_(?:live|test)_[0-9a-zA-Z]{24,}\b"), "API Key Token"),
    (re.compile(r"\bghp_[0-9a-zA-Z]{36}\b"), "GitHub Personal Access Token"),
    (re.compile(r"\bgithub_pat_[0-9a-zA-Z_]{50,}\b"), "Fine-Grained GitHub Token"),
    (re.compile(r"\beyJ[a-zA-Z0-9_\-]{20,}\.[a-zA-Z0-9_\-]{20,}\.[a-zA-Z0-9_\-]{20,}\b"), "JWT Token"),
    (re.compile(r"\b[A-Za-z0-9._%+-]+@(?!(?:example\.com|users\.noreply\.github\.com|domain\.com)\b)[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b", re.IGNORECASE), "Plain Email Address in Content"),
]


def check_file_hygiene(file_path: str, rel_path: str) -> list[str]:
    violations = []
    if rel_path.replace("\\", "/") == "scripts/check_repo_safety.py":
        return violations

    try:
        with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.read().splitlines()

        for line_no, line in enumerate(lines, start=1):
            if WINDOWS_USER_PATH_RE.search(line):
                violations.append(f"{rel_path}:{line_no}: Hardcoded Windows user directory path leak: {line.strip()[:100]}")
            if USER_HOME_PATH_RE.search(line):
                violations.append(f"{rel_path}:{line_no}: Hardcoded user profile directory path: {line.strip()[:100]}")
            if UNIX_USER_PATH_RE.search(line):
                violations.append(f"{rel_path}:{line_no}: Hardcoded Unix user home directory path: {line.strip()[:100]}")
            if ACTIVE_USER_PATH_RE and ACTIVE_USER_PATH_RE.search(line):
                violations.append(f"{rel_path}:{line_no}: Active OS user path component: {line.strip()[:100]}")
            if USER_FILE_URI_RE.search(line):
                violations.append(f"{rel_path}:{line_no}: Local user file URI (file:///): {line.strip()[:100]}")

            for pattern, desc in SECRET_PATTERNS:
                if pattern.search(line):
                    violations.append(f"{rel_path}:{line_no}: Potential secret/credential ({desc}): {line.strip()[:60]}...")

    except Exception as ex:
        violations.append(f"{rel_path}: Failed to read file ({ex})")

    return violations


def check_windhawk_mod_metadata() -> list[str]:
    violations = []
    mod_files = ["caster-taskbar-hud.wh.cpp", os.path.join("mods", "caster-taskbar-hud.wh.cpp")]
    verified_any = False

    for rel_mod in mod_files:
        full_path = os.path.join(REPO_ROOT, rel_mod)
        if not os.path.exists(full_path):
            continue

        verified_any = True
        try:
            with open(full_path, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()

            # Check header tags
            if "// ==WindhawkMod==" not in content or "// ==/WindhawkMod==" not in content:
                violations.append(f"{rel_mod}: Missing // ==WindhawkMod== header block.")
                continue

            header_match = re.search(r"// ==WindhawkMod==\s*(.*?)\s*// ==/WindhawkMod==", content, re.DOTALL)
            if not header_match:
                violations.append(f"{rel_mod}: Malformed WindhawkMod header block.")
                continue

            header_text = header_match.group(1)
            required_tags = [
                "@id",
                "@name",
                "@description",
                "@version",
                "@author",
                "@github",
                "@homepage",
                "@include",
                "@architecture",
                "@compilerOptions",
            ]

            for tag in required_tags:
                if not re.search(rf"//\s+{re.escape(tag)}\b", header_text):
                    violations.append(f"{rel_mod}: Missing required metadata tag '{tag}' in header block.")

            # Validate version semver
            ver_match = re.search(r"//\s+@version\s+([0-9]+\.[0-9]+\.[0-9]+)", header_text)
            if not ver_match:
                violations.append(f"{rel_mod}: @version must follow semantic versioning (e.g. 1.1.2).")

            # Validate Windhawk sections
            if "// ==WindhawkModReadme==" not in content:
                violations.append(f"{rel_mod}: Missing // ==WindhawkModReadme== section.")
            if "// ==WindhawkModSettings==" not in content:
                violations.append(f"{rel_mod}: Missing // ==WindhawkModSettings== section.")

        except Exception as e:
            violations.append(f"{rel_mod}: Error parsing mod metadata: {e}")

    if not verified_any:
        violations.append("No caster-taskbar-hud.wh.cpp file found in repository root or mods/ directory.")

    return violations


def check_python_scripts_syntax() -> list[str]:
    violations = []
    scripts_dir = os.path.join(REPO_ROOT, "scripts")
    if os.path.isdir(scripts_dir):
        for fname in os.listdir(scripts_dir):
            if fname.endswith(".py"):
                fpath = os.path.join(scripts_dir, fname)
                try:
                    py_compile.compile(fpath, doraise=True)
                except py_compile.PyCompileError as pe:
                    violations.append(f"Python syntax error in {fname}: {pe}")
                except Exception as ex:
                    violations.append(f"Failed to check syntax in {fname}: {ex}")
    return violations


def check_git_tracked_binaries() -> list[str]:
    violations = []
    try:
        res = subprocess.run(
            ["git", "ls-files"],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True,
        )
        for tracked in res.stdout.splitlines():
            ext = os.path.splitext(tracked)[1].lower()
            if ext in {".pyc", ".pyo", ".pyd", ".dll", ".exe", ".so", ".dylib", ".pdb", ".obj", ".o"}:
                violations.append(f"Tracked binary/compiled file in git: {tracked}")
            if tracked.startswith(".venv/") or tracked.startswith("venv/"):
                violations.append(f"Tracked virtual environment file in git: {tracked}")
    except Exception:
        pass
    return violations


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    print(f"Scanning repository for safety, secrets, path hygiene, and mod metadata: {REPO_ROOT}")
    all_violations = []

    # 1. Mod metadata validation
    print("Checking Windhawk mod metadata integrity...")
    all_violations.extend(check_windhawk_mod_metadata())

    # 2. Python scripts syntax check
    print("Checking Python script syntax...")
    all_violations.extend(check_python_scripts_syntax())

    # 3. Git tracked binaries
    print("Checking git tracked file hygiene...")
    all_violations.extend(check_git_tracked_binaries())

    # 4. Text file content scan
    print("Scanning text files for private paths and secrets...")
    for root, dirs, files in os.walk(REPO_ROOT):
        dirs[:] = [d for d in dirs if d not in IGNORED_DIRS]

        for file in files:
            ext = os.path.splitext(file)[1].lower()
            if ext in IGNORED_EXTENSIONS:
                continue

            file_path = os.path.join(root, file)
            rel_path = os.path.relpath(file_path, REPO_ROOT).replace("\\", "/")
            violations = check_file_hygiene(file_path, rel_path)
            all_violations.extend(violations)

    if all_violations:
        print(f"\n[FAILED] Found {len(all_violations)} hygiene violation(s):\n")
        for v in all_violations:
            print(f"  - {v}")
        print("\nPlease resolve the above issues before committing or publishing.")
        return 1

    print("\n[PASSED] Zero path leaks, secrets, binary artifacts, or metadata inconsistencies detected.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
