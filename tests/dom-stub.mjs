// 极小的 DOM 桩：只实现 renderer.js 用到的那部分 API。
//
// 为什么值得写：
// renderer 的动画序列（滑动 → 合并换值 → 删掉被吸收的块 → 生成新块）
// 是最容易藏 bug 的地方 —— 而它全部依赖 DOM。没有浏览器就意味着这段代码
// 平时根本跑不到，只能靠肉眼看。
//
// 有了这个桩，就能在 Node 里把真实 renderer 跑起来，断言：
//   - 动画结束后方块对象数量 = 棋盘上的非空格数
//   - 每个方块的位置与逻辑状态一致
//   - 被吸收的块确实被删掉（不泄漏 DOM 节点）
//   - busy() 在动画期间为真、结束后为假
//
// 它**不是** jsdom，不追求通用；够用就行，也不进项目仓库（放 tests\dom-stub.mjs）。
// 注意：项目本身保持零依赖，这个桩只在测试时用。

class FakeClassList {
  constructor() {
    this._set = new Set();
  }
  add(...names) {
    for (const n of names) this._set.add(n);
  }
  remove(...names) {
    for (const n of names) this._set.delete(n);
  }
  contains(name) {
    return this._set.has(name);
  }
  toggle(name, force) {
    const want = force === undefined ? !this._set.has(name) : Boolean(force);
    if (want) this._set.add(name);
    else this._set.delete(name);
    return want;
  }
  toString() {
    return [...this._set].join(' ');
  }
}

class FakeElement {
  constructor(tag) {
    this.tagName = String(tag).toUpperCase();
    this.children = [];
    this.parentElement = null;
    this.classList = new FakeClassList();
    this._text = '';
    this._listeners = new Map();
    this._innerHTML = '';
    // style 用普通对象即可；renderer 只做赋值与 setProperty
    this.style = {
      _props: new Map(),
      setProperty(k, v) {
        this._props.set(k, v);
      },
      getPropertyValue(k) {
        return this._props.get(k) ?? '';
      },
    };
    // renderer 会读 clientWidth 计算格子尺寸
    this.clientWidth = 460;
    this.offsetWidth = 460;
    // renderer 用 dataset 标记方块的值与位数（CSS 按 data-value 上色）
    this.dataset = {};
    // getBoundingClientRect：庆祝烟花模块用它拿画布尺寸
    this._rect = { width: 460, height: 460, top: 0, left: 0 };
  }

  getBoundingClientRect() {
    return this._rect;
  }

  get className() {
    return this.classList.toString();
  }

  set className(value) {
    this.classList = new FakeClassList();
    for (const name of String(value).split(/\s+/)) {
      if (name) this.classList.add(name);
    }
  }

  get textContent() {
    return this._text;
  }

  set textContent(value) {
    this._text = String(value);
  }

  get innerHTML() {
    return this._innerHTML;
  }

  set innerHTML(value) {
    this._innerHTML = String(value);
    if (value === '') {
      for (const child of this.children) child.parentElement = null;
      this.children = [];
    }
  }

  appendChild(child) {
    child.parentElement = this;
    this.children.push(child);
    return child;
  }

  remove() {
    if (!this.parentElement) return;
    const list = this.parentElement.children;
    const index = list.indexOf(this);
    if (index >= 0) list.splice(index, 1);
    this.parentElement = null;
  }

  addEventListener(type, handler) {
    if (!this._listeners.has(type)) this._listeners.set(type, []);
    this._listeners.get(type).push(handler);
  }

  dispatch(type, event = {}) {
    for (const handler of this._listeners.get(type) || []) handler(event);
  }

  /** 只支持 "." 类选择器与标签名，够 renderer 用。 */
  querySelectorAll(selector) {
    const out = [];
    const walk = (node) => {
      for (const child of node.children) {
        if (selector.startsWith('.') ? child.classList.contains(selector.slice(1)) : child.tagName === selector.toUpperCase()) {
          out.push(child);
        }
        walk(child);
      }
    };
    walk(this);
    return out;
  }
}

class FakeDocument {
  constructor() {
    this.body = new FakeElement('body');
  }
  createElement(tag) {
    return new FakeElement(tag);
  }
  getElementById() {
    return null;
  }
  addEventListener() {}
}

/** 建立一个最小的 window/getComputedStyle 环境，返回恢复函数。 */
export function installDomStub() {
  const document = new FakeDocument();
  const saved = {
    document: globalThis.document,
    window: globalThis.window,
    getComputedStyle: globalThis.getComputedStyle,
    setTimeout: globalThis.setTimeout,
  };

  globalThis.document = document;
  globalThis.window = {
    matchMedia: () => ({ matches: false, addEventListener() {}, removeEventListener() {} }),
  };
  // renderer 用它读 --gap
  globalThis.getComputedStyle = () => ({
    getPropertyValue: (name) => (name === '--gap' ? '12' : ''),
  });
  // 动画里用的 setTimeout 保持真实实现（毫秒级），所以不需要替换

  return {
    document,
    restore() {
      globalThis.document = saved.document;
      globalThis.window = saved.window;
      globalThis.getComputedStyle = saved.getComputedStyle;
    },
  };
}

export { FakeElement, FakeDocument };
