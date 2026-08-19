# UE4SS MCP Server

External MCP gateway for the UE4SS in-process MCP bridge.

## UE4SS configuration

Enable the bridge in `UE4SS-settings.ini`:

```ini
[MCP]
Enabled = 1
AllowLuaEval = 1
```

Leave `PipeName` empty to let UE4SS use `UE4SS-MCP-<pid>`, or set a fixed local pipe name.

## Build

```powershell
npm install
npm run build
```

## stdio transport

Use this mode from an MCP host that launches servers as subprocesses:

```powershell
node .\dist\index.js --pipe UE4SS-MCP-12345
```

You can also use environment variables:

```powershell
$env:UE4SS_MCP_PIPE = "UE4SS-MCP-12345"
node .\dist\index.js
```

The stdio transport writes only MCP JSON-RPC messages to stdout. Logs go to stderr.

## local HTTP transport

```powershell
node .\dist\index.js --transport http --http-port 8765 --pipe UE4SS-MCP-12345
```

The HTTP server binds to `127.0.0.1` and serves `/mcp` without authentication.

## Tools

- `ue4ss.ping`
- `ue4ss.search_objects`
- `ue4ss.inspect_object`
- `ue4ss.get_property`
- `ue4ss.set_property`
- `ue4ss.call_function`
- `ue4ss.exec_console`
- `ue4ss.reload_mod`
- `ue4ss.watch_function`
- `ue4ss.unwatch`
- `ue4ss.events_recent`
- `ue4ss.run_lua`

`ue4ss.run_lua` is a developer-mode capability and is audited in the UE4SS log.

Every request is dispatched to GameThread before Lua executes. The response reports
`gameThread: true` as the actual execution context and echoes the deprecated input
as `requestedGameThread`; the input no longer creates a second deferred queue hop,
so Lua return values remain synchronous for both `true` and `false` callers.
