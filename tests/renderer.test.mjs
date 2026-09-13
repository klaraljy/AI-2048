// renderer 的 headless 冒烟测试。
//
// 用 _tmp 里的 DOM 桩把真实 renderer 跑起来，验证动画序列的**内部一致性**：
// 这是没有浏览器时唯一能覆盖 renderer 的办法。
//
// 运行： node tests\renderer.test.mjs
//
// 覆盖不到的部分（如实记录，不假装覆盖了）：
//   - 视觉正确性（颜色、字号、位置是否好看）
//   - CSS 过渡是否真的被浏览器执行
//   - 音效是否真的发声
//   - 触屏手势
// 这些只能人工在浏览器里走 AGENTS.md 的验收清单。

import { installDomStub } from './dom-stub.mjs';

installDomStub();

const { Renderer } = await import('../web/js/renderer.js');
const { Game, DIRECTION } = await import('../web/js/game.js');

let failures = 0;
function check(label, condition, detail = '') {
  if (condition) {
    console.log(`  ✓ ${label}`);
  } else {
    failures += 1;
    console.error(`  ✗ ${label}${detail ? `  —— ${detail}` : ''}`);
  }
}

/** 棋盘上**实际显示**的数字（从 DOM 读文字，不是读逻辑状态）。
 *  撤销动画的"闪"正是显示层的问题 —— 逻辑状态一直是对的，
 *  所以判据必须看 DOM，不能看 renderer.tiles。 */
function domValues(renderer) {
  return [...renderer.tilesLayer.querySelectorAll('.tile')]
    .map((node) => Number(node.textContent))
    .sort((a, b) => a - b);
}

function makeRenderer() {  const { document } = globalThis;
  const gridNode = document.createElement('div');
  const tilesNode = document.createElement('div');
  return {
    gridNode,
    tilesNode,
    renderer: new Renderer({
      tilesLayer: tilesNode,
      gridLayer: gridNode,
      scoreElement: document.createElement('strong'),
      bestElement: document.createElement('strong'),
    }),
  };
}

/** 逻辑状态里的方块数量（Map 大小）。 */
function tileCount(renderer) {
  return renderer.tiles.size;
}

function domCount(renderer) {
  return renderer.tilesLayer.children.length;
}

function nonEmptyCells(board) {
  let n = 0;
  for (const row of board) for (const v of row) if (v !== 0) n += 1;
  return n;
}

/** 逻辑状态与棋盘是否一致（每个非空格恰好一个方块，且指数相符）。 */
function stateMatchesBoard(renderer, board) {
  const seen = new Set();
  for (const [, tile] of renderer.tiles) {
    const key = `${tile.row},${tile.col}`;
    if (seen.has(key)) return `位置重复: ${key}`;
    seen.add(key);
    if (board[tile.row][tile.col] !== tile.exponent) {
      return `位置 ${key} 的逻辑指数 ${tile.exponent} 与棋盘 ${board[tile.row][tile.col]} 不符`;
    }
  }
  if (seen.size !== nonEmptyCells(board)) {
    return `方块数 ${seen.size} 与棋盘非空格数 ${nonEmptyCells(board)} 不符`;
  }
  return null;
}

// ---------------------------------------------------------------------------

console.log('renderer 冒烟测试：');

// 1. 初始渲染
{
  const { renderer } = makeRenderer();
  const game = new Game(1);
  renderer.reset(game.board, { score: 0, best: 0 });
  check('初始渲染：DOM 节点数 = 非空格数', domCount(renderer) === nonEmptyCells(game.board),
    `DOM=${domCount(renderer)} 棋盘=${nonEmptyCells(game.board)}`);
  const mismatch = stateMatchesBoard(renderer, game.board);
  check('初始渲染：逻辑状态与棋盘一致', mismatch === null, mismatch || '');
}

// 2. 连续走子后的一致性 —— 这是最容易出错的地方
{
  const { renderer } = makeRenderer();
  const game = new Game(7);
  renderer.reset(game.board, { score: 0, best: 0 });

  const order = [DIRECTION.left, DIRECTION.down, DIRECTION.right, DIRECTION.up];
  let steps = 0;
  let firstError = null;

  for (let i = 0; i < 120 && !game.gameOver; i++) {
    const before = game.board.map((row) => row.slice());
    const step = game.step(order[i % order.length]);
    if (!step.moved) continue;
    steps += 1;
    await renderer.animateMove(step, before, game.score, 0);

    const mismatch = stateMatchesBoard(renderer, game.board);
    if (mismatch && !firstError) {
      firstError = `第 ${steps} 步后：${mismatch}`;
      break;
    }
    if (domCount(renderer) !== tileCount(renderer) && !firstError) {
      firstError = `第 ${steps} 步后 DOM 节点数 ${domCount(renderer)} != 逻辑方块数 ${tileCount(renderer)}`;
      break;
    }
  }
  check(`连续 ${steps} 步走子：状态始终与棋盘一致`, firstError === null, firstError || '');
  check('连续走子后 DOM 节点数 = 逻辑方块数', domCount(renderer) === tileCount(renderer),
    `DOM=${domCount(renderer)} 逻辑=${tileCount(renderer)}`);
  check('连续走子后没有动画残留（busy 为假）', renderer.busy() === false);
}

// 3. 合并时被吸收的块必须被删掉（不能泄漏 DOM 节点）
{
  const { renderer } = makeRenderer();
  // 造一个必定合并的局面：[2,2,4,4] 在第一行
  const board = [[1, 1, 2, 2], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]];
  renderer.reset(board, { score: 0, best: 0 });
  const before = board.map((row) => row.slice());

  // 手工构造 step（不经 Game，以免生成新块干扰计数）
  const { applyMove } = await import('../web/js/game.js');
  const result = applyMove(before, DIRECTION.left);
  const step = {
    moved: true,
    spawned: false,
    spawn: null,
    moves: result.moves,
    gained: result.gained,
  };
  const mergedCountBefore = result.moves.filter((m) => m.merged).length;

  await renderer.animateMove(step, before, result.gained, 0);

  check('合并前有 2 条被吸收的轨迹', mergedCountBefore === 2, `实际 ${mergedCountBefore}`);
  check('两对合并后只剩 2 个方块', tileCount(renderer) === 2, `实际 ${tileCount(renderer)}`);
  check('DOM 节点数 = 2（被吸收的块已删除）', domCount(renderer) === 2, `实际 ${domCount(renderer)}`);
  const mismatch = stateMatchesBoard(renderer, result.board);
  check('合并后逻辑状态与棋盘一致', mismatch === null, mismatch || '');

  // 数值必须更新为合并结果
  const values = [...renderer.tiles.values()].map((t) => 2 ** t.exponent).sort((a, b) => a - b);
  check('合并后数值为 4 和 8', values.join(',') === '4,8', `实际 ${values.join(',')}`);
}

// 4. 走动不动的方向不能产生任何变化
{
  const { renderer } = makeRenderer();
  const board = [[1, 2, 1, 2], [2, 1, 2, 1], [0, 0, 0, 0], [0, 0, 0, 0]];
  const { applyMove } = await import('../web/js/game.js');
  renderer.reset(board, { score: 0, best: 0 });
  const before = domCount(renderer);

  const result = applyMove(board, DIRECTION.left); // 第一行推不动
  if (!result.moved) {
    // 模拟 main.js 的行为：moved=false 时不调用 animateMove
    check('走不动时不产生轨迹', result.moves.length === 0);
  } else {
    check('这组局面本应推不动（构造有误）', false, '第一行 1 2 1 2 不该能左移');
  }
  check('走不动时 DOM 未变', domCount(renderer) === before);
}

// 5. busy() 的时序
{
  const { renderer } = makeRenderer();
  const board = [[1, 1, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]];
  const { applyMove } = await import('../web/js/game.js');
  renderer.reset(board, { score: 0, best: 0 });

  check('空闲时 busy 为假', renderer.busy() === false);

  const result = applyMove(board, DIRECTION.left);
  const promise = renderer.animateMove(
    { moved: true, spawned: false, spawn: null, moves: result.moves, gained: result.gained },
    board,
    0,
    0
  );
  check('动画期间 busy 为真', renderer.busy() === true);
  await promise;
  check('动画结束后 busy 为假', renderer.busy() === false);
}

// 6. 合并轨迹里的 exponent 是**合并前**的值
//
// 这条断言来自一个真实的 off-by-one：庆祝时"合成 8 却弹出 4"，
// 而且永远是一半 —— 因为调用方把被吸收方块的 exponent 当成了结果值。
// 这里把这个语义钉死，以后谁改动 moves 的含义会立刻失败。
{
  const { applyMove } = await import('../web/js/game.js');

  // 两个 4（指数 2）合成一个 8（指数 3）
  const board = [[2, 2, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]];
  const result = applyMove(board, DIRECTION.left);

  const absorbed = result.moves.filter((m) => m.merged);
  check('两个 4 合成后有一条"被吸收"的轨迹', absorbed.length === 1, `实际 ${absorbed.length}`);

  if (absorbed.length === 1) {
    check(
      '被吸收轨迹的 exponent 是合并**前**的值（2，也就是 4）',
      absorbed[0].exponent === 2,
      `实际 ${absorbed[0].exponent}`
    );
    check(
      '合并**结果**的指数 = 被吸收的指数 + 1（3，也就是 8）',
      absorbed[0].exponent + 1 === 3,
      '调用方必须 +1 才能拿到结果值'
    );
    check(
      '棋盘上的结果确实是 8（指数 3）',
      result.board[0][0] === 3,
      `实际 ${result.board[0][0]}`
    );
  }
}

// 7. 合成更大块的同一语义（1024 = 指数 10，庆祝阈值）
{
  const { applyMove } = await import('../web/js/game.js');
  const board = [[9, 9, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]];
  const result = applyMove(board, DIRECTION.left);
  const absorbed = result.moves.filter((m) => m.merged);
  check('两个 512 合成 1024：被吸收的 exponent 是 9', absorbed[0]?.exponent === 9);
  check('结果指数是 10（=1024，庆祝阈值）', result.board[0][0] === 10, `实际 ${result.board[0][0]}`);
}

// 8. 撤销动画
//
// 这条来自用户反馈："这个撤回动画怎么会一闪一闪的"。
// 根因：合并块的**数值回退**原来发生在按下撤销的同一瞬间（t=0），
// 而要走的那半块还要 140ms 才淡出 —— 画面上就是先闪一下。
// 修法是把数值回退挪到"滑动"那一帧。这里把时序钉死。
{
  const { applyMove } = await import('../web/js/game.js');
  const { renderer } = makeRenderer();

  // 两个 2（指数 1）合成 4，再撤销回两个 2
  const before = [[1, 1, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]];
  const result = applyMove(before, DIRECTION.left);
  const afterBoard = result.board; // 撤销前：一块 4

  renderer.reset(afterBoard, { score: 4, best: 0 });
  check('撤销前棋盘只有一块 4', tileCount(renderer) === 1, `实际 ${tileCount(renderer)}`);

  const promise = renderer.animateUndo(before, 4, 0, 0);

  // t=0（还没 await，也就是"按下撤销的那一帧"）：合并块必须**还没回退**。
  // ⚠️ 判据是"那块 4 还在"，不是"没有 2"：渐显的块此时已经插入 DOM
  // （带 .undo-appear，opacity:0，肉眼不可见），所以 DOM 里本来就会有 2。
  const startValues = domValues(renderer);
  check(
    '按下撤销的瞬间，合并块的 4 还在（这是"一闪一闪"的回归点）',
    startValues.includes(4),
    `实际 [${startValues.join(',')}]`
  );
  const hidden = [...renderer.tilesLayer.querySelectorAll('.undo-appear')].length;
  check('渐显的块此时是隐藏状态（挂着 .undo-appear）', hidden === 2, `实际 ${hidden} 个`);

  await promise;

  const valuesAfter = domValues(renderer);
  check(
    '动画结束后数值已回退成两个 2',
    valuesAfter.filter((v) => v === 2).length === 2,
    `实际 [${valuesAfter.join(',')}]`
  );
  check('动画结束后块数与撤销后的棋盘一致', tileCount(renderer) === 2, `实际 ${tileCount(renderer)}`);
  check('撤销动画结束后 busy 为假', renderer.busy() === false);
}

console.log('');
if (failures > 0) {
  console.error(`renderer 冒烟测试失败：${failures} 项`);
  process.exit(1);
}
console.log('renderer 冒烟测试通过');
