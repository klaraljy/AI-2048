// ai2048-server 端到端测试：用**独立实现**的 WebSocket 客户端连真的服务端进程。
//
// 这一步是整个里程碑 4 的验收点。之前所有 net 单测都是"自己验自己"——
// 同一个 SHA-1、同一套帧解析，写错了照样通过。这里客户端用的是 Node 内置
// crypto 和新写的帧编解码，只有两边真的都符合 RFC 6455 才能握上手。
//
// 运行：node tests/server.test.mjs
// 前置：engine\build\ai2048-server.exe 已构建

import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { connect, createServer } from 'node:net';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { connectWebSocket } from './ws-client.mjs';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const SERVER_EXE = join(ROOT, 'engine', 'build', 'ai2048-server.exe');

// ---------------------------------------------------------------- 测试脚手架

let passed = 0;
const failures = [];

function check(name, condition, detail) {
  if (condition) {
    passed++;
    console.log(`  ✓ ${name}`);
  } else {
    failures.push(`${name}${detail === undefined ? '' : ` —— ${detail}`}`);
    console.log(`  ✗ ${name}${detail === undefined ? '' : ` —— ${detail}`}`);
  }
}

/** 让操作系统分配一个空闲端口，避免固定端口被占用导致测试假失败。 */
function findFreePort() {
  return new Promise((resolve, reject) => {
    const probe = createServer();
    probe.unref();
    probe.on('error', reject);
    probe.listen(0, '127.0.0.1', () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });
}

/** 试着连一次端口，用于探测服务端是否就绪。 */
function tryConnect(port) {
  return new Promise((resolve) => {
    const socket = connect({ host: '127.0.0.1', port });
    const done = (ok) => {
      socket.removeAllListeners();
      socket.destroy();
      resolve(ok);
    };
    socket.once('connect', () => done(true));
    socket.once('error', () => done(false));
  });
}

/**
 * 启动服务端并等它就绪。
 *
 * 就绪判据是**端口能连上**，不是"日志里出现了某句话"。
 * 用日志等会跟 stdout 缓冲策略耦合：一旦服务端被重定向到管道，
 * 缓冲行为变了，测试就假失败 —— 而那时服务端其实早就好了。
 * 端口可连才和"它能不能干活"直接相关。
 */
async function startServer() {
  const port = await findFreePort();

  // 同时留一份**原始字节**：编码类断言必须查字节，查字符串捕不到乱码
  // （GBK 误读产生的乱码在字节层面仍是合法 UTF-8）。
  const chunks = [];
  const child = spawn(SERVER_EXE, ['--port', String(port), '--depth', '4'], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  let exited = null;
  const onData = (chunk) => chunks.push(chunk);
  child.stdout.on('data', onData);
  child.stderr.on('data', onData);
  child.on('exit', (code) => {
    exited = code;
  });

  const deadline = Date.now() + 10000;
  for (;;) {
    if (exited !== null) {
      throw new Error(
        `服务端启动即退出（code ${exited}）：\n${Buffer.concat(chunks).toString('utf8')}`
      );
    }
    if (await tryConnect(port)) break;
    if (Date.now() > deadline) {
      throw new Error(
        `服务端 10 秒内未监听 ${port}，输出：\n${Buffer.concat(chunks).toString('utf8')}`
      );
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }

  return {
    child,
    port,
    readOutput: () => Buffer.concat(chunks).toString('utf8'),
    readOutputBuffer: () => Buffer.concat(chunks),
  };
}

const FULL_BOARD = [
  [2, 4, 2, 4],
  [4, 2, 4, 2],
  [2, 4, 2, 4],
  [4, 2, 4, 2],
];

const OPENING_BOARD = [
  [0, 0, 0, 0],
  [0, 0, 0, 0],
  [0, 0, 0, 0],
  [0, 0, 2, 2],
];

/** 带默认校验的 best-move 请求。 */
async function askMove(client, board, id, options) {
  const payload = { state: { board } };
  if (options !== undefined) payload.options = options;
  return client.request({ id, type: 'best-move', payload });
}

// ---------------------------------------------------------------- 主流程

async function main() {
  if (!existsSync(SERVER_EXE)) {
    console.error(`找不到 ${SERVER_EXE}\n请先构建：cmake --build engine\\build`);
    process.exit(1);
  }

  const { child, port, readOutput, readOutputBuffer } = await startServer();
  const url = `ws://127.0.0.1:${port}`;
  const clients = [];

  try {
    console.log('\n[0] 启动自检');
    // stdout 被重定向到管道时默认是全缓冲，启动信息会卡在缓冲区里不吐出来。
    // 服务端用 setvbuf 关掉了缓冲 —— 这条断言保证以后不会有人无意中改回去。
    await new Promise((resolve) => setTimeout(resolve, 150));
    const banner = readOutput();
    check(
      '重定向到管道时启动日志立即可见（未被缓冲吞掉）',
      banner.includes('已启动') && banner.includes(String(port)),
      `输出长度 ${banner.length}`
    );

    // 编码断言必须**查字节**，不能只查字符串。
    //
    // 踩过的坑：曾经只断言"输出里有'已启动'"，测试全绿，但双击运行时
    // Windows 控制台按 GBK(936) 解读 UTF-8 字节，用户看到的是
    // "锛堣鍒欓泦" 这样的乱码。乱码在**字节层面仍是合法 UTF-8**，
    // 所以字符串断言完全捕不到 —— 只有比对十六进制才行。
    const raw = readOutputBuffer();
    const expectedBytes = Buffer.from('已启动', 'utf8'); // E5 B7 B2 E5 90 AF E5 8A A8
    let byteIndex = -1;
    for (let i = 0; i + expectedBytes.length <= raw.length; i++) {
      if (raw.subarray(i, i + expectedBytes.length).equals(expectedBytes)) {
        byteIndex = i;
        break;
      }
    }
    check(
      "中文按 UTF-8 原始字节输出（含 E5 B7 B2…），未被转成 GBK",
      byteIndex >= 0,
      byteIndex >= 0
        ? `偏移 ${byteIndex}`
        : `未找到，前 32 字节：${raw.subarray(0, 32).toString('hex')}`
    );

    console.log(`      （启动输出前 24 字节：${raw.subarray(0, 24).toString('hex')}）`);

    console.log('\n[1] 握手');
    const client = await connectWebSocket(url).catch((error) => {
      // 握手失败时把服务端说过的话打出来 —— 否则只有一个"超时"，
      // 无从判断是服务端拒绝了、还是压根没收到请求。
      throw new Error(`${error.message}\n\n服务端输出：\n${readOutput()}`);
    });
    clients.push(client);
    // connectWebSocket 内部会核对 Sec-WebSocket-Accept（用 Node crypto 算的），
    // 能返回就说明服务端的 SHA-1 + Base64 是对的。
    check('RFC 6455 握手成功且 Accept 校验通过', true);

    const second = await connectWebSocket(url).catch((error) => {
      throw new Error(`第二个连接失败：${error.message}\n\n服务端输出：\n${readOutput()}`);
    });
    clients.push(second);
    check('支持多客户端并发连接', true);

    console.log('\n[2] 应用层 ping/pong');
    const ping = await client.request({ id: 1, type: 'ping' });
    check('ping 返回 result', ping.type === 'result', JSON.stringify(ping));
    check('ping 回包 id 正确', ping.id === 1, `id=${ping.id}`);
    check('ping payload 为 {ok:true}', ping.payload?.ok === true, JSON.stringify(ping.payload));

    console.log('\n[3] configure');
    const configured = await client.request({
      id: 2,
      type: 'configure',
      payload: { config: { baseDepth: 3, timeBudgetMs: 2000 } },
    });
    check('configure 返回 result', configured.type === 'result', JSON.stringify(configured));

    console.log('\n[4] best-move 正常路径');
    const move = await askMove(client, OPENING_BOARD, 3);
    check('best-move 返回 result', move.type === 'result', JSON.stringify(move).slice(0, 200));
    check(
      'move 是合法方向',
      ['up', 'down', 'left', 'right'].includes(move.payload?.move),
      `move=${JSON.stringify(move.payload?.move)}`
    );
    const info = move.payload?.debugInfo;
    check('debugInfo 存在且 aiType 为 expectimax', info?.aiType === 'expectimax', info?.aiType);
    check('debugInfo.baseDepth 反映了 configure', info?.baseDepth === 3, `baseDepth=${info?.baseDepth}`);
    check('debugInfo.nodes > 0', typeof info?.nodes === 'number' && info.nodes > 0, `nodes=${info?.nodes}`);
    check(
      'evaluatedMoves 至少含 1 个合法方向',
      Array.isArray(info?.evaluatedMoves) && info.evaluatedMoves.length >= 1,
      `len=${info?.evaluatedMoves?.length}`
    );
    check(
      'evaluatedMoves 里每一项都标为 legal（引擎只上报合法走子）',
      info.evaluatedMoves.every((m) => m.legal === true),
      JSON.stringify(info.evaluatedMoves.map((m) => m.legal))
    );
    check(
      'evaluatedMoves 按总分降序，首项即被选中的方向',
      info.evaluatedMoves[0]?.direction === move.payload?.move,
      `first=${info.evaluatedMoves[0]?.direction} chosen=${move.payload?.move}`
    );

    console.log('\n[5] 终局棋盘 → move 为 null');
    const terminal = await askMove(client, FULL_BOARD, 4);
    check('终局返回 result', terminal.type === 'result', JSON.stringify(terminal));
    check(
      '无合法走子时 move 为 null',
      terminal.payload?.move === null,
      `move=${JSON.stringify(terminal.payload?.move)}`
    );
    check(
      '终局 evaluatedMoves 为 4 条且全部 illegal',
      Array.isArray(terminal.payload?.debugInfo?.evaluatedMoves) &&
        terminal.payload.debugInfo.evaluatedMoves.length === 4 &&
        terminal.payload.debugInfo.evaluatedMoves.every((m) => m.legal === false),
      JSON.stringify(terminal.payload?.debugInfo?.evaluatedMoves?.map((m) => m.legal))
    );

    console.log('\n[6] lastMove 透传');
    const withLast = await askMove(client, OPENING_BOARD, 5, { lastMove: 'left' });
    check('带 options.lastMove 仍返回合法方向', withLast.payload?.move !== null, JSON.stringify(withLast.payload?.move));
    const badLast = await askMove(client, OPENING_BOARD, 6, { lastMove: '斜着走' });
    check('非法 lastMove 被忽略而不是报错', badLast.type === 'result', JSON.stringify(badLast));

    console.log('\n[7] 错误路径');
    const malformed = await client.request({ id: 10, type: 'best-move' });
    check('best-move 缺 payload → error', malformed.type === 'error', JSON.stringify(malformed));
    check('error 回包沿用请求 id', malformed.id === 10, `id=${malformed.id}`);
    check('error payload 有 message', typeof malformed.payload?.message === 'string', JSON.stringify(malformed.payload));

    const unknown = await client.request({ id: 11, type: '没有这个类型' });
    check('未知 type → error', unknown.type === 'error', JSON.stringify(unknown));

    // 板子形状错误
    const cases = [
      ['3 行', [[2, 4, 2, 4], [4, 2, 4, 2], [2, 4, 2, 4]]],
      ['行内 3 格', [[2, 4, 2], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]],
      ['字符串格', [['x', 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]],
      ['负数', [[-2, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]],
      ['非 2 的幂', [[3, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]],
      ['非整数 2.5', [[2.5, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]],
      ['超上限 65536', [[65536, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]],
      ['board 不是数组', 'nope'],
    ];
    for (let i = 0; i < cases.length; i++) {
      const [label, board] = cases[i];
      const response = await askMove(client, board, 20 + i);
      check(
        `非法棋盘（${label}）→ error 且说明原因`,
        response.type === 'error' && typeof response.payload?.message === 'string',
        JSON.stringify(response).slice(0, 160)
      );
    }

    // 边界：32768 是引擎上限，必须接受
    const maxTile = [
      [32768, 0, 0, 0],
      [0, 0, 0, 0],
      [0, 0, 0, 0],
      [0, 0, 0, 0],
    ];
    const maxResponse = await askMove(client, maxTile, 40);
    check('32768（引擎上限）被接受', maxResponse.type === 'result', JSON.stringify(maxResponse).slice(0, 160));

    console.log('\n[8] 畸形报文不拖垮连接');
    client.send('这不是 JSON');
    const afterGarbage = await client.request({ id: 50, type: 'ping' });
    check(
      '发非 JSON 后连接仍可用（回 error 且继续服务）',
      afterGarbage.type === 'result',
      JSON.stringify(afterGarbage)
    );

    client.send('[1,2,3]');
    const afterArray = await client.request({ id: 51, type: 'ping' });
    check('发 JSON 数组后连接仍可用', afterArray.type === 'result', JSON.stringify(afterArray));

    // 空对象
    const empty = await client.request({ id: 52 });
    check('空对象 → error（未知 type）', empty.type === 'error', JSON.stringify(empty));

    console.log('\n[9] 长连接 / 会话保持');
    const sequence = [];
    for (let i = 0; i < 5; i++) {
      sequence.push(await askMove(client, OPENING_BOARD, 60 + i));
    }
    check(
      '连续 5 次请求全部返回 result',
      sequence.every((r) => r.type === 'result'),
      sequence.map((r) => r.type).join(',')
    );
    check(
      '每次都为对应 id 回包',
      sequence.every((r, i) => r.id === 60 + i),
      sequence.map((r) => r.id).join(',')
    );

    console.log('\n[10] 第二个客户端独立');
    const secondMove = await askMove(second, OPENING_BOARD, 70);
    check('第二个客户端可独立请求', secondMove.type === 'result', JSON.stringify(secondMove).slice(0, 160));

    console.log('\n[11] 关闭');
    second.close();
    await new Promise((resolve) => setTimeout(resolve, 200));
    const afterPeerClose = await client.request({ id: 80, type: 'ping' });
    check(
      '一个客户端断开不影响另一个',
      afterPeerClose.type === 'result',
      JSON.stringify(afterPeerClose)
    );

    client.close();
    await new Promise((resolve) => setTimeout(resolve, 200));

    // 断开后能否重连（验证 session 清理没把服务端搞坏）
    const third = await connectWebSocket(url);
    clients.push(third);
    const reconnect = await third.request({ id: 90, type: 'ping' });
    check('断开后仍可重连', reconnect.type === 'result', JSON.stringify(reconnect));
    third.close();

    console.log('\n[12] 多客户端并发');
    const concurrent = await Promise.all(
      [0, 1, 2, 3].map(async (i) => {
        const c = await connectWebSocket(url);
        clients.push(c);
        return c.request({ id: 100 + i, type: 'ping' });
      })
    );
    check(
      '4 个并发连接各自收到正确回包',
      concurrent.every((r, i) => r.type === 'result' && r.id === 100 + i),
      concurrent.map((r) => `${r.id}:${r.type}`).join(' ')
    );
  } finally {
    for (const c of clients) {
      try {
        c.close();
      } catch {
        // 已经关了就算了
      }
    }
    await new Promise((resolve) => setTimeout(resolve, 150));
    child.kill();
  }

  console.log(`\n${'─'.repeat(56)}`);
  if (failures.length === 0) {
    console.log(`server.test.mjs：${passed} 项全部通过`);
    process.exit(0);
  }
  console.log(`server.test.mjs：${passed} 项通过，${failures.length} 项失败`);
  for (const f of failures) console.log(`  ✗ ${f}`);
  process.exit(1);
}

main().catch((error) => {
  console.error(`\n测试中断：${error.message}`);
  process.exit(1);
});
