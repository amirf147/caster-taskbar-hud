#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Amir Farhadi

"""
Interactive / Automated Test Harness for Caster & ADCE Taskbar HUD

Streams mock speech hypotheses, recognized voice commands, rule transitions,
and ADCE zone switches directly to \\\\.\\pipe\\CasterTaskbarHud to verify:
1. Native XAML UI rendering in Windows 11 Taskbar.
2. Low-latency IPC throughput.
3. State coloring (Streaming Amber -> Recognized Green -> Sleeping Red).
4. Auto-clear timer back to Ready.
"""

import argparse
import ctypes
from ctypes import wintypes
import json
import os
import sys
import time

GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

kernel32 = ctypes.windll.kernel32


def send_packet(pipe_name: str, payload: dict) -> bool:
    pipe_path = f"\\\\.\\pipe\\{pipe_name}"
    handle = kernel32.CreateFileW(
        pipe_path,
        GENERIC_WRITE,
        0,
        None,
        OPEN_EXISTING,
        0,
        None
    )
    if handle == INVALID_HANDLE_VALUE or handle == 0:
        return False

    try:
        raw = json.dumps(payload).encode("utf-8")
        written = wintypes.DWORD(0)
        ok = kernel32.WriteFile(handle, raw, len(raw), ctypes.byref(written), None)
        return bool(ok)
    finally:
        kernel32.CloseHandle(handle)


def run_demo(pipe_name: str):
    print(f"[*] Connecting to Taskbar HUD Named Pipe at \\\\.\\pipe\\{pipe_name}...")

    # Test initial connection
    if not send_packet(pipe_name, {"command": "Connected", "rules": "Global", "adce_zone": "Init", "status": "recognized"}):
        print(f"[-] ERROR: Could not connect to \\\\.\\pipe\\{pipe_name}.")
        print("    Make sure the Windhawk mod 'Caster & ADCE Taskbar HUD' is compiled and running in Windhawk!")
        sys.exit(1)

    print("[+] Successfully connected! Streaming live simulation sequence...")
    time.sleep(1.0)

    scenarios = [
        # Scenario 1: Streaming voice command
        {"adce_zone": "Editor", "rules": "Python", "command": "form...", "status": "streaming"},
        {"adce_zone": "Editor", "rules": "Python", "command": "format doc...", "status": "streaming"},
        {"adce_zone": "Editor", "rules": "Python", "command": "format document", "status": "recognized"},

        # Scenario 2: Zone change via ADCE
        {"adce_zone": "Terminal", "rules": "Terminal, Bash", "command": "Ready", "status": "idle"},
        {"adce_zone": "Terminal", "rules": "Terminal, Bash", "command": "run...", "status": "streaming"},
        {"adce_zone": "Terminal", "rules": "Terminal, Bash", "command": "run pytest", "status": "recognized"},

        # Scenario 3: Git command in Git view
        {"adce_zone": "Source Control", "rules": "Git", "command": "git push...", "status": "streaming"},
        {"adce_zone": "Source Control", "rules": "Git", "command": "git push origin main", "status": "recognized"},

        # Scenario 4: Sleeping microphone
        {"adce_zone": "Desktop", "rules": "Global", "command": "Microphone Sleeping", "status": "sleeping"},

        # Scenario 5: Wake up
        {"adce_zone": "Editor", "rules": "VS Code, Global", "command": "Listening", "status": "recognized"},
    ]

    for step, item in enumerate(scenarios, 1):
        print(f"  [{step}/{len(scenarios)}] Push: Zone='{item['adce_zone']}', Rules='{item['rules']}', Cmd='{item['command']}', Status={item['status']}")
        send_packet(pipe_name, item)
        time.sleep(1.5)

    print("\n[+] Demo sequence complete! Testing auto-clear timer (waiting 5 seconds)...")
    time.sleep(5.0)
    print("[+] Done.")


def main():
    parser = argparse.ArgumentParser(description="Test harness for Caster & ADCE Taskbar HUD")
    parser.add_argument("--pipe", default="CasterTaskbarHud", help="Named pipe name (default: CasterTaskbarHud)")
    parser.add_argument("--command", "-c", help="Send single command text")
    parser.add_argument("--rules", "-r", help="Send single rules text")
    parser.add_argument("--zone", "-z", help="Send single ADCE zone text")
    parser.add_argument("--status", "-s", default="recognized", choices=["idle", "streaming", "recognized", "sleeping", "error"], help="Recognition status")

    args = parser.parse_args()

    if args.command or args.rules or args.zone:
        payload = {}
        if args.command: payload["command"] = args.command
        if args.rules: payload["rules"] = args.rules
        if args.zone: payload["adce_zone"] = args.zone
        payload["status"] = args.status
        ok = send_packet(args.pipe, payload)
        if ok:
            print(f"[+] Sent: {payload}")
        else:
            print(f"[-] Failed to connect to \\\\.\\pipe\\{args.pipe}")
            sys.exit(1)
    else:
        run_demo(args.pipe)


if __name__ == "__main__":
    main()
