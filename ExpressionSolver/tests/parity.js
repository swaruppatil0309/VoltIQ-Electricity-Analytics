#!/usr/bin/env node
/* ==========================================================================
   parity.js — proves the JavaScript mirror returns the same analysis as the
   compiled C++ engine. Every case is run through `bin/xpr analyze` and through
   standalone/engine.js; the two JSON documents must agree.
     node tests/parity.js
   ========================================================================== */
'use strict';

const { execFileSync } = require('child_process');
const path = require('path');
const engine = require('../standalone/engine.js');

const BIN = path.join(__dirname, '..', 'bin', 'xpr');

const CASES = [
  '3 + 4 * 2', '(3 + 4) * 2', '2 ^ 3 ^ 2', '-2 ^ 2', '2^-2', '-2^2 + 10',
  '3(4+5)', '(1+2)(3+4)', '2x^2 - 3x + 1', 'sqrt(81) + max(2, 7)',
  '2*3 + 4*5 - 6/2 + 7*8', '((3+4)*2 - 5) / 7 + 2^3*2 - sqrt(81) + x',
  '1250 * 18 % 100', '3.14159 * r ^ 2', 'min(3,8)*max(2,5)', 'abs(-3) - -2',
  '1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10', 'x(x+1)', 'pow(2, 10) / 4',
  'log(100) + ln(1)', '2 ^ -3', 'round(2.6) + floor(2.9) + ceil(2.1)',
  '3 + * 4', '(3+4', '3/0', 'q + 1', 'sin(1,2)', '3 4', '3 $ 4', ' ',
  '1/z', 'x + y + r', 'max(2,7) + min(3,8)', '2(3+4)^2 - sqrt(81)/3 + x'
];

function runCpp(expr) {
  try {
    const out = execFileSync(BIN, ['analyze', expr], { encoding: 'utf8', timeout: 20000 });
    return JSON.parse(out);
  } catch (err) {
    // non-zero exit code still prints the analysis on stdout
    if (err.stdout) return JSON.parse(err.stdout);
    throw err;
  }
}

function runJs(expr) {
  const eng = new engine.Engine(16);
  eng.setVar('x', 6);
  eng.setVar('y', 3);
  eng.setVar('r', 5);
  return eng.analyze(expr, true);
}

const FIELDS = [
  ['ok', d => d.ok],
  ['program.raw', d => d.program ? d.program.raw : null],
  ['program.active', d => d.program ? d.program.active : null],
  ['program.folded', d => d.program ? d.program.folded : null],
  ['result', d => (d.eval && d.eval.ok ? Number(d.eval.value) : null)],
  ['convert.steps', d => d.convert.steps.length],
  ['eval.steps', d => d.eval.steps.length],
  ['stats.instructions', d => d.stats.instructions],
  ['ast.prefix', d => d.ast.prefix],
  ['error.message', d => (d.error ? d.error.message : null)],
  ['error.pos', d => (d.error ? d.error.pos : null)]
];

let failures = 0;
console.log('C++ ↔ JavaScript parity test');
console.log('-'.repeat(78));
for (const expr of CASES) {
  const a = runCpp(expr);
  const b = runJs(expr);
  const diffs = [];
  for (const [name, pick] of FIELDS) {
    const left = pick(a);
    const right = pick(b);
    if (left !== right && !(typeof left === 'number' && typeof right === 'number' &&
        Math.abs(left - right) < 1e-9)) {
      diffs.push(`${name}: cpp=${JSON.stringify(left)} js=${JSON.stringify(right)}`);
    }
  }
  if (diffs.length) {
    failures++;
    console.log(`FAIL  ${expr}`);
    diffs.forEach(d => console.log('        ' + d));
  } else {
    console.log(`ok    ${expr || '(empty)'}`);
  }
}
console.log('-'.repeat(78));
console.log(failures === 0
  ? `ALL ${CASES.length} CASES MATCH`
  : `${failures} / ${CASES.length} CASES DIVERGED`);
process.exit(failures === 0 ? 0 : 1);
