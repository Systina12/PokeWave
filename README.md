# PokeWave

TeamSpeak 3 native client plugin for controlled poke testing on a server you administer.

## Features

- Windows native Win32 GUI opened from the TeamSpeak global plugin menu.
- Refreshes visible clients and supports multi-select with checkboxes.
- Configures poke rate, total count and message from the GUI.
- Command interface remains available through `/pokewave`.
- No project-defined upper cap on rate or count. Rate must be positive and finite; count is a positive uint64 value.
- Stop button, `stop` command, disconnect handling and plugin unload stop active work.

## Windows installation

1. Download `PokeWave-windows-x64.zip` from the v0.3.1 Release.
2. Extract `pokewave.dll`.
3. Copy it to `%APPDATA%\\TS3Client\\plugins`.
4. Restart TeamSpeak 3.
5. Open the global plugin menu and choose `PokeWave GUI`. The same window is available from the plugin Configure button.

The plugin calls the official TeamSpeak 3 client `requestClientPoke` API. The server still decides whether the account has permission and may apply its own flood protection.

## Commands

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

Speed is expressed in poke operations per second. Targets are sent in round-robin order.

## Build

Clone the official TeamSpeak plugin SDK and configure with CMake:

```bash
git clone --depth=1 https://github.com/TeamSpeak-Systems/ts3client-pluginsdk.git /tmp/ts3client-pluginsdk
cmake -S . -B build -DTS3_SDK_DIR=/tmp/ts3client-pluginsdk
cmake --build build --config Release
```

GitHub Actions builds Linux x64 and Windows x64 artifacts. The command keyword is registered as `/pokewave`. The Windows job also downloads TeamSpeak 3 Client 3.6.2, starts it and verifies that the client loads `pokewave.dll`.
