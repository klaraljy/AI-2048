// AI-2048 启动器 —— 双击 start-ai2048.bat 时实际跑的就是这个。
//
// 为什么启动逻辑在这里而不在批处理里：
//   cmd 被强制结束（用户直接叉掉窗口）时，批处理后面的收尾语句**根本不会执行**，
//   结果是引擎留在后台继续跑，下次启动还会撞端口。实测确认过这一点。
//   Node 有可靠的 exit / SIGINT 处理，能把子进程收干净。
//
// 它按顺序做三件事：
//   1. 清掉可能残留的旧引擎（否则端口被占，新引擎起不来）
//   2. 启动引擎，输出直接显示在本窗口 —— 出问题时能立刻看到
//   3. 启动静态服务器、等引擎就绪、开浏览器，并一直守着
//
// 本窗口关掉 / 按 Ctrl+C → 引擎一起收走。

import { spawn, spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
const ENGINE_EXE = join(ROOT, 'engine', 'build', 'ai2048-server.exe');
const WEB_PORT = Number(process.env.AI2048_PORT ?? 3000);
const ENGINE_PORT = Number(process.env.AI2048_ENGINE_PORT ?? 8765);
const ENGINE_URL = `ws://127.0.0.1:${ENGINE_PORT}`;

function fail(message, hint) {
  console.error('');
  console.error(`  [错误] ${message}`);
  if (hint) {
    console.error('');
    for (const line of hint) console.error(`  ${line}`);
  }
  console.error('');
  process.exitCode = 1;
}

/**
 * 结束路径匹配的 ai2048-server 进程。
 *
 * 按**可执行文件路径**匹配，不按进程名 —— 本机可能同时跑着别的服务，
 * 按名字批量杀有误伤风险（这条在别处真实踩过一次）。
 */
function killStaleEngines() {
  const script = [
    "Get-CimInstance Win32_Process -Filter \"Name='ai2048-server.exe'\"",
    `  | Where-Object { $_.ExecutablePath -eq '${ENGINE_EXE.replace(/'/g, "''")}' }`,
    '  | ForEach-Object { Stop-Process -Id $_.ProcessId -ErrorAction SilentlyContinue }',
  ].join(' ');
  spawnSync('powershell', ['-NoProfile', '-Command', script], { stdio: 'ignore' });
}

function main() {
  console.log('');
  console.log('  ==========================================');
  console.log('   AI-2048  -  启动中');
  console.log('  ==========================================');
  console.log('');

  if (!existsSync(ENGINE_EXE)) {
    fail(`找不到引擎：${ENGINE_EXE}`, [
      '请先构建引擎：',
      '  cmake -S engine -B engine\\build -G Ninja -DCMAKE_BUILD_TYPE=Release',
      '  cmake --build engine\\build',
    ]);
    return;
  }

  killStaleEngines();

  // 引擎输出直接进本窗口：这样"引擎报什么错"和"前端在等什么"在同一个地方，
  // 排查时不用在两个窗口之间来回看。
  const engine = spawn(ENGINE_EXE, ['--port', String(ENGINE_PORT)], {
    cwd: ROOT,
    stdio: 'inherit',
  });
  engine.on('error', (error) => fail(`引擎启动失败：${error.message}`));
  engine.on('exit', (code) => {
    // 引擎自己退了（比如端口被占）就没必要继续守着一个连不上的前端
    if (!shuttingDown && code !== 0) {
      console.error(`\n  [错误] 引擎意外退出（代码 ${code}）。`);
      shutdown(code ?? 1);
    }
  });

  const web = spawn(
    process.execPath,
    [
      join(HERE, 'static-server.mjs'),
      '--port',
      String(WEB_PORT),
      '--engine',
      ENGINE_URL,
      '--open',
    ],
    { cwd: ROOT, stdio: 'inherit' }
  );
  web.on('exit', (code) => shutdown(code ?? 0));

  let shuttingDown = false;
  function shutdown(code) {
    if (shuttingDown) return;
    shuttingDown = true;
    console.log('\n  正在停止引擎…');
    try {
      engine.kill();
    } catch {
      // 已经退了
    }
    // 给引擎一点时间自己收尾，再确认端口释放
    setTimeout(() => {
      killStaleEngines();
      console.log('  已停止。');
      process.exitCode = code;
    }, 400);
  }

  process.on('SIGINT', () => shutdown(0));
  process.on('SIGTERM', () => shutdown(0));
}

main();
