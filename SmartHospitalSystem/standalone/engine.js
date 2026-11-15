/* ==========================================================================
   engine.js — standalone (offline) build of the VoltIQ Care DSA engine.
   A faithful JavaScript mirror of cpp/hospital.hpp, cpp/priority_queue.hpp,
   cpp/hash_table.hpp, cpp/search.hpp and cpp/sparse_matrix.hpp, exposing the
   same /api/* surface so the dashboard runs with no server at all.

   The canonical implementation is the C++ engine (SmartHospitalSystem/cpp).
   This port exists so the project can be opened as a single HTML file,
   published to GitHub Pages, or submitted without a compiler.
   ========================================================================== */
(function (global) {
  'use strict';

  /* ─────────────── models ─────────────── */
  const SEV_WEIGHT = { critical: 4, emergency: 3, urgent: 2, standard: 1 };
  const SEV_NAME = { critical: 'Critical', emergency: 'Emergency', urgent: 'Urgent', standard: 'Standard' };
  const SEV_ORDER = { CRITICAL: 'critical', EMERGENCY: 'emergency', URGENT: 'urgent', STANDARD: 'standard' };

  const clock = (tick) => {
    const total = 8 * 60 + tick;
    return String(Math.floor((total / 60) % 24)).padStart(2, '0') + ':' + String(total % 60).padStart(2, '0');
  };
  const nowString = () => {
    const d = new Date();
    return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0') + ':' + String(d.getSeconds()).padStart(2, '0');
  };

  /* ─────────────── priority queue: binary max-heap ─────────────── */
  function higherPriority(a, b) {
    if (a.weight !== b.weight) return a.weight > b.weight;      // severity first
    if (a.arrivalTick !== b.arrivalTick) return a.arrivalTick < b.arrivalTick; // longest wait
    return a.seq < b.seq;                                        // FIFO
  }

  class ArrayMaxHeap {
    constructor() { this.v = []; }
    get size() { return this.v.length; }
    clear() { this.v = []; }
    push(e) {
      this.v.push(e);
      let i = this.v.length - 1;
      while (i > 0) {
        const p = (i - 1) >> 1;
        if (!higherPriority(this.v[i], this.v[p])) break;
        [this.v[i], this.v[p]] = [this.v[p], this.v[i]];
        i = p;
      }
    }
    pop() {
      if (!this.v.length) return null;
      const top = this.v[0];
      const last = this.v.pop();
      if (this.v.length) { this.v[0] = last; this.siftDown(0); }
      return top;
    }
    siftDown(i) {
      const n = this.v.length;
      for (;;) {
        const l = 2 * i + 1, r = 2 * i + 2;
        let best = i;
        if (l < n && higherPriority(this.v[l], this.v[best])) best = l;
        if (r < n && higherPriority(this.v[r], this.v[best])) best = r;
        if (best === i) break;
        [this.v[i], this.v[best]] = [this.v[best], this.v[i]];
        i = best;
      }
    }
    removeById(id) {
      const i = this.v.findIndex((e) => e.patientId === id);
      if (i < 0) return false;
      const last = this.v.pop();
      if (i < this.v.length) {
        this.v[i] = last;
        // re-heapify both directions (re-triage)
        let j = i;
        while (j > 0) {
          const p = (j - 1) >> 1;
          if (!higherPriority(this.v[j], this.v[p])) break;
          [this.v[j], this.v[p]] = [this.v[p], this.v[j]];
          j = p;
        }
        this.siftDown(j);
      }
      return true;
    }
    validate() {
      for (let i = 1; i < this.v.length; i++) {
        const p = (i - 1) >> 1;
        if (higherPriority(this.v[i], this.v[p])) return false;
      }
      return true;
    }
    snapshot() { return this.v.slice(); }
    height() { let h = 0, n = this.v.length; while (n > 0) { h++; n >>= 1; } return h; }
  }

  /* ─────────────── hash table: separate chaining ─────────────── */
  class PatientHashTable {
    constructor(cap = 128) {
      let p = 8;
      while (p < cap) p <<= 1;
      this.buckets = Array.from({ length: p }, () => []);
      this.size = 0;
    }
    hash(key) {
      let k = BigInt.asUintN(64, BigInt(key));
      k = BigInt.asUintN(64, k * 2654435761n);
      k = k ^ (k >> 16n);
      return Number(k & BigInt(this.buckets.length - 1));
    }
    put(key, value) {
      if ((this.size + 1) * 4 > this.buckets.length * 3) this.rehash(this.buckets.length * 2);
      const idx = this.hash(key);
      const hit = this.buckets[idx].find((e) => e.key === key);
      if (hit) { hit.value = value; return; }
      this.buckets[idx].push({ key, value });
      this.size++;
    }
    get(key) {
      const hit = this.buckets[this.hash(key)].find((e) => e.key === key);
      return hit ? hit.value : null;
    }
    remove(key) {
      const chain = this.buckets[this.hash(key)];
      const i = chain.findIndex((e) => e.key === key);
      if (i < 0) return false;
      chain.splice(i, 1);
      this.size--;
      return true;
    }
    rehash(n) {
      const old = this.buckets;
      this.buckets = Array.from({ length: n }, () => []);
      this.size = 0;
      old.forEach((chain) => chain.forEach((e) => this.put(e.key, e.value)));
    }
    loadFactor() { return this.size / this.buckets.length; }
    longestChain() { return this.buckets.reduce((m, c) => Math.max(m, c.length), 0); }
    values() { return this.buckets.flatMap((c) => c.map((e) => e.value)); }
  }

  /* ─────────────── sorted index (binary search) + BST ─────────────── */
  class SortedIndex {
    constructor() { this.v = []; }
    insert(id, name) {
      const pair = { id, nameLower: name.toLowerCase() };
      let lo = 0, hi = this.v.length;
      while (lo < hi) { const mid = (lo + hi) >> 1; if (this.v[mid].id < id) lo = mid + 1; else hi = mid; }
      this.v.splice(lo, 0, pair);
    }
    erase(id) {
      const i = this.v.findIndex((p) => p.id === id);
      if (i >= 0) this.v.splice(i, 1);
    }
    binarySearchId(id) {
      let lo = 0, hi = this.v.length - 1, probes = 0;
      while (lo <= hi) {
        const mid = lo + ((hi - lo) >> 1);
        probes++;
        if (this.v[mid].id === id) return { pair: this.v[mid], probes };
        if (this.v[mid].id < id) lo = mid + 1; else hi = mid - 1;
      }
      return { pair: null, probes };
    }
    searchNamePrefix(prefix) {
      const p = prefix.toLowerCase();
      const sorted = this.v.slice().sort((a, b) =>
        a.nameLower !== b.nameLower ? (a.nameLower < b.nameLower ? -1 : 1) : a.id - b.id);
      let lo = 0, hi = sorted.length, probes = 0;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        probes++;
        if (sorted[mid].nameLower.slice(0, p.length) < p) lo = mid + 1; else hi = mid;
      }
      const hits = [];
      for (let i = lo; i < sorted.length; i++) {
        if (sorted[i].nameLower.slice(0, p.length) !== p) break;
        hits.push(sorted[i].id);
      }
      return { hits, probes };
    }
  }

  class NameBst {
    constructor() { this.root = null; this.count = 0; }
    insert(name, id) {
      const key = name.toLowerCase();
      const node = { key, id, left: null, right: null };
      if (!this.root) { this.root = node; this.count++; return; }
      let cur = this.root;
      for (;;) {
        if (key < cur.key) { if (!cur.left) { cur.left = node; this.count++; return; } cur = cur.left; }
        else if (key > cur.key) { if (!cur.right) { cur.right = node; this.count++; return; } cur = cur.right; }
        else { cur.id = id; return; }
      }
    }
    find(name) {
      const key = name.toLowerCase();
      let cur = this.root, comparisons = 0;
      while (cur) {
        comparisons++;
        if (key === cur.key) return { id: cur.id, comparisons };
        cur = key < cur.key ? cur.left : cur.right;
      }
      return { id: -1, comparisons };
    }
    heightOf(node) { return node ? 1 + Math.max(this.heightOf(node.left), this.heightOf(node.right)) : 0; }
    height() { return this.heightOf(this.root); }
  }

  /* ─────────────── sparse matrix: row-major triplets ─────────────── */
  class SparseMatrix {
    constructor(rows, cols) { this.rows = rows; this.cols = cols; this.data = []; } // sorted row-major
    lowerBound(r, c) {
      let lo = 0, hi = this.data.length;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        const t = this.data[mid];
        if (t.row < r || (t.row === r && t.col < c)) lo = mid + 1; else hi = mid;
      }
      return lo;
    }
    get(r, c) {
      const i = this.lowerBound(r, c);
      const t = this.data[i];
      return t && t.row === r && t.col === c ? t.value : 0;
    }
    occupied(r, c) { return this.get(r, c) !== 0; }
    set(r, c, value) {
      if (r < 0 || c < 0 || r >= this.rows || c >= this.cols) return;
      const i = this.lowerBound(r, c);
      const t = this.data[i];
      if (t && t.row === r && t.col === c) {
        if (value === 0) this.data.splice(i, 1); else t.value = value;
        return;
      }
      if (value === 0) return;
      this.data.splice(i, 0, { row: r, col: c, value });
    }
    clearCell(r, c) {
      const i = this.lowerBound(r, c);
      const t = this.data[i];
      if (t && t.row === r && t.col === c) { this.data.splice(i, 1); return true; }
      return false;
    }
    nonZero() { return this.data.length; }
    capacity() { return this.rows * this.cols; }
    density() { return this.capacity() ? (100 * this.data.length) / this.capacity() : 0; }
    allocate(strategy) {
      if (strategy === 'column') {
        for (let c = 0; c < this.cols; c++)
          for (let r = 0; r < this.rows; r++)
            if (!this.occupied(r, c)) return { found: true, row: r, col: c };
        return { found: false };
      }
      if (strategy === 'spread') {
        let best = 99, bestR = -1, bestC = -1;
        for (let r = 0; r < this.rows; r++) {
          for (let c = 0; c < this.cols; c++) {
            if (this.occupied(r, c)) continue;
            let score = 0;
            [[-1, 0], [1, 0], [0, -1], [0, 1]].forEach(([dr, dc]) => {
              const nr = r + dr, nc = c + dc;
              if (nr >= 0 && nc >= 0 && nr < this.rows && nc < this.cols && this.occupied(nr, nc)) score++;
            });
            if (score < best) { best = score; bestR = r; bestC = c; }
          }
        }
        return bestR >= 0 ? { found: true, row: bestR, col: bestC } : { found: false };
      }
      for (let r = 0; r < this.rows; r++)
        for (let c = 0; c < this.cols; c++)
          if (!this.occupied(r, c)) return { found: true, row: r, col: c };
      return { found: false };
    }
  }

  /* ─────────────── the hospital engine ─────────────── */
  const WARD_LAYOUT = [
    ['ICU', 3, 8], ['Emergency', 4, 10], ['General Ward A', 4, 14],
    ['General Ward B', 4, 14], ['Pediatrics', 3, 8], ['Maternity', 3, 8]
  ];

  const DOCTORS = [
    ['Aarav Sharma', 'Cardiology', 'MD, DM Cardio'], ['Priya Menon', 'Emergency Medicine', 'MBBS, MEM'],
    ['Rohit Deshmukh', 'General Medicine', 'MBBS, MD'], ['Sneha Iyer', 'Pediatrics', 'MBBS, DCH'],
    ['Vikram Rao', 'Orthopaedics', 'MS Ortho'], ['Ananya Kulkarni', 'Maternity', 'MBBS, MS OBG'],
    ['Imran Sheikh', 'Anaesthesiology', 'MBBS, DA'], ['Kavya Nair', 'Pulmonology', 'MD Pulmo'],
    ['Aditya Joshi', 'Neurology', 'DM Neuro'], ['Meera Patil', 'General Medicine', 'MBBS'],
    ['Sanjay Gupta', 'Trauma Surgery', 'MS Gen Surg'], ['Farah Khan', 'Internal Medicine', 'MD']
  ];

  const SEEDS = [
    ['Ramesh Patil', 67, 'Male', '98220 11001', 'critical', 'Chest pain, ST elevation', 'Cardiology'],
    ['Sunita Deshpande', 54, 'Female', '98220 11002', 'emergency', 'Severe breathlessness', 'Pulmonology'],
    ['Arjun Mehta', 29, 'Male', '98220 11003', 'urgent', 'Fractured forearm', 'Orthopaedics'],
    ['Kavita Joshi', 38, 'Female', '98220 11004', 'standard', 'Routine check-up', 'General Medicine'],
    ['Mohammed Ansari', 72, 'Male', '98220 11005', 'critical', 'Stroke symptoms', 'Neurology'],
    ['Priyanka Shinde', 31, 'Female', '98220 11006', 'emergency', 'High fever, dehydration', 'General Medicine'],
    ['Baby Aarohi Kale', 2, 'Female', '98220 11007', 'urgent', 'Persistent vomiting', 'Pediatrics'],
    ['Vishal Chavan', 45, 'Male', '98220 11008', 'standard', 'BP follow-up', 'General Medicine'],
    ['Anjali Kulkarni', 27, 'Female', '98220 11009', 'urgent', 'Maternity admission', 'Maternity'],
    ['Dinesh Pawar', 58, 'Male', '98220 11010', 'emergency', 'Road accident, head trauma', 'Trauma Surgery'],
    ['Snehal Bhosale', 34, 'Female', '98220 11011', 'standard', 'Migraine consultation', 'Neurology'],
    ['Rafiq Shaikh', 61, 'Male', '98220 11012', 'urgent', 'Diabetic foot ulcer', 'General Medicine'],
    ['Rohit Sharma', 49, 'Male', '98220 11013', 'critical', 'Crush injury, internal bleeding', 'Trauma Surgery'],
    ['Meenakshi Rao', 66, 'Female', '98220 11014', 'critical', 'Acute MI, unstable rhythm', 'Cardiology'],
    ['Tanvi Shah', 23, 'Female', '98220 11015', 'emergency', 'Severe asthma attack', 'Pulmonology'],
    ['Aniket More', 41, 'Male', '98220 11016', 'urgent', 'Deep laceration', 'General Medicine']
  ];

  const WAVE = [
    ['Nisha Rane', 35, 'Female', '98220 11017', 'critical', 'Anaphylaxis after drug reaction', 'Emergency Medicine'],
    ['Suresh Kamble', 70, 'Male', '98220 11018', 'critical', 'Acute breathlessness, low SpO2', 'Pulmonology'],
    ['Isha Sethi', 19, 'Female', '98220 11019', 'emergency', 'High fever with seizures', 'Neurology'],
    ['Varun Nair', 52, 'Male', '98220 11020', 'urgent', 'Chest discomfort on exertion', 'Cardiology']
  ];

  const INPATIENTS = [
    ['Sadanand Kulkarni', 74, 'Male', 'critical', 'Post-bypass recovery', 'Cardiology'],
    ['Lata Deshpande', 69, 'Female', 'critical', 'Ventilator support', 'Pulmonology'],
    ['Omkar Bhide', 55, 'Male', 'emergency', 'Fractured femur, traction', 'Orthopaedics'],
    ['Rekha Sathe', 62, 'Female', 'emergency', 'Pneumonia, IV antibiotics', 'Pulmonology'],
    ['Chinmay Gokhale', 33, 'Male', 'urgent', 'Appendectomy follow-up', 'General Medicine'],
    ['Asha Wagh', 48, 'Female', 'urgent', 'Dengue monitoring', 'General Medicine'],
    ['Nitin Bhosale', 51, 'Male', 'urgent', 'Renal colic', 'General Medicine'],
    ['Pallavi Jagtap', 36, 'Female', 'standard', 'Observation after fall', 'Orthopaedics'],
    ['Girish Naik', 44, 'Male', 'standard', 'Hypertension control', 'General Medicine'],
    ['Shital Pawar', 29, 'Female', 'standard', 'Migraine protocol', 'Neurology'],
    ['Baby Vedant Sane', 1, 'Male', 'urgent', 'Bronchiolitis', 'Pediatrics'],
    ['Baby Ira Bhagat', 3, 'Female', 'standard', 'Gastroenteritis', 'Pediatrics'],
    ['Aarohi Sule', 42, 'Female', 'emergency', 'Post C-section recovery', 'Maternity'],
    ['Madhuri Deo', 31, 'Female', 'urgent', 'Pre-eclampsia monitoring', 'Maternity'],
    ['Kiran Kothari', 58, 'Male', 'emergency', 'Diabetic ketoacidosis', 'General Medicine'],
    ['Vaishali Rane', 47, 'Female', 'urgent', 'Thyroid storm', 'General Medicine'],
    ['Sandeep Chavan', 40, 'Male', 'standard', 'Fractured wrist, cast', 'Orthopaedics'],
    ['Nandini Bhat', 65, 'Female', 'critical', 'Arrhythmia, telemetry', 'Cardiology'],
    ['Yogesh Datar', 38, 'Male', 'urgent', 'Acute pancreatitis', 'General Medicine'],
    ['Smita Kher', 53, 'Female', 'standard', 'Post-op observation', 'General Medicine'],
    ['Rahul Bhandari', 26, 'Male', 'urgent', 'Road accident, clavicle', 'Trauma Surgery'],
    ['Aparna Limaye', 34, 'Female', 'emergency', 'Severe anaemia, transfusion', 'General Medicine'],
    ['Prashant Shelar', 49, 'Male', 'standard', 'Alcohol withdrawal', 'General Medicine'],
    ['Vaibhavi Kale', 30, 'Female', 'urgent', 'Ectopic pregnancy watch', 'Maternity'],
    ['Suresh Marathe', 71, 'Male', 'emergency', 'COPD exacerbation', 'Pulmonology'],
    ['Anagha Phadke', 39, 'Female', 'standard', 'Fever of unknown origin', 'General Medicine'],
    ['Devendra Patil', 56, 'Male', 'urgent', 'Cellulitis, IV therapy', 'General Medicine'],
    ['Rutuja Shinde', 22, 'Female', 'standard', 'Observation after seizure', 'Neurology']
  ];

  const WALKIN_NAMES = ['Nikhil Kadam', 'Pooja Salunkhe', 'Sameer Wagh', 'Ritika More', 'Ganesh Shinde',
    'Aarti Gaikwad', 'Yash Chougule', 'Neha Bhosale', 'Sagar Jadhav', 'Divya Mane',
    'Tushar Kulkarni', 'Shweta Pawar', 'Akash Deshmukh', 'Manasi Patil', 'Harsh Vardhan', 'Rupali Jadhav'];
  const WALKIN_COND = ['Acute abdominal pain', 'High-grade fever', 'Minor laceration', 'Chest tightness',
    'Severe allergic reaction', 'Suspected fracture', 'Persistent cough', 'Dehydration',
    'Hypertensive crisis', 'Post-operative review'];
  const WALKIN_DEPT = ['General Medicine', 'Emergency Medicine', 'Orthopaedics', 'Cardiology',
    'Pulmonology', 'Neurology', 'Pediatrics', 'Maternity'];

  class Hospital {
    constructor() {
      this.wards = WARD_LAYOUT.map(([name, r, c], i) => ({
        index: i, name, rows: r, cols: c, capacity: r * c, occupied: 0
      }));
      this.beds = WARD_LAYOUT.map(([, r, c]) => new SparseMatrix(r, c));
      this.doctors = DOCTORS.map(([name, specialty, qualification], i) => ({
        id: i, name, specialty, qualification, shift: 'Day', busy: false,
        currentPatient: -1, consultStartTick: 0, treatedCount: 0
      }));
      this.patients = new PatientHashTable(128);
      this.index = new SortedIndex();
      this.bst = new NameBst();
      this.pq = new ArrayMaxHeap();
      this.events = [];
      this.nextId = 1001;
      this.tick_ = 0;
      this.seq = 0;
      this.strategy = 'first';
      this.pending = 0;
      this.walkInCounter = 0;
    }

    /* ── logging ── */
    log(type, message) {
      this.events.push(`[${clock(this.tick_)}] ${type} · ${message}`);
      while (this.events.length > 80) this.events.shift();
    }
    shift() {
      const h = Math.floor(((8 * 60 + this.tick_) / 60) % 24);
      return h >= 8 && h < 20 ? 'Day' : 'Night';
    }

    /* ── registration ── */
    register(name, age, gender, phone, severity, condition, department) {
      const p = {
        id: this.nextId++,
        name: name || 'Unknown',
        age: Math.max(0, Math.min(Number(age) || 0, 130)),
        gender: gender || 'Other',
        phone: phone || '',
        severity: SEV_WEIGHT[severity] ? severity : 'standard',
        condition: condition || 'General observation',
        department: department || 'General Medicine',
        arrivalTick: this.tick_,
        arrivalReal: clock(this.tick_),
        status: 'waiting',
        wardIndex: -1, bedRow: -1, bedCol: -1, doctorId: -1,
        waitTicks: 0, treatedTick: 0
      };
      this.patients.put(p.id, p);
      this.index.insert(p.id, p.name);
      this.bst.insert(p.name, p.id);
      this.pq.push({
        patientId: p.id, weight: SEV_WEIGHT[p.severity],
        arrivalTick: p.arrivalTick, seq: this.seq++, severity: p.severity
      });
      this.log('REGISTER', `Patient #${p.id} ${p.name} triaged as ${SEV_NAME[p.severity]}`);
      return p.id;
    }

    get(id) { return this.patients.get(id); }

    /* ── search: hash → binary search, or BST → prefix binary search ── */
    search(query) {
      const q = (query || '').trim();
      const t0 = performance.now();
      const out = { results: [], algorithm: 'empty query', probes: 0, microseconds: 0, viaHash: false };
      if (!q) return out;

      if (/^\d+$/.test(q)) {
        out.viaHash = true;
        out.algorithm = 'Hash table O(1) → binary search O(log n) verification';
        const hit = this.patients.get(Number(q));
        if (hit) {
          const { pair, probes } = this.index.binarySearchId(hit.id);
          out.probes = probes;
          if (pair) out.results.push(hit);
        } else {
          out.probes = 1;
        }
      } else {
        out.algorithm = 'BST exact match O(log n) → binary-search prefix scan O(log n)';
        const bst = this.bst.find(q);
        out.probes += bst.comparisons;
        if (bst.id > 0) {
          const p = this.patients.get(bst.id);
          if (p) out.results.push(p);
        }
        const { hits, probes } = this.index.searchNamePrefix(q);
        hits.forEach((id) => {
          const p = this.patients.get(id);
          if (p && !out.results.some((r) => r.id === p.id)) out.results.push(p);
        });
        out.probes += probes;
      }
      out.microseconds = performance.now() - t0;
      return out;
    }

    /* ── triage ── */
    pickDoctor(p) {
      const dep = (p.department || '').toLowerCase();
      const bySpecialty = this.doctors.find((d) => !d.busy && d.specialty.toLowerCase().includes(dep));
      if (bySpecialty) return bySpecialty.id;
      const anyFree = this.doctors.find((d) => !d.busy);
      return anyFree ? anyFree.id : -1;
    }

    autoCallNext() {
      if (!this.pq.size) return false;
      const skipped = [];
      let called = false;
      while (this.pq.size) {
        const e = this.pq.pop();
        const p = this.patients.get(e.patientId);
        if (!p || p.status !== 'waiting') continue;
        const doc = this.pickDoctor(p);
        if (doc < 0) { skipped.push(e); continue; }
        p.status = 'admitted';
        p.waitTicks = this.tick_ - p.arrivalTick;
        p.doctorId = doc;
        const d = this.doctors[doc];
        d.busy = true; d.currentPatient = p.id; d.consultStartTick = this.tick_; d.treatedCount++;
        this.log('CONSULT', `Dr. ${d.name} called patient #${p.id} (${SEV_NAME[p.severity]})`);
        called = true;
        break;
      }
      skipped.forEach((e) => this.pq.push(e));
      return called;
    }

    callNext() {
      const skipped = [];
      let called = false;
      while (this.pq.size) {
        const e = this.pq.pop();
        const p = this.patients.get(e.patientId);
        if (!p || p.status !== 'waiting') continue;
        const doc = this.pickDoctor(p);
        if (doc < 0) { skipped.push(e); continue; }
        p.status = 'admitted';
        p.waitTicks = this.tick_ - p.arrivalTick;
        p.doctorId = doc;
        const d = this.doctors[doc];
        d.busy = true; d.currentPatient = p.id; d.consultStartTick = this.tick_; d.treatedCount++;
        this.log('CONSULT', `Dr. ${d.name} called patient #${p.id} (${SEV_NAME[p.severity]} · ${p.waitTicks} min wait)`);
        called = true;
        break;
      }
      skipped.forEach((e) => this.pq.push(e));
      return called;
    }

    escalate(id, severity) {
      const p = this.patients.get(id);
      if (!p || p.status !== 'waiting') return false;
      if (SEV_WEIGHT[severity] <= SEV_WEIGHT[p.severity]) return false;
      this.pq.removeById(id);
      p.severity = severity;
      this.pq.push({ patientId: id, weight: SEV_WEIGHT[severity], arrivalTick: p.arrivalTick, seq: this.seq++, severity });
      this.log('ESCALATE', `Patient #${id} ${p.name} re-triaged to ${SEV_NAME[severity]}`);
      return true;
    }

    complete(id) {
      const p = this.patients.get(id);
      if (!p) return false;
      if (p.doctorId >= 0 && this.doctors[p.doctorId].currentPatient === id) {
        this.doctors[p.doctorId].busy = false;
        this.doctors[p.doctorId].currentPatient = -1;
      }
      p.status = 'treated';
      p.doctorId = -1;
      this.log('TREATED', `Patient #${id} ${p.name} completed consultation`);
      return true;
    }

    remove(id) {
      const p = this.patients.get(id);
      if (!p) return false;
      if (p.bedRow >= 0 && p.wardIndex >= 0) this.beds[p.wardIndex].clearCell(p.bedRow, p.bedCol);
      if (p.doctorId >= 0 && this.doctors[p.doctorId].currentPatient === id) {
        this.doctors[p.doctorId].busy = false;
        this.doctors[p.doctorId].currentPatient = -1;
      }
      this.pq.removeById(id);
      this.index.erase(id);
      this.patients.remove(id);
      this.log('DELETE', `Patient #${id} removed from registry`);
      return true;
    }

    /* ── beds ── */
    autoWard(p) {
      if (p.severity === 'critical') return 0;
      if (p.severity === 'emergency') return 1;
      if (p.age > 0 && p.age < 14) return 4;
      if ((p.department || '').toLowerCase() === 'maternity') return 5;
      let best = 2, bestFree = -1;
      for (let i = 2; i <= 3; i++) {
        const free = this.wards[i].capacity - this.beds[i].nonZero();
        if (free > bestFree) { bestFree = free; best = i; }
      }
      return best;
    }

    admit(id, wardIndex, row, col) {
      const p = this.patients.get(id);
      if (!p) return false;
      let wi = Number.isFinite(wardIndex) && wardIndex >= 0 && wardIndex < this.wards.length ? wardIndex : this.autoWard(p);
      const m = this.beds[wi];
      if (!(row >= 0) || !(col >= 0)) {
        const alloc = m.allocate(this.strategy);
        if (!alloc.found) {
          this.log('FULL', `${this.wards[wi].name} is full — patient #${id} not admitted`);
          return false;
        }
        row = alloc.row; col = alloc.col;
      }
      if (m.occupied(row, col)) return false;
      if (p.bedRow >= 0 && p.wardIndex >= 0) this.beds[p.wardIndex].clearCell(p.bedRow, p.bedCol);
      m.set(row, col, id);
      p.wardIndex = wi; p.bedRow = row; p.bedCol = col;
      if (p.status === 'waiting') { this.pq.removeById(id); p.waitTicks = this.tick_ - p.arrivalTick; }
      p.status = 'admitted';
      this.wards[wi].occupied = m.nonZero();
      this.log('ADMIT', `Patient #${p.id} ${p.name} → ${this.wards[wi].name} bed [${row},${col}]`);
      return true;
    }

    dischargeBed(id) {
      const p = this.patients.get(id);
      if (!p || p.bedRow < 0) return false;
      if (p.wardIndex >= 0) {
        this.beds[p.wardIndex].clearCell(p.bedRow, p.bedCol);
        this.wards[p.wardIndex].occupied = this.beds[p.wardIndex].nonZero();
      }
      p.bedRow = -1; p.bedCol = -1; p.wardIndex = -1;
      this.log('BED-FREE', `Bed released for patient #${id}`);
      return true;
    }

    setStrategy(s) {
      if (!['first', 'column', 'spread'].includes(s)) return false;
      this.strategy = s;
      return true;
    }

    /* ── simulation ── */
    spawnArrival() {
      const i = this.walkInCounter++;
      const sev = ['critical', 'emergency', 'urgent', 'standard'][i % 4];
      const id = this.register(
        WALKIN_NAMES[i % WALKIN_NAMES.length],
        18 + ((i * 13) % 70),
        i % 2 === 0 ? 'Male' : 'Female',
        '98220 2' + (1000 + i),
        sev,
        WALKIN_COND[(i * 7 + 3) % WALKIN_COND.length],
        WALKIN_DEPT[(i * 5 + 1) % WALKIN_DEPT.length]
      );
      const p = this.patients.get(id);
      if (p) this.log('ARRIVAL', `Walk-in patient #${id} ${p.name} (${SEV_NAME[sev]})`);
    }

    tick(minutes = 1, autoArrivals = false, autoCall = true) {
      for (let i = 0; i < minutes; i++) {
        this.tick_++;
        if (autoArrivals && this.tick_ % 3 === 0 && this.pending > 0) {
          this.spawnArrival();
          this.pending--;
        }
        this.doctors.forEach((d) => {
          if (!d.busy || d.currentPatient < 0) return;
          const p = this.patients.get(d.currentPatient);
          if (!p) { d.busy = false; d.currentPatient = -1; return; }
          const stay = 14 - 2 * SEV_WEIGHT[p.severity];
          if (this.tick_ - d.consultStartTick >= stay) {
            d.busy = false;
            p.status = 'treated';
            d.currentPatient = -1;
            this.log('TREATED', `Patient #${p.id} ${p.name} released after consultation`);
          }
        });
        if (autoCall) this.autoCallNext();
      }
    }

    startMassCasualty(count) {
      this.pending += Math.max(0, count);
      this.log('ALERT', `Mass-casualty intake started: ${count} incoming patients queued`);
    }

    /* ── snapshot, mirroring jsonState() in main.cpp ── */
    snapshot() {
      const patients = this.patients.values().sort((a, b) => a.id - b.id);
      this.wards.forEach((w, i) => { w.occupied = this.beds[i].nonZero(); });

      const byId = new Map(patients.map((p) => [p.id, p]));
      const heap = this.pq.snapshot();

      const stats = {
        totalPatients: patients.length,
        waiting: patients.filter((p) => p.status === 'waiting').length,
        admitted: patients.filter((p) => p.status === 'admitted').length,
        treated: patients.filter((p) => p.status === 'treated').length,
        criticalWaiting: patients.filter((p) => p.status === 'waiting' && p.severity === 'critical').length,
        doctors: this.doctors.length,
        doctorsBusy: this.doctors.filter((d) => d.busy).length,
        bedsTotal: this.wards.reduce((a, w) => a + w.capacity, 0),
        bedsOccupied: this.wards.reduce((a, w) => a + w.occupied, 0),
        bedDensity: 0,
        queueSize: heap.length,
        hashLoadFactor: this.patients.loadFactor() * 100,
        hashBuckets: this.patients.buckets.length,
        longestChain: this.patients.longestChain(),
        heapHeight: this.pq.height(),
        bstHeight: this.bst.height(),
        avgWaitMinutes: 0,
        shift: this.shift(),
        clock: clock(this.tick_)
      };
      stats.bedDensity = stats.bedsTotal ? (100 * stats.bedsOccupied) / stats.bedsTotal : 0;
      const waits = patients.filter((p) => p.waitTicks > 0);
      stats.avgWaitMinutes = waits.length ? waits.reduce((a, p) => a + p.waitTicks, 0) / waits.length : 0;

      // service order: pop a copy of the heap
      const copy = new ArrayMaxHeap();
      heap.forEach((e) => copy.push(e));
      const serviceOrder = [];
      while (copy.size) {
        const e = copy.pop();
        const p = byId.get(e.patientId);
        serviceOrder.push({
          id: e.patientId, name: p ? p.name : '?',
          severity: p ? p.severity : 'standard', weight: e.weight
        });
      }

      return {
        generatedAt: nowString(),
        stats,
        patients: patients.map((p) => ({
          id: p.id, name: p.name, age: p.age, gender: p.gender, phone: p.phone,
          severity: p.severity, severityLabel: SEV_NAME[p.severity], weight: SEV_WEIGHT[p.severity],
          status: p.status,
          statusLabel: p.status === 'admitted' ? 'Admitted' : p.status === 'treated' ? 'Treated' : 'Waiting',
          condition: p.condition, department: p.department, arrival: p.arrivalReal,
          arrivalTick: p.arrivalTick, waitMinutes: p.waitTicks,
          ward: p.wardIndex >= 0 ? this.wards[p.wardIndex].name : null,
          bedRow: p.bedRow, bedCol: p.bedCol,
          doctor: p.doctorId >= 0 ? this.doctors[p.doctorId].name : null
        })),
        doctors: this.doctors.map((d) => ({
          id: d.id, name: d.name, specialty: d.specialty, qualification: d.qualification,
          shift: d.shift, busy: d.busy, currentPatient: d.busy ? d.currentPatient : null,
          treated: d.treatedCount
        })),
        wards: this.wards.map((w, i) => ({
          index: w.index, name: w.name, rows: w.rows, cols: w.cols,
          capacity: w.capacity, occupied: w.occupied, free: w.capacity - w.occupied,
          occupancy: w.capacity ? (100 * w.occupied) / w.capacity : 0,
          denseBytes: w.capacity * 4,
          sparseBytes: this.beds[i].nonZero() * 12,
          triplets: this.beds[i].data.map((t) => ({ row: t.row, col: t.col, patient: t.value }))
        })),
        heapArray: heap.map((e) => {
          const p = byId.get(e.patientId);
          return {
            id: e.patientId, weight: e.weight, name: p ? p.name : '?',
            severity: p ? p.severity : 'standard', arrivalTick: e.arrivalTick
          };
        }),
        serviceOrder,
        heapValid: this.pq.validate(),
        allocationStrategy: this.strategy,
        events: this.events.slice()
      };
    }

    /* ── benchmarks ── */
    benchmark() {
      const N = 60000, probes = 6000;
      const SEV4 = ['standard', 'urgent', 'emergency', 'critical'];
      let sink = 0;

      const table = new PatientHashTable(1024);
      const vec = [];
      for (let i = 0; i < N; i++) {
        table.put(i, { id: i, name: 'P' + i });
        vec.push({ id: i });
      }
      let t0 = performance.now();
      for (let i = 0; i < probes; i++) {
        const hit = table.get((i * 7919) % N);
        sink += hit ? hit.id : -1;
      }
      const hashUs = ((performance.now() - t0) * 1000) / probes;

      t0 = performance.now();
      for (let i = 0; i < probes; i++) {
        const target = (i * 7919) % N;
        for (let j = 0; j < vec.length; j++) if (vec[j].id === target) { sink += 1; break; }
      }
      const linearUs = ((performance.now() - t0) * 1000) / probes;

      const sorted = new Array(N);
      for (let i = 0; i < N; i++) sorted[i] = i;
      t0 = performance.now();
      for (let i = 0; i < probes; i++) {
        const target = (i * 7919) % N;
        let lo = 0, hi = N - 1;
        while (lo <= hi) {
          const mid = (lo + hi) >> 1;
          if (sorted[mid] === target) { sink += 1; break; }
          if (sorted[mid] < target) lo = mid + 1; else hi = mid - 1;
        }
      }
      const binaryUs = ((performance.now() - t0) * 1000) / probes;

      const heap = new ArrayMaxHeap();
      t0 = performance.now();
      for (let i = 0; i < N; i++) heap.push({ patientId: i, weight: 1 + (i % 4), arrivalTick: i, seq: i, severity: 'x' });
      const heapUs = ((performance.now() - t0) * 1000) / N;

      const sortedVec = [];
      const vecN = 20000;
      t0 = performance.now();
      for (let i = 0; i < vecN; i++) {
        const e = { patientId: i, weight: 1 + (i % 4), arrivalTick: i, seq: i, severity: 'x' };
        let lo = 0, hi = sortedVec.length;
        while (lo < hi) { const mid = (lo + hi) >> 1; if (higherPriority(sortedVec[mid], e)) lo = mid + 1; else hi = mid; }
        sortedVec.splice(lo, 0, e);
      }
      const vecUs = ((performance.now() - t0) * 1000) / vecN;

      const R = 60, C = 60;
      const m = new SparseMatrix(R, C);
      for (let i = 0; i < (R * C) / 8; i++) m.set(Math.floor(i / C), i % C, 1);
      const denseBytes = R * C * 4;
      const sparseBytes = m.nonZero() * 12;
      sink += m.nonZero();
      window.__benchSink = sink;

      return {
        n: N, probes,
        rows: [
          { operation: 'Patient lookup', detail: `${probes / 1000},000 queries over ${N / 1000},000 records`,
            fast: 'Hash table', fastBigO: 'O(1)', fastValue: hashUs,
            slow: 'Linear scan', slowBigO: 'O(n)', slowValue: linearUs, unit: 'µs / query' },
          { operation: 'Search in sorted register', detail: `binary vs linear over ${N / 1000},000 ids`,
            fast: 'Binary search', fastBigO: 'O(log n)', fastValue: binaryUs,
            slow: 'Linear scan', slowBigO: 'O(n)', slowValue: linearUs, unit: 'µs / query' },
          { operation: 'Triage insertion', detail: `${N / 1000},000 priority arrivals`,
            fast: 'Binary heap', fastBigO: 'O(log n)', fastValue: heapUs,
            slow: 'Sorted vector insert', slowBigO: 'O(n)', slowValue: vecUs, unit: 'µs / insert' },
          { operation: 'Ward grid memory', detail: '60 × 60 beds, 12.5% occupied',
            fast: 'Sparse triplets', fastBigO: 'O(nnz)', fastValue: sparseBytes,
            slow: 'Dense grid', slowBigO: 'O(rows × cols)', slowValue: denseBytes, unit: 'bytes' }
        ],
        memory: {
          denseBytes, sparseBytes,
          density: m.density(),
          savedPercent: 100 * (1 - sparseBytes / denseBytes)
        }
      };
    }

    /* ── demo seed, mirroring seedDemo() ── */
    seedDemo() {
      SEEDS.forEach(([n, a, g, ph, sev, cond, dep]) => this.register(n, a, g, ph, sev, cond, dep));
      this.admit(1013, 0, -1, -1);
      this.admit(1014, 0, -1, -1);
      this.admit(1010, 1, -1, -1);
      this.admit(1003, 2, -1, -1);
      this.admit(1006, 2, -1, -1);
      this.admit(1012, 3, -1, -1);
      this.admit(1007, 4, -1, -1);
      this.admit(1009, 5, -1, -1);
      this.tick(3, false, false);
      for (let i = 0; i < 4; i++) this.callNext();
      WAVE.forEach(([n, a, g, ph, sev, cond, dep]) => this.register(n, a, g, ph, sev, cond, dep));
      this.tick(1, false, false);

      INPATIENTS.forEach(([n, a, g, sev, cond, dep], i) => {
        const id = this.register(n, a, g, '98230 4' + (1000 + i), sev, cond, dep);
        this.pq.removeById(id);
        const p = this.patients.get(id);
        if (p) { p.status = 'admitted'; p.waitTicks = 0; }
        this.admit(id, this.autoWard(p), -1, -1);
      });
    }
  }

  /* ─────────────── offline API shim (mirrors server.js + main.cpp) ─────────────── */
  function createEngine() {
    const hospital = new Hospital();
    hospital.seedDemo();

    const json = (data, status = 200) =>
      Promise.resolve({
        ok: status >= 200 && status < 300,
        status,
        json: () => Promise.resolve(data)
      });
    const err = (message, status = 400) => json({ ok: false, error: message }, status);
    const ok = (extra = {}) => json(Object.assign({ ok: true }, extra));

    const readBody = (opts) => {
      if (!opts || !opts.body) return {};
      try { return JSON.parse(opts.body); } catch (e) { return {}; }
    };

    return async function engineFetch(url, opts) {
      const method = (opts && opts.method) || 'GET';
      const u = new URL(url, 'http://local');
      const path = u.pathname;
      const body = readBody(opts);

      if (method === 'GET' && path === '/api/health') {
        return json({ ok: true, service: 'voltiq-hospital-dsa', engine: 'javascript-standalone' });
      }
      if (method === 'GET' && path === '/api/state') return json(hospital.snapshot());

      if (method === 'GET' && path === '/api/search') {
        const q = u.searchParams.get('q') || '';
        const rep = hospital.search(q);
        return json({
          ok: true, query: q, algorithm: rep.algorithm, probes: rep.probes,
          microseconds: rep.microseconds, count: rep.results.length,
          results: rep.results.map((p) => {
            const snap = { wards: hospital.wards, doctors: hospital.doctors };
            return {
              id: p.id, name: p.name, age: p.age, gender: p.gender,
              severity: p.severity, severityLabel: SEV_NAME[p.severity], weight: SEV_WEIGHT[p.severity],
              status: p.status,
              statusLabel: p.status === 'admitted' ? 'Admitted' : p.status === 'treated' ? 'Treated' : 'Waiting',
              condition: p.condition, department: p.department, arrival: p.arrivalReal,
              waitMinutes: p.waitTicks,
              ward: p.wardIndex >= 0 ? snap.wards[p.wardIndex].name : null,
              bedRow: p.bedRow, bedCol: p.bedCol,
              doctor: p.doctorId >= 0 ? snap.doctors[p.doctorId].name : null
            };
          })
        });
      }
      if (method === 'GET' && path === '/api/benchmark') return json(hospital.benchmark());

      if (method === 'POST' && path === '/api/register') {
        if (!body.name) return err('Patient name is required');
        const sev = SEV_WEIGHT[body.severity] ? body.severity : 'standard';
        const id = hospital.register(body.name, body.age, body.gender, body.phone, sev, body.condition, body.department);
        const snap = hospital.snapshot();
        return ok({ id, patient: snap.patients.find((p) => p.id === id) });
      }
      if (method === 'POST' && path === '/api/admit') {
        const okAdmit = hospital.admit(Number(body.id), body.ward != null ? Number(body.ward) : -1,
          body.row != null ? Number(body.row) : -1, body.col != null ? Number(body.col) : -1);
        return okAdmit ? ok() : err('Unable to admit patient — ward may be full or the bed is taken');
      }
      if (method === 'POST' && path === '/api/discharge-bed') {
        return hospital.dischargeBed(Number(body.id)) ? ok() : err('Patient does not occupy a bed');
      }
      if (method === 'POST' && path === '/api/call-next') {
        return hospital.callNext() ? ok() : err('No doctor is free right now');
      }
      if (method === 'POST' && path === '/api/escalate') {
        const sev = SEV_WEIGHT[body.severity] ? body.severity : 'critical';
        return hospital.escalate(Number(body.id), sev)
          ? ok() : err('Patient is not waiting or severity did not increase');
      }
      if (method === 'POST' && path === '/api/complete') {
        return hospital.complete(Number(body.id)) ? ok() : err('Patient not found');
      }
      if (method === 'POST' && path === '/api/delete') {
        return hospital.remove(Number(body.id)) ? ok() : err('Patient not found');
      }
      if (method === 'POST' && path === '/api/tick') {
        const minutes = Math.max(1, Math.min(Number(body.minutes) || 1, 600));
        hospital.tick(minutes, false);
        return ok();
      }
      if (method === 'POST' && path === '/api/strategy') {
        return hospital.setStrategy(body.strategy || 'first') ? ok() : err('Unknown allocation strategy');
      }
      if (method === 'POST' && path === '/api/mass-casualty') {
        const count = Math.max(1, Math.min(Number(body.count) || 10, 60));
        hospital.startMassCasualty(count);
        hospital.tick(1, true);
        return ok({ incoming: count });
      }
      return err('Unknown endpoint ' + path, 404);
    };
  }

  global.VoltIQEngine = { createEngine, Hospital, ArrayMaxHeap, PatientHashTable, SortedIndex, NameBst, SparseMatrix };
})(typeof window !== 'undefined' ? window : globalThis);
