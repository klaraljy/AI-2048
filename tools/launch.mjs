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
//
// ---------------------------------------------------------------------------
// ⚠️ start-ai2048.bat 必须保持**纯 ASCII**
//
// 这不是风格偏好，是 cmd.exe 的硬限制，实测踩过。
//
// cmd 执行 .bat 时按**字节偏移**在文件里定位，且每执行一条命令都要重新定位。
// 而那个文件第 2 行是 `chcp 65001`（切到 UTF-8）。一旦切了代码页，
// cmd 的偏移计算与文件里的多字节 UTF-8 序列就对不上了 ——
// 它会落在某个字符的中间，把本来无害的 `rem` 注释行**从中间劈开当命令执行**。
//
// 实际看到的报错：
//     'vel'' is not recognized as an internal or external command
//     '<某句中文注释的后半截>' is not recognized ...
// 那些碎片全部来自批处理文件自己的注释头。
//
// 所以中文说明写在这里（JS 没有这个限制），批处理里只留 ASCII。
// 项目笔记里关于 LF 换行的那条教训是同一类问题的另一面 ——
// 那次是 `'rlevel' is not recognized`，同样是从中间劈开。
// ---------------------------------------------------------------------------

import { spawn, spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { createServer } from 'node:net';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
const ENGINE_EXE = join(ROOT, 'engine', 'build', 'ai2048-server.exe');
const WEB_PORT = Number(process.env.AI2048_PORT ?? 3000);
const ENGINE_PORT = Number(process.env.AI2048_ENGINE_PORT ?? 8765);

/** 连续被占时最多往上试几个端口。 */
const PORT_PROBE_LIMIT = 20;

/**
 * 找一个能用的端口，从 `start` 开始往上试。
 *
 * 为什么需要它（真实踩过两次）：原先写死了端口，一旦被占整个启动就失败，
 * 用户看到的是「端口 3000 已被占用。换个端口：--port 3001」，
 * 而双击快捷方式的人根本没法传参数 —— 等于打不开。
 * 罪魁祸首往往是**上一次没退干净的自己**（引擎和静态服务器是子进程，
 * 窗口被强杀时不一定能收走）。
 *
 * @returns 可用的端口；全部被占则返回 null
 */
function findFreePort(start) {
  return new Promise((resolve) => {
    let candidate = start;
    let attempts = 0;

    const tryNext = () => {
      if (attempts >= PORT_PROBE_LIMIT) {
        resolve(null);
        return;
      }
      attempts += 1;
      const probe = createServer();
      probe.once('error', () => {
        // EADDRINUSE（或其它占用）：换下一个
        candidate += 1;
        tryNext();
      });
      probe.once('listening', () => {
        probe.close(() => resolve(candidate));
      });
      probe.listen(candidate, '127.0.0.1');
    };

    tryNext();
  });
}

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

async function main() {
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

  // 选端口：从配置的端口往上找空闲的。
  //
  // 引擎端口与前端端口分别探测。引擎端口被占通常意味着**上一次的引擎
  // 还活着**（killStaleEngines 只按可执行文件路径清理，如果它卡住或路径
  // 对不上就会漏），前端端口被占则多半是上次的静态服务器没收干净。
  // 与其让用户看到一句他没法照做的「换个端口」，不如自己让开。
  const enginePort = await findFreePort(ENGINE_PORT);
  if (enginePort === null) {
    fail(`端口 ${ENGINE_PORT} 起连续 ${PORT_PROBE_LIMIT} 个都被占用，无法启动。`);
    return;
  }
  if (enginePort !== ENGINE_PORT) {
    console.log(`  端口 ${ENGINE_PORT} 被占，引擎改用 ${enginePort}`);
  }

  const webPort = await findFreePort(WEB_PORT);
  if (webPort === null) {
    fail(`端口 ${WEB_PORT} 起连续 ${PORT_PROBE_LIMIT} 个都被占用，无法启动。`);
    return;
  }
  if (webPort !== WEB_PORT) {
    console.log(`  端口 ${WEB_PORT} 被占，前端改用 ${webPort}`);
  }

  const engineUrl = `ws://127.0.0.1:${enginePort}`;

  // 可选：用训练好的 n-tuple 权重做叶子评估。
  //
  // 设了 AI2048_NET=<权重路径> 才启用，默认不设 —— 也就是默认仍是手写启发式，
  // 与历史基准一致。理由：实测学习权重目前比手写启发式**弱**（d7 17,430
  // 对 d4 42,792），默认切过去等于让用户莫名其妙地变弱。它现在的价值是
  // 便宜和可训练，不是更强。想试的人显式打开。
  //
  // 权重文件是 `ai2048-cli train --out <路径>` 的产物。仓库不收录（体积 +
  // 可再生成），所以路径由使用者自己给。
  const engineArgs = ['--port', String(enginePort)];
  const netFile = process.env.AI2048_NET;
  if (netFile && netFile.trim() !== '') {
    engineArgs.push('--net-file', netFile.trim());
    console.log(`  叶子评估：学习权重 ${netFile.trim()}`);
  }

  // 引擎输出直接进本窗口：这样"引擎报什么错"和"前端在等什么"在同一个地方，
  // 排查时不用在两个窗口之间来回看。
  const engine = spawn(ENGINE_EXE, engineArgs, {
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
      String(webPort),
      '--engine',
      engineUrl,
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

main().catch((error) => {
  fail(String(error && error.stack ? error.stack : error));
});
