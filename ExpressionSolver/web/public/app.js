/* ==========================================================================
   VoltIQ Expr — dashboard app
   Talks to the C++ engine through /api/*, or to the bundled JS mirror when the
   page is opened as a single offline file (see standalone/engine.js).
   ========================================================================== */
(function () {
  'use strict';

  const $ = (sel) => document.querySelector(sel);
  const $$ = (sel) => Array.from(document.querySelectorAll(sel));

  const state = {
    expr: '2x^2 - 3x + 1',
    vars: { x: 6, y: 3, r: 5 },
    folding: false,
    analysis: null,
    timeline: [],
    index: -1,
    playing: false,
    timer: null,
    speed: 800,
    autoplayed: false,
    examples: []
  };

  const els = {
    input: $('#exprInput'),
    solve: $('#solveBtn'),
    examples: $('#exampleChips'),
    varChips: $('#varChips'),
    addVarForm: $('#addVarForm'),
    newVar: $('#newVar'),
    errorBanner: $('#errorBanner'),
    errorMessage: $('#errorMessage'),
    errorSource: $('#errorSource'),
    resultValue: $('#resultValue'),
    resultSub: $('#resultSub'),
    activeProgram: $('#activeProgram'),
    statChips: $('#statChips'),
    tokenStrip: $('#tokenStrip'),
    opStack: $('#opStack'),
    outQueue: $('#outQueue'),
    convertNote: $('#convertNote'),
    convertHint: $('#convertHint'),
    rpnStrip: $('#rpnStrip'),
    valStack: $('#valStack'),
    liveValue: $('#liveValue'),
    liveOp: $('#liveOp'),
    evalNote: $('#evalNote'),
    evalHint: $('#evalHint'),
    evalPane: $('#evalPane'),
    convertPane: $('#convertPane'),
    tabConvert: $('#tabConvert'),
    tabEval: $('#tabEval'),
    play: $('#playBtn'),
    prev: $('#prevBtn'),
    next: $('#nextBtn'),
    reset: $('#resetBtn'),
    end: $('#endBtn'),
    speed: $('#speedSelect'),
    timelineFill: $('#timelineFill'),
    stepCounter: $('#stepCounter'),
    foldToggle: $('#foldToggle'),
    rawProgram: $('#rawProgram'),
    foldedProgram: $('#foldedProgram'),
    rawCount: $('#rawCount'),
    foldedCount: $('#foldedCount'),
    optimiserNote: $('#optimiserNote'),
    treeCanvas: $('#treeCanvas'),
    traversals: $('#traversals'),
    benchBody: $('#benchBody'),
    benchBtn: $('#benchBtn'),
    engineGrid: $('#engineGrid'),
    cacheList: $('#cacheList'),
    engineBadge: $('#engineBadge'),
    engineBadgeText: $('#engineBadgeText'),
    copyLink: $('#copyLinkBtn'),
    resetEngine: $('#resetEngineBtn'),
    toast: $('#toast')
  };

  // ------------------------------------------------------------------ utils
  const escapeHtml = (s) => String(s).replace(/[&<>"']/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  function toast(message) {
    els.toast.textContent = message;
    els.toast.classList.add('show');
    clearTimeout(toast._t);
    toast._t = setTimeout(() => els.toast.classList.remove('show'), 1900);
  }

  async function api(path, body) {
    const options = body
      ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }
      : undefined;
    const res = await fetch(path, options);
    return res.json();
  }

  function fmtMicros(us) {
    if (us === null || us === undefined || isNaN(us)) return '—';
    if (us >= 1000) return (us / 1000).toFixed(2) + ' ms';
    if (us >= 10) return us.toFixed(1) + ' µs';
    return us.toFixed(2) + ' µs';
  }

  // ------------------------------------------------------------ input state
  function currentExpr() { return els.input.value.trim(); }

  function syncHash() {
    const params = [];
    params.push('e=' + encodeURIComponent(state.expr));
    params.push('v=' + encodeURIComponent(JSON.stringify(state.vars)));
    if (state.folding) params.push('f=1');
    const url = location.pathname + location.search + '#' + params.join('&');
    try { history.replaceState(null, '', url); } catch (e) { /* file:// */ }
  }

  function readHash() {
    const hash = (location.hash || '').replace(/^#/, '');
    if (!hash) return false;
    const out = {};
    hash.split('&').forEach((pair) => {
      const [k, ...rest] = pair.split('=');
      out[k] = decodeURIComponent(rest.join('='));
    });
    if (out.e) {
      state.expr = out.e;
      els.input.value = out.e;
    }
    if (out.v) {
      try {
        const parsed = JSON.parse(out.v);
        if (parsed && typeof parsed === 'object') state.vars = parsed;
      } catch (e) { /* ignore */ }
    }
    if (out.f === '1') {
      state.folding = true;
      els.foldToggle.checked = true;
    }
    return true;
  }

  // ------------------------------------------------------------ variables UI
  function renderVars() {
    const names = Object.keys(state.vars).sort();
    els.varChips.innerHTML = names.map((name) => `
      <span class="var-chip" data-var="${escapeHtml(name)}">
        ${escapeHtml(name)} =
        <input type="text" inputmode="decimal" aria-label="${escapeHtml(name)} value"
               data-var-input="${escapeHtml(name)}" value="${escapeHtml(state.vars[name])}" />
        <button class="del" data-var-del="${escapeHtml(name)}" title="Remove variable" aria-label="Remove ${escapeHtml(name)}">×</button>
      </span>`).join('');
  }

  els.varChips.addEventListener('change', (event) => {
    const name = event.target.getAttribute('data-var-input');
    if (!name) return;
    const value = Number(event.target.value);
    if (!isFinite(value)) {
      toast('variables must be numbers');
      renderVars();
      return;
    }
    state.vars[name] = value;
    analyze();
  });

  els.varChips.addEventListener('click', (event) => {
    const name = event.target.getAttribute('data-var-del');
    if (!name) return;
    delete state.vars[name];
    renderVars();
    analyze();
  });

  els.addVarForm.addEventListener('submit', (event) => {
    event.preventDefault();
    const raw = els.newVar.value.trim();
    if (!raw) return;
    const match = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(-?[0-9.]+(?:e[+-]?\d+)?)$/i.exec(raw);
    if (!match) {
      toast('use name=value, e.g. z=4');
      return;
    }
    state.vars[match[1]] = Number(match[2]);
    els.newVar.value = '';
    renderVars();
    analyze();
  });

  // ------------------------------------------------------------ examples
  function renderExamples() {
    els.examples.innerHTML = state.examples.map((ex) =>
      `<button class="chip${ex.expr === state.expr ? ' active' : ''}" data-expr="${escapeHtml(ex.expr)}" title="${escapeHtml(ex.label)}">${escapeHtml(ex.expr)}</button>`
    ).join('');
  }

  els.examples.addEventListener('click', (event) => {
    const expr = event.target.closest('[data-expr]');
    if (!expr) return;
    els.input.value = expr.getAttribute('data-expr');
    analyze();
  });

  // ------------------------------------------------------------ analysis
  let analyzeTimer = null;
  function scheduleAnalyze() {
    clearTimeout(analyzeTimer);
    analyzeTimer = setTimeout(analyze, 260);
  }

  async function analyze() {
    state.expr = currentExpr();
    if (!state.expr) {
      clearResult();
      return;
    }
    syncHash();
    renderExamples();

    let analysis;
    try {
      analysis = await api('/api/analyze', {
        expr: state.expr,
        vars: state.vars,
        fold: state.folding,
        trace: true
      });
    } catch (err) {
      setBadge('error', 'engine unreachable — run ./run.sh');
      return;
    }

    state.analysis = analysis;
    state.timeline = buildTimeline(analysis);
    state.index = -1;
    stopPlaying();

    renderError(analysis);
    renderResult(analysis);
    renderProgram(analysis);
    renderTree(analysis);
    renderEngine(analysis.engine);
    renderStrips(analysis);
    applyStep(-1);

    if (!state.autoplayed && analysis.ok && state.timeline.length > 1) {
      state.autoplayed = true;
      setTimeout(() => play(), 450);
    }
  }

  function buildTimeline(analysis) {
    const timeline = [];
    if (analysis.convert && analysis.convert.steps) {
      analysis.convert.steps.forEach((s) => timeline.push(Object.assign({ phase: 'convert' }, s)));
    }
    if (analysis.eval && analysis.eval.steps) {
      analysis.eval.steps.forEach((s) => timeline.push(Object.assign({ phase: 'eval' }, s)));
    }
    return timeline;
  }

  function clearResult() {
    state.analysis = null;
    state.timeline = [];
    els.resultValue.textContent = '—';
    els.resultValue.classList.remove('error');
    els.resultSub.textContent = 'waiting for input';
    els.errorBanner.hidden = true;
    applyStep(-1);
  }

  // ------------------------------------------------------------ renderers
  function setBadge(kind, text) {
    els.engineBadge.className = 'badge' + (kind ? ' ' + kind : '');
    els.engineBadgeText.textContent = text;
  }

  function renderError(analysis) {
    if (!analysis.error) {
      els.errorBanner.hidden = true;
      return;
    }
    const pos = Math.max(0, Math.min(analysis.error.pos || 0, analysis.expr.length));
    const len = Math.max(1, analysis.error.len || 1);
    const caret = ' '.repeat(pos) + '^' + '~'.repeat(len - 1);
    els.errorBanner.hidden = false;
    els.errorMessage.textContent = analysis.error.message;
    els.errorSource.innerHTML =
      escapeHtml(analysis.expr) + '\n' +
      '<span class="caret">' + escapeHtml(caret) + '</span>';
  }

  function renderResult(analysis) {
    const stats = analysis.stats || {};
    if (analysis.error && !analysis.eval.ok) {
      els.resultValue.textContent = 'error';
      els.resultValue.classList.add('error');
      els.resultSub.textContent = analysis.error.message;
    } else {
      els.resultValue.classList.remove('error');
      els.resultValue.textContent = analysis.eval.valueText;
      els.resultSub.textContent = analysis.normalized || '';
    }

    els.activeProgram.textContent = (analysis.program && analysis.program.active) || '—';

    const cache = stats.cacheHit ? 'hit' : 'miss';
    const chips = [
      ['tokens', stats.tokens, ''],
      ['instructions', stats.instructions, 'mint'],
      ['folded away', stats.foldedConstants, stats.foldedConstants ? 'amber' : ''],
      ['peak stack', Math.max(stats.peakOpStack || 0, stats.peakEvalStack || 0), 'blue'],
      ['compile', fmtMicros(stats.compileUs), ''],
      ['evaluate', fmtMicros(stats.evalUs), ''],
      ['cache', cache, stats.cacheHit ? 'mint' : '']
    ];
    els.statChips.innerHTML = chips.map(([k, v, cls]) =>
      `<div class="stat"><span class="k">${k}</span><span class="v ${cls}">${escapeHtml(v)}</span></div>`
    ).join('');
  }

  function renderProgram(analysis) {
    const program = analysis.program || { raw: '', folded: '', active: '', foldingActive: false };
    const stats = analysis.stats || {};
    els.rawProgram.textContent = program.raw || '—';
    els.foldedProgram.textContent = program.folded || '—';
    els.rawCount.textContent = (stats.rawInstructions || 0) + ' instructions';
    els.foldedCount.textContent = (program.folded ? program.folded.split(' ').length : 0) + ' instructions';
    els.foldToggle.checked = state.folding;

    const blocks = $$('.program-block');
    if (blocks.length === 2) {
      blocks[0].classList.toggle('active', !state.folding);
      blocks[1].classList.toggle('active', state.folding);
    }

    const saved = stats.rawInstructions - (program.folded ? program.folded.split(' ').length : 0);
    let note;
    if (!saved) {
      note = 'This expression has no all-constant subtree, so folding leaves the program unchanged.';
    } else if (state.folding) {
      note = `Folding removed <b>${saved}</b> instruction${saved === 1 ? '' : 's'} — the shortened program is what runs.`;
    } else {
      note = `Folding is off, so the raw program runs. Turn it on to remove <b>${saved}</b> instruction${saved === 1 ? '' : 's'}.`;
    }
    if (stats.cacheHit) note += ' Compiled earlier — this run came from the LRU cache.';
    els.optimiserNote.innerHTML = note;
  }

  function renderStrips(analysis) {
    const tokens = analysis.tokens || [];
    els.tokenStrip.innerHTML = tokens.length
      ? tokens.map((t, i) => {
        const classes = ['token'];
        if (t.implicit) classes.push('implicit');
        if (t.folded) classes.push('folded-token');
        return `<span class="${classes.join(' ')}" data-token="${i}" title="${escapeHtml(t.type)} at column ${t.pos + 1}">${escapeHtml(t.text)}</span>`;
      }).join('')
      : '<span class="empty">no tokens</span>';

    const evalSteps = (analysis.eval && analysis.eval.steps) || [];
    els.rpnStrip.innerHTML = evalSteps.length
      ? evalSteps.map((s, i) =>
        `<span class="token" data-rpn="${i}" title="instruction ${i + 1}">${escapeHtml(s.token)}</span>`).join('')
      : '<span class="empty">the postfix program appears here</span>';
  }

  // ------------------------------------------------------------ step engine
  function stackMarkup(items, kind) {
    if (!items || !items.length) return '<span class="empty">empty</span>';
    return items.map((item, i) => {
      const cls = ['brick'];
      if (kind === 'values') cls.push('value');
      else if (item.endsWith('()')) cls.push('fn');
      else if (item === '(') cls.push('paren');
      else if (/^[+\-*/%^]/.test(item)) cls.push('op');
      if (i === items.length - 1) cls.push('top');
      return `<span class="${cls.join(' ')}">${escapeHtml(item)}</span>`;
    }).join('');
  }

  function queueMarkup(items) {
    if (!items || !items.length) return '<span class="empty">empty</span>';
    return items.map((item) =>
      `<span class="q-item${item.endsWith('()') ? ' fn' : ''}">${escapeHtml(item)}</span>`).join('');
  }

  function lastConvertSnapshot() {
    const steps = (state.analysis && state.analysis.convert && state.analysis.convert.steps) || [];
    return steps.length ? steps[steps.length - 1] : null;
  }

  function paintConvert(step, done) {
    const tokens = $$('#tokenStrip .token');
    // tokenIndex is -1 for the "flush the stack" steps at the end: every token
    // has been read by then, so the whole strip counts as processed.
    const readUpTo = !step ? 0 : (step.tokenIndex < 0 ? tokens.length : step.tokenIndex);
    tokens.forEach((el, i) => {
      el.classList.toggle('done', done || i < readUpTo);
      el.classList.toggle('watch', !!step && step.tokenIndex === i);
    });
    els.opStack.innerHTML = stackMarkup(step ? step.stack : [], 'ops');
    els.outQueue.innerHTML = queueMarkup(step ? step.output : []);
    if (step) {
      els.convertNote.innerHTML = `<span class="action">${escapeHtml(step.action)}</span>${escapeHtml(step.note)}`;
      els.convertHint.textContent = step.token ? 'token “' + step.token + '”' : 'flushing the stack';
    } else {
      els.convertNote.innerHTML = 'Press <b>Play</b> or step through to watch the stack push and pop.';
      els.convertHint.textContent = 'reading tokens left to right';
    }
  }

  function paintEval(step, finalConvert) {
    const rpn = $$('#rpnStrip .token');
    rpn.forEach((el, i) => {
      el.classList.toggle('done', !!step && i < step.tokenIndex);
      el.classList.toggle('watch', !!step && i === step.tokenIndex);
    });
    els.valStack.innerHTML = stackMarkup(step ? step.stack : [], 'values');
    if (step) {
      els.liveValue.textContent = step.value || '—';
      els.liveOp.textContent = step.token + ' · ' + step.action;
      els.evalNote.innerHTML = `<span class="action">${escapeHtml(step.action)}</span>${escapeHtml(step.note)}`;
      els.evalHint.textContent = 'instruction ' + (step.tokenIndex + 1) + ' of ' + rpn.length;
    } else if (finalConvert) {
      els.liveValue.textContent = '—';
      els.liveOp.textContent = 'no operation yet';
      els.evalNote.innerHTML = 'Postfix program ready — press <b>Play</b> to run it on the stack.';
      els.evalHint.textContent = 'waiting to run';
    } else {
      els.liveValue.textContent = '—';
      els.liveOp.textContent = 'no operation yet';
      els.evalNote.textContent = 'The postfix program is executed one instruction at a time.';
      els.evalHint.textContent = 'waiting for the postfix program';
    }
  }

  function applyStep(index) {
    state.index = index;
    const total = state.timeline.length;
    const step = index >= 0 && index < total ? state.timeline[index] : null;
    const analysis = state.analysis;
    const hasAnalysis = !!analysis && !!analysis.program;
    const finalConvert = hasAnalysis ? lastConvertSnapshot() : null;

    if (!step) {
      paintConvert(null, false);
      paintEval(null, null);
      els.tabConvert.classList.add('active');
      els.tabConvert.classList.remove('done');
      els.tabEval.classList.remove('active', 'done');
      els.convertPane.classList.remove('dim', 'focus');
      els.evalPane.classList.add('dim');
    } else if (step.phase === 'convert') {
      paintConvert(step, false);
      paintEval(null, hasAnalysis && analysis.convert && analysis.convert.ok ? null : null);
      els.tabConvert.classList.add('active');
      els.tabConvert.classList.remove('done');
      els.tabEval.classList.remove('active', 'done');
      els.convertPane.classList.remove('dim');
      els.convertPane.classList.add('focus');
      els.evalPane.classList.add('dim');
      els.evalPane.classList.remove('focus');
    } else {
      paintConvert(finalConvert, true);
      paintEval(step, finalConvert);
      els.tabConvert.classList.remove('active');
      els.tabConvert.classList.add('done');
      els.tabEval.classList.add('active');
      els.tabEval.classList.remove('done');
      els.convertPane.classList.add('dim');
      els.convertPane.classList.remove('focus');
      els.evalPane.classList.remove('dim');
      els.evalPane.classList.add('focus');
    }

    els.stepCounter.textContent = 'step ' + (index + 1) + ' / ' + total;
    els.timelineFill.style.width = total ? ((index + 1) / total * 100).toFixed(1) + '%' : '0%';

    els.prev.disabled = index < 0;
    els.reset.disabled = index < 0;
    els.next.disabled = total === 0 || index >= total - 1;
    els.end.disabled = total === 0 || index >= total - 1;
  }

  function next() {
    if (state.index >= state.timeline.length - 1) {
      stopPlaying();
      return false;
    }
    applyStep(state.index + 1);
    return true;
  }

  function prev() {
    applyStep(Math.max(-1, state.index - 1));
  }

  function play() {
    if (!state.timeline.length) return;
    if (state.index >= state.timeline.length - 1) applyStep(-1);
    state.playing = true;
    els.play.innerHTML = '&#10074;&#10074; Pause';
    els.play.setAttribute('aria-label', 'Pause');
    tick();
  }

  function tick() {
    if (!state.playing) return;
    state.timer = setTimeout(() => {
      if (!state.playing) return;
      if (!next()) {
        stopPlaying();
        return;
      }
      tick();
    }, Number(els.speed.value));
  }

  function stopPlaying() {
    state.playing = false;
    clearTimeout(state.timer);
    els.play.innerHTML = '&#9654; Play';
    els.play.setAttribute('aria-label', 'Play');
  }

  // ------------------------------------------------------------ tree
  function renderTree(analysis) {
    const tree = analysis.ast;
    if (!tree || tree.root < 0 || !tree.nodeCount) {
      els.treeCanvas.innerHTML = '<p class="empty-state">The tree appears once the expression parses.</p>';
      els.traversals.innerHTML = '';
      return;
    }
    if (tree.nodeCount > 70) {
      els.treeCanvas.innerHTML = '<p class="empty-state">Expression too large to draw (' + tree.nodeCount + ' nodes).</p>';
      els.traversals.innerHTML = '';
      return;
    }

    const nodes = tree.nodes;
    const order = [];   // in-order positions
    let column = 0;
    const xIndex = new Array(nodes.length).fill(0);
    const depthOf = new Array(nodes.length).fill(0);

    (function walk(id, depth) {
      if (id < 0 || id === undefined) return;
      depthOf[id] = depth;
      walk(nodes[id].left, depth + 1);
      xIndex[id] = column++;
      walk(nodes[id].right, depth + 1);
    })(tree.root, 0);

    const labelOf = (n) => n.label;
    let maxWidth = 0;
    nodes.forEach((n) => { maxWidth = Math.max(maxWidth, 20 + labelOf(n).length * 8.2); });
    const gapX = maxWidth + 14;
    const rowH = 64;
    const padding = 16;
    const width = Math.max(column * gapX + padding * 2, 260);
    const maxDepth = Math.max.apply(null, depthOf.filter((_, i) => nodes[i].type || true));
    const height = (maxDepth + 1) * rowH + padding * 2;

    const px = (id) => padding + xIndex[id] * gapX + maxWidth / 2;
    const py = (id) => padding + depthOf[id] * rowH + 13;

    let edges = '';
    let boxes = '';
    nodes.forEach((n, id) => {
      const x = px(id);
      const y = py(id);
      const w = 20 + labelOf(n).length * 8.2;
      [n.left, n.right].forEach((child) => {
        if (child >= 0) {
          edges += `<line class="tree-edge" x1="${x}" y1="${y + 13}" x2="${px(child)}" y2="${py(child) - 13}" />`;
        }
      });
      const cls = ['tree-node', n.type === 'operator' ? 'op' : n.type === 'function' ? 'fn'
        : n.type === 'variable' ? 'var' : 'num'];
      if (n.folded) cls.push('folded');
      boxes += `<g class="${cls.join(' ')}"><rect x="${x - w / 2}" y="${y - 13}" width="${w}" height="26" rx="8" />`
        + `<text x="${x}" y="${y + 1}">${escapeHtml(labelOf(n))}</text>`
        + (n.folded ? `<text class="tree-tag" x="${x}" y="${y + 24}">folded</text>` : '')
        + '</g>';
    });

    els.treeCanvas.innerHTML =
      `<svg viewBox="0 0 ${width} ${height}" width="${Math.min(width, 1100)}" height="${height}" role="img" aria-label="Expression tree">`
      + edges + boxes + '</svg>';

    els.traversals.innerHTML =
      `<div>prefix <span>${escapeHtml(tree.prefix)}</span></div>`
      + `<div>infix <span>${escapeHtml(tree.infix)}</span></div>`
      + `<div>height <span>${tree.height}</span> · nodes <span>${tree.nodeCount}</span></div>`;
  }

  // ------------------------------------------------------------ engine panel
  function renderEngine(engine) {
    if (!engine) return;
    const lookups = engine.cacheHits + engine.cacheMisses;
    const hitRate = lookups ? (engine.cacheHits / lookups * 100).toFixed(1) + '%' : '—';
    const metrics = [
      ['compiles', engine.compiles],
      ['cache hits', engine.cacheHits],
      ['cache misses', engine.cacheMisses],
      ['hit rate', hitRate],
      ['evaluations', engine.evals],
      ['tokens lexed', engine.tokensLexed],
      ['cache', engine.cacheSize + '/' + engine.cacheCapacity],
      ['folded', engine.foldedConstants]
    ];
    els.engineGrid.innerHTML = metrics.map(([k, v]) =>
      `<div class="engine-metric"><span class="k">${k}</span><span class="v">${escapeHtml(v)}</span></div>`
    ).join('');

    const cache = engine.cacheOrder || [];
    els.cacheList.innerHTML = cache.length
      ? cache.map((k) => `<span class="cache-item" title="${escapeHtml(k)}">${escapeHtml(k)}</span>`).join('')
      : '<span class="cache-empty">cache is empty</span>';
  }

  // ------------------------------------------------------------ benchmarks
  async function runBench() {
    els.benchBtn.disabled = true;
    els.benchBtn.textContent = 'Running…';
    els.benchBody.innerHTML = '<p class="empty-state">Timing both pipelines…</p>';
    try {
      const bench = await api('/api/bench');
      renderBench(bench);
    } catch (err) {
      els.benchBody.innerHTML = '<p class="empty-state">Benchmark failed — is the engine running?</p>';
    } finally {
      els.benchBtn.disabled = false;
      els.benchBtn.textContent = 'Run';
    }
  }

  function renderBench(bench) {
    if (!bench || !bench.rows) {
      els.benchBody.innerHTML = '<p class="empty-state">No benchmark data.</p>';
      return;
    }
    const source = bench.measuredIn || (window.VoltIQOffline ? 'browser (JS mirror)' : 'measured in the C++ engine');
    const maxSlow = Math.max.apply(null, bench.rows.map((r) => r.slowUs)) || 1;

    const rows = bench.rows.map((row) => `
      <div class="bench-row">
        <div class="bench-top">
          <span class="bench-label">${escapeHtml(row.label)}</span>
          <span class="bench-speed">${row.speedup.toFixed ? row.speedup.toFixed(1) : row.speedup}× faster</span>
        </div>
        <div class="bench-bars">
          <div class="bench-bar fast">
            <span>${escapeHtml(row.fastLabel)}</span>
            <span class="track"><span class="fill" style="width:${Math.max(2, (row.fastUs / maxSlow) * 100)}%"></span></span>
            <span class="us">${row.fastUs < 1 ? row.fastUs.toFixed(3) : row.fastUs.toFixed(2)} µs</span>
          </div>
          <div class="bench-bar slow">
            <span>${escapeHtml(row.slowLabel)}</span>
            <span class="track"><span class="fill" style="width:${Math.max(2, (row.slowUs / maxSlow) * 100)}%"></span></span>
            <span class="us">${row.slowUs < 1 ? row.slowUs.toFixed(3) : row.slowUs.toFixed(2)} µs</span>
          </div>
        </div>
        <div class="bench-note">${escapeHtml(row.note)}</div>
      </div>`).join('');

    const scaling = (bench.scaling || []).map((s) => `
      <tr><td>${s.tokens}</td><td>${s.micros.toFixed(2)} µs</td><td>${s.perTokenUs.toFixed(3)} µs</td></tr>`).join('');

    els.benchBody.innerHTML = rows + `
      <div class="scaling">
        <h5>Tokenizer + parser scaling <span class="bench-source">· ${escapeHtml(source)}</span></h5>
        <table>
          <thead><tr><th>tokens</th><th>total</th><th>per token</th></tr></thead>
          <tbody>${scaling}</tbody>
        </table>
        <div class="bench-note">Cost per token stays flat → the pipeline is linear, O(n).</div>
      </div>`;
  }

  // ------------------------------------------------------------ events
  els.solve.addEventListener('click', () => { state.autoplayed = true; analyze(); });

  els.input.addEventListener('input', scheduleAnalyze);
  els.input.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      state.autoplayed = true;
      analyze();
      els.input.blur();
    }
  });

  els.play.addEventListener('click', () => {
    if (state.playing) stopPlaying();
    else play();
  });
  els.next.addEventListener('click', () => { stopPlaying(); next(); });
  els.prev.addEventListener('click', () => { stopPlaying(); prev(); });
  els.reset.addEventListener('click', () => { stopPlaying(); applyStep(-1); });
  els.end.addEventListener('click', () => { stopPlaying(); applyStep(state.timeline.length - 1); });
  els.speed.addEventListener('change', () => { state.speed = Number(els.speed.value); });

  els.foldToggle.addEventListener('change', () => {
    state.folding = els.foldToggle.checked;
    state.autoplayed = true;
    analyze();
  });

  els.tokenStrip.addEventListener('click', (event) => {
    const el = event.target.closest('[data-token]');
    if (!el) return;
    const tokenIndex = Number(el.getAttribute('data-token'));
    const found = state.timeline.findIndex((s) => s.phase === 'convert' && s.tokenIndex === tokenIndex);
    if (found >= 0) { stopPlaying(); applyStep(found); }
  });

  els.rpnStrip.addEventListener('click', (event) => {
    const el = event.target.closest('[data-rpn]');
    if (!el) return;
    const instr = Number(el.getAttribute('data-rpn'));
    const found = state.timeline.findIndex((s) => s.phase === 'eval' && s.tokenIndex === instr);
    if (found >= 0) { stopPlaying(); applyStep(found); }
  });

  els.tabConvert.addEventListener('click', () => {
    stopPlaying();
    applyStep(-1);
  });
  els.tabEval.addEventListener('click', () => {
    stopPlaying();
    const first = state.timeline.findIndex((s) => s.phase === 'eval');
    if (first >= 0) applyStep(first);
  });

  els.benchBtn.addEventListener('click', runBench);
  $('#footerBench').addEventListener('click', () => {
    runBench();
    document.querySelector('.bench-card').scrollIntoView({ behavior: 'smooth', block: 'center' });
  });
  $('#footerHelp').addEventListener('click', () =>
    toast('Space play/pause · ← → step · Home start · End finish · Enter solve'));

  els.resetEngine.addEventListener('click', async () => {
    try {
      await api('/api/reset', {});
      state.vars = {};
      renderVars();
      analyze();
      toast('variables and cache cleared');
    } catch (e) {
      toast('could not reset the engine');
    }
  });

  els.copyLink.addEventListener('click', async () => {
    syncHash();
    const url = location.href;
    try {
      await navigator.clipboard.writeText(url);
      toast('link copied — send it to a friend');
    } catch (e) {
      toast(url);
    }
  });

  document.addEventListener('keydown', (event) => {
    const tag = (event.target.tagName || '').toLowerCase();
    if (tag === 'input' || tag === 'textarea' || tag === 'select') return;
    if (event.key === ' ' || event.key === 'Spacebar') {
      event.preventDefault();
      if (state.playing) stopPlaying();
      else play();
    } else if (event.key === 'ArrowRight') {
      event.preventDefault();
      stopPlaying();
      next();
    } else if (event.key === 'ArrowLeft') {
      event.preventDefault();
      stopPlaying();
      prev();
    } else if (event.key === 'Home') {
      stopPlaying();
      applyStep(-1);
    } else if (event.key === 'End') {
      stopPlaying();
      applyStep(state.timeline.length - 1);
    }
  });

  // ------------------------------------------------------------ boot
  async function boot() {
    const hadHash = readHash();
    if (!hadHash) els.input.value = state.expr;
    renderVars();
    if (state.folding) els.foldToggle.checked = true;

    try {
      const health = await api('/api/health');
      const offline = !!window.VoltIQOffline;
      setBadge(offline ? 'offline' : '', offline
        ? 'offline build · JS mirror'
        : 'C++17 engine · ' + (health.engine || 'live'));
    } catch (e) {
      setBadge('error', 'engine offline — run ./run.sh');
    }

    try {
      const examples = await api('/api/examples');
      state.examples = examples.examples || [];
      renderExamples();
    } catch (e) { /* chips are optional */ }

    analyze();
  }

  boot();
})();
