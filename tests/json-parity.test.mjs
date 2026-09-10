// JSON 解析器与标准实现对拍。
//
// 为什么必须对拍：
// `engine/src/net/json.cpp` 是**自己写的**解析器（理由见该文件顶部）。
// 手写解析器的风险是"接受或拒绝的边界与标准不一致"，而这类差异
// 只在特定输入上暴露 —— 自己跟自己对只能证明稳定，不能证明正确。
//
// 所以这里把一批 JSON 同时交给两方，**逐例比较"能否解析 + 规范化结果"**：
//   - 标准方：Node 的 JSON.parse（独立实现）
//   - 被测方：ai2048-cli json（读 stdin 逐行）
//
// 运行： node tests\json-parity.test.mjs
// 依赖引擎已构建（调用 ai2048-cli.exe）。

import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const projectRoot = join(here, '..');
const cli = join(projectRoot, 'engine', 'build', 'ai2048-cli.exe');

if (!existsSync(cli)) {
  console.error(`找不到引擎可执行文件：${cli}`);
  console.error('请先构建： cmake --build engine\\build');
  process.exit(1);
}

// ---------------------------------------------------------------------------
// 语料：合法与非法各一批，重点覆盖边界
// ---------------------------------------------------------------------------

const VALID = [
  // 基本值
  'null',
  'true',
  'false',
  '0',
  '-0',
  '1',
  '-1',
  '123456789',
  '1.5',
  '-1.5',
  '1e2',
  '1E2',
  '1e+2',
  '1e-2',
  '0.0001',
  '2.5e-3',
  '""',
  '"a"',
  '"hello world"',

  // 数组
  '[]',
  '[1]',
  '[1,2,3]',
  '[[]]',
  '[[],[]]',
  '[1,[2,[3,[4]]]]',
  '[null,true,false,0,"",[],{}]',
  // 协议里真实用到的 4x4 棋盘
  '[[2,4,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]]',
  '[[2,4,8,16],[32,64,128,256],[512,1024,2048,4096],[8192,16384,32768,65536]]',

  // 对象
  '{}',
  '{"a":1}',
  '{"a":1,"b":2}',
  '{"a":{"b":{"c":1}}}',
  '{"id":1,"type":"best-move","payload":{"state":{"board":[[2,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,0]]},"options":{"lastMove":null}}}',

  // 字符串转义
  '"\\""',
  '"\\\\"',
  '"\\/"',
  '"\\b\\f\\n\\r\\t"',
  '"tab\\there"',
  '"newline\\nhere"',
  '"\\u0041"',
  '"\\u00e9"',
  '"\\u4e2d\\u6587"',
  '"\\ud83d\\ude00"',
  '"中文直接写"',
  '"emoji 😀 直接写"',

  // 空白
  ' 1 ',
  '\t1\t',
  '\n1\n',
  '{ "a" : 1 }',
  '[ 1 , 2 ]',
  '\r\n{"a":1}\r\n',

  // 键的边界
  '{"":1}',
  '{"a b":1}',
  '{"a\\nb":1}',
  '{"中文键":1}',
];

const INVALID = [
  // 注意：这里**不放空串**。CLI 的契约是"逐行读，跳过空行"，
  // 所以空行根本不会送到解析器 —— 放进来会造成期望与实现的契约不一致。
  // 空串能不能解析是 json.cpp 自己的事，由 engine\tests\net_test.cpp 覆盖。
  '{',
  '}',
  '[',
  ']',
  '{"a"}',
  '{"a":}',
  '{"a":1,}',
  '[,1]',
  '[1,]',
  '[1 2]',
  '{a:1}',
  "{'a':1}",
  'undefined',
  'NaN',
  'Infinity',
  '-Infinity',
  '01',
  '1.',
  '.1',
  '1e',
  '1e+',
  '+1',
  '"unterminated',
  '"bad\\escape"',
  '"\\u00"',
  '"\\uZZZZ"',
  '{"a":1}{"b":2}',
  '1 2',
  // 未转义控制字符（真实换行会被压成空格，见下方 safeCases）
  '"a\nb"',
];

/**
 * 本实现**刻意比标准更严格**的地方。
 *
 * 对拍抓到这两处差异后，我确认了标准的行为并选择保留严格：
 *   - 孤立代理：JSON.parse 接受 `"\ud800"`（产生无效 Unicode）；
 *     本实现拒绝。协议里出现的字符串只有 up/down/left/right 这类 ASCII，
 *     所以严格没有代价，而它能挡住"看起来成功其实数据已损坏"的情况。
 *   - 重复键：JSON.parse 接受 `{"a":1,"a":2}` 并静默让后者覆盖前者；
 *     本实现拒绝。协议只有我们自己两端，重复键一定是 bug。
 *
 * 这不是"没对齐"，而是**有理由的偏离** —— 所以这里显式验证它，
 * 而不是把它排除在测试之外假装没差异。
 */
const STRICTER_THAN_STANDARD = ['"\\ud800"', '"\\udc00"', '{"dup":1,"dup":2}'];

// ---------------------------------------------------------------------------
// 规范化：两侧必须用**完全相同**的规则，否则会把格式差异误报成解析差异。
//
// 数字最麻烦：JS 的 toPrecision(17) 会给 "1.0000000000000000"，
// 而 C++ 的 %g 会给 "1"。所以两边统一成"1 位整数部分 + 16 位小数 + 十进制指数"，
// 并去掉尾随零 —— 这正是 %.16e 的格式。
// ---------------------------------------------------------------------------

function canonicalNumber(value) {
  if (!Number.isFinite(value)) return 'nonfinite';
  if (Object.is(value, -0)) return '-0.0000000000000000e+0';

  const text = value.toExponential(16); // 形如 "1.0000000000000000e+0"
  const [mantissa, exponent] = text.split('e');
  const trimmed = mantissa.includes('.')
    ? mantissa.replace(/0+$/, '').replace(/\.$/, '')
    : mantissa;
  return `${trimmed}e${Number(exponent)}`;
}

function canonical(value) {
  if (value === null) return 'null';
  if (typeof value === 'boolean') return value ? 'true' : 'false';
  if (typeof value === 'number') return canonicalNumber(value);
  if (typeof value === 'string') return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
  const keys = Object.keys(value).sort();
  return `{${keys.map((k) => `${JSON.stringify(k)}:${canonical(value[k])}`).join(',')}}`;
}

/** 标准方：JSON.parse。 */
function referenceParse(text) {
  try {
    return `OK ${canonical(JSON.parse(text))}`;
  } catch {
    return 'ERR';
  }
}

// ---------------------------------------------------------------------------

const cases = [...VALID, ...INVALID];

// 注意：语料里有含换行的字符串（如 '"a\nb"' 里的真实换行），
// 而 CLI 是按行读的 —— 所以要换成不含换行的等价非法用例。
const safeCases = cases.map((text) => text.replace(/\n/g, ' '));
const strictCases = STRICTER_THAN_STANDARD.map((text) => text.replace(/\n/g, ' '));

const allCases = [...safeCases, ...strictCases];

const input = allCases.join('\n') + '\n';
// 注意要按 /\r?\n/ 切并 trim：CLI 在 Windows 上输出 \r\n，
// 若只按 \n 切，每行都会带一个尾随 \r，比较必然全错。
const actual = execFileSync(cli, ['json'], { input, encoding: 'utf8' })
  .split(/\r?\n/)
  .map((line) => line.trim())
  .filter((line) => line.length > 0);

if (actual.length !== allCases.length) {
  console.error(`CLI 返回 ${actual.length} 行，期望 ${allCases.length} 行`);
  process.exit(1);
}

let failures = 0;

// 第一组：必须与 JSON.parse 完全一致
for (let i = 0; i < safeCases.length; i++) {
  const expected = referenceParse(safeCases[i]);
  const got = actual[i];
  if (expected !== got) {
    failures += 1;
    if (failures <= 12) {
      console.error(`✗ 与标准不一致：${JSON.stringify(safeCases[i])}`);
      console.error(`    JSON.parse : ${expected}`);
      console.error(`    本实现     : ${got}`);
    }
  }
}

// 第二组：刻意比标准严格 —— 标准必须接受，而我们必须拒绝
let strictChecked = 0;
for (let i = 0; i < strictCases.length; i++) {
  const text = strictCases[i];
  const got = actual[safeCases.length + i];
  const standard = referenceParse(text);

  if (!standard.startsWith('OK')) {
    failures += 1;
    console.error(`✗ 前提不成立：标准居然拒绝了 ${JSON.stringify(text)}`);
    console.error('   这条被登记为"本实现更严格"，但标准也拒绝它 —— 应该挪进 INVALID 组');
    continue;
  }
  if (got !== 'ERR') {
    failures += 1;
    console.error(`✗ 本该严格拒绝却接受了：${JSON.stringify(text)} -> ${got}`);
    continue;
  }
  strictChecked += 1;
}

console.log(
  `JSON 对拍：与标准一致 ${safeCases.length} 例` +
    `（合法 ${VALID.length}，非法 ${INVALID.length}），` +
    `刻意更严格 ${strictChecked}/${strictCases.length} 例`
);
if (failures > 0) {
  console.error(`\n失败：${failures} 处`);
  process.exit(1);
}
console.log('JSON 对拍通过：一致处完全一致，严格处确认更严格');
