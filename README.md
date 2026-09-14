# PokeWave

[![Build](https://github.com/Systina12/PokeWave/actions/workflows/build.yml/badge.svg)](https://github.com/Systina12/PokeWave/actions/workflows/build.yml)
[![Latest Release](https://img.shields.io/github/v/release/Systina12/PokeWave?display_name=tag)](https://github.com/Systina12/PokeWave/releases)
[简体中文](README.zh-CN.md)

> A controllable multi-target poke testing plugin for TeamSpeak 3 servers you administer.

PokeWave is a native TeamSpeak 3 client plugin for controlled poke testing on servers you administer. It uses the official TeamSpeak client plugin API to submit poke requests. Server permissions and Flood Protection remain in effect.

## Features

- Native Windows GUI opened from the top-level `Plugins → PokeWave GUI` menu.
- Refreshes visible clients and supports checkbox-based multi-selection.
- Configurable poke rate, total count, and message.
- `/pokewave` command interface with responses printed to the current TeamSpeak tab.
- Immediate stop support; disconnecting or unloading the plugin stops active work.
- No project-defined upper bound for rate or count; rate must be positive and finite, and count must be a positive `uint64`.
- Uses the official `requestClientPoke` API; the server still decides whether the request is permitted.

## Installation

Current release: [v0.3.2](https://github.com/Systina12/PokeWave/releases/tag/v0.3.2)

1. Exit TeamSpeak 3 completely.
2. Download [PokeWave-windows-x64.zip](https://github.com/Systina12/PokeWave/releases/download/v0.3.2/PokeWave-windows-x64.zip).
3. Extract `pokewave.dll`.
4. Copy the DLL to:

   ```text
   %APPDATA%\TS3Client\plugins
   ```

5. Make sure no older duplicate `pokewave.dll` is being loaded, then start TeamSpeak 3.
6. Open `Plugins → PokeWave GUI` from the top menu.

The control panel is deliberately not exposed through Settings/Configure. This plugin targets the TeamSpeak 3 x64 client and does not support TeamSpeak 5.

## Commands

Run these commands in the TeamSpeak 3 chat input:

```text
/pokewave help
/pokewave list
/pokewave select 12,13,14
/pokewave add 15
/pokewave remove 13
/pokewave speed 500
/pokewave count 1000000
/pokewave message controlled test
/pokewave start
/pokewave status
/pokewave stop
```

- `list`: lists clients visible to the plugin on the current server.
- `select`: clears the current selection and selects the given Client IDs.
- `add/remove`: adds or removes targets.
- `speed`: sets poke operations per second.
- `count`: sets the total number of requests.
- `message`: sets the poke message.
- `start/stop/status`: starts, stops, or reports the current task.

Targets are sent in round-robin order. For example, selecting 12, 13, and 14 sends requests to 12 → 13 → 14 → 12.

## GUI

The left side of the GUI shows visible clients and their Client IDs:

1. Click **Refresh** to load the latest client list.
2. Check targets, or use **Select All/Clear**.
3. Enter the rate, count, and message on the right.
4. Click **Start** to begin or **Stop** to terminate.
5. Monitor progress and errors in the status area.

## Permissions and safety

PokeWave is intended for testing servers you administer or have explicit authorization to test. High-frequency operations may trigger server Flood Protection, permission errors, or client/server rate limiting. The plugin does not bypass TeamSpeak permissions, connect to servers automatically, or start tasks on its own.

## Building

Use the official TeamSpeak plugin SDK:

```bash
git clone --depth=1 https://github.com/teamspeak/ts3client-pluginsdk.git /tmp/ts3client-pluginsdk
cmake -S . -B build -DTS3_SDK_DIR=/tmp/ts3client-pluginsdk
cmake --build build --config Release
```

Run the tests:

```bash
ctest --test-dir build --output-on-failure
```

GitHub Actions builds Linux x64 and Windows x64 artifacts, and verifies command feedback, the GUI callback, Windows UTF-8 compilation, and loading inside the TeamSpeak 3 client.
