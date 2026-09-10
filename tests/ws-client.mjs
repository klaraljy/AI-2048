// 极小的 WebSocket 客户端，只用于测试我们的服务端。
//
// 为什么自己写而不是用 npm 的 ws：
//   1. 保持项目零依赖（前端已经是零依赖，测试也不该例外）
//   2. **独立性**：客户端的握手用 Node 内置 crypto 算 SHA-1，
//      与服务端自己写的 SHA-1 是两套实现。两边能握上手，
//      说明服务端的实现与标准一致 —— 如果共用一份代码，就变成自己跟自己对。
//
// 只支持测试需要的：文本帧、ping/pong、关闭。不做分片。

import { createHash, randomBytes } from 'node:crypto';
import { EventEmitter } from 'node:events';
import { connect } from 'node:net';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

/** 按 RFC 6455 造一个**带掩码**的客户端帧。 */
export function buildClientFrame(opcode, payload = Buffer.alloc(0)) {
  const body = Buffer.isBuffer(payload) ? payload : Buffer.from(payload, 'utf8');
  const mask = randomBytes(4);

  const header = [];
  header.push(Buffer.from([0x80 | opcode])); // FIN + opcode

  if (body.length < 126) {
    header.push(Buffer.from([0x80 | body.length]));
  } else if (body.length <= 0xffff) {
    const buf = Buffer.alloc(3);
    buf[0] = 0x80 | 126;
    buf.writeUInt16BE(body.length, 1);
    header.push(buf);
  } else {
    const buf = Buffer.alloc(9);
    buf[0] = 0x80 | 127;
    buf.writeBigUInt64BE(BigInt(body.length), 1);
    header.push(buf);
  }

  header.push(mask);
  const masked = Buffer.alloc(body.length);
  for (let i = 0; i < body.length; i++) masked[i] = body[i] ^ mask[i % 4];
  header.push(masked);
  return Buffer.concat(header);
}

/** 解开一个**服务端发来**的帧（不带掩码）。返回 {frame, rest}。 */
export function parseServerFrame(buffer) {
  if (buffer.length < 2) return null;
  const fin = (buffer[0] & 0x80) !== 0;
  const opcode = buffer[0] & 0x0f;
  const masked = (buffer[1] & 0x80) !== 0;
  let length = buffer[1] & 0x7f;
  let offset = 2;

  if (length === 126) {
    if (buffer.length < offset + 2) return null;
    length = buffer.readUInt16BE(offset);
    offset += 2;
  } else if (length === 127) {
    if (buffer.length < offset + 8) return null;
    length = Number(buffer.readBigUInt64BE(offset));
    offset += 8;
  }

  if (masked) throw new Error('服务端不该给帧加掩码');

  if (buffer.length < offset + length) return null;
  const payload = buffer.subarray(offset, offset + length);
  return { frame: { fin, opcode, payload }, rest: buffer.subarray(offset + length) };
}

/**
 * 连到一个 WebSocket 服务端并完成握手。
 * @returns {Promise<TestWebSocket>}
 */
export function connectWebSocket(url, { timeoutMs = 3000 } = {}) {
  return new Promise((resolve, reject) => {
    const match = /^ws:\/\/([^:/]+):(\d+)(\/.*)?$/.exec(url);
    if (!match) {
      reject(new Error(`无法解析 URL: ${url}`));
      return;
    }
    const [, host, portText, path = '/'] = match;

    const key = randomBytes(16).toString('base64');
    const expectedAccept = createHash('sha1')
      .update(key + GUID)
      .digest('base64');

    const socket = connect({ host, port: Number(portText) });
    const client = new TestWebSocket(socket);
    let handshakeBuffer = Buffer.alloc(0);
    let settled = false;

    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      socket.destroy();
      reject(new Error(`连接 ${url} 超时`));
    }, timeoutMs);

    const onHandshakeData = (chunk) => {
      handshakeBuffer = Buffer.concat([handshakeBuffer, chunk]);
      const end = handshakeBuffer.indexOf('\r\n\r\n');
      if (end < 0) return;

      socket.removeListener('data', onHandshakeData);
      const head = handshakeBuffer.subarray(0, end + 4).toString('utf8');

      if (!head.startsWith('HTTP/1.1 101')) {
        settled = true;
        clearTimeout(timer);
        socket.destroy();
        reject(new Error(`握手未返回 101：${head.split('\r\n')[0]}`));
        return;
      }

      const acceptMatch = /sec-websocket-accept:\s*(\S+)/i.exec(head);
      if (!acceptMatch) {
        settled = true;
        clearTimeout(timer);
        socket.destroy();
        reject(new Error('握手响应缺少 Sec-WebSocket-Accept'));
        return;
      }
      if (acceptMatch[1] !== expectedAccept) {
        settled = true;
        clearTimeout(timer);
        socket.destroy();
        reject(
          new Error(`Sec-WebSocket-Accept 不匹配：期望 ${expectedAccept}，得到 ${acceptMatch[1]}`)
        );
        return;
      }

      settled = true;
      clearTimeout(timer);
      client._attach(handshakeBuffer.subarray(end + 4));
      resolve(client);
    };

    socket.on('data', onHandshakeData);
    socket.on('error', (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      reject(error);
    });

    socket.write(
      `GET ${path} HTTP/1.1\r\n` +
        `Host: ${host}:${portText}\r\n` +
        `Upgrade: websocket\r\n` +
        `Connection: Upgrade\r\n` +
        `Sec-WebSocket-Key: ${key}\r\n` +
        `Sec-WebSocket-Version: 13\r\n` +
        `\r\n`
    );
  });
}

class TestWebSocket extends EventEmitter {
  constructor(socket) {
    super();
    this.socket = socket;
    this.buffer = Buffer.alloc(0);
    this.closed = false;
    this.received = [];
    this._waiters = [];
  }

  _attach(initial) {
    if (initial.length > 0) this.buffer = Buffer.concat([this.buffer, initial]);
    this.socket.on('data', (chunk) => {
      this.buffer = Buffer.concat([this.buffer, chunk]);
      this._drain();
    });
    this.socket.on('close', () => {
      this.closed = true;
      this.emit('close');
      this._drain();
    });
    this.socket.on('error', (error) => this.emit('error', error));
    this._drain();
  }

  _drain() {
    for (;;) {
      let parsed;
      try {
        parsed = parseServerFrame(this.buffer);
      } catch (error) {
        this.emit('error', error);
        return;
      }
      if (parsed === null) break;
      this.buffer = parsed.rest;

      const { opcode, payload } = parsed.frame;
      if (opcode === 0x1) {
        const text = payload.toString('utf8');
        this.received.push(text);
        this.emit('message', text);
      } else if (opcode === 0x8) {
        this.closed = true;
        this.emit('close');
      } else if (opcode === 0x9) {
        this.socket.write(buildClientFrame(0xa, payload));
      }
      // 0xa (pong) 忽略
    }
    while (this._waiters.length > 0 && this.received.length > 0) {
      this._waiters.shift()(this.received.shift());
    }
  }

  send(text) {
    this.socket.write(buildClientFrame(0x1, Buffer.from(text, 'utf8')));
  }

  /** 等一条消息。超时抛错 —— 不要用无限等待把测试挂死。 */
  nextMessage(timeoutMs = 3000) {
    if (this.received.length > 0) return Promise.resolve(this.received.shift());
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('等待服务端消息超时')), timeoutMs);
      this._waiters.push((value) => {
        clearTimeout(timer);
        resolve(value);
      });
    });
  }

  /**
   * 发一条请求并等**对应 id** 的回包。
   *
   * 注意不要写成"发完就取队列里下一条"——那等于假设服务端永远按序、
   * 且不会主动推送任何东西。按 id 配对才是协议真正的语义。
   */
  async request(message, timeoutMs = 5000) {
    const object = typeof message === 'string' ? JSON.parse(message) : message;
    this.send(JSON.stringify(object));

    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const remaining = deadline - Date.now();
      if (remaining <= 0) {
        throw new Error(`等待 id=${object.id} 的回包超时（已收到 ${this.received.length} 条）`);
      }
      const raw = await this.nextMessage(remaining);
      const parsed = JSON.parse(raw);
      if (parsed.id === object.id) return parsed;
      // 不是我们等的 id：留着并继续（服务端可能推送心跳等）
    }
  }

  close() {
    if (this.closed) return;
    this.socket.write(buildClientFrame(0x8, Buffer.from([0x03, 0xe8])));
    this.socket.end();
  }
}
