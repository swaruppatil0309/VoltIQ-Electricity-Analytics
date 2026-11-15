# ⚡ VoltIQ — Electricity Consumption Analytics

Electricity consumption analytics using **probability, statistics and linear regression**, served as a live
Flask dashboard over the UCI *Individual household electric power consumption* dataset.

> **Heads-up: the dataset is big.** ~2.08 M one-minute readings, a **127 MiB** compressed source file
> (**~2.07 M rows**). It is **not committed to git** and is fetched on demand. The app is built around that
> constraint — see [Large dataset handling](#large-dataset-handling).

---

## Dashboard

| Panel | What it shows |
| --- | --- |
| **Dataset footprint** | Raw file size, valid rows, Parquet cache size and RAM working set — the dataset size made explicit |
| Descriptive statistics | Mean, median, standard deviation, min/max, valid reading count |
| Empirical probability | P(reading > sample mean) |
| 95 % confidence interval | t-based CI for the population mean |
| Outlier detection | IQR fences, outlier count and share |
| Trend analysis | Daily / weekly / monthly mean consumption with min–max band |
| Load shape | Average consumption per hour of day |
| Regression forecast | Chronological 80/20 split, MAE / RMSE / R², slope, 7-day forecast |
| Exports | Streamed 1-minute CSV, or a tiny daily-summary CSV |

Filters: date range plus a hard-coded set of quick ranges (last year / 90 / 30 days) and granularity switching.

---

## Dataset

**Source:** [UCI ML Repository — Individual household electric power consumption](https://archive.ics.uci.edu/dataset/235/individual+household+electric+power+consumption)
(semicolon-separated, `Date;Time;Global_active_power;...`).

```bash
python scripts/fetch_data.py          # downloads into data/ (tries the UCI archive, then mirrors)
python scripts/fetch_data.py --force  # re-download
```

Or place `data/household_power_consumption.txt` (or the `.zip`) manually. The loader accepts either.
Override the location with `VOLTIQ_DATA_DIR=/some/dir` or `VOLTIQ_DATASET=/path/to/file.txt`.

### Size profile (measured on the real file)

| Form | Size | Notes |
| --- | --- | --- |
| Raw `.txt` on disk | **127 MiB** | 2,075,259 lines, 8 columns |
| CSV-equivalent in memory | **145 MiB** | what a naive `pd.read_csv` of the whole frame costs |
| **Parquet cache** (`data/cache/readings-v*.parquet`) | **18.5 MiB** | zstd, float32 — **14.6 %** of the source |
| **RAM working set** | **70 MiB** | 36 B/row, float32 columns only |
| Full readings CSV export | **132 MiB** / 2,049,280 rows | streamed in 100 k-row chunks |

---

## Large dataset handling

1. **Parse once, cache as Parquet.** The raw text is read in 250 k-row chunks with explicit `float32`
   dtypes and only the 8 needed columns (peak RSS ~400 MB, ~7 s, once). Every later boot reads the
   18.5 MiB Parquet cache in **~0.2 s**.
   Rebuild with `python app.py build`, or delete `data/cache/` to force it.
2. **One shared, read-only DataFrame** per process — no per-request reload.
3. **Binary-search date slicing** instead of a 2 M-row boolean mask; month-sized slices answer in ~15 ms.
4. **Streamed exports** — the 132 MiB download is generated in chunks, so it never sits in RAM.
5. **Fork-safe loading** — the dataset is loaded in the gunicorn master *before* workers fork (with
   `preload_app`), so workers share it copy-on-write (~200 MB PSS each rather than 400 MB).
   Warm-up threads are never left running across a fork: a child process would inherit a native
   mutex locked by a thread that no longer exists and deadlock on its first query.
6. **Gzip on JSON** — the 1,433-point daily series compresses ~10× on the wire.

Measured on 2 vCPU / 4 GB: full-range analytics **~180 ms**, one-year slice **~55 ms**, one-month
slice **~15 ms**, `/api/dataset` **~5 ms**.

---

## Run locally

```bash
python -m pip install -r requirements.txt
python scripts/fetch_data.py           # once, ~127 MiB download
python app.py                          # dev server -> http://localhost:5000
```

Production:

```bash
gunicorn -c gunicorn.conf.py app:app    # 2 workers x 2 threads, preloaded dataset
```

```bash
python -m pytest -q                    # 12 tests, synthetic data, no download needed
```

### Environment variables

| Variable | Default | Purpose |
| --- | --- | --- |
| `PORT` | `5000` | HTTP port (also used by gunicorn config) |
| `VOLTIQ_DATA_DIR` | `./data` | Where the dataset + cache live |
| `VOLTIQ_DATASET` | – | Explicit path to a dataset file |
| `WEB_CONCURRENCY` / `WEB_THREADS` | `2` / `2` | Gunicorn sizing |
| `VOLTIQ_PRELOAD` | – | Force synchronous preload outside gunicorn |
| `VOLTIQ_BACKGROUND_WARM` | `0` | Background warm-up (single-process dev only) |

> Keep `preload_app = True` and the synchronous load. Do not enable background warm-up behind a
> forking server — see note 5 above.

---

## Deploy

The 127 MiB dataset is **never committed**. Every deployment path below fetches it once and reuses the
18.5 MiB Parquet cache.

### Docker (recommended)

```bash
docker build -t voltiq .
docker run -p 5000:5000 voltiq
```

The Dockerfile fetches the dataset and builds the cache **at image build time**, runs as an unprivileged
user and ships a `/api/health` healthcheck. Already have the file locally?

```bash
docker build --build-arg FETCH_DATA=0 -t voltiq .   # uses ./data from the build context
```

### Render / Railway / Heroku (Procfile)

`Procfile` → `web: gunicorn -c gunicorn.conf.py app:app`. Add a **build step** that fetches the data:

```bash
python scripts/fetch_data.py --out-dir data   # build command
```

Verified memory footprint: ~200 MB PSS per worker after preload, so a 512 MB instance runs 1 worker
comfortably (`WEB_CONCURRENCY=1`). Bump to 1 GB for 2 workers + a full 132 MiB export in flight.

### Google App Engine

`app.yaml` pins `python311`, one worker with four threads and `VOLTIQ_DATA_DIR=/tmp/voltiq-data`
(instance filesystem is ephemeral, so the cache rebuilds per instance — still only ~7 s once).

### Any VM / systemd

```ini
[Service]
WorkingDirectory=/opt/voltiq
Environment=PORT=5000 VOLTIQ_DATA_DIR=/var/lib/voltiq
ExecStart=/opt/voltiq/.venv/bin/gunicorn -c gunicorn.conf.py app:app
Restart=always
```

Then `nginx` → `proxy_pass http://127.0.0.1:5000;` with `proxy_buffering off;` so the big CSV export
streams straight through.

---

## API

| Endpoint | Description |
| --- | --- |
| `GET /` | Dashboard |
| `GET /api/analytics?start=&end=&grain=daily\|weekly\|monthly` | All statistics for a slice |
| `GET /api/dataset` | Dataset footprint (rows, source/cache/RAM bytes, load time) |
| `GET /api/health` | Readiness probe + loaded row count |
| `GET /api/export?start=&end=` | Streamed 1-minute readings CSV (~132 MiB for the full range) |
| `GET /api/export?level=daily` | Daily summary CSV (~1.4 k rows, 52 KB) |

Errors return `{"error": "..."}` with HTTP 400; JSON responses are gzipped when the client asks for it.

---

## Project structure

```
app.py                  Flask app, cache/loader, statistics, streaming exports
gunicorn.conf.py        Production server config (preload + worker sizing)
scripts/fetch_data.py   Dataset downloader with mirror fallback
templates/index.html    Dashboard markup
static/app.js           Charts (Chart.js 4, vendored) + filters + footprint panel
static/style.css        Dark navy / electric-blue theme
static/vendor/          chart.umd.js (no CDN dependency)
tests/test_app.py       12 API tests on a synthetic dataset
Dockerfile / Procfile / app.yaml
data/                   Dataset + Parquet cache (git-ignored)
```

Chart.js is vendored, so the dashboard works fully offline. Only the Inter webfont is fetched from
Google Fonts, with an `Arial` fallback.

Built with Python, Flask, Pandas, NumPy, SciPy, Scikit-learn and Chart.js.
