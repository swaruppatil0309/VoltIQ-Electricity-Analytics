from flask import Flask, render_template, request, jsonify, send_file
import pandas as pd
import numpy as np
from pathlib import Path
from io import BytesIO
from sklearn.linear_model import LinearRegression
from sklearn.metrics import mean_absolute_error, mean_squared_error, r2_score
from scipy import stats

app = Flask(__name__)
BASE_DIR = Path(__file__).resolve().parent
DATA_PATH = BASE_DIR / "data" / "household_power_consumption.txt"

def load_data():
    if not DATA_PATH.exists():
        raise FileNotFoundError(
            "Dataset not found. Put household_power_consumption.txt inside the data folder."
        )
    df = pd.read_csv(DATA_PATH, sep=";", low_memory=False, na_values=["?", ""])
    required = {"Date", "Time", "Global_active_power"}
    if not required.issubset(df.columns):
        raise ValueError("Dataset format is not recognized. Expected Date, Time and Global_active_power columns.")
    df["datetime"] = pd.to_datetime(
        df["Date"].astype(str) + " " + df["Time"].astype(str),
        dayfirst=True, errors="coerce"
    )
    numeric_cols = [
        "Global_active_power", "Global_reactive_power", "Voltage",
        "Global_intensity", "Sub_metering_1", "Sub_metering_2", "Sub_metering_3"
    ]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    df = df.dropna(subset=["datetime", "Global_active_power"])
    df = df.sort_values("datetime").reset_index(drop=True)
    return df

def analyze(df):
    power = df["Global_active_power"].dropna()
    if power.empty:
        raise ValueError("No valid power readings in the selected date range.")
    mean = float(power.mean())
    std = float(power.std(ddof=1)) if len(power) > 1 else 0.0
    sem = stats.sem(power) if len(power) > 1 else 0.0
    ci_low, ci_high = stats.t.interval(0.95, len(power)-1, loc=mean, scale=sem) if len(power) > 1 else (mean, mean)
    q1, q3 = power.quantile([0.25, 0.75])
    lower, upper = q1 - 1.5*(q3-q1), q3 + 1.5*(q3-q1)
    outliers = power[(power < lower) | (power > upper)]
    threshold = mean
    daily = df.set_index("datetime")["Global_active_power"].resample("D").mean().dropna()
    regression = {"slope": None, "intercept": None, "mae": None, "rmse": None, "r2": None}
    prediction = []
    if len(daily) >= 3:
        x = np.arange(len(daily)).reshape(-1, 1)
        split = max(2, int(len(x)*0.8))
        model = LinearRegression().fit(x[:split], daily.values[:split])
        pred = model.predict(x[split:])
        actual = daily.values[split:]
        regression = {
            "slope": float(model.coef_[0]),
            "intercept": float(model.intercept_),
            "mae": float(mean_absolute_error(actual, pred)) if len(actual) else 0,
            "rmse": float(np.sqrt(mean_squared_error(actual, pred))) if len(actual) else 0,
            "r2": float(r2_score(actual, pred)) if len(actual) > 1 else None
        }
        future_x = np.arange(len(daily), len(daily)+7).reshape(-1, 1)
        prediction = [round(float(v), 3) for v in model.predict(future_x)]
    by_day = df.set_index("datetime")["Global_active_power"].resample("D").mean().dropna()
    return {
        "count": int(power.count()),
        "mean": round(mean, 4),
        "median": round(float(power.median()), 4),
        "std": round(std, 4),
        "min": round(float(power.min()), 4),
        "max": round(float(power.max()), 4),
        "ci": [round(float(ci_low), 4), round(float(ci_high), 4)],
        "prob_above_mean": round(float((power > threshold).mean()), 4),
        "outlier_count": int(len(outliers)),
        "outlier_pct": round(float(len(outliers)/len(power)*100), 2),
        "dates": [d.strftime("%Y-%m-%d") for d in by_day.index],
        "daily_mean": [round(float(v), 4) for v in by_day.values],
        "regression": regression,
        "forecast": prediction,
        "columns": list(df.columns)
    }

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/analytics")
def analytics():
    try:
        df = load_data()
        start, end = request.args.get("start"), request.args.get("end")
        if start:
            df = df[df["datetime"] >= pd.to_datetime(start)]
        if end:
            df = df[df["datetime"] < pd.to_datetime(end) + pd.Timedelta(days=1)]
        result = analyze(df)
        result["start_date"] = df["datetime"].min().strftime("%Y-%m-%d")
        result["end_date"] = df["datetime"].max().strftime("%Y-%m-%d")
        return jsonify(result)
    except Exception as exc:
        return jsonify({"error": str(exc)}), 400

@app.route("/api/export")
def export():
    try:
        df = load_data()
        start, end = request.args.get("start"), request.args.get("end")
        if start:
            df = df[df["datetime"] >= pd.to_datetime(start)]
        if end:
            df = df[df["datetime"] < pd.to_datetime(end) + pd.Timedelta(days=1)]
        output = BytesIO()
        df.to_csv(output, index=False)
        output.seek(0)
        return send_file(output, mimetype="text/csv", as_attachment=True,
                         download_name="VoltIQ_filtered_consumption.csv")
    except Exception as exc:
        return jsonify({"error": str(exc)}), 400

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5000, debug=True)
