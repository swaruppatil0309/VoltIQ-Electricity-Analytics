/* ==========================================================================
   VoltIQ Care — dashboard client
   Talks to the compiled C++ DSA engine through /api/* (proxied by server.js)
   ========================================================================== */
'use strict';

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

const state = {
  snapshot: null,
  view: 'overview',
  wardTab: 0,
  statusFilter: 'all',
  strategy: 'first',
  autoTick: false,
  lastProbe: null,
  bench: null,
  polling: null,
  simTimer: null
};

const SEV_ORDER = ['critical', 'emergency', 'urgent', 'standard'];
const SEV_LABEL = { critical: 'Critical', emergency: 'Emergency', urgent: 'Urgent', standard: 'Standard' };

/* ────────────────────────────── api ────────────────────────────── */
async function api(path, options = {}) {
  const res = await fetch(path, {
    headers: { 'Content-Type': 'application/json' },
    ...options
  });
  let data;
  try {
    data = await res.json();
  } catch (err) {
    throw new Error('Engine returned an invalid response');
  }
  if (data && data.engineOffline) {
    setEngineOffline();
    throw new Error('C++ engine offline');
  }
  if (!res.ok || data.ok === false) {
    throw new Error((data && data.error) || `Request failed (${res.status})`);
  }
  return data;
}

const post = (path, body) => api(path, { method: 'POST', body: JSON.stringify(body || {}) });

/* ────────────────────────────── helpers ────────────────────────────── */
const initials = (name) =>
  (name || '')
    .replace(/[^A-Za-z ]/g, '')
    .split(' ')
    .filter(Boolean)
    .slice(0, 2)
    .map((w) => w[0].toUpperCase())
    .join('') || '?';

const esc = (s) =>
  String(s == null ? '' : s).replace(/[&<>"']/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const sevChip = (sev) =>
  `<span class="sev-chip ${sev}">${SEV_LABEL[sev] || sev}</span>`;

const statusChip = (status, label) =>
  `<span class="status-chip ${status}">${label || status}</span>`;

const fmtTime = (minutes) => {
  if (minutes == null || minutes < 0) return '—';
  if (minutes < 60) return `${minutes}m`;
  const h = Math.floor(minutes / 60);
  const m = minutes % 60;
  return m ? `${h}h ${m}m` : `${h}h`;
};

function toast(title, message, tone = 'info') {
  const stack = $('#toasts');
  const el = document.createElement('div');
  el.className = `toast ${tone}`;
  el.innerHTML = `<div><b>${esc(title)}</b>${message ? `<span>${esc(message)}</span>` : ''}</div>`;
  stack.appendChild(el);
  setTimeout(() => {
    el.classList.add('out');
    setTimeout(() => el.remove(), 260);
  }, 3600);
}

function setEngineOffline() {
  const dot = $('#engineDot');
  dot.className = 'dot down';
  $('#engineLabel').textContent = 'Engine offline';
}

/* ────────────────────────────── rendering ────────────────────────────── */
function render() {
  const snap = state.snapshot;
  if (!snap) return;
  const s = snap.stats;

  $('#virtualClock').textContent = s.clock;
  $('#shiftLabel').textContent = `${s.shift} shift`;
  $('#navQueueCount').textContent = s.queueSize;

  $('#sideHeapHeight').textContent = s.heapHeight;
  $('#sideLoad').textContent = `${s.hashLoadFactor.toFixed(1)}%`;

  renderAlert(snap);
  renderKpis(snap);
  renderSeverity(snap);
  renderWardBars(snap);
  renderNext(snap);
  renderActivity(snap);

  if (state.view === 'triage') renderTriage(snap);
  if (state.view === 'patients') renderPatients(snap);
  if (state.view === 'wards') renderWards(snap);
  if (state.view === 'algorithms') renderAlgorithms(snap);
}

function renderAlert(snap) {
  const s = snap.stats;
  const strip = $('#alertStrip');
  const critical = snap.patients.filter((p) => p.severity === 'critical' && p.status === 'waiting');
  if (!critical.length) {
    strip.hidden = true;
    return;
  }
  strip.hidden = false;
  strip.innerHTML = `
    <svg viewBox="0 0 24 24"><path d="M12 3.5 21 20H3z"/><path d="M12 9.5v4.5M12 17.2v.1"/></svg>
    <span>${critical.length} critical patient${critical.length > 1 ? 's' : ''} waiting —
    ${esc(critical.map((p) => p.name).slice(0, 3).join(', '))}${critical.length > 3 ? ' and others' : ''}.
    The heap has them at the front of the queue.</span>`;
}

function renderKpis(snap) {
  const s = snap.stats;
  $('#kpiTotal').textContent = s.totalPatients.toLocaleString();
  $('#kpiWaiting').textContent = s.waiting.toLocaleString();
  $('#kpiCritical').textContent = s.criticalWaiting;
  $('#kpiBeds').innerHTML = `${s.bedsOccupied} <small>/ ${s.bedsTotal}</small>`;
  $('#kpiDoctors').textContent = `${s.doctorsBusy} <small>/ ${s.doctors}</small>`;
  $('#kpiWait').innerHTML = `${s.avgWaitMinutes.toFixed(0)}<small> min</small>`;
  $('#kpiTreated').textContent = `${s.treated} treated`;
  $('#kpiHashInfo').textContent = `${s.hashBuckets} buckets · ${s.hashLoadFactor.toFixed(1)}% load`;
  $('#kpiHeapInfo').textContent = `heap height ${s.heapHeight} · ${s.queueSize} queued`;
  $('#kpiShiftInfo').textContent = `${s.shift.toLowerCase()} shift · ${s.doctors} on roster`;
  const pct = s.bedsTotal ? (s.bedsOccupied / s.bedsTotal) * 100 : 0;
  $('#kpiBedBar').style.width = `${pct.toFixed(1)}%`;
  $('#kpiDensity').textContent = `${s.bedDensity.toFixed(1)}% full`;

  const badge = $('#pressureBadge');
  const pressure = s.criticalWaiting > 0 ? 'critical' : s.waiting > 8 ? 'busy' : s.waiting > 3 ? 'moderate' : 'stable';
  badge.textContent = pressure;
  badge.className = 'badge ' + (pressure === 'critical' ? 'bad' : pressure === 'busy' ? 'warn' : 'good');
}

function renderSeverity(snap) {
  const counts = { critical: 0, emergency: 0, urgent: 0, standard: 0 };
  snap.patients.forEach((p) => {
    if (p.status === 'waiting' || p.status === 'admitted') {
      if (counts[p.severity] != null) counts[p.severity]++;
    }
  });
  const total = SEV_ORDER.reduce((a, k) => a + counts[k], 0) || 1;

  const stack = $('#severityStack');
  if (!total || Object.values(counts).every((v) => v === 0)) {
    stack.innerHTML = `<div class="sev-seg empty">no active patients</div>`;
  } else {
    stack.innerHTML = SEV_ORDER.map(
      (k) => `<div class="sev-seg ${k}" style="flex-grow:${counts[k]}" title="${SEV_LABEL[k]}: ${counts[k]}">${counts[k] ? counts[k] : ''}</div>`
    ).join('');
  }
  $('#severityLegend').innerHTML = SEV_ORDER.map(
    (k) => `<span><i class="${k}"></i>${SEV_LABEL[k]} <b>${counts[k]}</b></span>`
  ).join('');
}

function renderWardBars(snap) {
  $('#wardBars').innerHTML = snap.wards
    .map((w) => {
      const pct = w.occupancy;
      const cls = pct >= 95 ? 'full' : pct >= 70 ? 'high' : '';
      return `<div class="ward-bar-row">
        <span title="${esc(w.name)}">${esc(w.name)}</span>
        <div class="ward-bar"><i class="${cls}" style="width:${Math.max(1.5, pct).toFixed(1)}%"></i></div>
        <em>${w.occupied}/${w.capacity}</em>
      </div>`;
    })
    .join('');
}

function renderNext(snap) {
  const list = $('#nextList');
  const order = (snap.serviceOrder || []).slice(0, 5);
  if (!order.length) {
    list.innerHTML = `<li class="next-empty" style="padding-left:0;background:none;border:0">Queue is empty — every patient has been seen.</li>`;
    return;
  }
  list.innerHTML = order
    .map(
      (o) => `<li>
        <span class="sev-chip ${o.severity}">${SEV_LABEL[o.severity]}</span>
        <b>${esc(o.name)}</b><span class="mono">#${o.id}</span></li>`
    )
    .join('');
}

function renderActivity(snap) {
  const feed = $('#activityFeed');
  const events = (snap.events || []).slice().reverse().slice(0, 24);
  if (!events.length) {
    feed.innerHTML = `<li><time>—</time><div>No events yet.</div></li>`;
    return;
  }
  feed.innerHTML = events
    .map((e) => {
      const m = e.match(/^\[(\d{2}:\d{2})\]\s*([A-Z-]+)\s*·\s*(.*)$/);
      if (!m) return `<li><time>—</time><div>${esc(e)}</div></li>`;
      return `<li><time>${m[1]}</time><div><span class="tag ${m[2]}">${m[2]}</span>${esc(m[3])}</div></li>`;
    })
    .join('');
}

/* ────────────────────────────── triage view ────────────────────────────── */
function renderTriage(snap) {
  const heap = snap.heapArray || [];
  const badge = $('#heapValidBadge');
  badge.textContent = snap.heapValid ? 'heap valid' : 'heap violated';
  badge.className = 'badge ' + (snap.heapValid ? 'good' : 'bad');

  const arr = $('#heapArray');
  if (!heap.length) {
    arr.innerHTML = `<p class="next-empty">The priority queue is empty — all patients have been triaged.</p>`;
  } else {
    arr.innerHTML = heap
      .map(
        (h, i) => `<div class="heap-cell ${h.severity} ${i === 0 ? 'root' : ''}" data-index="${i}" title="${esc(h.name)} · priority weight ${h.weight}">
          <span class="w">${h.weight}</span>
          <span class="n">${esc(h.name.split(' ')[0])}</span>
        </div>`
      )
      .join('');
  }

  $('#serviceOrder').innerHTML = (snap.serviceOrder || [])
    .slice(0, 14)
    .map(
      (o, i) => `<li><span class="idx">${String(i + 1).padStart(2, '0')}</span>
        <b>${esc(o.name)}</b> <span class="mono muted">#${o.id}</span>
        <span class="sev-chip ${o.severity}" style="margin-left:auto">${SEV_LABEL[o.severity]}</span></li>`
    )
    .join('') || `<li><span class="idx">—</span><span class="muted">Nobody waiting</span></li>`;

  const waiting = (snap.patients || [])
    .filter((p) => p.status === 'waiting')
    .sort((a, b) => b.weight - a.weight || a.arrivalTick - b.arrivalTick || a.id - b.id);

  $('#waitingBadge').textContent = `${waiting.length} waiting`;
  const tbody = $('#waitingTable tbody');
  if (!waiting.length) {
    tbody.innerHTML = `<tr class="empty-row"><td colspan="7">The waiting room is empty.</td></tr>`;
  } else {
    tbody.innerHTML = waiting
      .map(
        (p, i) => `<tr>
          <td class="id-cell">${String(i + 1).padStart(2, '0')}</td>
          <td>
            <div class="name-cell">
              <span class="avatar">${initials(p.name)}</span>
              <div><b>${esc(p.name)}</b><small>#${p.id} · ${p.age}y ${esc(p.gender)}</small></div>
            </div>
          </td>
          <td>${sevChip(p.severity)}</td>
          <td class="mono muted">${esc(p.arrival)}</td>
          <td class="mono">${fmtTime(p.waitMinutes)}</td>
          <td class="muted">${esc(p.condition)}</td>
          <td class="right">
            <button class="icon-action accent" data-escalate="${p.id}" title="Escalate to critical">
              <svg viewBox="0 0 24 24"><path d="M12 19V5M6 11l6-6 6 6"/></svg>
            </button>
            <button class="icon-action accent" data-patient-detail="${p.id}" title="Admit / view">
              <svg viewBox="0 0 24 24"><path d="M4 12h16M12 4v16"/></svg>
            </button>
            <button class="icon-action danger" data-delete="${p.id}" title="Remove patient">
              <svg viewBox="0 0 24 24"><path d="M5 7h14M9 7V5h6v2M7 7l1 13h8l1-13"/></svg>
            </button>
          </td>
        </tr>`
      )
      .join('');
  }
}

/* ────────────────────────────── patients view ────────────────────────────── */
function renderPatients(snap) {
  const filter = state.statusFilter;
  const rows = (snap.patients || []).filter((p) => filter === 'all' || p.status === filter);
  const tbody = $('#patientTable tbody');
  $('#registrySub').textContent = `${rows.length} record${rows.length === 1 ? '' : 's'}${
    filter === 'all' ? ' in the hash table' : ` · ${filter}`
  }`;
  $('#registryEmpty').hidden = rows.length > 0;

  if (!rows.length) {
    tbody.innerHTML = `<tr class="empty-row"><td colspan="8">No patients match this filter.</td></tr>`;
    return;
  }

  tbody.innerHTML = rows
    .slice()
    .sort((a, b) => b.id - a.id)
    .map((p) => {
      const loc =
        p.bedRow >= 0 ? `${esc(p.ward)} · [${p.bedRow},${p.bedCol}]` : p.status === 'treated' ? '—' : 'not admitted';
      return `<tr>
        <td class="id-cell">#${p.id}</td>
        <td>
          <div class="name-cell">
            <span class="avatar">${initials(p.name)}</span>
            <div><b>${esc(p.name)}</b><small>${esc(p.condition)}</small></div>
          </div>
        </td>
        <td class="muted">${p.age} · ${esc(p.gender)}</td>
        <td>${sevChip(p.severity)}</td>
        <td>${statusChip(p.status, p.statusLabel)}</td>
        <td class="muted">${loc}</td>
        <td class="muted">${p.doctor ? esc(p.doctor) : '—'}</td>
        <td class="right">
          ${p.bedRow < 0 && p.status !== 'treated'
            ? `<button class="icon-action accent" data-admit="${p.id}" title="Allocate bed">
                 <svg viewBox="0 0 24 24"><path d="M3 18v-6h18v6M6 12V8h12v4"/></svg>
               </button>`
            : ''}
          ${p.bedRow >= 0
            ? `<button class="icon-action accent" data-freebed="${p.id}" title="Release bed">
                 <svg viewBox="0 0 24 24"><path d="M4 12l5 5L20 6"/></svg>
               </button>`
            : ''}
          ${p.status === 'waiting'
            ? `<button class="icon-action accent" data-escalate="${p.id}" title="Escalate">
                 <svg viewBox="0 0 24 24"><path d="M12 19V5M6 11l6-6 6 6"/></svg>
               </button>`
            : ''}
          <button class="icon-action danger" data-delete="${p.id}" title="Delete">
            <svg viewBox="0 0 24 24"><path d="M5 7h14M9 7V5h6v2M7 7l1 13h8l1-13"/></svg>
          </button>
        </td>
      </tr>`;
    })
    .join('');
}

/* ────────────────────────────── wards view ────────────────────────────── */
function renderWards(snap) {
  const wards = snap.wards || [];
  if (!wards.length) return;
  if (state.wardTab >= wards.length) state.wardTab = 0;

  $('#wardTabs').innerHTML = wards
    .map(
      (w, i) => `<button class="ward-tab ${i === state.wardTab ? 'active' : ''}" data-ward="${i}">
        ${esc(w.name)} <em>${w.occupied}/${w.capacity}</em>
      </button>`
    )
    .join('');

  const ward = wards[state.wardTab];
  const trips = ward.triplets || [];

  const occupied = new Map();
  trips.forEach((t) => occupied.set(`${t.row},${t.col}`, t.patient));
  const patients = new Map((snap.patients || []).map((p) => [p.id, p]));

  const cols = ward.cols;
  let html = `<div class="bed-cell col-label"></div>`;
  for (let c = 0; c < cols; c++) html += `<div class="bed-cell col-label">C${c + 1}</div>`;
  for (let r = 0; r < ward.rows; r++) {
    html += `<div class="bed-cell row-label">R${r + 1}</div>`;
    for (let c = 0; c < cols; c++) {
      const pid = occupied.get(`${r},${c}`);
      if (pid == null) {
        html += `<div class="bed-cell" title="Free bed [${r},${c}]">${r}·${c}</div>`;
      } else {
        const p = patients.get(pid);
        const sev = p ? p.severity : 'standard';
        html += `<div class="bed-cell occupied ${sev}" title="${esc(p ? p.name : 'Patient #' + pid)} · ${SEV_LABEL[sev]} · [${r},${c}]">${pid}</div>`;
      }
    }
  }
  const grid = $('#bedGrid');
  grid.style.gridTemplateColumns = `auto repeat(${cols}, minmax(0, 1fr))`;
  grid.innerHTML = html;

  $('#matrixStats').innerHTML = `
    <div><b>${ward.rows} × ${ward.cols}</b><small>matrix</small></div>
    <div><b>${ward.capacity}</b><small>total beds</small></div>
    <div><b>${ward.occupied}</b><small>triplets (nnz)</small></div>
    <div><b>${ward.occupancy.toFixed(1)}%</b><small>density</small></div>
    <div><b>${ward.free}</b><small>free beds</small></div>
    <div><b>${(ward.occupied / Math.max(1, ward.capacity) * 100).toFixed(2)}%</b><small>sparse</small></div>`;

  $('#tripletList').innerHTML = trips.length
    ? trips
        .map((t) => {
          const p = patients.get(t.patient);
          return `<div class="triplet">
            <span class="coord">r${t.row}</span>
            <span class="coord">c${t.col}</span>
            <span class="pid">#${p.patient} ${esc(p.name)}</span>
          </div>`;
        })
        .join('')
    : `<p class="next-empty">No occupied beds — the sparse matrix is empty.</p>`;

  const denseBytes = ward.capacity * 4;
  const sparseBytes = ward.occupied * 12;
  const saved = denseBytes ? Math.max(0, 100 * (1 - Math.min(1, sparseBytes / denseBytes))) : 0;
  $('#memoryCompare').innerHTML = `
    <div class="mem-row">
      <span>Dense grid</span>
      <div class="mem-track dense"><i style="width:100%"></i></div>
      <em>${(denseBytes / 1024).toFixed(2)} KB</em>
    </div>
    <div class="mem-row">
      <span>Sparse</span>
      <div class="mem-track sparse"><i style="width:${Math.max(2, (sparseBytes / denseBytes) * 100).toFixed(1)}%"></i></div>
      <em>${(sparseBytes / 1024).toFixed(2)} KB</em>
    </div>
    <p class="foot-note" style="margin-top:2px">${saved.toFixed(0)}% smaller than a dense ${ward.rows}×${ward.cols} int grid
    (12 bytes per triplet vs 4 bytes per cell).</p>`;

  const admitted = (snap.patients || []).filter((p) => p.bedRow >= 0);
  const tbody = $('#admittedTable tbody');
  tbody.innerHTML = admitted.length
    ? admitted
        .map(
          (p) => `<tr>
            <td class="id-cell">#${p.id}</td>
            <td><div class="name-cell"><span class="avatar">${initials(p.name)}</span><div><b>${esc(p.name)}</b><small>${esc(p.condition)}</small></div></div></td>
            <td>${sevChip(p.severity)}</td>
            <td class="muted">${esc(p.ward || '—')}</td>
            <td class="mono">[${p.bedRow}, ${p.bedCol}]</td>
            <td class="muted">${p.doctor ? esc(p.doctor) : '—'}</td>
            <td class="right">
              <button class="icon-action accent" data-freebed="${p.id}" title="Release bed">
                <svg viewBox="0 0 24 24"><path d="M4 12l5 5L20 6"/></svg>
              </button>
            </td>
          </tr>`
        )
        .join('')
    : `<tr class="empty-row"><td colspan="7">No admitted patients.</td></tr>`;
}

/* ────────────────────────────── algorithms view ────────────────────────────── */
function renderAlgorithms(snap) {
  const s = snap.stats;
  $('#asHeapSize').textContent = s.queueSize;
  $('#asHeapHeight').textContent = s.heapHeight;
  $('#asHeapValid').textContent = snap.heapValid ? 'valid' : 'broken';
  $('#asBuckets').textContent = s.hashBuckets;
  $('#asLoad').textContent = `${s.hashLoadFactor.toFixed(1)}%`;
  $('#asChain').textContent = s.longestChain;
  $('#asBstHeight').textContent = s.bstHeight;
  $('#asIndexed').textContent = s.totalPatients;
  $('#asProbes').textContent = state.lastProbe != null ? state.lastProbe : '—';
  const totalCells = snap.wards.reduce((a, w) => a + w.capacity, 0);
  const occ = snap.wards.reduce((a, w) => a + w.occupied, 0);
  $('#asCells').textContent = occ;
  $('#asDensity').textContent = `${totalCells ? ((occ / totalCells) * 100).toFixed(1) : 0}%`;
  $('#asSaved').textContent = `${totalCells ? (100 * (1 - occ / totalCells)).toFixed(0) : 0}%`;
}

/* ────────────────────────────── benchmark ────────────────────────────── */
async function runBenchmark() {
  const body = $('#benchBody');
  body.innerHTML = `<p class="foot-note">Timing 200,000 records inside the C++ engine…</p>`;
  try {
    const data = await api('/api/benchmark');
    state.bench = data;
    const maxByRow = data.rows.map((r) => Math.max(r.fastValue, r.slowValue) || 1);
    body.innerHTML = data.rows
      .map((r, idx) => {
        const max = maxByRow[idx];
        const speedup =
          r.fastValue > 0 && r.slowValue > 0 ? (r.slowValue / r.fastValue).toFixed(0) + '×' : '—';
        const fmt = (v) => (v >= 1000 ? `${v.toFixed(0)}` : v >= 1 ? v.toFixed(2) : v.toFixed(4));
        return `<div class="bench-row">
          <header>
            <div><h3>${esc(r.operation)}</h3><small>${esc(r.detail)}</small></div>
            <span class="speedup">${speedup} faster</span>
          </header>
          <div class="bench-bars">
            <div class="bench-bar fast">
              <div class="label"><b>${esc(r.fast)}</b><small>${esc(r.fastBigO)}</small></div>
              <div class="track"><i style="width:${Math.max(1.5, (r.fastValue / max) * 100).toFixed(1)}%"></i></div>
              <div class="value">${fmt(r.fastValue)} ${esc(r.unit)}</div>
            </div>
            <div class="bench-bar slow">
              <div class="label"><b>${esc(r.slow)}</b><small>${esc(r.slowBigO)}</small></div>
              <div class="track"><i style="width:${Math.max(1.5, (r.slowValue / max) * 100).toFixed(1)}%"></i></div>
              <div class="value">${fmt(r.slowValue)} ${esc(r.unit)}</div>
            </div>
          </div>
        </div>`;
      })
      .join('') + `<p class="foot-note">
        Sparse storage saves <b>${data.memory.savedPercent}%</b> memory at ${data.memory.density}% density
        (${data.memory.sparseBytes} B vs ${data.memory.denseBytes} B).
        Measured across ${data.n.toLocaleString()} records / ${data.probes.toLocaleString()} probe queries.</p>`;
    toast('Benchmark complete', `${data.n.toLocaleString()} records timed in the C++ engine`, 'success');
  } catch (err) {
    body.innerHTML = `<p class="foot-note">Benchmark failed: ${esc(err.message)}</p>`;
    toast('Benchmark failed', err.message, 'error');
  }
}

/* ────────────────────────────── search ────────────────────────────── */
async function runSearch(query) {
  if (!query) return;
  try {
    const data = await api(`/api/search?q=${encodeURIComponent(query)}`);
    state.lastProbe = data.probes;
    $('#searchMetrics').hidden = false;
    $('#algoName').textContent = data.algorithm;
    $('#probeCount').textContent = data.probes;
    $('#searchTime').textContent = `${data.microseconds.toFixed(2)} µs`;
    $('#matchCount').textContent = data.count;

    const box = $('#searchResults');
    if (!data.results.length) {
      box.innerHTML = `<p class="next-empty">No patient matched “${esc(query)}”. The hash table and BST both returned no hit.</p>`;
    } else {
      box.innerHTML = data.results
        .map(
          (p) => `<div class="result-row">
            <span class="avatar">${initials(p.name)}</span>
            <div class="grow">
              <b>${esc(p.name)}</b> <span class="mono muted">#${p.id}</span>
              <small>${p.age}y ${esc(p.gender)} · ${esc(p.condition)} · ${esc(p.department)}</small>
            </div>
            ${sevChip(p.severity)}
            ${statusChip(p.status, p.statusLabel)}
          </div>`
        )
        .join('');
    }
    if (state.view === 'algorithms') $('#asProbes').textContent = data.probes;
  } catch (err) {
    toast('Search failed', err.message, 'error');
  }
}

/* ────────────────────────────── data flow ────────────────────────────── */
async function refresh(quiet = true) {
  try {
    const snap = await api('/api/state');
    state.snapshot = snap;
    setEngineOnline(snap);
    render();
  } catch (err) {
    if (!quiet) toast('Refresh failed', err.message, 'error');
  }
}

function setEngineOnline(snap) {
  const dot = $('#engineDot');
  dot.className = 'dot live';
  $('#engineLabel').textContent = `Engine online${snap ? ` · ${snap.stats.totalPatients} records` : ''}`;
}

async function action(fn, successMessage) {
  try {
    await fn();
    await refresh();
    if (successMessage) toast(successMessage.title, successMessage.body, 'success');
  } catch (err) {
    toast('Action failed', err.message, 'error');
  }
}

/* ────────────────────────────── view switching ────────────────────────────── */
const VIEW_META = {
  overview: ['Live Overview', 'Real-time triage, capacity and emergency response'],
  triage: ['Triage Queue', 'Priority-queue based emergency handling'],
  patients: ['Patients', 'Hash table · BST · binary search lookup'],
  wards: ['Bed Allocator', 'Sparse-matrix bed representation and allocation'],
  algorithms: ['Data Structures', 'How each structure keeps the hospital fast'],
  benchmarks: ['Benchmarks', 'Measured complexity against naive alternatives']
};

function setView(view) {
  state.view = view;
  $$('.view').forEach((v) => v.classList.toggle('active', v.id === `view-${view}`));
  $$('.nav-item').forEach((b) => b.classList.toggle('active', b.dataset.view === view));
  const [title, sub] = VIEW_META[view] || ['VoltIQ Care', ''];
  $('#viewTitle').textContent = title;
  $('#viewSub').textContent = sub;
  $('.sidebar').classList.remove('open');
  if (state.view === 'benchmarks' && !state.bench) runBenchmark();
  render();
}

/* ────────────────────────────── events ────────────────────────────── */
function wire() {
  $('#nav').addEventListener('click', (e) => {
    const btn = e.target.closest('.nav-item');
    if (btn) setView(btn.dataset.view);
  });

  $$('[data-view-jump]').forEach((b) =>
    b.addEventListener('click', () => setView(b.dataset.viewJump))
  );

  $('#menuToggle').addEventListener('click', () => $('.sidebar').classList.toggle('open'));

  // simulation
  const tick = (minutes) =>
    action(() => post('/api/tick', { minutes }), { title: `${minutes} minute(s) advanced`, body: 'Doctors keep working through the queue' });
  $('#tickBtn').addEventListener('click', () => tick(1));
  $('#tick5Btn').addEventListener('click', () => tick(5));
  $('#tick10Btn').addEventListener('click', () => tick(10));

  $('#callNextBtn').addEventListener('click', () =>
    action(() => post('/api/call-next'), { title: 'Next patient called', body: 'Highest-priority patient popped from the heap' })
  );
  $('#massBtn').addEventListener('click', () =>
    action(() => post('/api/mass-casualty', { count: 12 }), { title: 'Mass casualty drill', body: '12 emergency arrivals injected into the heap' })
  );

  $('#autoTick').addEventListener('change', (e) => {
    state.autoTick = e.target.checked;
    clearInterval(state.simTimer);
    if (state.autoTick) {
      state.simTimer = setInterval(async () => {
        try {
          await post('/api/tick', { minutes: 1 });
          await refresh();
        } catch (err) {
          clearInterval(state.simTimer);
          $('#autoTick').checked = false;
          state.autoTick = false;
          toast('Simulation stopped', err.message, 'error');
        }
      }, 2000);
      toast('Simulation running', 'One virtual minute every 2 seconds', 'success');
    } else {
      toast('Simulation paused', '', 'info');
    }
  });

  // register modal
  const openModal = () => {
    $('#modal').hidden = false;
    setTimeout(() => $('#registerForm [name="name"]').focus(), 60);
  };
  const closeModal = () => ($('#modal').hidden = true);
  $('#registerBtn').addEventListener('click', openModal);
  $('#registerBtn2').addEventListener('click', openModal);
  $('#modalClose').addEventListener('click', closeModal);
  $('#modalCancel').addEventListener('click', closeModal);
  $('#modal').addEventListener('click', (e) => {
    if (e.target.id === 'modal') closeModal();
  });

  $('#registerForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    const form = e.target;
    const btn = $('#submitPatient');
    const payload = Object.fromEntries(new FormData(form).entries());
    btn.disabled = true;
    btn.textContent = 'Adding…';
    try {
      const res = await post('/api/register', payload);
      closeModal();
      form.reset();
      await refresh();
      toast('Patient registered', `#${res.id} ${res.patient.name} pushed into the triage heap`, 'success');
    } catch (err) {
      toast('Registration failed', err.message, 'error');
    } finally {
      btn.disabled = false;
      btn.textContent = 'Add to queue';
    }
  });

  // patient table actions (delegated)
  document.addEventListener('click', async (e) => {
    const esc1 = e.target.closest('[data-escalate]');
    if (esc1) {
      await action(() => post('/api/escalate', { id: Number(esc1.dataset.escalate), severity: 'critical' }), {
        title: 'Patient escalated',
        body: 'Re-triaged to critical and pushed to the front of the heap'
      });
      return;
    }
    const del = e.target.closest('[data-delete]');
    if (del) {
      await action(() => post('/api/delete', { id: Number(del.dataset.delete) }), {
        title: 'Patient removed',
        body: 'Deleted from the hash table, index and BST'
      });
      return;
    }
    const admit = e.target.closest('[data-admit]');
    if (admit) {
      await action(() => post('/api/admit', { id: Number(admit.dataset.admit) }), {
        title: 'Bed allocated',
        body: 'Sparse matrix first-fit assignment'
      });
      return;
    }
    const free = e.target.closest('[data-freebed]');
    if (free) {
      await action(() => post('/api/discharge-bed', { id: Number(free.dataset.freebed) }), {
        title: 'Bed released',
        body: 'Triplet removed from the sparse matrix'
      });
      return;
    }
    const detail = e.target.closest('[data-patient-detail]');
    if (detail) {
      const id = Number(detail.dataset.patientDetail);
      setView('patients');
      $('#searchInput').value = String(id);
      runSearch(String(id));
      return;
    }
    const wardTab = e.target.closest('[data-ward]');
    if (wardTab) {
      state.wardTab = Number(wardTab.dataset.ward);
      renderWards(state.snapshot);
      return;
    }
    const strategy = e.target.closest('[data-strategy]');
    if (strategy) {
      const value = strategy.dataset.strategy;
      await action(async () => {
        await post('/api/strategy', { strategy: value });
        state.strategy = value;
      }, { title: `Strategy: ${strategy.textContent}`, body: 'New admissions use this fill order' });
      $$('#strategyPicker button').forEach((b) => b.classList.toggle('active', b === strategy));
      return;
    }
    const filter = e.target.closest('[data-filter]');
    if (filter) {
      state.statusFilter = filter.dataset.filter;
      $$('#statusFilter button').forEach((b) => b.classList.toggle('active', b === filter));
      renderPatients(state.snapshot);
      return;
    }
    const action2 = e.target.closest('[data-action]');
    if (action2) {
      if (action2.dataset.action === 'mass-casualty') {
        await action(() => post('/api/mass-casualty', { count: 12 }), {
          title: 'Mass casualty drill',
          body: '12 arrivals queued for triage'
        });
      } else if (action2.dataset.action === 'call-next') {
        await action(() => post('/api/call-next'), {
          title: 'Next patient called',
          body: 'Heap top removed and assigned to a doctor'
        });
      }
    }
  });

  // search
  $('#searchBtn').addEventListener('click', () => runSearch($('#searchInput').value.trim()));
  $('#searchInput').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') runSearch($('#searchInput').value.trim());
  });
  let searchDebounce;
  $('#searchInput').addEventListener('input', (e) => {
    clearTimeout(searchDebounce);
    const v = e.target.value.trim();
    if (!v) return;
    searchDebounce = setTimeout(() => runSearch(v), 320);
  });
  $('#clearSearchBtn').addEventListener('click', () => {
    $('#searchInput').value = '';
    $('#searchResults').innerHTML = '';
    $('#searchMetrics').hidden = true;
  });

  $('#runBenchBtn').addEventListener('click', runBenchmark);

  document.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
      e.preventDefault();
      setView('patients');
      $('#searchInput').focus();
    }
    if (e.key === 'Escape') $('#modal').hidden = true;
  });
}

/* ────────────────────────────── boot ────────────────────────────── */
async function boot() {
  wire();
  await refresh(false);
  state.polling = setInterval(() => refresh(), 4000);
}

document.addEventListener('DOMContentLoaded', boot);
