// server.js — static dashboard + reverse proxy to the C++ DSA engine
//
// The whole "backend" is the compiled C++ binary (see ../cpp). This tiny Node
// process only serves the dashboard assets and forwards /api/* to it, so the
// browser never needs to know about a second port.
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = Number(process.env.PORT || 3000);
const HOST = process.env.HOST || '0.0.0.0';
const ENGINE_HOST = process.env.ENGINE_HOST || '127.0.0.1';
const ENGINE_PORT = Number(process.env.ENGINE_PORT || 8001);
const PUBLIC_DIR = path.join(__dirname, 'public');

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
  '.txt': 'text/plain; charset=utf-8',
  '.map': 'application/json; charset=utf-8'
};

function proxy(req, res) {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    const body = Buffer.concat(chunks);
    const options = {
      host: ENGINE_HOST,
      port: ENGINE_PORT,
      method: req.method,
      path: req.url,
      headers: {
        'Content-Type': req.headers['content-type'] || 'application/json',
        'Content-Length': body.length
      },
      timeout: 15000
    };

    const upstream = http.request(options, (up) => {
      const parts = [];
      up.on('data', (c) => parts.push(c));
      up.on('end', () => {
        res.writeHead(up.statusCode || 502, {
          'Content-Type': up.headers['content-type'] || 'application/json',
          'Cache-Control': 'no-store'
        });
        res.end(Buffer.concat(parts));
      });
    });

    upstream.on('timeout', () => {
      upstream.destroy(new Error('engine timeout'));
    });

    upstream.on('error', (err) => {
      res.writeHead(503, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      res.end(JSON.stringify({
        ok: false,
        engineOffline: true,
        error: 'C++ engine is not reachable — ' + err.message
      }));
    });

    if (body.length) upstream.write(body);
    upstream.end();
  });
}

function serveStatic(req, res) {
  let urlPath = decodeURIComponent((req.url || '/').split('?')[0]);
  if (urlPath === '/' || urlPath === '') urlPath = '/index.html';

  const safePath = path.normalize(path.join(PUBLIC_DIR, urlPath));
  if (!safePath.startsWith(PUBLIC_DIR)) {
    res.writeHead(403, { 'Content-Type': 'text/plain' });
    res.end('Forbidden');
    return;
  }

  fs.readFile(safePath, (err, data) => {
    if (err) {
      // SPA fallback for unknown client routes
      fs.readFile(path.join(PUBLIC_DIR, 'index.html'), (err2, html) => {
        if (err2) {
          res.writeHead(404, { 'Content-Type': 'text/plain' });
          res.end('Not found');
          return;
        }
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        res.end(html);
      });
      return;
    }
    const ext = path.extname(safePath).toLowerCase();
    res.writeHead(200, {
      'Content-Type': MIME[ext] || 'application/octet-stream',
      'Cache-Control': ext === '.html' ? 'no-store' : 'public, max-age=300'
    });
    res.end(data);
  });
}

const server = http.createServer((req, res) => {
  if ((req.url || '').startsWith('/api/')) {
    proxy(req, res);
    return;
  }
  serveStatic(req, res);
});

server.listen(PORT, HOST, () => {
  console.log(`VoltIQ Care dashboard  →  http://${HOST}:${PORT}`);
  console.log(`C++ DSA engine proxy   →  ${ENGINE_HOST}:${ENGINE_PORT}`);
});

process.on('SIGTERM', () => server.close(() => process.exit(0)));
process.on('SIGINT', () => server.close(() => process.exit(0)));
