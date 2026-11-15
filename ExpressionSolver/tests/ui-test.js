#!/usr/bin/env node
/* ==========================================================================
   ui-test.js — headless smoke test of the single-file dashboard.
   Loads VoltIQ-Expr-Standalone.html in jsdom and drives the real UI:
   playback, error reporting, constant folding, examples, variables, keyboard
   shortcuts and the benchmark panel.

     node standalone/build-standalone.js   # build the file under test
     npm install --no-save jsdom           # only needed for this test
     node tests/ui-test.js
   ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');

let JSDOM = null;
try {
  JSDOM = require(path.join(process.cwd(), 'node_modules', 'jsdom')).JSDOM;
} catch (e) {
  try {
    JSDOM = require('jsdom').JSDOM;
  } catch (e2) {
    console.log('jsdom is not installed — run: npm install --no-save jsdom');
    process.exit(0);
  }
}

const html = fs.readFileSync(path.join(process.cwd(), 'VoltIQ-Expr-Standalone.html'), 'utf8');

let failures = 0;
const check = (cond, label, extra) => {
  console.log((cond ? '  PASS  ' : '  FAIL  ') + label + (cond || extra === undefined ? '' : '  → ' + extra));
  if (!cond) failures++;
};

const dom = new JSDOM(html, {
  runScripts: 'dangerously',
  pretendToBeVisual: true,
  url: 'https://example.test/index.html',
  beforeParse(window) {
    window.navigator.clipboard = { writeText: () => Promise.resolve() };
  }
});

const { window } = dom;
const doc = window.document;

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  // stop the demo autoplay so the DOM is deterministic
  await wait(700);
  const play = doc.querySelector('#playBtn');
  if (play && play.textContent.includes('Pause')) play.click();
  await wait(150);

  const val = doc.querySelector('#resultValue').textContent.trim();
  check(val === '55', 'default expression 2x^2 - 3x + 1 with x=6 evaluates to 55', val);

  const active = doc.querySelector('#activeProgram').textContent.trim();
  check(active === '2 x 2 ^ * 3 x * - 1 +', 'compiled program is shown', active);

  const tokenCount = doc.querySelectorAll('#tokenStrip .token').length;
  check(tokenCount === 11, 'token strip rendered (11 tokens)', tokenCount);

  const rpnCount = doc.querySelectorAll('#rpnStrip .token').length;
  check(rpnCount === 11, 'rpn strip rendered (11 instructions)', rpnCount);

  check(doc.querySelectorAll('#statChips .stat').length === 7, 'seven stat chips rendered');
  check(doc.querySelector('#treeCanvas svg') !== null, 'expression tree svg rendered');
  check(doc.querySelectorAll('#treeCanvas .tree-node').length === 11, 'tree has 11 nodes');
  check(doc.querySelectorAll('#engineGrid .engine-metric').length === 8, 'engine metrics rendered');

  // play through every step
  doc.querySelector('#resetBtn').click();
  await wait(50);
  let guard = 0;
  while (guard++ < 200) {
    const next = doc.querySelector('#nextBtn');
    if (next.disabled) break;
    next.click();
    await wait(5);
  }
  check(guard < 200, 'reached the last timeline step', 'iterations ' + guard);
  check(doc.querySelector('#stepCounter').textContent.includes('step'), 'step counter updates');
  check(doc.querySelector('#liveValue').textContent.trim() === '55', 'final stack value is 55',
    doc.querySelector('#liveValue').textContent);
  check(doc.querySelector('#evalPane').className.includes('focus'), 'evaluation pane takes focus at the end');

  // error handling
  const input = doc.querySelector('#exprInput');
  input.value = '(3+4';
  input.dispatchEvent(new window.Event('input', { bubbles: true }));
  await wait(600);
  check(!doc.querySelector('#errorBanner').hidden, 'error banner shown for an unclosed parenthesis');
  check(doc.querySelector('#errorMessage').textContent.includes('unclosed'), 'error message is specific',
    doc.querySelector('#errorMessage').textContent);
  check(doc.querySelector('#resultValue').textContent === 'error', 'result shows an error state');

  // a valid expression again
  input.value = 'sqrt(81) + max(2,7)';
  input.dispatchEvent(new window.Event('input', { bubbles: true }));
  await wait(600);
  check(doc.querySelector('#resultValue').textContent.trim() === '16', 'sqrt(81)+max(2,7) = 16',
    doc.querySelector('#resultValue').textContent);
  check(doc.querySelector('#errorBanner').hidden, 'error banner cleared');

  // constant folding toggle
  doc.querySelector('#foldToggle').click();
  await wait(600);
  const folded = doc.querySelector('#foldedProgram').textContent.trim();
  check(folded === '16', 'folding collapses the whole program to 16', folded);
  check(doc.querySelector('#resultValue').textContent.trim() === '16', 'folding preserves the value');

  // examples chip
  doc.querySelector('#foldToggle').click();
  await wait(400);
  doc.querySelector('#exampleChips .chip:nth-child(3)').click();
  await wait(600);
  check(input.value === '2 ^ 3 ^ 2', 'example chip loads its expression', input.value);
  check(doc.querySelector('#resultValue').textContent.trim() === '512', '2^3^2 = 512 (right associative)');

  // add + remove a variable
  const newVar = doc.querySelector('#newVar');
  newVar.value = 'z=4';
  doc.querySelector('#addVarForm').dispatchEvent(new window.Event('submit', { bubbles: true, cancelable: true }));
  await wait(500);
  check(doc.querySelectorAll('#varChips .var-chip').length === 4, 'variable added to the chips',
    doc.querySelectorAll('#varChips .var-chip').length);

  // keyboard stepping
  doc.body.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true }));
  await wait(50);
  check(doc.querySelector('#stepCounter').textContent.includes('step 1 /'), 'arrow key steps forward',
    doc.querySelector('#stepCounter').textContent);

  // benchmark
  doc.querySelector('#benchBtn').click();
  await wait(4000);
  const rows = doc.querySelectorAll('.bench-row').length;
  check(rows === 3, 'benchmark rendered three rows', rows);

  console.log('-' .repeat(60));
  console.log(failures === 0 ? 'UI SMOKE TEST PASSED' : failures + ' UI CHECK(S) FAILED');
  process.exit(failures === 0 ? 0 : 1);
})();

window.addEventListener('error', (e) => {
  console.error('window error:', e.message);
});
