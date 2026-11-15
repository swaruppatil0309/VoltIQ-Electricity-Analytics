"""Tests for the VoltIQ analytics API.

Run with:  python -m pytest -q      (uses a small synthetic dataset, no downloads)
"""
import os
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import pytest


@pytest.fixture(scope="module")
def client(tmp_path_factory):
    """Build a tiny dataset, point the app at it, and return a test client."""
    data_dir = tmp_path_factory.mktemp("voltiq-data")
    rows = 24 * 60 * 120  # 120 days of 1-minute readings
    timestamps = pd.date_range("2020-01-01", periods=rows, freq="min")
    rng = np.random.default_rng(42)
    frame = pd.DataFrame({
        "Date": timestamps.strftime("%d/%m/%Y"),
        "Time": timestamps.strftime("%H:%M:%S"),
        "Global_active_power": np.round(rng.gamma(2.0, 0.6, rows), 3),
        "Global_reactive_power": np.round(rng.gamma(1.2, 0.1, rows), 3),
        "Voltage": np.round(rng.normal(241, 3, rows), 3),
        "Global_intensity": np.round(rng.gamma(2.0, 2.5, rows), 3),
        "Sub_metering_1": 0.0,
        "Sub_metering_2": 1.0,
        "Sub_metering_3": 17.0,
    })
    # Sprinkle missing values the way the real UCI file does.
    frame["Global_active_power"] = frame["Global_active_power"].astype(object)
    frame.loc[frame.index[::997], "Global_active_power"] = "?"
    txt = data_dir / "household_power_consumption.txt"
    frame.to_csv(txt, sep=";", index=False)

    os.environ["VOLTIQ_DATA_DIR"] = str(data_dir)
    os.environ["VOLTIQ_DATASET"] = str(txt)
    for mod in [m for m in list(sys.modules) if m == "app"]:
        del sys.modules[mod]

    import app as voltiq

    voltiq.get_dataset(force=True)
    voltiq.app.config.update(TESTING=True)
    with voltiq.app.test_client() as test_client:
        yield test_client


def test_index_page(client):
    res = client.get("/")
    assert res.status_code == 200
    assert b"VoltIQ" in res.data
    assert b"chart.umd.js" in res.data


def test_health(client):
    body = client.get("/api/health").get_json()
    assert body["status"] == "ok"
    assert body["rows"] > 0
    assert body["cache_present"] is True


def test_dataset_footprint(client):
    body = client.get("/api/dataset").get_json()
    assert body["rows"] > 100_000
    assert body["cache_bytes"] > 0
    assert body["memory_bytes"] > 0
    assert body["memory_per_row"] < 60          # float32, not float64/objects
    assert body["cache_bytes"] < body["source_bytes"]
    assert body["columns"][0] == "Global_active_power"


def test_analytics_statistics(client):
    body = client.get("/api/analytics").get_json()
    assert body["count"] > 0
    assert body["ci"][0] < body["mean"] < body["ci"][1]
    assert 0 <= body["prob_above_mean"] <= 1
    assert body["min"] <= body["median"] <= body["max"]
    assert body["fences"][0] < body["fences"][1]
    assert 0 <= body["outlier_pct"] <= 100
    assert len(body["hour_profile"]) == 24
    assert len(body["dates"]) == len(body["daily_mean"])
    assert body["dataset_rows"] == body["count"] or body["dataset_rows"] >= body["count"]


def test_analytics_regression(client):
    reg = client.get("/api/analytics").get_json()["regression"]
    assert reg["train_days"] > 0
    assert reg["test_days"] > 0
    assert 0 <= reg["mae"]
    assert 0 <= reg["rmse"]
    assert reg["r2"] is None or -5 <= reg["r2"] <= 1


def test_date_filter_and_grain(client):
    daily = client.get("/api/analytics?start=2020-02-01&end=2020-02-29").get_json()
    assert daily["start_date"] == "2020-02-01"
    assert daily["end_date"] == "2020-02-29"
    assert len(daily["dates"]) == 29
    assert daily["range_rows"] < daily["dataset_rows"]

    monthly = client.get("/api/analytics?grain=monthly").get_json()
    assert monthly["grain"] == "monthly"
    assert len(monthly["dates"]) < len(daily["dates"]) + 5
    assert monthly["daily_min"] is None

    weekly = client.get("/api/analytics?grain=weekly").get_json()
    assert weekly["grain"] == "weekly"


def test_invalid_range_and_date(client):
    empty = client.get("/api/analytics?start=2035-01-01")
    assert empty.status_code == 400
    assert "No readings" in empty.get_json()["error"]

    bad = client.get("/api/analytics?start=not-a-date")
    assert bad.status_code == 400


def test_export_readings_is_streamed_csv(client):
    res = client.get("/api/export?start=2020-01-01&end=2020-01-02")
    assert res.status_code == 200
    assert res.mimetype == "text/csv"
    assert "attachment" in res.headers["Content-Disposition"]
    lines = res.data.decode().strip().splitlines()
    assert lines[0].split(";")[0] == "datetime"
    assert len(lines) > 1000
    assert res.headers["X-Rows-Exported"].isdigit()


def test_export_daily_summary_is_small(client):
    res = client.get("/api/export?level=daily")
    assert res.status_code == 200
    lines = res.data.decode().strip().splitlines()
    assert lines[0].startswith("Date;Mean_kW")
    assert 1 < len(lines) <= 200


def test_json_is_gzipped(client):
    res = client.get("/api/analytics", headers={"Accept-Encoding": "gzip"})
    assert res.headers.get("Content-Encoding") == "gzip"


def test_lock_is_recreated_after_fork():
    """Regression: a lock inherited across fork must not deadlock workers."""
    import os
    import threading

    import app as voltiq

    parent_pid = os.getpid()
    pid = os.fork()
    if pid == 0:  # child: simulate the forked gunicorn worker
        try:
            acquired = voltiq._lock().acquire(timeout=3)
            os._exit(0 if acquired else 1)
        except BaseException:
            os._exit(2)
    else:
        _, status = os.waitpid(pid, 0)
        assert os.getpid() == parent_pid
        assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, "child deadlocked on inherited lock"


def test_dataset_loader_works_in_forked_child():
    """End-to-end guard: a forked worker can still serve analytics."""
    import json
    import os

    pid = os.fork()
    if pid == 0:
        try:
            import app as voltiq

            voltiq.get_dataset()  # would block forever on the inherited lock
            client = voltiq.app.test_client()
            ok = client.get("/api/analytics").status_code == 200
            os._exit(0 if ok else 1)
        except BaseException:
            os._exit(2)
    else:
        _, status = os.waitpid(pid, 0)
        assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0
