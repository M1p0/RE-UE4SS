#!/usr/bin/env node

import http from "node:http";
import net from "node:net";
import { randomUUID } from "node:crypto";
import { McpServer, ResourceTemplate } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { z } from "zod";

type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };

type BridgeResponse =
  | { id: string; ok: true; result: JsonValue }
  | { id: string; ok: false; error: { code: string; message: string } };

type CliConfig = {
  pipe: string;
  token: string;
  transport: "stdio" | "http";
  httpPort: number;
  httpToken?: string;
};

const REQUEST_TIMEOUT_MS = 10_000;

function argValue(name: string): string | undefined {
  const args = process.argv.slice(2);
  for (let i = 0; i < args.length; i += 1) {
    if (args[i] === name) return args[i + 1];
    if (args[i]?.startsWith(`${name}=`)) return args[i].slice(name.length + 1);
  }
  return undefined;
}

function normalizePipePath(pipe: string): string {
  if (pipe.startsWith("\\\\.\\pipe\\")) return pipe;
  return `\\\\.\\pipe\\${pipe}`;
}

function readConfig(): CliConfig {
  const pipeFromPid = argValue("--pid") ?? process.env.UE4SS_MCP_PID;
  const pipe =
    argValue("--pipe") ??
    process.env.UE4SS_MCP_PIPE ??
    (pipeFromPid ? `UE4SS-MCP-${pipeFromPid}` : undefined);
  const token = argValue("--token") ?? process.env.UE4SS_MCP_TOKEN;
  const transport = (argValue("--transport") ?? process.env.UE4SS_MCP_TRANSPORT ?? "stdio").toLowerCase();
  const httpPort = Number(argValue("--http-port") ?? process.env.UE4SS_MCP_HTTP_PORT ?? "8765");
  const httpToken = argValue("--http-token") ?? process.env.UE4SS_MCP_HTTP_TOKEN;

  if (!pipe) {
    throw new Error("Missing UE4SS MCP pipe. Pass --pipe UE4SS-MCP-<pid> or set UE4SS_MCP_PIPE.");
  }
  if (!token) {
    throw new Error("Missing UE4SS MCP token. Pass --token <token> or set UE4SS_MCP_TOKEN.");
  }
  if (transport !== "stdio" && transport !== "http") {
    throw new Error(`Unsupported transport '${transport}'. Use stdio or http.`);
  }
  if (!Number.isInteger(httpPort) || httpPort <= 0 || httpPort > 65535) {
    throw new Error("Invalid --http-port.");
  }

  return {
    pipe: normalizePipePath(pipe),
    token,
    transport,
    httpPort,
    httpToken,
  };
}

class UE4SSBridgeClient {
  constructor(private readonly pipePath: string, private readonly token: string) {}

  call(method: string, params: Record<string, unknown> = {}): Promise<JsonValue> {
    const id = randomUUID();
    const request = JSON.stringify({ id, token: this.token, method, params }) + "\n";

    return new Promise<JsonValue>((resolve, reject) => {
      const socket = net.createConnection(this.pipePath);
      let buffer = "";
      const timeout = setTimeout(() => {
        socket.destroy();
        reject(new Error(`UE4SS bridge request timed out: ${method}`));
      }, REQUEST_TIMEOUT_MS);

      socket.on("connect", () => {
        socket.write(request, "utf8");
      });

      socket.on("data", (chunk: Buffer) => {
        buffer += chunk.toString("utf8");
        const newline = buffer.indexOf("\n");
        if (newline === -1) return;

        const line = buffer.slice(0, newline).trim();
        clearTimeout(timeout);
        socket.end();

        try {
          const response = JSON.parse(line) as BridgeResponse;
          if (!response.ok) {
            reject(new Error(`${response.error.code}: ${response.error.message}`));
            return;
          }
          resolve(response.result);
        } catch (error) {
          reject(error);
        }
      });

      socket.on("error", (error) => {
        clearTimeout(timeout);
        reject(error);
      });

      socket.on("close", () => {
        clearTimeout(timeout);
      });
    });
  }
}

function asText(result: JsonValue) {
  return {
    content: [
      {
        type: "text" as const,
        text: JSON.stringify(result, null, 2),
      },
    ],
  };
}

function asResource(uri: URL, result: JsonValue) {
  return {
    contents: [
      {
        uri: uri.href,
        mimeType: "application/json",
        text: JSON.stringify(result, null, 2),
      },
    ],
  };
}

function registerTools(server: McpServer, bridge: UE4SSBridgeClient) {
  server.registerTool(
    "ue4ss.ping",
    {
      title: "Ping UE4SS",
      description: "Check that the UE4SS MCP bridge is reachable.",
    },
    async () => asText(await bridge.call("ping")),
  );

  server.registerTool(
    "ue4ss.search_objects",
    {
      title: "Search UObjects",
      description: "Search live Unreal UObjects by full name and optional class name.",
      inputSchema: {
        query: z.string().default(""),
        className: z.string().optional(),
        limit: z.number().int().positive().optional(),
        cursor: z.number().int().nonnegative().optional(),
      },
    },
    async (args) => asText(await bridge.call("objects.search", args)),
  );

  server.registerTool(
    "ue4ss.inspect_object",
    {
      title: "Inspect UObject",
      description: "Inspect a live UObject by opaque handle or full name.",
      inputSchema: {
        handle: z.string().optional(),
        fullName: z.string().optional(),
        includeValues: z.boolean().optional(),
        propertyQuery: z.string().optional(),
        limit: z.number().int().positive().optional(),
      },
    },
    async (args) => asText(await bridge.call("object.inspect", args)),
  );

  server.registerTool(
    "ue4ss.get_property",
    {
      title: "Get Property",
      description: "Read a UObject property using UE4SS text export semantics.",
      inputSchema: {
        handle: z.string(),
        propertyName: z.string(),
      },
    },
    async (args) => asText(await bridge.call("property.get", args)),
  );

  server.registerTool(
    "ue4ss.set_property",
    {
      title: "Set Property",
      description: "Write a UObject property using UE4SS/Unreal ImportText semantics.",
      inputSchema: {
        handle: z.string(),
        propertyName: z.string(),
        value: z.string(),
      },
    },
    async (args) => asText(await bridge.call("property.set", args)),
  );

  server.registerTool(
    "ue4ss.invoke_delegate",
    {
      title: "Invoke UObject Delegate",
      description: "Invoke a zero-parameter reflected single-cast or multicast delegate property on a live UObject.",
      inputSchema: {
        handle: z.string().min(1),
        propertyName: z.string().min(1),
      },
    },
    async (args) => asText(await bridge.call("delegate.invoke", args)),
  );

  server.registerTool(
    "ue4ss.load_asset",
    {
      title: "Load Unreal Asset",
      description: "Load one exact Unreal asset through the in-process AssetRegistry on the game thread.",
      inputSchema: {
        assetPath: z.string().min(1),
      },
    },
    async (args) => asText(await bridge.call("asset.load", args)),
  );

  server.registerTool(
    "ue4ss.call_function",
    {
      title: "Call Function",
      description: "Invoke a UFunction through UE4SS console-style ProcessConsoleExec.",
      inputSchema: {
        handle: z.string(),
        functionName: z.string(),
        functionHandle: z.string().optional(),
        functionPath: z.string().optional(),
        paramHex: z.string().regex(/^(?:[0-9A-Fa-f]{2})*$/).optional(),
        objectArgHandle: z.string().optional(),
        objectArgOffset: z.number().int().nonnegative().optional(),
        args: z.array(z.string()).default([]),
      },
    },
    async (args) => asText(await bridge.call("function.call", args)),
  );

  server.registerTool(
    "ue4ss.inspect_function",
    {
      title: "Inspect UFunction",
      description: "Inspect one reflected UFunction on a specific UObject and its class/super chain, including parameter types, offsets, sizes, and flags.",
      inputSchema: {
        functionName: z.string().min(1),
        handle: z.string().optional(),
        functionHandle: z.string().optional(),
      },
    },
    async (args) => asText(await bridge.call("function.inspect", args)),
  );

  server.registerTool(
    "ue4ss.exec_console",
    {
      title: "Execute Console Command",
      description: "Execute an Unreal console command against a UObject context or PlayerController.",
      inputSchema: {
        command: z.string(),
        handle: z.string().optional(),
      },
    },
    async (args) => asText(await bridge.call("console.exec", args)),
  );

  server.registerTool(
    "ue4ss.reload_mod",
    {
      title: "Reload Lua Mod",
      description: "Queue one loaded UE4SS Lua mod for a clean uninstall and reinstall by exact mod name.",
      inputSchema: {
        modName: z.string().min(1),
      },
    },
    async (args) => asText(await bridge.call("mod.reload", args)),
  );

  server.registerTool(
    "ue4ss.watch_function",
    {
      title: "Watch Function",
      description: "Reserve a UFunction watch request. Event streaming is implemented in a later bridge iteration.",
      inputSchema: {
        functionName: z.string(),
      },
    },
    async (args) => asText(await bridge.call("function.watch", args)),
  );

  server.registerTool(
    "ue4ss.unwatch",
    {
      title: "Remove Watch",
      description: "Reserve removal of a function watch. Event streaming is implemented in a later bridge iteration.",
      inputSchema: {
        watchId: z.string().optional(),
      },
    },
    async (args) => asText(await bridge.call("watch.remove", args)),
  );

  server.registerTool(
    "ue4ss.events_recent",
    {
      title: "Recent Events",
      description: "Read recent UE4SS MCP bridge events.",
    },
    async () => asText(await bridge.call("events.recent")),
  );

  server.registerTool(
    "ue4ss.run_lua",
    {
      title: "Run Lua",
      description: "Execute Lua in a running UE4SS Lua mod. Use gameThread=true to queue through ExecuteInGameThread.",
      inputSchema: {
        script: z.string(),
        modName: z.string().optional(),
        gameThread: z.boolean().default(false),
      },
    },
    async (args) => asText(await bridge.call("lua.run", args)),
  );
}

function registerResources(server: McpServer, bridge: UE4SSBridgeClient) {
  server.registerResource(
    "ue4ss-session-info",
    "ue4ss://session/info",
    {
      title: "UE4SS Session Info",
      mimeType: "application/json",
    },
    async (uri) => asResource(uri, await bridge.call("session.info")),
  );

  server.registerResource(
    "ue4ss-events-recent",
    "ue4ss://events/recent",
    {
      title: "UE4SS Recent Events",
      mimeType: "application/json",
    },
    async (uri) => asResource(uri, await bridge.call("events.recent")),
  );

  server.registerResource(
    "ue4ss-logs-recent",
    "ue4ss://logs/recent",
    {
      title: "UE4SS MCP Logs",
      mimeType: "application/json",
    },
    async (uri) => asResource(uri, await bridge.call("logs.recent")),
  );

  server.registerResource(
    "ue4ss-objects-search",
    new ResourceTemplate("ue4ss://objects/search/{query}", { list: undefined }),
    {
      title: "UE4SS Object Search",
      mimeType: "application/json",
    },
    async (uri, variables) =>
      asResource(uri, await bridge.call("objects.search", { query: variables.query ?? "" })),
  );

  server.registerResource(
    "ue4ss-object",
    new ResourceTemplate("ue4ss://object/{handle}", { list: undefined }),
    {
      title: "UE4SS UObject",
      mimeType: "application/json",
    },
    async (uri, variables) => asResource(uri, await bridge.call("object.inspect", { handle: variables.handle })),
  );
}

function createServer(bridge: UE4SSBridgeClient): McpServer {
  const server = new McpServer(
    {
      name: "ue4ss-mcp-server",
      version: "0.2.0",
    },
    {
      instructions:
        "Operate only on the local UE4SS game session. Inspect objects before writing, avoid class default objects (Default__), and prefer opaque handles over addresses. After editing a Lua mod, call ue4ss.reload_mod with its exact name, then verify state and recent logs. Use run_lua only when structured tools are insufficient.",
    },
  );

  registerTools(server, bridge);
  registerResources(server, bridge);
  return server;
}

function isAllowedOrigin(origin: string | undefined): boolean {
  if (!origin) return true;
  try {
    const url = new URL(origin);
    return url.hostname === "127.0.0.1" || url.hostname === "localhost" || url.hostname === "::1";
  } catch {
    return false;
  }
}

async function runHttp(server: McpServer, config: CliConfig) {
  const transport = new StreamableHTTPServerTransport({
    sessionIdGenerator: undefined,
  });
  await server.connect(transport);

  const httpServer = http.createServer(async (req, res) => {
    const url = new URL(req.url ?? "/", `http://${req.headers.host ?? "127.0.0.1"}`);
    if (url.pathname !== "/mcp") {
      res.writeHead(404).end();
      return;
    }
    if (!isAllowedOrigin(req.headers.origin)) {
      res.writeHead(403).end("Forbidden origin");
      return;
    }
    if (config.httpToken) {
      const expected = `Bearer ${config.httpToken}`;
      if (req.headers.authorization !== expected) {
        res.writeHead(401).end("Unauthorized");
        return;
      }
    }

    try {
      await transport.handleRequest(req, res);
    } catch (error) {
      console.error("[ue4ss-mcp-server] HTTP transport error:", error);
      if (!res.headersSent) res.writeHead(500).end("Internal Server Error");
    }
  });

  httpServer.listen(config.httpPort, "127.0.0.1", () => {
    console.error(`[ue4ss-mcp-server] HTTP MCP endpoint listening on http://127.0.0.1:${config.httpPort}/mcp`);
  });
}

async function main() {
  const config = readConfig();
  const bridge = new UE4SSBridgeClient(config.pipe, config.token);
  const server = createServer(bridge);

  if (config.transport === "http") {
    await runHttp(server, config);
    return;
  }

  await server.connect(new StdioServerTransport());
}

main().catch((error) => {
  console.error("[ue4ss-mcp-server]", error);
  process.exit(1);
});
