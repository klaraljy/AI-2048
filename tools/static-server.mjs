// 前端的静态文件服务器 —— 零依赖，只用 Node 内置模块。
//
// 为什么不直接 `npx serve web`：
//   1. 那个包要联网下载，首次运行会卡在 "Ok to proceed?" 上
//   2. 本项目的前端本来就是零依赖零构建，起服务这件事也不该例外
//
// 为什么需要它：前端的 ES module 在 file:// 下会被浏览器拦住
// （CORS 策略），必须经由 HTTP 提供。双击网页文件是不行的。
//
// 用法：
//   node tools/static-server.mjs [--port 3000] [--root web] [--open]

import { createServer } from 'node:http';
import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import { connect } from 'node:net';
import { extname, join, normalize, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = fileURLToPath(new URL('.', import.meta.url));
const PROJECT_ROOT = resolve(HERE, '..');

// 扩展名 → MIME。
//
// .js 与 .mjs 必须是 JavaScript 类型：写成 text/plain 会被浏览器的
// module 加载器以"MIME type 不被允许"为由拒绝，页面直接白屏。
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.webp': 'image/webp',
  '.ico': 'image/x-icon',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
  '.txt': 'text/plain; charset=utf-8',
  '.map': 'application/json; charset=utf-8',
  // 音频：音效采样用 fetch + decodeAudioData 读，MIME 不影响解码，
  // 但写对了在 DevTools 里更容易看出资源是否加载正确
  '.wav': 'audio/wav',
  '.mp3': 'audio/mpeg',
  '.ogg': 'audio/ogg',
};

function parseArgs(argv) {
  const options = { port: 3000, root: 'web', open: false, engine: 'ws://127.0.0.1:8765' };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--port' && argv[i + 1]) options.port = Number(argv[++i]);
    else if (arg === '--root' && argv[i + 1]) options.root = argv[++i];
    else if (arg === '--engine' && argv[i + 1]) options.engine = argv[++i];
    else if (arg === '--open') options.open = true;
  }
  return options;
}

/** 试连一次 TCP，用来探测引擎是否已经起来。 */
function canConnect(host, port) {
  return new Promise((resolve) => {
    const socket = connect({ host, port });
    const done = (ok) => {
      socket.removeAllListeners();
      socket.destroy();
      resolve(ok);
    };
    socket.setTimeout(300, () => done(false));
    socket.once('connect', () => done(true));
    socket.once('error', () => done(false));
  });
}

/**
 * 等引擎监听端口就绪。
 *
 * 为什么要等：双击启动时两个进程是并发的，引擎要花几百毫秒才 listen。
 * 立刻打开浏览器的话，前端会连不上并降级成本地弱 AI ——
 * 用户看到的就是"AI 怎么这么笨"，而不是一个明显的错误。
 */
async function waitForEngine(engineUrl, timeoutMs = 8000) {
  const match = /^ws:\/\/([^:/]+):(\d+)/.exec(engineUrl);
  if (!match) return false;
  const [, host, portText] = match;
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    if (await canConnect(host, Number(portText))) return true;
    if (Date.now() > deadline) return false;
    await new Promise((r) => setTimeout(r, 120));
  }
}

/**
 * 把 URL 路径解析成磁盘路径。
 *
 * 必须挡住 `../` 穿越：静态服务器最常见的漏洞就是让人读到 web/ 之外的文件。
 * 做法是先 normalize 再用 root + sep 前缀校验，不靠字符串黑名单。
 */
function resolveRequestPath(root, urlPath) {
  let decoded;
  try {
    decoded = decodeURIComponent(urlPath.split('?')[0]);
  } catch {
    return null;  // 非法百分号编码
  }
  if (decoded.includes('\0')) return null;

  const relative = normalize(decoded).replace(/^([/\\])+/, '');
  const full = resolve(root, relative);
  if (full !== root && !full.startsWith(root + sep)) return null;
  return full;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const root = resolve(PROJECT_ROOT, options.root);

  try {
    const info = await stat(root);
    if (!info.isDirectory()) throw new Error('不是目录');
  } catch {
    console.error(`找不到前端目录：${root}`);
    process.exit(1);
  }

  const server = createServer(async (request, response) => {
    const target = resolveRequestPath(root, request.url ?? '/');
    if (target === null) {
      response.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
      response.end('400 Bad Request\n');
      return;
    }

    let filePath = target;
    try {
      let info = await stat(filePath);
      if (info.isDirectory()) {
        filePath = join(filePath, 'index.html');
        info = await stat(filePath);
      }
      if (!info.isFile()) throw new Error('不是文件');

      const type = MIME[extname(filePath).toLowerCase()] ?? 'application/octet-stream';
      response.writeHead(200, {
        'Content-Type': type,
        // 开发期不缓存：改完源码刷新就能看到效果，不用清缓存
        'Cache-Control': 'no-store',
      });
      createReadStream(filePath).pipe(response);
    } catch {
      response.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      response.end(`404 Not Found: ${request.url}\n`);
    }
  });

  server.on('error', (error) => {
    if (error.code === 'EADDRINUSE') {
      console.error(`端口 ${options.port} 已被占用。换个端口：--port 3001`);
    } else {
      console.error(`启动失败：${error.message}`);
    }
    process.exit(1);
  });

  server.listen(options.port, '127.0.0.1', async () => {
    // 引擎地址要带进 URL：前端从 ?engine= 读它，否则会用本地降级 AI
    const url = `http://127.0.0.1:${options.port}/?engine=${encodeURIComponent(options.engine)}`;
    console.log(`AI-2048 前端已启动： ${url}`);
    console.log(`  引擎地址： ${options.engine}`);

    if (!options.open) {
      console.log('  按 Ctrl+C 停止');
      return;
    }

    console.log('  正在等引擎就绪…');
    const ready = await waitForEngine(options.engine);
    if (ready) {
      console.log('  引擎已就绪，正在打开浏览器');
    } else {
      // 不阻塞、不退出：前端会自行降级并在页面上提示，
      // 但这里必须如实说出"引擎没起来"，否则用户只会觉得 AI 很笨。
      console.log(`  ⚠️ 等不到引擎（${options.engine}）—— 浏览器仍会打开，`);
      console.log('     但页面会降级成本地弱 AI。请检查引擎窗口是否报错。');
    }

    // 用系统默认浏览器打开；失败也不影响服务本身
    const { spawn } = await import('node:child_process');
    spawn('cmd', ['/c', 'start', '', url], { detached: true, stdio: 'ignore' }).unref();
    console.log('  按 Ctrl+C 停止');
  });
}

main();
