"""
VoltIQ - Electricity Consumption Analytics
==========================================

Backend for the VoltIQ dashboard. Built to stay fast and memory-lean on the
full UCI "Individual household electric power consumption" dataset:

    2,075,259 raw rows  |  127 MiB source text  |  2,049,280 valid readings

Large-dataset strategy
----------------------
1. The raw text file is parsed once (chunked, float32, only the 8 columns we
   need) and persisted as a compressed Parquet cache inside data/cache/.
2. Every later start reads the 18.5 MiB cache (~0.2 s instead of ~7 s) and keeps
   it in one process-wide, read-only DataFrame that all requests share.
3. Date filters are resolved with a binary search over the sorted timestamp
   column instead of a full 2 M-row boolean mask.
4. CSV exports are streamed chunk-by-chunk so a 132 MiB download never has to
   sit in RAM, and the daily-summary export exists for lightweight downloads.
5. Under gunicorn the dataset loads in the master *before* workers fork, so the
   70 MiB working set is shared copy-on-write (never fork from a warm thread).

Run
---
    python app.py                 # dev server on http://0.0.0.0:5000
    python app.py build           # only (re)build the Parquet cache
    gunicorn -c gunicorn.conf.py app:app
"""
from __future__ import annotations

import gzip
import io
import logging
import os
import sys
import threading
import time
import zipfile
from pathlib import Path

import numpy as np
import pandas as pd
from flask import Flask, Response, jsonify, render_template, request, stream_with_context
from scipy import stats
from sklearn.linear_model import LinearRegression
from sklearn.metrics import mean_absolute_error, mean_squared_error, r2_score

# --------------------------------------------------------------------------- #
# Configuration
# --------------------------------------------------------------------------- #
BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = Path(os.getenv("VOLTIQ_DATA_DIR", BASE_DIR / "data"))
CACHE_DIR = DATA_DIR / "cache"
CACHE_VERSION = 3
CACHE_FILE = CACHE_DIR / f"readings-v{CACHE_VERSION}.parquet"

RAW_TXT = DATA_DIR / "household_power_consumption.txt"
RAW_ZIP = DATA_DIR / "household_power_consumption.zip"
RAW_ANY = (RAW_TXT, RAW_ZIP)

NUMERIC_COLS = [
    "Global_active_power",
    "Global_reactive_power",
    "Voltage",
    "Global_intensity",
    "Sub_metering_1",
    "Sub_metering_2",
    "Sub_metering_3",
]
FLOAT_DTYPES = {col: "float32" for col in NUMERIC_COLS}
DATE_FORMAT = "%d/%m/%Y %H:%M:%S"
CSV_CHUNK_ROWS = 250_000          # rows parsed per chunk from the raw text
EXPORT_CHUNK_ROWS = 100_000       # rows streamed per chunk on download
GRAIN_RULES = {"daily": "D", "weekly": "W-SUN", "monthly": "MS"}

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("voltiq")

app = Flask(__name__)
app.config["JSON_SORT_KEYS"] = False

# --------------------------------------------------------------------------- #
# Dataset cache: one shared, read-only DataFrame per worker process
# --------------------------------------------------------------------------- #
_LOCK = threading.Lock()
_LOCK_PID = os.getpid()
_STATE: dict[str, object] = {"df": None, "meta": {}, "built_at": None}


def _lock() -> threading.Lock:
    """Fork-aware lock.

    gunicorn (or Flask's reloader) forks workers while the background warm-up
    thread may be holding the dataset lock. A plain Lock is *never* released in
    the child - the owning thread does not exist there - so every request would
    block forever. Re-created on first use in a new process; the dataset in the
    forked child is already complete, so re-entering the loader is harmless.
    """
    global _LOCK, _LOCK_PID
    pid = os.getpid()
    if pid != _LOCK_PID:
        _LOCK = threading.Lock()
        _LOCK_PID = pid
    return _LOCK


class DatasetMissing(RuntimeError):
    """Raised when no usable source file exists inside data/."""


def _raw_source() -> Path:
    """Locate the source dataset (.txt preferred, .zip accepted)."""
    override = os.getenv("VOLTIQ_DATASET")
    if override:
        path = Path(override)
        if path.exists():
            return path
        raise DatasetMissing(f"VOLTIQ_DATASET points to a missing file: {path}")
    for candidate in RAW_ANY:
        if candidate.exists():
            return candidate
    raise DatasetMissing(
        "No dataset found. Place household_power_consumption.txt (or .zip) inside "
        f"{DATA_DIR}. See README.md#dataset for download instructions."
    )


def parse_raw(path: Path) -> tuple[pd.DataFrame, dict]:
    """Chunked, memory-lean parse of the raw UCI text/zip file.

    Reading 2 M rows in 250 k-row chunks keeps peak RSS around 400 MB while the
    cache is being built; afterwards the working set is ~75 MB (float32).
    """
    started = time.perf_counter()
    opener = zipfile.ZipFile(path) if path.suffix.lower() == ".zip" else None
    compression = "zip" if opener else None
    try:
        header = pd.read_csv(
            path, sep=";", nrows=0, compression=compression, engine="python"
        ).columns.tolist()
        missing = {"Date", "Time", "Global_active_power"} - set(header)
        if missing:
            raise ValueError(
                "Unrecognised dataset format; missing columns: "
                + ", ".join(sorted(missing))
            )
        present = [c for c in NUMERIC_COLS if c in header]
        usecols = ["Date", "Time"] + present
        dtypes = {**FLOAT_DTYPES, "Date": "string", "Time": "string"}

        frames = []
        reader = pd.read_csv(
            path,
            sep=";",
            usecols=usecols,
            dtype=dtypes,
            na_values=["?", ""],
            chunksize=CSV_CHUNK_ROWS,
            low_memory=False,
            compression=compression,
            engine="c",
        )
        raw_rows = 0
        for chunk in reader:
            raw_rows += len(chunk)
            chunk["datetime"] = pd.to_datetime(
                chunk["Date"] + " " + chunk["Time"],
                format=DATE_FORMAT,
                errors="coerce",
            )
            chunk = chunk.dropna(subset=["datetime", "Global_active_power"])
            frames.append(chunk[["datetime"] + present])
    finally:
        if opener is not None:
            opener.close()

    if not frames:
        raise ValueError("The dataset file contains no readable rows.")

    df = pd.concat(frames, ignore_index=True)
    df = df.sort_values("datetime", kind="stable").reset_index(drop=True)
    for col in present:
        df[col] = df[col].astype("float32")

    meta = {
        "raw_rows": int(raw_rows),
        "valid_rows": int(len(df)),
        "dropped_rows": int(raw_rows - len(df)),
        "columns": present,
        "dropped_pct": round((1 - len(df) / raw_rows) * 100, 2) if raw_rows else 0.0,
        "parse_seconds": round(time.perf_counter() - started, 2),
        "source_name": path.name,
        "source_bytes": path.stat().st_size,
    }
    return df, meta


def build_cache() -> tuple[pd.DataFrame, dict]:
    """Parse the raw dataset and persist a compressed Parquet cache."""
    source = _raw_source()
    log.info("Building Parquet cache from %s", source)
    df, meta = parse_raw(source)

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    tmp = CACHE_FILE.with_suffix(".tmp")
    try:
        df.to_parquet(tmp, compression="zstd", index=False)
        tmp.replace(CACHE_FILE)
        meta["cache_bytes"] = CACHE_FILE.stat().st_size
        meta["cache_written"] = True
    except Exception as exc:  # pyarrow missing / read-only volume
        log.warning("Could not write Parquet cache (%s); serving from memory", exc)
        meta["cache_bytes"] = 0
        meta["cache_written"] = False
    finally:
        if tmp.exists():
            tmp.unlink(missing_ok=True)

    meta["cache_file"] = CACHE_FILE.name
    meta["warm_seconds"] = meta["parse_seconds"]
    meta["cache_hit"] = False
    return df, meta


def load_cache() -> tuple[pd.DataFrame, dict]:
    started = time.perf_counter()
    df = pd.read_parquet(CACHE_FILE)
    df = df.sort_values("datetime", kind="stable").reset_index(drop=True)
    cache_bytes = int(CACHE_FILE.stat().st_size)
    meta = {
        "columns": [c for c in df.columns if c != "datetime"],
        "valid_rows": int(len(df)),
        "raw_rows": int(len(df)),
        "dropped_rows": 0,
        "dropped_pct": 0.0,
        "source_name": CACHE_FILE.name,
        "source_bytes": cache_bytes,
        "cache_bytes": cache_bytes,
        "cache_file": CACHE_FILE.name,
        "cache_written": True,
        "cache_hit": True,
        "warm_seconds": round(time.perf_counter() - started, 3),
    }
    # The raw text/zip file is the real footprint; the cache is only a derivative
    # of it. Report the original size whenever the source is still on disk.
    for raw in RAW_ANY:
        if raw.exists():
            meta["source_name"] = raw.name
            meta["source_bytes"] = int(raw.stat().st_size)
            break
    return df, meta


def get_dataset(force: bool = False) -> tuple[pd.DataFrame, dict]:
    """Return the shared dataset DataFrame, building/loading it exactly once.

    Always prefer the Parquet cache; re-parsing the 133 MB text file only
    happens when the cache is absent (or explicitly forced via `python app.py
    build`). This keeps container restarts to ~0.15 s instead of ~7 s.
    """
    if _STATE["df"] is not None and not force:
        return _STATE["df"], _STATE["meta"]  # type: ignore[return-value]

    with _lock():
        if _STATE["df"] is not None and not force:
            return _STATE["df"], _STATE["meta"]  # type: ignore[return-value]

        df: pd.DataFrame | None = None
        meta: dict = {}
        if CACHE_FILE.exists() and not force:
            try:
                df, meta = load_cache()
                log.info("Loaded %s readings from Parquet cache in %.2fs",
                         f"{meta['valid_rows']:,}", meta["warm_seconds"])
            except Exception as exc:
                log.warning("Parquet cache unreadable (%s); rebuilding", exc)
                df = None
        if df is None:
            df, meta = build_cache()
            log.info("Dataset ready: %s readings (%.1f MB in memory)",
                     f"{len(df):,}", df.memory_usage(deep=True).sum() / 1e6)

        # Read-only dataset shared by every request in this process.
        _STATE["df"] = df
        _STATE["meta"] = {**meta, "loaded_at": time.time()}
        _STATE["built_at"] = time.time()
        return df, _STATE["meta"]  # type: ignore[return-value]


# --------------------------------------------------------------------------- #
# Analytics
# --------------------------------------------------------------------------- #
def slice_bounds(df: pd.DataFrame, start: str | None, end: str | None) -> tuple[int, int]:
    """Resolve an inclusive date range to positions with a binary search."""
    ts = df["datetime"].to_numpy()
    lo, hi = 0, len(df)
    # Both "2009-01-01" and "2009-01-01T05:00" are accepted.
    if start:
        lo = int(np.searchsorted(ts, np.datetime64(pd.to_datetime(start)), side="left"))
    if end:
        edge = np.datetime64(pd.to_datetime(end) + pd.Timedelta(days=1))
        hi = int(np.searchsorted(ts, edge, side="left"))
    return lo, hi


def daily_frame(frame: pd.DataFrame) -> pd.DataFrame:
    """Daily mean / min / max consumption, the basis for every time series."""
    agg = (
        frame.set_index("datetime")["Global_active_power"]
        .resample("D")
        .agg(["mean", "min", "max", "count"])
        .dropna(subset=["mean"])
    )
    return agg


def analyze(frame: pd.DataFrame, grain: str = "daily") -> dict:
    if frame.empty:
        raise ValueError("No valid power readings in the selected date range.")

    power = frame["Global_active_power"].to_numpy(dtype=np.float64)
    n = power.size
    mean = float(power.mean())
    std = float(power.std(ddof=1)) if n > 1 else 0.0

    if n > 1:
        sem = float(std / np.sqrt(n))
        ci_low, ci_high = stats.t.interval(0.95, n - 1, loc=mean, scale=sem)
    else:
        ci_low = ci_high = mean

    q1, q3 = np.percentile(power, [25, 75])
    iqr = q3 - q1
    lower, upper = q1 - 1.5 * iqr, q3 + 1.5 * iqr
    outlier_count = int(np.count_nonzero((power < lower) | (power > upper)))

    daily = daily_frame(frame)
    regression = {"slope": None, "intercept": None, "mae": None, "rmse": None,
                  "r2": None, "train_days": 0, "test_days": 0}
    forecast: list[float] = []

    if len(daily) >= 3:
        x = np.arange(len(daily)).reshape(-1, 1)
        split = max(2, int(len(x) * 0.8))
        model = LinearRegression().fit(x[:split], daily["mean"].to_numpy()[:split])
        pred = model.predict(x[split:])
        actual = daily["mean"].to_numpy()[split:]
        regression = {
            "slope": float(model.coef_[0]),
            "intercept": float(model.intercept_),
            "mae": float(mean_absolute_error(actual, pred)) if len(actual) else 0.0,
            "rmse": float(np.sqrt(mean_squared_error(actual, pred))) if len(actual) else 0.0,
            "r2": float(r2_score(actual, pred)) if len(actual) > 1 else None,
            "train_days": int(split),
            "test_days": int(len(x) - split),
        }
        future_x = np.arange(len(daily), len(daily) + 7).reshape(-1, 1)
        forecast = [round(float(v), 3) for v in model.predict(future_x)]

    # Chart series at the requested granularity (daily by default).
    rule = GRAIN_RULES.get(grain, "D")
    if rule == "D":
        series = daily["mean"]
        series_min = [round(float(v), 3) for v in daily["min"]]
        series_max = [round(float(v), 3) for v in daily["max"]]
    else:
        series = (
            frame.set_index("datetime")["Global_active_power"]
            .resample(rule)
            .mean()
            .dropna()
        )
        series_min = series_max = None

    hour_profile = (
        frame.groupby(frame["datetime"].dt.hour)["Global_active_power"]
        .mean()
        .reindex(range(24))
    )

    return {
        "count": int(n),
        "mean": round(mean, 4),
        "median": round(float(np.median(power)), 4),
        "std": round(std, 4),
        "min": round(float(power.min()), 4),
        "max": round(float(power.max()), 4),
        "ci": [round(float(ci_low), 4), round(float(ci_high), 4)],
        "prob_above_mean": round(float(np.count_nonzero(power > mean) / n), 4),
        "outlier_count": outlier_count,
        "outlier_pct": round(outlier_count / n * 100, 4),
        "iqr": [round(float(q1), 4), round(float(q3), 4)],
        "fences": [round(float(lower), 4), round(float(upper), 4)],
        "grain": grain,
        "dates": [d.strftime("%Y-%m-%d") for d in series.index],
        "daily_mean": [round(float(v), 4) for v in series.to_numpy()],
        "daily_min": series_min,
        "daily_max": series_max,
        "hour_profile": [None if np.isnan(v) else round(float(v), 4)
                         for v in hour_profile.to_numpy()],
        "regression": regression,
        "forecast": forecast,
        "columns": list(frame.columns),
    }


def parse_range() -> tuple[str | None, str | None, str]:
    grain = request.args.get("grain", "daily").lower()
    if grain not in GRAIN_RULES:
        grain = "daily"
    return request.args.get("start") or None, request.args.get("end") or None, grain


# --------------------------------------------------------------------------- #
# Routes
# --------------------------------------------------------------------------- #
@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/analytics")
def analytics():
    try:
        df, meta = get_dataset()
        start, end, grain = parse_range()
        lo, hi = slice_bounds(df, start, end)
        if hi <= lo:
            raise ValueError("No readings inside the selected date range.")
        frame = df.iloc[lo:hi]
        result = analyze(frame, grain)
        result["start_date"] = frame["datetime"].iloc[0].strftime("%Y-%m-%d")
        result["end_date"] = frame["datetime"].iloc[-1].strftime("%Y-%m-%d")
        result["range_rows"] = int(hi - lo)
        result["dataset_rows"] = int(len(df))
        return jsonify(result)
    except DatasetMissing as exc:
        return jsonify({"error": str(exc), "missing_dataset": True}), 400
    except Exception as exc:  # noqa: BLE001 - shown directly on the dashboard
        return jsonify({"error": str(exc)}), 400


@app.route("/api/dataset")
def dataset_info():
    """Footprint of the dataset - the "size is high" panel on the dashboard."""
    try:
        df, meta = get_dataset()
        mem_bytes = int(df.memory_usage(deep=True).sum())
        return jsonify({
            "rows": int(len(df)),
            "columns": [c for c in df.columns if c != "datetime"],
            "first_timestamp": df["datetime"].iloc[0].strftime("%Y-%m-%d %H:%M"),
            "last_timestamp": df["datetime"].iloc[-1].strftime("%Y-%m-%d %H:%M"),
            "source_name": meta.get("source_name"),
            "source_bytes": int(meta.get("source_bytes") or 0),
            "cache_bytes": int(meta.get("cache_bytes") or 0),
            "cache_file": meta.get("cache_file"),
            "cache_hit": bool(meta.get("cache_hit")),
            "memory_bytes": mem_bytes,
            "memory_per_row": round(mem_bytes / len(df), 2) if len(df) else 0,
            "load_seconds": meta.get("warm_seconds"),
            "parse_seconds": meta.get("parse_seconds"),
            "raw_rows": meta.get("raw_rows"),
            "dropped_rows": meta.get("dropped_rows"),
            "dropped_pct": meta.get("dropped_pct"),
            "naive_csv_bytes": int(len(df) * 74),  # ~74 B per row as CSV text
            "raw_text_bytes": next(
                (p.stat().st_size for p in RAW_ANY if p.exists()), 0
            ),
            "cache_ratio_pct": round(
                (meta.get("cache_bytes") or 0) / (meta.get("source_bytes") or 1) * 100, 2
            ),
        })
    except Exception as exc:  # noqa: BLE001
        return jsonify({"error": str(exc)}), 400


@app.route("/api/health")
def health():
    df = _STATE["df"]
    meta = _STATE["meta"]
    return jsonify({
        "status": "ok" if df is not None else "warming",
        "rows": int(len(df)) if df is not None else 0,
        "cache_present": CACHE_FILE.exists(),
        "cache_file": CACHE_FILE.name,
        "loaded_at": meta.get("loaded_at"),
        "warm_seconds": meta.get("warm_seconds"),
    })


def _iter_csv(df: pd.DataFrame, lo: int, hi: int) -> "object":
    """Stream rows as semicolon CSV so 132 MiB never lands in memory at once.

    Works straight off the shared DataFrame (positions lo..hi) - each 100 k-row
    chunk is formatted, yielded and released, so peak extra memory stays ~5 MB.
    """
    yield ";".join(df.columns) + "\n"
    for start in range(lo, hi, EXPORT_CHUNK_ROWS):
        buf = io.StringIO()
        chunk = df.iloc[start:min(start + EXPORT_CHUNK_ROWS, hi)]
        chunk.to_csv(buf, sep=";", index=False, header=False, float_format="%.3f",
                     date_format="%d/%m/%Y %H:%M:%S")
        yield buf.getvalue()


@app.route("/api/export")
def export():
    """Stream the selected slice as CSV.

    ?level=readings (default) -> every 1-minute reading (can be ~150 MB)
    ?level=daily              -> daily summary rows (~1.4 k rows, tiny)
    """
    try:
        df, _ = get_dataset()
        start, end, grain = parse_range()
        level = request.args.get("level", "readings").lower()
        lo, hi = slice_bounds(df, start, end)
        if hi <= lo:
            raise ValueError("No readings inside the selected date range.")
        frame = df.iloc[lo:hi]

        if level == "daily":
            out = daily_frame(frame).reset_index()
            out.columns = ["Date", "Mean_kW", "Min_kW", "Max_kW", "Readings"]
            buf = io.StringIO()
            out.to_csv(buf, index=False, sep=";", float_format="%.4f",
                       date_format="%Y-%m-%d")
            payload = buf.getvalue().encode()
            return Response(
                payload,
                mimetype="text/csv",
                headers={
                    "Content-Disposition":
                        'attachment; filename="VoltIQ_daily_summary.csv"',
                    "Content-Length": str(len(payload)),
                    "X-Rows-Exported": str(len(out)),
                },
            )

        filename = "VoltIQ_readings.csv"
        response = Response(
            stream_with_context(_iter_csv(df, lo, hi)),
            mimetype="text/csv",
            headers={
                "Content-Disposition": f'attachment; filename="{filename}"',
                "X-Rows-Exported": str(hi - lo),
                "X-Estimated-Bytes": str(int((hi - lo) * 74)),
                "Cache-Control": "no-store",
            },
        )
        return response
    except DatasetMissing as exc:
        return jsonify({"error": str(exc), "missing_dataset": True}), 400
    except Exception as exc:  # noqa: BLE001
        return jsonify({"error": str(exc)}), 400


@app.after_request
def compress_json(response: Response) -> Response:
    """Gzip JSON payloads - the daily series is the only chunky response."""
    if (
        response.mimetype == "application/json"
        and response.status_code == 200
        and not response.headers.get("Content-Encoding")
        and "gzip" in request.headers.get("Accept-Encoding", "")
    ):
        data = response.get_data()
        if len(data) > 1024:
            compressed = gzip.compress(data, compresslevel=5)
            response.set_data(compressed)
            response.headers["Content-Encoding"] = "gzip"
            response.headers["Content-Length"] = str(len(compressed))
            response.headers["Vary"] = "Accept-Encoding"
    return response


# --------------------------------------------------------------------------- #
# Startup
# --------------------------------------------------------------------------- #
def _running_under_gunicorn() -> bool:
    """True in gunicorn's master (which will fork) and in its workers."""
    return bool(os.getenv("SERVER_SOFTWARE", "").startswith("gunicorn")) or "gunicorn" in sys.argv[0]


def warm_async() -> None:
    """Warm in the background - only safe where no fork will follow (dev server)."""
    def _run() -> None:
        try:
            get_dataset()
        except Exception as exc:  # noqa: BLE001
            log.warning("Dataset warm-up skipped: %s", exc)

    threading.Thread(target=_run, name="voltiq-warm", daemon=True).start()


# A process must NEVER fork while a thread is inside pandas/pyarrow: the child
# would inherit a native mutex locked by a thread that no longer exists and
# deadlock on its very first query (as gunicorn workers did). So:
#   * gunicorn / plain WSGI import -> load synchronously *before* the first fork
#     (preload_app=True then shares the loaded dataset with every worker)
#   * `python app.py` dev server -> single process, background warm is fine
if _running_under_gunicorn() or os.getenv("VOLTIQ_PRELOAD") == "1":
    try:
        get_dataset()
    except Exception as exc:  # noqa: BLE001 - surfaced on the dashboard instead
        log.warning("Preload skipped: %s", exc)
elif os.getenv("VOLTIQ_BACKGROUND_WARM", "0") == "1":
    warm_async()


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "build":
        df, meta = get_dataset(force=True)
        print(f"Cache ready: {CACHE_FILE} ({meta.get('cache_bytes', 0) / 1e6:.1f} MB)")
        print(f"Rows: {len(df):,}  |  parse {meta.get('parse_seconds')}s")
        return
    port = int(os.getenv("PORT", "5000"))
    debug = os.getenv("FLASK_DEBUG", "0") == "1"
    app.run(host="0.0.0.0", port=port, debug=debug, threaded=True)


if __name__ == "__main__":
    main()
