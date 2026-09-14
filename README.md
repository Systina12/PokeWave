# PokeWave

[![Build](https://github.com/Systina12/PokeWave/actions/workflows/build.yml/badge.svg)](https://github.com/Systina12/PokeWave/actions/workflows/build.yml)
[![Latest Release](https://img.shields.io/github/v/release/Systina12/PokeWave?display_name=tag)](https://github.com/Systina12/PokeWave/releases)

> **中文：** 面向自有或获授权 TeamSpeak 3 服务器的可控多目标 Poke 测试插件。  
> **English:** A controllable multi-target poke testing plugin for TeamSpeak 3 servers you administer.

---

## 中文

PokeWave 是一个 TeamSpeak 3 原生客户端插件，用于在你管理的服务器上进行可控的 Poke/戳一戳功能测试。它使用 TeamSpeak 官方客户端插件 API 发送 Poke 请求，服务器权限和自身的 Flood Protection 仍然有效。

### 功能

- Windows 原生 GUI，从顶部菜单 `Plugins → PokeWave GUI` 打开。
- 刷新当前可见客户端，并通过复选框多选目标。
- 可调 Poke 速率、总次数和消息内容。
- 支持 `/pokewave` 命令，结果会显示在当前 TeamSpeak 标签页。
- 可随时停止，断开服务器或卸载插件时会停止任务。
- 项目不人为设置速率和次数上限；速率必须为正的有限数，次数为正的 `uint64`。
- 使用官方 `requestClientPoke` API，最终是否允许发送由服务器权限决定。

### 安装

当前版本：[v0.3.2](https://github.com/Systina12/PokeWave/releases/tag/v0.3.2)

1. 完全退出 TeamSpeak 3。
2. 下载 [PokeWave-windows-x64.zip](https://github.com/Systina12/PokeWave/releases/download/v0.3.2/PokeWave-windows-x64.zip)。
3. 解压 `pokewave.dll`。
4. 将 DLL 放入：

   ```text
   %APPDATA%\\TS3Client\\plugins
   ```

5. 确认机器上没有其他旧版 `pokewave.dll`，然后启动 TeamSpeak 3。
6. 从顶部菜单打开 `Plugins → PokeWave GUI`。

插件面板不会放在 Settings/Configure 中。当前插件面向 TeamSpeak 3 x64 客户端，TeamSpeak 5 不适用。

### 命令

在 TeamSpeak 3 的聊天输入框执行：

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

参数说明：

- `list`：列出当前服务器上插件能够看到的客户端。
- `select`：清空当前选择并选择指定 Client ID。
- `add/remove`：增加或移除目标。
- `speed`：设置每秒 Poke 操作数。
- `count`：设置总发送次数。
- `message`：设置 Poke 消息。
- `start/stop/status`：开始、停止或查看任务状态。

目标按轮询顺序发送。例如选择 12、13、14 后，会依次发送给 12 → 13 → 14 → 12。

### GUI

GUI 左侧显示可见客户端和 Client ID：

1. 点击“刷新”获取最新客户端列表。
2. 勾选目标，或使用“全选/清空”。
3. 在右侧填写速率、次数和消息。
4. 点击“开始”启动，点击“停止”终止。
5. 状态区域会显示当前进度和错误。

### 权限与安全

PokeWave 面向你拥有管理权限或明确获授权的服务器测试。高频操作可能触发服务器 Flood Protection、权限拒绝或客户端/服务器限流。插件不会绕过 TeamSpeak 权限，也不会自动连接服务器或自动开始任务。

### 构建

需要官方 TeamSpeak 插件 SDK：

```bash
git clone --depth=1 https://github.com/teamspeak/ts3client-pluginsdk.git /tmp/ts3client-pluginsdk
cmake -S . -B build -DTS3_SDK_DIR=/tmp/ts3client-pluginsdk
cmake --build build --config Release
```

运行测试：

```bash
ctest --test-dir build --output-on-failure
```

GitHub Actions 会构建 Linux x64 和 Windows x64 产物，并验证命令回显、GUI 回调、Windows UTF-8 编译以及 TeamSpeak 3 客户端加载。

---

## English

PokeWave is a native TeamSpeak 3 client plugin for controlled poke testing on servers you administer. It uses the official TeamSpeak client plugin API to submit poke requests. Server permissions and Flood Protection remain in effect.

### Features

- Native Windows GUI opened from the top-level `Plugins → PokeWave GUI` menu.
- Refreshes visible clients and supports checkbox-based multi-selection.
- Configurable poke rate, total count, and message.
- `/pokewave` command interface with responses printed to the current TeamSpeak tab.
- Immediate stop support; disconnecting or unloading the plugin stops active work.
- No project-defined upper bound for rate or count; rate must be positive and finite, and count must be a positive `uint64`.
- Uses the official `requestClientPoke` API; the server still decides whether the request is permitted.

### Installation

Current release: [v0.3.2](https://github.com/Systina12/PokeWave/releases/tag/v0.3.2)

1. Exit TeamSpeak 3 completely.
2. Download [PokeWave-windows-x64.zip](https://github.com/Systina12/PokeWave/releases/download/v0.3.2/PokeWave-windows-x64.zip).
3. Extract `pokewave.dll`.
4. Copy the DLL to:

   ```text
   %APPDATA%\\TS3Client\\plugins
   ```

5. Make sure no older duplicate `pokewave.dll` is being loaded, then start TeamSpeak 3.
6. Open `Plugins → PokeWave GUI` from the top menu.

The control panel is deliberately not exposed through Settings/Configure. This plugin targets the TeamSpeak 3 x64 client and does not support TeamSpeak 5.

### Commands

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

Command reference:

- `list`: lists clients visible to the plugin on the current server.
- `select`: clears the current selection and selects the given Client IDs.
- `add/remove`: adds or removes targets.
- `speed`: sets poke operations per second.
- `count`: sets the total number of requests.
- `message`: sets the poke message.
- `start/stop/status`: starts, stops, or reports the current task.

Targets are sent in round-robin order. For example, selecting 12, 13, and 14 sends requests to 12 → 13 → 14 → 12.

### GUI

The left side of the GUI shows visible clients and their Client IDs:

1. Click “Refresh” to load the latest client list.
2. Check targets, or use “Select All/Clear”.
3. Enter the rate, count, and message on the right.
4. Click “Start” to begin or “Stop” to terminate.
5. Monitor progress and errors in the status area.

### Permissions and safety

PokeWave is intended for testing servers you administer or have explicit authorization to test. High-frequency operations may trigger server Flood Protection, permission errors, or client/server rate limiting. The plugin does not bypass TeamSpeak permissions, connect to servers automatically, or start tasks on its own.

### Building

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
