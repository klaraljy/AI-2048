// 验证「双击启动器」这条链路真的能玩：
//   静态服务器提供页面 → 页面里的 ?engine= 指向引擎 → 引擎接受 WebSocket
//
// 不启动真实浏览器（那没法自动化），但把浏览器会做的每一步都走一遍。

import { spawn } from 'node:child_process';
import { connect } from 'node:net';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { connectWebSocket } from './ws-client.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const WEB_PORT = 3021;
const ENGINE_PORT = 15221;
const ENGINE_URL = `ws://127.0.0.1:${ENGINE_PORT}`;

let failures = 0;
function check(name, ok, detail) {
  console.log(`  ${ok ? '✓' : '✗'} ${name}${detail ? ` —— ${detail}` : ''}`);
  if (!ok) failures++;
}

function waitForPort(port, timeoutMs = 8000) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve) => {
    const attempt = () => {
      const socket = connect({ host: '127.0.0.1', port });
      socket.once('connect', () => {
        socket.destroy();
        resolve(true);
      });
      socket.once('error', () => {
        socket.destroy();
        if (Date.now() > deadline) resolve(false);
        else setTimeout(attempt, 100);
      });
    };
    attempt();
  });
}

const children = [];

async function main() {
  // 引擎
  const engine = spawn(
    join(ROOT, 'engine', 'build', 'ai2048-server.exe'),
    ['--port', String(ENGINE_PORT), '--depth', '4'],
    { stdio: 'ignore' }
  );
  children.push(engine);

  // 静态服务器（与启动器用的同一个脚本）
  const web = spawn(
    process.execPath,
    [
      join(ROOT, 'tools', 'static-server.mjs'),
      '--port',
      String(WEB_PORT),
      '--engine',
      ENGINE_URL,
    ],
    { stdio: 'ignore', cwd: ROOT }
  );
  children.push(web);

  console.log('\n[1] 前端页面');
  check('静态服务器起来了', await waitForPort(WEB_PORT), `端口 ${WEB_PORT}`);
  check('引擎起来了', await waitForPort(ENGINE_PORT), `端口 ${ENGINE_PORT}`);

  const indexResponse = await fetch(`http://127.0.0.1:${WEB_PORT}/`);
  check('index.html 可访问', indexResponse.status === 200, `status=${indexResponse.status}`);
  check(
    'index.html 是 text/html',
    (indexResponse.headers.get('content-type') ?? '').includes('text/html'),
    indexResponse.headers.get('content-type') ?? ''
  );

  const html = await indexResponse.text();
  check('页面引用了前端入口脚本', html.includes('js/main.js'), '找不到 js/main.js');

  // 用 URL 查询参数选引擎，走的是 main.js 的 readEngineUrl()。
  // 参数名一旦改了而启动器没跟着改，用户会静默降级成弱 AI —— 很难自己发现。
  const indexSource = await (await fetch(`http://127.0.0.1:${WEB_PORT}/js/main.js`)).text();
  check(
    '前端确实从 URL 的 engine 参数读引擎地址（与启动器约定一致）',
    indexSource.includes("params.get('engine')"),
    "main.js 里找不到 params.get('engine')"
  );

  console.log('\n[2] ES module 的 MIME 类型');
  // 这一步很关键：类型不对浏览器会以"MIME type 不被允许"为由拒绝加载，
  // 页面直接白屏，而服务器日志里看不出任何异常。
  const mainJs = await fetch(`http://127.0.0.1:${WEB_PORT}/js/main.js`);
  const jsType = mainJs.headers.get('content-type') ?? '';
  check(
    'main.js 是 JavaScript MIME（否则浏览器拒绝加载 module）',
    jsType.includes('javascript'),
    jsType
  );
  const transportJs = await fetch(`http://127.0.0.1:${WEB_PORT}/js/transport.js`);
  check(
    'transport.js 也是 JavaScript MIME',
    (transportJs.headers.get('content-type') ?? '').includes('javascript'),
    transportJs.headers.get('content-type') ?? ''
  );

  console.log('\n[3] 浏览器会走的那一步：按 ?engine= 连引擎');
  // 模拟启动器打开的那个 URL，并按前端的方式取出 engine 参数
  const launcherUrl = `http://127.0.0.1:${WEB_PORT}/?engine=${encodeURIComponent(ENGINE_URL)}`;
  const parsed = new URL(launcherUrl);
  const engineFromUrl = parsed.searchParams.get('engine');
  check(
    '启动器 URL 里的 engine 参数能被正确解析回原值',
    engineFromUrl === ENGINE_URL,
    `解析得到 ${engineFromUrl}`
  );

  const socket = await connectWebSocket(engineFromUrl);
  const pong = await socket.request({ id: 1, type: 'ping' });
  check('用解析出的地址能完成 WebSocket 握手并通信', pong.type === 'result', JSON.stringify(pong));

  const move = await socket.request({
    id: 2,
    type: 'best-move',
    payload: {
      state: {
        board: [
          [0, 0, 0, 0],
          [0, 0, 0, 0],
          [0, 0, 0, 0],
          [0, 0, 2, 2],
        ],
      },
    },
  });
  check(
    '引擎能给出走子决策（前端拿到的就是它）',
    move.type === 'result' && typeof move.payload?.move === 'string',
    `move=${JSON.stringify(move.payload?.move)}`
  );
  socket.close();

  console.log('\n[4] 目录穿越防护');
  const escaped = await fetch(`http://127.0.0.1:${WEB_PORT}/%2e%2e%2fAGENTS.md`);
  check('编码后的 ../ 被挡住（不能读到 web/ 之外）', escaped.status !== 200, `status=${escaped.status}`);

  console.log(`\n${'─'.repeat(56)}`);
  console.log(failures === 0 ? '启动链路测试全部通过' : `${failures} 项失败`);
}

main()
  .catch((error) => {
    console.error(`\n测试中断：${error.message}`);
    failures++;
  })
  .finally(async () => {
    for (const child of children) {
      try {
        child.kill();
      } catch {
        // 已经退了就算了
      }
    }
    // 等子进程真正退出再收尾。
    // 直接 process.exit() 会在 libuv 的 handle 还处于 CLOSING 时强行收摊，
    // 触发 "Assertion failed: !(handle->flags & UV_HANDLE_CLOSING)" 而崩溃 ——
    // 测试明明全过，退出码却是 1，反而看起来像失败。
    await new Promise((resolve) => setTimeout(resolve, 300));
    process.exitCode = failures === 0 ? 0 : 1;
  });
