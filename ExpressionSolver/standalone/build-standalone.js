#!/usr/bin/env node
/* ==========================================================================
   build-standalone.js
   Bundles the dashboard (index.html + styles.css + app.js) and the offline
   engine (engine.js) into ONE self-contained HTML file that runs from file://
   with no server, no compiler and no internet.

     node build-standalone.js
     →  ../VoltIQ-Expr-Standalone.html
     →  ../../docs/index.html          (GitHub Pages copy)
   ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');

const HERE = __dirname;
const PUBLIC = path.join(HERE, '..', 'web', 'public');
const OUT = path.join(HERE, '..', 'VoltIQ-Expr-Standalone.html');
const DOCS = path.join(HERE, '..', '..', 'docs', 'index.html');

const html = fs.readFileSync(path.join(PUBLIC, 'index.html'), 'utf8');
const css = fs.readFileSync(path.join(PUBLIC, 'styles.css'), 'utf8');
const app = fs.readFileSync(path.join(PUBLIC, 'app.js'), 'utf8');
const engine = fs.readFileSync(path.join(HERE, 'engine.js'), 'utf8');

// The offline engine answers /api/* before app.js boots.
const shim = `
(function () {
  var realFetch = window.fetch ? window.fetch.bind(window) : null;
  var offline = window.VoltIQEngine.createEngine();
  window.VoltIQOffline = true;
  window.fetch = function (url, opts) {
    if (typeof url === 'string' && url.indexOf('/api/') === 0) {
      return offline(url, opts || {});
    }
    if (realFetch) return realFetch(url, opts);
    return Promise.reject(new Error('offline'));
  };
})();
`;

// NOTE: replacements are functions on purpose — string replacements treat
// `$$`, `$&` and `$1` specially and would corrupt the bundle.
let out = html
  .replace('<link rel="stylesheet" href="/styles.css" />', () => `<style>\n${css}\n</style>`)
  .replace(
    '<script src="/app.js"></script>',
    () => `<script>\n${engine}\n</script>\n<script>\n${shim}\n</script>\n<script>\n${app}\n</script>`
  )
  .replace(
    '<title>VoltIQ Expr · Expression Solver &amp; Visualizer</title>',
    '<title>VoltIQ Expr · Expression Solver &amp; Visualizer (offline build)</title>'
  );

fs.writeFileSync(OUT, out, 'utf8');

// GitHub Pages copy: same page plus a link to the other project's demo, which
// is published next to it as docs/hospital.html (Pages serves /docs only).
fs.mkdirSync(path.dirname(DOCS), { recursive: true });
const HOSPITAL = path.join(HERE, '..', '..', 'SmartHospitalSystem', 'VoltIQ-Care-Standalone.html');
let docsCopy = out;
if (fs.existsSync(HOSPITAL)) {
  fs.copyFileSync(HOSPITAL, path.join(path.dirname(DOCS), 'hospital.html'));
  docsCopy = out.replace(
    '<span class="footer-links">',
    '<span class="footer-links"><a href="hospital.html">VoltIQ Care demo ↗</a>'
  );
}
fs.writeFileSync(DOCS, docsCopy, 'utf8');

const kb = (Buffer.byteLength(out) / 1024).toFixed(1);
console.log(`✓ ${path.relative(process.cwd(), OUT)}  (${kb} KB, single file, no dependencies)`);
console.log(`✓ ${path.relative(process.cwd(), DOCS)}  (GitHub Pages copy)`);
