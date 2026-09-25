# Caster & ADCE Taskbar HUD

[![CI & Safety Hygiene](https://github.com/amirf147/caster-taskbar-hud/actions/workflows/ci.yml/badge.svg)](https://github.com/amirf147/caster-taskbar-hud/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20(x64)-0078D6.svg)](https://www.microsoft.com/windows)
[![Windhawk Mod](https://img.shields.io/badge/Windhawk-Mod-blueviolet.svg)](https://windhawk.net/)

A high-performance [Windhawk](https://windhawk.net/) mod that injects real-time voice programming telemetry, recognized speech commands, active grammar rules, and [Active Desktop Context Engine (ADCE)](https://github.com/amirf147/active-desktop-context-engine) semantic zones directly into the Windows 11 Taskbar adjacent to the System Tray.

Eliminates the need for screen-obscuring desktop overlay windows by embedding telemetry natively into Explorer's XAML layout.

---

## Key Features

- **Live Status Dot Indicator**: Instant visual feedback for microphone and speech engine states:
  - 🟢 **Emerald Green**: Microphone awake and listening.
  - 🔴 **Crimson Red**: Microphone sleeping / muted.
  - 🟡 **Amber**: Streaming active speech hypotheses.
- **Rotating Carousel Mode (Default)**:
  - While speaking, immediately displays streaming hypotheses and recognized commands.
  - When idle, smoothly cycles through the semantic ADCE Zone (purple-themed), active contextual CCR rules (blue-themed), and the last recognized command.
- **Multi-Box Discrete Pills Mode**:
  - Side-by-side mode rendering 3 distinct, color-coded pills simultaneously for Zone, Rules, and Command.
- **Unified Single Strip Mode**:
  - Compact static text strip optimized for dense taskbars.
- **In-Situ Context Menu**:
  - Right-click directly on the HUD on your taskbar to toggle display modes, individual component visibility, or open settings without navigating external configuration apps.
  - Runtime selections persist across shell restarts via HKCU registry overrides.
- **Zero-Latency Named Pipe IPC**:
  - Runs an asynchronous Win32 Named Pipe server on `\\.\pipe\CasterTaskbarHud`.
  - Telemetry packets are parsed and dispatched to WinRT UI threads in `< 0.5ms`.
- **Native Fluent XAML Integration**:
  - Injected directly into Windows 11's `SystemTrayFrameGrid` via `taskbar.dll` hooks.
  - Automatic light/dark theme adaptation, DPI scaling, and multi-monitor secondary taskbar support.
  - Safe teardown: fully unhooks and cleans up UI elements on mod unload or shell restart.

---

## Architecture Overview

```
+------------------------------------+
|  Caster Voice Framework / Engine   |
|  (Dragonfly, Kaldi, WSR, etc.)     |
+-----------------+------------------+
                  |
                  v
+------------------------------------+
|   Caster 'taskbar_hud' Plugin      |  (Installed from caster-plugins)
|   - Context Resolver (AST Rules)   |
|   - ADCE Focus Tracker             |
|   - Named Pipe IPC Client          |
+-----------------+------------------+
                  |  JSON over Win32 Named Pipe
                  |  \\.\pipe\CasterTaskbarHud
                  v
+------------------------------------+
|   Caster & ADCE Taskbar HUD Mod    |  (Windhawk Mod in explorer.exe)
|   - Named Pipe Server Thread       |
|   - Taskbar Hook (taskbar.dll)     |
|   - WinRT XAML Elements Injected   |
|     into SystemTrayFrameGrid       |
+------------------------------------+
```

---

## Installation & Setup

### Step 1: Install Windhawk
Download and install [Windhawk](https://windhawk.net/) (Windows 11 22H2 or higher).

### Step 2: Install the Taskbar HUD Mod
1. Open the **Windhawk** application.
2. Navigate to **Advanced Settings** &rarr; **Developer Mode** &rarr; Enable Developer Mode.
3. Click **New Mod**, replace the editor contents with the code from [`caster-taskbar-hud.wh.cpp`](caster-taskbar-hud.wh.cpp), and click **Compile and run**.
4. The mod will compile using the built-in Clang toolchain and inject the HUD directly into your taskbar.

### Step 3: Install the Companion Caster Plugin
Install the companion bridge from the [caster-plugins](https://github.com/amirf147/caster-plugins) repository:

```powershell
py -3.10 -m castervoice.bin.plugin_cli install taskbar_hud
```

The plugin will automatically activate in `%LOCALAPPDATA%\caster\caster_user_content\plugins\taskbar_hud` and stream speech events on startup.

---

## Standalone Test Harness

You can test and preview the HUD immediately without having Caster, Dragon, or Kaldi running.

A zero-dependency Python Win32 test harness is included in [`scripts/test_taskbar_hud_stream.py`](scripts/test_taskbar_hud_stream.py):

### Run Live Simulation Demo
```powershell
python scripts/test_taskbar_hud_stream.py
```
This runs an automated sequence simulating:
1. Streaming voice hypotheses (`form...` &rarr; `format document`)
2. ADCE zone transitions (`Editor` &rarr; `Terminal` &rarr; `Source Control`)
3. Contextual rule updates (`Python` &rarr; `Git` &rarr; `Global`)
4. Microphone sleep & wake cycles (`sleeping` &rarr; `recognized`)
5. Auto-clear idle hold timer

### Send Individual Packets
```powershell
# Send custom command
python scripts/test_taskbar_hud_stream.py --command "format document" --rules "Python" --zone "VS Code" --status recognized

# Test sleeping mic indicator
python scripts/test_taskbar_hud_stream.py --command "Microphone Sleeping" --status sleeping

# Test live speech streaming
python scripts/test_taskbar_hud_stream.py --command "git push..." --status streaming
```

---

## Configuration Settings

The mod exposes the following settings in Windhawk:

| Setting | Default | Description |
|---|---|---|
| `displayMode` | `rotate` | `rotate` (Carousel), `unified` (Single Strip), or `multi_box` (Discrete Pills) |
| `showAdceZone` | `true` | Show or hide the ADCE semantic zone indicator |
| `showActiveRules` | `true` | Show or hide active contextual grammar rules |
| `showCommandStream` | `true` | Show or hide voice command strings and hypotheses |
| `carouselIntervalSec` | `4` | Seconds before rotating to the next indicator when idle |
| `commandClearTimeoutSec` | `6` | Seconds to hold recognized command before resuming idle carousel |
| `customPipeName` | `CasterTaskbarHud` | Custom Named Pipe name (`\\.\pipe\<name>`) |
| `maxCommandLength` | `35` | Maximum character length before truncating command text |

---

## Repository Safety & Quality Gates

This repository enforces strict open-source safety and hygiene standards:
- **Zero Path Leaks**: Scanned for hardcoded personal user directories or environment paths.
- **Zero Secret Leakage**: Scanned for private keys, API tokens, and credentials.
- **Mod Metadata Validation**: Validates all `@id`, `@name`, `@version`, `@author`, and `@compilerOptions` tags against Windhawk standards.
- **Automated CI**: Run locally via:
  ```powershell
  python scripts/check_repo_safety.py
  ```

---

## Ecosystem Links

- **Companion Caster Plugins**: [amirf147/caster-plugins](https://github.com/amirf147/caster-plugins)
- **Active Desktop Context Engine**: [amirf147/active-desktop-context-engine](https://github.com/amirf147/active-desktop-context-engine)
- **Caster Core**: [dictation-toolbox/Caster](https://github.com/dictation-toolbox/Caster)

---

## License

Apache License 2.0 &copy; 2026 Amir Farhadi. See [LICENSE](LICENSE) for details.
