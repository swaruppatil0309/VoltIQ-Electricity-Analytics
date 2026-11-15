/* ==========================================================================
   engine.js — JavaScript mirror of the C++17 expression engine.

   The C++ implementation in ../cpp is the canonical one; this file exists so
   the dashboard can also run as a single HTML file with no server at all.
   It reproduces byte-for-byte the same JSON that `xpr serve` returns, which is
   verified by tests/parity.js.

   Pipeline: tokenize → shunting-yard (trace) → AST → constant folding →
   optimised postfix → stack-machine evaluation (trace).
   ========================================================================== */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  root.VoltIQEngine = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  const K_UNARY_PREC = 3;

  const FUNCTIONS = {
    sin: 1, cos: 1, tan: 1, asin: 1, acos: 1, atan: 1,
    sqrt: 1, cbrt: 1, abs: 1, log: 1, ln: 1, log10: 1,
    exp: 1, floor: 1, ceil: 1, round: 1, sign: 1,
    min: 2, max: 2, pow: 2, mod: 2
  };

  function precedenceOf(op) {
    if (op === '+' || op === '-') return 1;
    if (op === '*' || op === '/' || op === '%') return 2;
    if (op === '^') return 4;
    return 0;
  }

  function isRightAssoc(op) { return op === '^'; }
  function isOperatorChar(c) { return '+-*/%^'.indexOf(c) >= 0; }
  function isDigit(c) { return c >= '0' && c <= '9'; }
  function isAlpha(c) { return /[A-Za-z_]/.test(c); }
  function isIdentChar(c) { return /[A-Za-z0-9_]/.test(c); }
  function isSpace(c) { return /\s/.test(c); }

  function formatNumber(v) {
    if (v === 0) return '0';
    if (!isFinite(v)) {
      if (isNaN(v)) return 'NaN';
      return v > 0 ? 'Infinity' : '-Infinity';
    }
    if (Math.abs(v) < 1e15 && Number.isInteger(v)) return String(v);
    let s = v.toPrecision(12);
    if (s.indexOf('e') < 0) {
      if (s.indexOf('.') >= 0) s = s.replace(/0+$/, '').replace(/\.$/, '');
      return s;
    }
    // normalise "1.50000000000e+21" → "1.5e+21"
    const parts = s.split('e');
    let mantissa = parts[0];
    if (mantissa.indexOf('.') >= 0) mantissa = mantissa.replace(/0+$/, '').replace(/\.$/, '');
    return mantissa + 'e' + parts[1];
  }

  function tokenTypeName(t) {
    return t;
  }

  function tokenLabel(t) {
    if (t.type === 'operator') return t.unary ? t.text + ' (unary)' : t.text;
    if (t.type === 'function') return t.text + '()';
    return t.text;
  }

  function makeToken(fields) {
    return Object.assign({
      type: 'number', text: '', value: 0, op: '', prec: 0, rightAssoc: false,
      unary: false, implicit: false, folded: false, pos: 0, len: 0, arity: 1
    }, fields);
  }

  // =========================================================================
  // tokenizer (mirrors cpp/tokenizer.hpp)
  // =========================================================================
  function tokenize(src) {
    const n = src.length;
    let i = 0;
    let expectOperand = true;
    let openParens = 0;
    const tokens = [];
    let implicitInserts = 0;

    function fail(error, errorPos, errorLen) {
      return { ok: false, error, errorPos, errorLen: errorLen < 1 ? 1 : errorLen, tokens, implicitInserts };
    }
    function implicitMul(pos) {
      tokens.push(makeToken({ type: 'operator', text: '*', op: '*', prec: precedenceOf('*'), implicit: true, pos, len: 0 }));
      implicitInserts++;
    }

    while (i < n) {
      const c = src[i];
      if (isSpace(c)) { i++; continue; }

      // number
      if (isDigit(c) || (c === '.' && i + 1 < n && isDigit(src[i + 1]))) {
        const m = /^[0-9]*\.?[0-9]+([eE][+-]?[0-9]+)?/.exec(src.slice(i));
        const text = m ? m[0] : c;
        if (!expectOperand) return fail('unexpected number — two values in a row', i, text.length);
        tokens.push(makeToken({ type: 'number', text, value: parseFloat(text), pos: i, len: text.length }));
        i += text.length;
        expectOperand = false;
        continue;
      }

      // identifier
      if (isAlpha(c)) {
        const start = i;
        while (i < n && isIdentChar(src[i])) i++;
        const name = src.slice(start, i);
        let j = i;
        while (j < n && isSpace(src[j])) j++;
        const arity = FUNCTIONS[name];
        const call = j < n && src[j] === '(' && arity !== undefined;
        if (!expectOperand) implicitMul(start);
        if (call) {
          tokens.push(makeToken({ type: 'function', text: name, arity, prec: 5, pos: start, len: name.length }));
          expectOperand = true;
        } else {
          tokens.push(makeToken({ type: 'variable', text: name, pos: start, len: name.length }));
          expectOperand = false;
        }
        continue;
      }

      // (
      if (c === '(') {
        if (!expectOperand) implicitMul(i);
        tokens.push(makeToken({ type: 'lparen', text: '(', pos: i, len: 1 }));
        openParens++;
        i++;
        expectOperand = true;
        continue;
      }

      // )
      if (c === ')') {
        if (expectOperand) return fail(tokens.length ? "expected a value before ')'" : 'empty parentheses', i, 1);
        if (openParens === 0) return fail("unmatched ')'", i, 1);
        tokens.push(makeToken({ type: 'rparen', text: ')', pos: i, len: 1 }));
        openParens--;
        i++;
        expectOperand = false;
        continue;
      }

      // ,
      if (c === ',') {
        if (expectOperand) return fail("expected a value before ','", i, 1);
        tokens.push(makeToken({ type: 'comma', text: ',', pos: i, len: 1 }));
        i++;
        expectOperand = true;
        continue;
      }

      // operators
      if (isOperatorChar(c)) {
        if (expectOperand) {
          if (c !== '+' && c !== '-') return fail("operator '" + c + "' needs a value on its left", i, 1);
          tokens.push(makeToken({
            type: 'operator', text: c, op: c, unary: true,
            prec: K_UNARY_PREC, rightAssoc: true, pos: i, len: 1
          }));
          i++;
          continue;
        }
        tokens.push(makeToken({
          type: 'operator', text: c, op: c, prec: precedenceOf(c),
          rightAssoc: isRightAssoc(c), pos: i, len: 1
        }));
        i++;
        expectOperand = true;
        continue;
      }

      return fail("unexpected character '" + c + "'", i, 1);
    }

    if (tokens.length === 0) return fail('expression is empty', 0, 1);
    if (openParens > 0) {
      let pos = 0;
      for (const t of tokens) if (t.type === 'lparen') pos = t.pos;
      return fail("unclosed '(' — add a matching ')'", pos, 1);
    }
    if (expectOperand) {
      const last = tokens[tokens.length - 1];
      return fail('expression ends where a value was expected', last.pos, last.len);
    }
    return { ok: true, error: '', errorPos: 0, errorLen: 1, tokens, implicitInserts };
  }

  function normalizedText(tokens) {
    return tokens.map(t => t.implicit ? '·' : t.text).join(' ');
  }

  function rpnText(rpn) {
    return rpn.map(t => {
      if (t.type === 'function') return t.text + '()';
      if (t.unary) return t.text + 'u';
      return t.text;
    }).join(' ');
  }

  // =========================================================================
  // shunting-yard (mirrors cpp/shunting_yard.hpp)
  // =========================================================================
  function stackLabel(t) {
    if (t.type === 'function') return t.text + '()';
    if (t.type === 'lparen') return '(';
    if (t.type === 'operator' && t.unary) return t.text + 'u';
    return t.text;
  }

  function shunting(tokens, wantTrace) {
    const ops = [];
    const argCounts = [];
    const out = [];
    const steps = [];
    let peakStack = 0;

    function snapshot() {
      const stack = ops.map(stackLabel);
      const output = out.map(t => t.text);
      if (ops.length > peakStack) peakStack = ops.length;
      return { stack, output };
    }

    function record(token, action, note, tokenIndex) {
      if (!wantTrace) return;
      const shot = snapshot();
      steps.push({
        step: steps.length + 1, token, action, note,
        stack: shot.stack, output: shot.output, tokenIndex
      });
    }

    function fail(error, errorPos) {
      return { ok: false, error, errorPos, rpn: out, steps, peakStack };
    }

    for (let ti = 0; ti < tokens.length; ti++) {
      const t = tokens[ti];
      const label = t.implicit ? '* (implicit)' : tokenLabel(t);

      if (t.type === 'number' || t.type === 'variable') {
        out.push(t);
        record(label, 'output', 'operand → output queue', ti);
        continue;
      }

      if (t.type === 'function') {
        ops.push(t);
        record(label, 'push', 'function waits on the stack for its arguments', ti);
        continue;
      }

      if (t.type === 'comma') {
        if (!ops.length) return fail("',' outside a function call", t.pos);
        while (ops.length && ops[ops.length - 1].type !== 'lparen') {
          out.push(ops.pop());
          record('', 'pop', "',' closes the previous argument: pop until '('", ti);
        }
        if (!ops.length || !argCounts.length) return fail("',' outside a function call", t.pos);
        argCounts[argCounts.length - 1] += 1;
        record(label, 'comma', 'argument boundary recorded', ti);
        continue;
      }

      if (t.type === 'lparen') {
        ops.push(t);
        argCounts.push(0);
        record(label, 'push', 'left parenthesis marks the start of a group', ti);
        continue;
      }

      if (t.type === 'rparen') {
        while (ops.length && ops[ops.length - 1].type !== 'lparen') {
          out.push(ops.pop());
          record('', 'pop', "')' forces pending operators out to the output", ti);
        }
        if (!ops.length) return fail("unmatched ')'", t.pos);
        ops.pop();
        const commas = argCounts.length ? argCounts.pop() : 0;
        record(label, 'match', "parenthesis matched — '(' is discarded", ti);

        if (ops.length && ops[ops.length - 1].type === 'function') {
          const fn = ops.pop();
          const args = commas + 1;
          if (args !== fn.arity) {
            return fail(fn.text + '() expects ' + fn.arity + (fn.arity === 1 ? ' argument' : ' arguments') +
              ' but got ' + args, fn.pos);
          }
          out.push(fn);
          record(fn.text + '()', 'output', 'arguments are complete — the function moves to the output', ti);
        }
        continue;
      }

      // binary / unary operator
      const prec = t.unary ? K_UNARY_PREC : t.prec;
      while (!t.unary && ops.length) {
        const top = ops[ops.length - 1];
        if (top.type === 'function') {
          out.push(ops.pop());
          record('', 'pop', 'functions bind tightest — pop it first', ti);
          continue;
        }
        if (top.type !== 'operator') break;
        const topPrec = top.unary ? K_UNARY_PREC : top.prec;
        const shouldPop = topPrec > prec || (topPrec === prec && !t.rightAssoc);
        if (!shouldPop) break;
        out.push(ops.pop());
        record('', 'pop', "operator '" + stackLabel(out[out.length - 1]) +
          "' binds tighter or is left-associative — pop it first", ti);
      }
      ops.push(t);
      record(label, 'push', t.unary ? 'unary operator waits on the stack'
        : "'" + t.op + "' waits: higher precedence stays on top", ti);
    }

    while (ops.length) {
      if (ops[ops.length - 1].type === 'lparen') return fail("unclosed '('", ops[ops.length - 1].pos);
      out.push(ops.pop());
      record('', 'pop', 'end of input — flush the remaining stack', -1);
    }
    if (wantTrace) snapshot();
    return { ok: true, error: '', errorPos: 0, rpn: out, steps, peakStack };
  }

  // =========================================================================
  // expression tree + constant folding (mirrors cpp/ast.hpp)
  // =========================================================================
  function applyOp(op, unary, a, b) {
    if (unary) return op === '-' ? -b : b;
    switch (op) {
      case '+': return a + b;
      case '-': return a - b;
      case '*': return a * b;
      case '/': return a / b;
      case '%': return a % b;
      case '^': return Math.pow(a, b);
      default: return 0;
    }
  }

  function applyFunction(name, args, arity) {
    const a = args[0];
    const b = arity > 1 ? args[1] : 0;
    switch (name) {
      case 'sin': return Math.sin(a);
      case 'cos': return Math.cos(a);
      case 'tan': return Math.tan(a);
      case 'asin': return Math.asin(a);
      case 'acos': return Math.acos(a);
      case 'atan': return Math.atan(a);
      case 'sqrt': return Math.sqrt(a);
      case 'cbrt': return Math.cbrt(a);
      case 'abs': return Math.abs(a);
      case 'log': case 'ln': return Math.log(a);
      case 'log10': return Math.log10(a);
      case 'exp': return Math.exp(a);
      case 'floor': return Math.floor(a);
      case 'ceil': return Math.ceil(a);
      case 'round': return Math.round(a);
      case 'sign': return Math.sign(a);
      case 'min': return Math.min(a, b);
      case 'max': return Math.max(a, b);
      case 'pow': return Math.pow(a, b);
      case 'mod': return a % b;
      default: return NaN;
    }
  }

  function buildAst(rpn) {
    const nodes = [];
    const st = [];
    function add(n) { nodes.push(n); return nodes.length - 1; }

    for (const t of rpn) {
      if (t.type === 'number' || t.type === 'variable') {
        st.push(add({
          type: t.type, name: t.text, text: t.text, value: t.value, op: '', unary: false,
          implicit: false, left: -1, right: -1, pos: t.pos, len: t.len, folded: false, foldText: ''
        }));
      } else if (t.type === 'operator') {
        const n = {
          type: 'operator', name: '', text: '', value: 0, op: t.op, unary: !!t.unary,
          implicit: !!t.implicit, left: -1, right: -1, pos: t.pos, len: t.len, folded: false, foldText: ''
        };
        if (st.length) n.right = st.pop();
        if (t.unary) { n.left = n.right; n.right = -1; }
        else if (st.length) n.left = st.pop();
        st.push(add(n));
      } else if (t.type === 'function') {
        const n = {
          type: 'function', name: t.text, text: '', value: 0, op: '', unary: false,
          implicit: false, left: -1, right: -1, pos: t.pos, len: t.len, folded: false, foldText: ''
        };
        if (t.arity === 2) {
          if (st.length) n.right = st.pop();
          if (st.length) n.left = st.pop();
        } else if (st.length) {
          n.left = st.pop();
        }
        st.push(add(n));
      }
    }
    return { nodes, root: st.length ? st[st.length - 1] : -1, foldedNodes: 0 };
  }

  function foldNode(tree, idx) {
    if (idx < 0) return false;
    const n = tree.nodes[idx];
    if (n.type === 'number') return true;
    if (n.type === 'variable') return false;

    const leftConst = foldNode(tree, n.left);
    const rightConst = n.right >= 0 ? foldNode(tree, n.right) : true;
    if (!leftConst || !rightConst) return false;

    const a = n.left >= 0 ? tree.nodes[n.left].value : 0;
    const b = n.right >= 0 ? tree.nodes[n.right].value : 0;
    let result = 0;

    if (n.type === 'operator') {
      if (!n.unary && (n.op === '/' || n.op === '%') && b === 0) return false;
      result = n.unary ? applyOp(n.op, true, 0, a) : applyOp(n.op, false, a, b);
    } else if (n.type === 'function') {
      result = applyFunction(n.name, [a, b], n.right >= 0 ? 2 : 1);
    } else {
      return false;
    }
    if (!isFinite(result)) return false;

    n.type = 'number';
    n.value = result;
    n.folded = true;
    n.left = n.right = -1;
    tree.foldedNodes++;
    return true;
  }

  function emitRpn(tree, idx, out) {
    if (idx < 0) return;
    const n = tree.nodes[idx];
    if (n.type === 'number') {
      out.push(makeToken({
        type: 'number', text: formatNumber(n.value), value: n.value,
        pos: n.pos, len: n.len, folded: !!n.folded, implicit: false
      }));
      return;
    }
    if (n.type === 'variable') {
      out.push(makeToken({ type: 'variable', text: n.name, pos: n.pos, len: n.len }));
      return;
    }
    emitRpn(tree, n.left, out);
    emitRpn(tree, n.right, out);
    out.push(makeToken({
      type: n.type,
      text: n.type === 'function' ? n.name : n.op,
      op: n.op, unary: !!n.unary, implicit: !!n.implicit,
      arity: n.type === 'function' ? (n.right >= 0 ? 2 : 1) : 1,
      prec: n.type === 'function' ? 5 : (n.unary ? K_UNARY_PREC : precedenceOf(n.op)),
      pos: n.pos, len: n.len
    }));
  }

  function astInfix(tree, idx) {
    if (idx < 0) return '';
    const n = tree.nodes[idx];
    if (n.type === 'number') return (n.folded || !n.text) ? formatNumber(n.value) : n.text;
    if (n.type === 'variable') return n.name;
    if (n.type === 'operator' && n.unary) return n.op + astInfix(tree, n.left);
    if (n.type === 'function') {
      if (n.right >= 0) return n.name + '(' + astInfix(tree, n.left) + ', ' + astInfix(tree, n.right) + ')';
      return n.name + '(' + astInfix(tree, n.left) + ')';
    }
    return '(' + astInfix(tree, n.left) + ' ' + n.op + ' ' + astInfix(tree, n.right) + ')';
  }

  function astPrefix(tree, idx) {
    if (idx < 0) return '';
    const n = tree.nodes[idx];
    if (n.type === 'number') return (n.folded || !n.text) ? formatNumber(n.value) : n.text;
    if (n.type === 'variable') return n.name;
    if (n.type === 'operator' && n.unary) return n.op + ' ' + astPrefix(tree, n.left);
    if (n.type === 'function') {
      let args = astPrefix(tree, n.left);
      if (n.right >= 0) args += ' ' + astPrefix(tree, n.right);
      return n.name + (args ? ' ' + args : '');
    }
    return n.op + ' ' + astPrefix(tree, n.left) + ' ' + astPrefix(tree, n.right);
  }

  function astCount(tree, idx) {
    if (idx < 0) return 0;
    const n = tree.nodes[idx];
    return 1 + astCount(tree, n.left) + astCount(tree, n.right);
  }

  function astHeight(tree, idx) {
    if (idx < 0) return 0;
    const n = tree.nodes[idx];
    const l = astHeight(tree, n.left);
    const r = astHeight(tree, n.right);
    return 1 + Math.max(l, r);
  }

  // =========================================================================
  // postfix evaluation (mirrors cpp/evaluator.hpp)
  // =========================================================================
  function evalRpn(rpn, vars, wantTrace) {
    const stack = [];
    const trace = [];
    let peakStack = 0;
    let opsApplied = 0;

    function record(tokenIndex, token, action, note, value) {
      if (!wantTrace) return;
      if (stack.length > peakStack) peakStack = stack.length;
      trace.push({
        step: trace.length + 1, tokenIndex, token, action, note, value,
        stack: stack.map(formatNumber)
      });
    }
    function fail(error, errorPos) {
      return { ok: false, error, errorPos, value: 0, peakStack, opsApplied, trace };
    }

    for (let i = 0; i < rpn.length; i++) {
      const t = rpn[i];
      const label = t.type === 'function' ? t.text + '()' : (t.implicit ? '* (implicit)' : tokenLabel(t));

      if (t.type === 'number') {
        stack.push(t.value);
        record(i, label, 'push', 'push the literal value', formatNumber(t.value));
        continue;
      }
      if (t.type === 'variable') {
        if (!Object.prototype.hasOwnProperty.call(vars, t.text)) {
          return fail("unknown variable '" + t.text + "' — give it a value first", t.pos);
        }
        const v = vars[t.text];
        stack.push(v);
        record(i, label, 'push', 'look the variable up in the hash map and push its value', formatNumber(v));
        continue;
      }
      if (t.type === 'operator') {
        const need = t.unary ? 1 : 2;
        if (stack.length < need) return fail("operator '" + t.op + "' is missing an operand", t.pos);
        const b = stack.pop();
        const a = t.unary ? 0 : stack.pop();
        if (!t.unary && (t.op === '/' || t.op === '%') && b === 0) {
          return fail(t.op === '/' ? 'division by zero' : 'modulo by zero', t.pos);
        }
        const result = applyOp(t.op, t.unary, a, b);
        stack.push(result);
        opsApplied++;
        const note = t.unary
          ? "unary '" + t.op + "' applied to " + formatNumber(b)
          : formatNumber(a) + ' ' + t.op + ' ' + formatNumber(b) + ' = ' + formatNumber(result);
        record(i, label, 'apply', note, formatNumber(result));
        continue;
      }
      if (t.type === 'function') {
        const arity = t.arity > 0 ? t.arity : 1;
        if (stack.length < arity) return fail('function ' + t.text + '() is missing an argument', t.pos);
        const args = [];
        for (let k = arity - 1; k >= 0; k--) args.unshift(stack.pop());
        const result = applyFunction(t.text, args, arity);
        if (isNaN(result) && !(args.every(x => isNaN(x)))) {
          // keep NaN results (sqrt(-1)) — they are legitimate answers
        }
        stack.push(result);
        opsApplied++;
        let call = t.text + '(' + formatNumber(args[0]);
        if (arity === 2) call += ', ' + formatNumber(args[1]);
        call += ') = ' + formatNumber(result);
        record(i, label, 'function', call, formatNumber(result));
        continue;
      }
    }

    if (stack.length === 0) return fail('the expression produced no value', 0);
    if (stack.length > 1) {
      return fail(stack.length + ' values left on the stack — expression is incomplete', 0);
    }
    if (wantTrace) {
      // final snapshot so the last frame shows the single remaining value
      if (stack.length > peakStack) peakStack = stack.length;
    }
    return { ok: true, error: '', errorPos: 0, value: stack[stack.length - 1], peakStack, opsApplied, trace };
  }

  // =========================================================================
  // engine (mirrors cpp/engine.hpp)
  // =========================================================================
  function Engine(capacity) {
    this.capacity = capacity || 64;
    this.vars = Object.create(null);
    this.cache = new Map();  // insertion order == LRU order (we re-insert on hit)
    this.folding = false;
    this.stats = {
      compiles: 0, cacheHits: 0, cacheMisses: 0, evals: 0,
      foldedConstants: 0, tokensLexed: 0, lastCompileUs: 0, lastEvalUs: 0
    };
  }

  Engine.prototype.setVar = function (name, value) { this.vars[name] = value; };
  Engine.prototype.clearVars = function () { this.vars = Object.create(null); };
  Engine.prototype.setFolding = function (on) {
    if (this.folding === on) return;
    this.folding = on;
    this.cache.clear();
  };
  Engine.prototype.resetCache = function () { this.cache.clear(); };

  Engine.prototype.cacheGet = function (key) {
    if (!this.cache.has(key)) return null;
    const value = this.cache.get(key);
    this.cache.delete(key);
    this.cache.set(key, value);  // most recently used
    return value;
  };
  Engine.prototype.cachePut = function (key, value) {
    if (this.cache.has(key)) this.cache.delete(key);
    this.cache.set(key, value);
    while (this.cache.size > this.capacity) this.cache.delete(this.cache.keys().next().value);
  };

  Engine.prototype.compile = function (expr) {
    const tk = tokenize(expr);
    if (!tk.ok) {
      return {
        ok: false, error: tk.error, errorPos: tk.errorPos, errorLen: tk.errorLen,
        expr, tokens: tk.tokens || []
      };
    }
    const cv = shunting(tk.tokens, false);
    if (!cv.ok) {
      return {
        ok: false, error: cv.error, errorPos: cv.errorPos, errorLen: 1,
        expr, tokens: tk.tokens, implicitMuls: tk.implicitInserts
      };
    }

    const rawTree = buildAst(cv.rpn);
    const foldedTree = JSON.parse(JSON.stringify(rawTree));
    foldNode(foldedTree, foldedTree.root);

    const foldedRpn = [];
    emitRpn(foldedTree, foldedTree.root, foldedRpn);

    return {
      ok: true, expr,
      tokens: tk.tokens,
      implicitMuls: tk.implicitInserts,
      normalized: normalizedText(tk.tokens),
      rawRpn: cv.rpn,
      foldedRpn,
      foldedTree,
      foldedSubtrees: foldedTree.foldedNodes,
      foldedInstructions: cv.rpn.length - foldedRpn.length,
      peakOpStack: cv.peakStack,
      foldingActive: this.folding,
      tree: this.folding ? foldedTree : rawTree,
      rpn: this.folding ? foldedRpn : cv.rpn
    };
  };

  // JSON shape must match cpp/main.cpp:jsonTokens exactly
  function serializeTokens(tokens) {
    return tokens.map((t, i) => ({
      i,
      text: t.implicit ? '\u00b7' : t.text,
      type: t.type,
      pos: t.pos,
      len: t.len,
      prec: t.unary ? K_UNARY_PREC : t.prec,
      implicit: !!t.implicit,
      folded: !!t.folded,
      unary: !!t.unary,
      arity: t.arity,
      value: t.value
    }));
  }

  Engine.prototype.analyze = function (expr, wantTrace) {
    const started = (typeof performance !== 'undefined' ? performance.now() : Date.now());
    if (wantTrace === undefined) wantTrace = true;
    if (expr.length > 2000) wantTrace = false;

    let program = this.cacheGet(expr);
    let hit = !!program;
    if (hit) {
      this.stats.cacheHits++;
    } else {
      this.stats.cacheMisses++;
      program = this.compile(expr);
      this.cachePut(expr, program);
      if (program.ok) {
        this.stats.compiles++;
        this.stats.foldedConstants += program.foldedInstructions;
        this.stats.tokensLexed += program.tokens.length;
      }
    }

    const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());
    const analysis = {
      ok: false, expr, normalized: '', tokens: [], foldingActive: false, foldedRpn: [],
      convert: { ok: false, rpn: '', peakStack: 0, steps: [] },
      eval: { ok: false, value: null, valueText: '', peakStack: 0, opsApplied: 0, steps: [] },
      ast: { root: -1, nodeCount: 0, height: 0, foldedNodes: 0, prefix: '', infix: '', nodes: [] },
      stats: {},
      engine: null,
      error: null
    };

    if (!program.ok) {
      analysis.ok = false;
      analysis.tokens = serializeTokens(program.tokens || []);
      analysis.program = {
        raw: rpnText(program.rawRpn || []), active: rpnText(program.rpn || []),
        folded: rpnText(program.foldedRpn || []), foldingActive: !!program.foldingActive
      };
      analysis.error = { message: program.error, pos: program.errorPos || 0, len: program.errorLen || 1 };
      analysis.stats = this.statsFor(analysis, program, hit, now() - started, 0);
      analysis.engine = this.state();
      return analysis;
    }

    analysis.tokens = serializeTokens(program.tokens);
    analysis.program = {
      raw: rpnText(program.rawRpn), active: rpnText(program.rpn),
      folded: rpnText(program.foldedRpn), foldingActive: !!program.foldingActive
    };
    analysis.normalized = program.normalized;
    analysis.foldingActive = program.foldingActive;
    analysis.foldedRpn = program.foldedRpn;

    analysis.convert = wantTrace
      ? (() => {
          const c = shunting(program.tokens, true);
          return { ok: c.ok, rpn: rpnText(c.rpn), peakStack: c.peakStack, steps: c.steps };
        })()
      : { ok: true, rpn: rpnText(program.rawRpn), peakStack: program.peakOpStack, steps: [] };

    const evalStart = now();
    const ev = evalRpn(program.rpn, this.vars, wantTrace);
    const evalUs = (now() - evalStart) * 1000;
    this.stats.evals++;
    this.stats.lastEvalUs = evalUs;

    analysis.eval = {
      ok: ev.ok,
      value: ev.ok ? ev.value : null,
      valueText: ev.ok ? formatNumber(ev.value) : '',
      peakStack: ev.peakStack,
      opsApplied: ev.opsApplied,
      steps: ev.trace
    };

    if (!ev.ok) {
      analysis.ok = false;
      analysis.error = { message: ev.error, pos: ev.errorPos, len: 1 };
    } else {
      analysis.ok = true;
    }

    analysis.ast = {
      root: program.tree.root,
      nodeCount: astCount(program.tree, program.tree.root),
      height: astHeight(program.tree, program.tree.root),
      foldedNodes: program.tree.foldedNodes,
      prefix: astPrefix(program.tree, program.tree.root),
      infix: astInfix(program.tree, program.tree.root),
      nodes: program.tree.nodes.map((n, id) => ({
        id,
        type: n.type,
        label: n.type === 'number' ? ((n.folded || !n.text) ? formatNumber(n.value) : n.text)
          : n.type === 'variable' ? n.name
            : n.type === 'function' ? n.name
              : (n.unary ? n.op + 'u' : n.op),
        left: n.left, right: n.right, folded: !!n.folded, unary: !!n.unary,
        implicit: !!n.implicit, pos: n.pos
      }))
    };

    analysis.stats = this.statsFor(analysis, program, hit, now() - started, evalUs);
    analysis.engine = this.state();
    return analysis;
  };

  Engine.prototype.statsFor = function (analysis, program, hit, compileMs, evalUs) {
    const raw = program.rawRpn ? program.rawRpn.length : 0;
    const active = program.rpn ? program.rpn.length : 0;
    return {
      tokens: program.tokens ? program.tokens.length : 0,
      rawInstructions: raw,
      instructions: active,
      foldedConstants: analysis.foldingActive ? (program.foldedInstructions || 0) : (program.foldedInstructions || 0),
      foldedSubtrees: program.foldedSubtrees || 0,
      implicitMuls: program.implicitMuls || 0,
      astNodes: analysis.ast ? analysis.ast.nodeCount : 0,
      astHeight: analysis.ast ? analysis.ast.height : 0,
      peakOpStack: analysis.convert ? analysis.convert.peakStack : 0,
      peakEvalStack: analysis.eval ? analysis.eval.peakStack : 0,
      traceSteps: (analysis.convert.steps ? analysis.convert.steps.length : 0) +
        (analysis.eval.steps ? analysis.eval.steps.length : 0),
      opsApplied: analysis.eval ? analysis.eval.opsApplied : 0,
      compileUs: hit ? 0 : compileMs * 1000,
      evalUs,
      cacheHit: hit
    };
  };

  Engine.prototype.evaluate = function (expr, wantTrace) {
    let program = this.cacheGet(expr);
    if (!program) {
      this.stats.cacheMisses++;
      program = this.compile(expr);
      this.cachePut(expr, program);
      if (program.ok) this.stats.compiles++;
    } else {
      this.stats.cacheHits++;
    }
    if (!program.ok) return { ok: false, error: program.error, value: 0 };
    const ev = evalRpn(program.rpn, this.vars, !!wantTrace);
    this.stats.evals++;
    return ev;
  };

  Engine.prototype.evaluateNaive = function (expr) {
    const tk = tokenize(expr);
    if (!tk.ok) return { ok: false, error: tk.error, value: 0 };
    const cv = shunting(tk.tokens, false);
    if (!cv.ok) return { ok: false, error: cv.error, value: 0 };
    return evalRpn(cv.rpn, this.vars, false);
  };

  Engine.prototype.state = function () {
    const vars = {};
    Object.keys(this.vars).sort().forEach(k => { vars[k] = this.vars[k]; });
    return {
      compiles: this.stats.compiles,
      cacheHits: this.stats.cacheHits,
      cacheMisses: this.stats.cacheMisses,
      evals: this.stats.evals,
      foldedConstants: this.stats.foldedConstants,
      tokensLexed: this.stats.tokensLexed,
      cacheSize: this.cache.size,
      cacheCapacity: this.capacity,
      cacheOrder: Array.from(this.cache.keys()).reverse(),
      vars
    };
  };

  // =========================================================================
  // benchmark (mirrors `xpr bench`)
  // =========================================================================
  function runBenchmark() {
    const engine = new Engine(256);
    engine.setFolding(true);
    engine.setVar('x', 6);
    engine.setVar('y', 2);

    const N = 20000;
    const expr = '((3+4)*2 - 5) / 7 + 2^3*2 - sqrt(81) + x';
    const foldedExpr = '2*3 + 4*5 - 6/2 + 7*8 - 9*1 + 2^5 + sqrt(16) - abs(-3) + max(2,7) + x';

    const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());
    let sink = 0;
    for (let i = 0; i < 500; i++) sink += engine.evaluateNaive(expr).value;

    let t0 = now();
    for (let i = 0; i < N; i++) sink += engine.evaluateNaive(expr).value;
    const naiveUs = ((now() - t0) * 1000) / N;

    t0 = now();
    for (let i = 0; i < N; i++) sink += engine.evaluate(expr).value || 0;
    const cachedUs = ((now() - t0) * 1000) / N;

    t0 = now();
    for (let i = 0; i < N; i++) sink += engine.evaluateNaive(foldedExpr).value;
    const foldNaiveUs = ((now() - t0) * 1000) / N;

    t0 = now();
    for (let i = 0; i < N; i++) sink += engine.evaluate(foldedExpr).value || 0;
    const foldFastUs = ((now() - t0) * 1000) / N;

    const pool = [];
    for (let i = 0; i < 400; i++) pool.push('(' + i + '+1)*2 - sqrt(16) + x');
    t0 = now();
    for (let pass = 0; pass < 3; pass++) for (const e of pool) sink += engine.evaluate(e).value || 0;
    const poolUs = ((now() - t0) * 1000) / (3 * pool.length);

    const a = engine.analyze(expr, false);
    const f = engine.analyze(foldedExpr, false);

    const scaling = [16, 256, 4096].map(terms => {
      let longExpr = '1';
      for (let i = 0; i < terms; i++) longExpr += '+1';
      const reps = terms <= 256 ? 300 : 20;
      const probe = new Engine(8);
      const s0 = now();
      let acc = 0;
      for (let i = 0; i < reps; i++) acc += probe.evaluateNaive(longExpr).value;
      const us = ((now() - s0) * 1000) / reps;
      return { terms: terms + 1, tokens: 2 * terms + 1, micros: us, perTokenUs: us / (2 * terms + 1) };
    });

    const hits = engine.cache.hits || 0;
    void sink;
    return {
      ok: true,
      iterations: N,
      measuredIn: 'browser (JS mirror)',
      rows: [
        {
          label: 'Evaluate one expression ' + N + ' times',
          fastLabel: 'cached compiled program', fastUs: cachedUs,
          slowLabel: 're-tokenize + re-parse every time', slowUs: naiveUs,
          speedup: naiveUs / (cachedUs || 1e-9),
          note: 'LRU cache removes lexing and parsing from the hot path'
        },
        {
          label: 'Constant-heavy expression',
          fastLabel: 'compiled + constant folding', fastUs: foldFastUs,
          slowLabel: 'naive pipeline', slowUs: foldNaiveUs,
          speedup: foldNaiveUs / (foldFastUs || 1e-9),
          note: f.stats.rawInstructions + ' instructions folded down to ' + f.stats.instructions
        },
        {
          label: '400 distinct expressions, cache warm',
          fastLabel: 'cached program', fastUs: poolUs,
          slowLabel: 're-parse', slowUs: naiveUs,
          speedup: naiveUs / (poolUs || 1e-9),
          note: 'LRU capacity ' + engine.capacity + ', hit rate ' +
            (100 * engine.stats.cacheHits / (engine.stats.cacheHits + engine.stats.cacheMisses + 1e-9)).toFixed(1) + '%'
        }
      ],
      example: {
        expr, raw: rpnText(a.convert ? shunting(engine.compile(expr).tokens, false).rpn : []),
        optimized: rpnText(engine.compile(expr).foldedRpn),
        result: formatNumber(engine.evaluate(expr).value)
      },
      foldedExample: {
        expr: foldedExpr,
        raw: rpnText(engine.compile(foldedExpr).rawRpn),
        optimized: rpnText(engine.compile(foldedExpr).foldedRpn),
        result: formatNumber(engine.evaluate(foldedExpr).value)
      },
      scaling,
      folding: {
        enabled: true,
        rawInstructions: f.stats.rawInstructions,
        optimizedInstructions: f.stats.instructions,
        foldedSubtrees: f.stats.foldedSubtrees
      }
    };
  }

  // =========================================================================
  // fetch-compatible offline API
  // =========================================================================
  const EXAMPLES = [
    { label: 'Basic precedence', expr: '3 + 4 * 2' },
    { label: 'Parentheses', expr: '(3 + 4) * 2' },
    { label: 'Right-associative power', expr: '2 ^ 3 ^ 2' },
    { label: 'Unary minus vs power', expr: '-2 ^ 2' },
    { label: 'Implicit multiply', expr: '2(3 + 4) ^ 2' },
    { label: 'Variables', expr: '2x^2 - 3x + 1' },
    { label: 'Functions', expr: 'sqrt(81) + max(2, 7)' },
    { label: 'Constant folding', expr: '2*3 + 4*5 - 6/2 + 7*8' },
    { label: 'Bigger program', expr: '((3+4)*2 - 5) / 7 + 2^3*2 - sqrt(81) + x' },
    { label: 'Percentage', expr: '1250 * 18 % 100' },
    { label: 'Circle area', expr: '3.14159 * r ^ 2' },
    { label: 'Error demo', expr: '3 + * 4' }
  ];

  function jsonResponse(payload) {
    const body = JSON.stringify(payload);
    return Promise.resolve({
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      text: () => Promise.resolve(body),
      json: () => Promise.resolve(JSON.parse(body))
    });
  }

  function readBody(opts) {
    if (!opts || !opts.body) return {};
    if (typeof opts.body === 'string') {
      try { return JSON.parse(opts.body); } catch (e) { return {}; }
    }
    return opts.body;
  }

  function applyVars(engine, vars) {
    if (!vars || typeof vars !== 'object') return;
    Object.keys(vars).forEach(k => {
      const v = Number(vars[k]);
      if (isFinite(v)) engine.setVar(k, v);
    });
  }

  function createEngine() {
    const engine = new Engine(128);
    engine.setVar('x', 6);
    engine.setVar('y', 3);
    engine.setVar('r', 5);

    return function offlineFetch(url, opts) {
      const path = String(url).split('?')[0];
      const method = ((opts && opts.method) || 'GET').toUpperCase();
      const body = readBody(opts);

      if (path === '/api/health') {
        return jsonResponse({ ok: true, service: 'voltiq-expr-dsa', engine: 'javascript mirror', version: '1.0' });
      }
      if (path === '/api/examples') {
        return jsonResponse({ ok: true, examples: EXAMPLES });
      }
      if (path === '/api/bench') {
        return jsonResponse(runBenchmark());
      }
      if (path === '/api/state') {
        return jsonResponse({ ok: true, engine: engine.state() });
      }
      if (method === 'POST' && path === '/api/analyze') {
        if (body.vars) engine.clearVars();
        applyVars(engine, body.vars);
        if (typeof body.fold === 'boolean') engine.setFolding(body.fold);
        const analysis = engine.analyze(body.expr || '', body.trace !== false);
        return jsonResponse(analysis);
      }
      if (method === 'POST' && path === '/api/eval') {
        if (body.vars) engine.clearVars();
        applyVars(engine, body.vars);
        const result = engine.evaluate(body.expr || '');
        return jsonResponse({
          ok: result.ok,
          value: result.ok ? result.value : null,
          valueText: result.ok ? formatNumber(result.value) : '',
          error: result.ok ? null : result.error,
          engine: engine.state()
        });
      }
      if (method === 'POST' && path === '/api/vars') {
        applyVars(engine, body.vars);
        return jsonResponse({ ok: true, engine: engine.state() });
      }
      if (method === 'POST' && path === '/api/reset') {
        engine.clearVars();
        engine.resetCache();
        return jsonResponse({ ok: true, engine: engine.state() });
      }
      return jsonResponse({ ok: false, error: 'not found' });
    };
  }

  return {
    createEngine, tokenize, shunting, evalRpn, Engine,
    formatNumber, runBenchmark, rpnText, EXAMPLES
  };
});
