#!/usr/bin/env node
/* ==========================================================================
   build-standalone.js
   Bundles the dashboard (index.html + styles.css + app.js) and the offline
   DSA engine (engine.js) into ONE self-contained HTML file that runs from
   file:// with no server, no build step and no internet.

     node build-standalone.js
     →  ../VoltIQ-Care-Standalone.html
   ========================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');

const HERE = __dirname;
const PUBLIC = path.join(HERE, '..', 'web', 'public');
const OUT = path.join(HERE, '..', 'VoltIQ-Care-Standalone.html');

const html = fs.readFileSync(path.join(PUBLIC, 'index.html'), 'utf8');
const css = fs.readFileSync(path.join(PUBLIC, 'styles.css'), 'utf8');
const app = fs.readFileSync(path.join(PUBLIC, 'app.js'), 'utf8');
const engine = fs.readFileSync(path.join(HERE, 'engine.js'), 'utf8');

// The offline engine replaces fetch() for /api/* only, before app.js boots.
const shim = `
(function () {
  var realFetch = window.fetch ? window.fetch.bind(window) : null;
  window.VoltIQOffline = true;
  var offline = window.VoltIQEngine.createEngine();
  window.fetch = function (url, opts) {
    if (typeof url === 'string' && url.indexOf('/api/') === 0) {
      return offline(url, opts || {});
    }
    if (realFetch) return realFetch(url, opts);
    return Promise.reject(new Error('offline'));
  };
})();
`;

// NOTE: always use a replacer FUNCTION here. String replacements interpret
// `$$`, `$&`, `` $` `` and `$1` specially — `const $$ = ...` in app.js would
// silently collapse to `const $ = ...` and break the whole bundle.
let out = html
  .replace(
    '<link rel="stylesheet" href="/styles.css" />',
    () => `<style>\n${css}\n</style>`
  )
  .replace(
    '<script src="/app.js"></script>',
    () => `<script>\n${engine}\n</script>\n<script>\n${shim}\n</script>\n<script>\n${app}\n</script>`
  );

// mark the document as the offline build (used by the banner + title suffix)
out = out.replace(
  '<title>VoltIQ Care · Smart Hospital Management System</title>',
  '<title>VoltIQ Care · Smart Hospital Management System (offline build)</title>'
);

fs.writeFileSync(OUT, out, 'utf8');
const kb = (Buffer.byteLength(out) / 1024).toFixed(1);
console.log(`✓ ${path.relative(process.cwd(), OUT)}  (${kb} KB, single file, no dependencies)`);
