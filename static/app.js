let trendChart, forecastChart, hourChart;
const $ = id => document.getElementById(id);
const fmt = (v, digits = 3) => (v === null || v === undefined ? "N/A" : Number(v).toFixed(digits));

function fmtBytes(b) {
  if (!b) return "0 B";
  const u = ["B", "KB", "MB", "GB"];
  let i = 0, n = Number(b);
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i += 1; }
  return `${n.toFixed(n < 10 && i > 0 ? 1 : 0)} ${u[i]}`;
}
const fmtInt = n => Number(n).toLocaleString("en-US");

function queryString() {
  const p = new URLSearchParams();
  if ($("startDate").value) p.set("start", $("startDate").value);
  if ($("endDate").value) p.set("end", $("endDate").value);
  if ($("grain").value !== "daily") p.set("grain", $("grain").value);
  return p.toString();
}

function syncDownloads(qs) {
  const suffix = qs ? `?${qs}` : "";
  $("downloadBtn").href = `/api/export${suffix}`;
  $("downloadDailyBtn").href = `/api/export${qs ? `${suffix}&level=daily` : "?level=daily"}`;
}

const axis = {
  ticks: { color: "#7892aa", maxTicksLimit: 10 },
  grid: { color: "#1b3046" }
};
const baseOptions = extra => ({
  responsive: true,
  maintainAspectRatio: false,
  animation: { duration: 350 },
  interaction: { mode: "index", intersect: false },
  plugins: { legend: { labels: { color: "#a9bfd4", boxWidth: 12 } } },
  scales: { x: { ...axis }, y: { ...axis } },
  ...extra
});

/* ---------------------------------------------------------------- footprint */
async function loadFootprint() {
  try {
    const res = await fetch("/api/dataset");
    const d = await res.json();
    if (!res.ok || d.error) return;
    $("footprint").hidden = false;
    $("heroRows").textContent = fmtInt(d.rows).replace(/,/g, "\u2009");
    $("fpSource").textContent = fmtBytes(d.source_bytes || d.raw_text_bytes);
    $("fpSourceNote").textContent = `${(d.source_name || "dataset").replace(/_/g, " ")} on disk`;
    $("fpRows").textContent = fmtInt(d.rows);
    $("fpRowsNote").textContent = d.dropped_rows
      ? `${fmtInt(d.dropped_rows)} rows (${d.dropped_pct}%) dropped as missing`
      : "valid 1-minute readings";
    $("fpCache").textContent = fmtBytes(d.cache_bytes);
    $("fpCacheNote").textContent = d.cache_file || "parquet";
    $("fpMemory").textContent = fmtBytes(d.memory_bytes);
    $("fpMemoryNote").textContent = `${d.memory_per_row} B per row (float32)`;
    $("fpCacheTag").textContent = d.cache_hit ? "CACHE HIT • FAST START" : "CACHE BUILT";
    $("fpNaive").textContent = fmtBytes(d.naive_csv_bytes);
    const ratio = d.source_bytes ? (d.cache_bytes / d.source_bytes) * 100 : 0;
    $("fpRatio").textContent = `Cache is ${ratio.toFixed(1)}% of the source size`;
    $("fpFill").style.width = `${Math.max(2, Math.min(100, ratio))}%`;
    if (d.load_seconds) {
      $("fpNote").textContent =
        `Parsed once from the raw text, cached as compressed Parquet (${fmtBytes(d.cache_bytes)}), ` +
        `and served from RAM in ${Number(d.load_seconds).toFixed(2)}s. ` +
        `Exports stream in 100k-row chunks so a full download never sits in memory.`;
    }
  } catch (_) { /* footprint panel is optional */ }
}

/* --------------------------------------------------------------- analytics */
async function loadAnalytics() {
  const msg = $("message");
  msg.className = "message";
  msg.textContent = "Calculating analytics…";
  const qs = queryString();
  syncDownloads(qs);
  try {
    const response = await fetch(`/api/analytics${qs ? `?${qs}` : ""}`);
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || "Unable to load data");

    const grainLabel = { daily: "day", weekly: "week", monthly: "month" }[data.grain] || "day";
    const days = data.dates.length;
    msg.textContent =
      `Dataset slice loaded • ${data.start_date} to ${data.end_date} • ` +
      `${fmtInt(data.range_rows)} readings • ${days} ${grainLabel}s • ` +
      `computed in ${Math.round(performance.now() - t0)} ms`;
    msg.dataset.rows = data.range_rows;

    $("mean").textContent = fmt(data.mean);
    $("median").textContent = fmt(data.median);
    $("std").textContent = fmt(data.std);
    $("count").textContent = fmtInt(data.count);
    $("countNote").textContent = data.dataset_rows
      ? `of ${fmtInt(data.dataset_rows)} total readings`
      : "observations";

    $("prob").textContent = (data.prob_above_mean * 100).toFixed(2) + "%";
    $("ciLow").textContent = fmt(data.ci[0]);
    $("ciHigh").textContent = fmt(data.ci[1]);
    $("outliers").textContent = `${fmtInt(data.outlier_count)} (${data.outlier_pct}%)`;
    $("fences").textContent =
      `Fences: ${fmt(data.fences[0])} kW to ${fmt(data.fences[1])} kW.`;
    $("min").textContent = fmt(data.min);
    $("max").textContent = fmt(data.max);
    $("dateRange").textContent =
      `Date range: ${data.start_date} — ${data.end_date}  •  ${days} ${grainLabel}s`;

    if (data.regression && data.regression.slope !== null) {
      $("mae").textContent = fmt(data.regression.mae);
      $("rmse").textContent = fmt(data.regression.rmse);
      $("r2").textContent = fmt(data.regression.r2);
      $("slope").textContent = Number(data.regression.slope).toExponential(2);
      $("split").textContent = `${data.regression.train_days} / ${data.regression.test_days}`;
    } else {
      ["mae", "rmse", "r2", "slope", "split"].forEach(id => { $(id).textContent = "N/A"; });
    }

    const title = { daily: "Daily", weekly: "Weekly", monthly: "Monthly" }[data.grain] || "Daily";
    $("trendTitle").textContent = `${title} average consumption`;
    $("trendTag").textContent = `${days} ${grainLabel.toUpperCase()}S`;
    $("trendNote").textContent = data.daily_min
      ? "Shaded band shows the daily minimum–maximum reading range."
      : "Aggregated mean of all 1-minute readings in each period.";

    drawTrend(data);
    drawForecast(data);
    drawHours(data);
  } catch (err) {
    msg.className = "message error";
    msg.textContent = err.message;
  }
}

/* ------------------------------------------------------------------ charts */
function drawTrend(d) {
  if (trendChart) trendChart.destroy();
  // Degrade gracefully: a 4-year daily series is dense, so thin out point marks.
  const dense = d.dates.length > 400;
  const datasets = [{
    label: "Mean (kW)", data: d.daily_mean, borderColor: "#35baff",
    backgroundColor: "#35baff22", fill: true, tension: 0.28,
    pointRadius: dense ? 0 : 1.5, borderWidth: 1.8
  }];
  if (d.daily_max && d.daily_min) {
    datasets.push(
      { label: "Daily max (kW)", data: d.daily_max, borderColor: "#1c5775",
        borderWidth: 1, pointRadius: 0, fill: false, tension: 0 },
      { label: "Daily min (kW)", data: d.daily_min, borderColor: "#1c5775",
        borderWidth: 1, pointRadius: 0, fill: false, tension: 0 }
    );
  }
  trendChart = new Chart($("trendChart"), {
    type: "line",
    data: { labels: d.dates, datasets },
    options: baseOptions({
      plugins: {
        legend: { labels: { color: "#a9bfd4", boxWidth: 12, filter: i => i.text !== "Daily min (kW)" } },
        tooltip: { callbacks: { title: items => `Date: ${items[0].label}` } }
      }
    })
  });
}

function drawForecast(d) {
  if (forecastChart) forecastChart.destroy();
  const window_ = Math.min(120, d.dates.length);
  const labels = d.dates.slice(-window_);
  const values = d.daily_mean.slice(-window_);
  const future = d.forecast || [];
  const last = d.dates.length ? new Date(d.dates[d.dates.length - 1] + "T00:00:00") : new Date();
  const futureLabels = future.map((_, i) => {
    const x = new Date(last);
    x.setDate(x.getDate() + i + 1);
    return x.toISOString().slice(0, 10);
  });
  forecastChart = new Chart($("forecastChart"), {
    type: "line",
    data: {
      labels: [...labels, ...futureLabels],
      datasets: [
        { label: "Observed mean", data: [...values, ...Array(future.length).fill(null)],
          borderColor: "#35baff", tension: 0.25, pointRadius: window_ > 60 ? 0 : 2 },
        { label: "7-day forecast",
          data: [...Array(Math.max(0, values.length - 1)).fill(null),
                 values[values.length - 1] ?? null, ...future],
          borderColor: "#53e3c2", borderDash: [6, 5], tension: 0.25, pointRadius: 3 }
      ]
    },
    options: baseOptions({})
  });
}

function drawHours(d) {
  if (hourChart) hourChart.destroy();
  hourChart = new Chart($("hourChart"), {
    type: "bar",
    data: {
      labels: Array.from({ length: 24 }, (_, h) => `${String(h).padStart(2, "0")}:00`),
      datasets: [{
        label: "Mean kW", data: d.hour_profile, backgroundColor: "#29a8ffcc",
        borderColor: "#52e0ef", borderWidth: 1, borderRadius: 3
      }]
    },
    options: baseOptions({ plugins: { legend: { display: false } } })
  });
}

/* ------------------------------------------------------------------- wiring */
let t0 = performance.now();
const run = () => { t0 = performance.now(); loadAnalytics(); };

function applyPreset(days) {
  const end = new Date();
  const start = new Date();
  start.setDate(end.getDate() - days);
  $("endDate").value = end.toISOString().slice(0, 10);
  $("startDate").value = start.toISOString().slice(0, 10);
  run();
}

$("applyBtn").addEventListener("click", run);
$("grain").addEventListener("change", run);
$("resetBtn").addEventListener("click", () => {
  $("startDate").value = "";
  $("endDate").value = "";
  $("grain").value = "daily";
  run();
});
document.querySelectorAll("[data-range]").forEach(btn =>
  btn.addEventListener("click", () => applyPreset(Number(btn.dataset.range)))
);

loadFootprint();
run();
