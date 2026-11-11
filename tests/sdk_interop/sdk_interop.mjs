#!/usr/bin/env node
/**
 * sdk_interop.mjs —— 用**官方 MCP TypeScript SDK** 作为 Host 验证 bt-mcp（v0.5 V2）。
 *
 * 为什么这条测试不可替代：
 *   - 差分测试比较「C 与 Rust 是否一致」；
 *   - conformance 用**自家**解析器校验结构；
 *   - golden 校验字节是否漂移。
 *   三者都无法回答「第三方实现能不能正确理解我们的输出」。本测试用官方 SDK 的
 *   Client + zod schema 真正解析我们的响应：不合规会直接抛错。
 *
 * 传输：spawn `btmcp_mock pipe`，管道上跑 `u16 len(LE) + JSON`。帧层（0xB7 分片）
 * 不参与——规范符合性只取决于 JSON；分片由差分/HIL 覆盖。
 */
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { spawn } from "node:child_process";
import process from "node:process";

let pass = 0, fail = 0;
function check(cond, msg) {
  if (cond) { pass++; }
  else { fail++; console.log(`FAIL ${msg}`); }
}

/** 把子进程的 u16-len 管道包装成 MCP Transport。
 *  注意：新版 SDK 的 Transport 是**纯接口**（无运行时基类），普通对象即可。 */
class PipeTransport {
  onmessage;
  onclose;
  onerror;
  constructor(cmd, args) {
    this.buf = Buffer.alloc(0);
    this.child = spawn(cmd, args, { stdio: ["pipe", "pipe", "inherit"] });
    this.child.stdout.on("data", (d) => {
      this.buf = Buffer.concat([this.buf, d]);
      for (;;) {
        if (this.buf.length < 2) break;
        const n = this.buf.readUInt16LE(0);
        if (this.buf.length < 2 + n) break;
        const json = this.buf.subarray(2, 2 + n).toString("utf8");
        this.buf = this.buf.subarray(2 + n);
        try {
          this.onmessage?.(JSON.parse(json));
        } catch (e) {
          console.log(`FAIL transport: 无法解析服务端 JSON: ${e.message}`);
          fail++;
        }
      }
    });
    this.child.on("exit", (code) => this.onclose?.());
  }

  async start() {/* 子进程已启动 */}
  async close() { this.child.kill(); }

  async send(message) {
    const body = Buffer.from(JSON.stringify(message), "utf8");
    const hdr = Buffer.alloc(2);
    hdr.writeUInt16LE(body.length, 0);
    this.child.stdin.write(Buffer.concat([hdr, body]));
  }
}

const MOCK = process.argv[2] || "build/btmcp_mock";

async function main() {
  const { readFileSync } = await import("node:fs");
  const { dirname, join } = await import("node:path");
  const sdkVersion = JSON.parse(readFileSync(
    join(dirname(new URL(import.meta.url).pathname),
         "node_modules/@modelcontextprotocol/sdk/package.json"), "utf8")).version;

  const transport = new PipeTransport(MOCK, ["pipe"]);
  // SDK 会依次做 initialize / initialized 握手，并用 zod 校验响应结构
  const client = new Client(
    { name: "bt-mcp-sdk-interop", version: "1.0.0" },
    { capabilities: {} },
  );

  try {
    await client.connect(transport);
  } catch (e) {
    console.log(`FAIL connect/initialize: ${e.message}`);
    process.exit(1);
  }
  pass++;
  check(client.getServerVersion()?.name === "bt-mcp-device",
    `serverInfo.name = ${client.getServerVersion()?.name}`);

  // 1) tools/list：SDK 按规范校验每项的 name/description/inputSchema
  let tools;
  try {
    tools = await client.listTools();
    pass++;
  } catch (e) {
    check(false, `tools/list 被 SDK 拒绝: ${e.message}`);
    process.exit(1);
  }
  const names = tools.tools.map((t) => t.name).sort();
  check(names.includes("echo"), `tools/list 含 echo（实际 ${names.join(",")}）`);
  check(names.includes("rich_tool"), "tools/list 含 rich_tool");
  const echo = tools.tools.find((t) => t.name === "echo");
  check(typeof echo?.description === "string", "echo.description 是字符串");
  check(echo?.inputSchema?.type === "object", "echo.inputSchema.type = object");
  check(echo?.annotations?.readOnlyHint === true, "annotations.readOnlyHint 透传");

  // 2) tools/call：SDK 校验 content[] 每项的判别式（text/image/audio/resource）
  try {
    const r = await client.callTool({
      name: "echo", arguments: { text: "hello sdk" },
    });
    pass++;
    check(r.content?.[0]?.type === "text", `text content type（${r.content?.[0]?.type}）`);
    check(r.content?.[0]?.text === "hello sdk", "text 内容正确");
    check(r.isError !== true, "isError 不为 true");
  } catch (e) {
    check(false, `tools/call(echo) 被 SDK 拒绝: ${e.message}`);
  }

  // 3) 富内容：image/audio/embedded resource 都要被 SDK 的 schema 接受
  try {
    const r = await client.callTool({ name: "rich_tool", arguments: {} });
    pass++;
    const types = r.content.map((c) => c.type);
    check(types.includes("text"), `富内容含 text（${types.join(",")}）`);
    check(types.includes("image"), "富内容含 image");
    check(types.includes("audio"), "富内容含 audio");
    check(types.includes("resource"), "富内容含 resource");
    const img = r.content.find((c) => c.type === "image");
    check(typeof img?.data === "string" && img?.mimeType === "image/png",
      "image 的 data/mimeType 合规");
    const res = r.content.find((c) => c.type === "resource");
    check(typeof res?.resource?.uri === "string", "resource.uri 合规");
  } catch (e) {
    check(false, `tools/call(rich_tool) 被 SDK 拒绝: ${e.message}`);
  }

  // 4) 工具失败：isError=true 且 content 仍在
  try {
    const r = await client.callTool({ name: "fail_tool", arguments: {} });
    pass++;
    check(r.isError === true, "失败工具 isError = true");
    check((r.content?.length ?? 0) > 0, "失败工具仍有 content");
  } catch (e) {
    check(false, `tools/call(fail_tool) 被 SDK 拒绝: ${e.message}`);
  }

  // 5) 不存在的工具：SDK 会看到 JSON-RPC error，必须带 code/message
  try {
    await client.callTool({ name: "nope", arguments: {} });
    check(false, "不存在的工具应返回错误");
  } catch (e) {
    pass++;
    check(typeof e.message === "string" && e.message.length > 0,
      `未知工具错误信息可读（${String(e.message).slice(0, 40)}）`);
  }

  // 6) resources/list + resources/read：SDK 校验 contents[]
  try {
    const rs = await client.listResources();
    pass++;
    check(rs.resources.length >= 2, `resources/list 数量 ${rs.resources.length}`);
    check(rs.resources.every((x) => typeof x.uri === "string"), "每个资源都有 uri");
    const r = await client.readResource({ uri: "bt-mcp://dev/log" });
    pass++;
    check(Array.isArray(r.contents) && r.contents.length === 1, "contents 是数组");
    check(typeof r.contents[0].text === "string", "contents[0].text 是字符串");
  } catch (e) {
    check(false, `resources 相关被 SDK 拒绝: ${e.message}`);
  }

  // 7) prompts/list + prompts/get：SDK 校验 messages[].role/content
  try {
    const ps = await client.listPrompts();
    pass++;
    check(ps.prompts.some((p) => p.name === "greet"), "prompts/list 含 greet");
    const pg = await client.getPrompt({ name: "greet" });
    pass++;
    check(Array.isArray(pg.messages) && pg.messages.length > 0, "prompt 有 messages");
    check(pg.messages[0].role === "user", `prompt role = ${pg.messages[0].role}`);
    check(pg.messages[0].content?.type === "text", "prompt content type = text");
  } catch (e) {
    check(false, `prompts 相关被 SDK 拒绝: ${e.message}`);
  }

  // 8) logging/setLevel：SDK 校验 level 枚举
  try {
    await client.setLoggingLevel("debug");
    pass++;
  } catch (e) {
    check(false, `logging/setLevel 被 SDK 拒绝: ${e.message}`);
  }

  // 9) ping：规范要求空结果
  try {
    await client.ping();
    pass++;
  } catch (e) {
    check(false, `ping 被 SDK 拒绝: ${e.message}`);
  }

  await client.close();

  if (fail) {
    console.log(`\nFAILED: ${fail} 项被官方 SDK 拒绝/不符预期（pass=${pass}）`);
    process.exit(1);
  }
  console.log(`\nALL GREEN: ${pass} 项由官方 MCP SDK ${sdkVersion} 校验通过`);
}

main().catch((e) => {
  console.log(`FAIL 未捕获异常: ${e.stack || e.message}`);
  process.exit(1);
});
