# PokeWave

TeamSpeak 3 原生客户端 Poke 测试插件，使用官方 Client Plugin SDK 的 `requestClientPoke`。

当前版本不设置速率和次数的业务上限：速率必须是正的有限数；总次数必须是大于 0 且能放入 `uint64` 的有限整数。任务可随时 `/pokewave stop`，断线和插件卸载会自动停止。

## 构建

```bash
git clone --depth=1 https://github.com/teamspeak/ts3client-pluginsdk.git
cmake -S . -B build -DTS3_SDK_DIR=/path/to/ts3client-pluginsdk
cmake --build build --config Release
```

`TS3_SDK_DIR` 必须指向包含 `include/ts3_functions.h` 的 SDK 根目录。

## 使用

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

`select` 替换多选列表，`add` 追加目标；client ID 来自 `list`。

高频测试只应在自己管理的服务器和明确同意的测试客户端上使用。服务器仍然会执行 poke 权限和 flood protection。

TeamSpeak SDK：https://github.com/teamspeak/ts3client-pluginsdk