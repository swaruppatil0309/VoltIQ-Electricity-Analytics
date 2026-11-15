
import os
import zipfile
import shutil

from urllib.request import urlopen
from flask import Flask, render_template, request, jsonify, send_file
import pandas as pd
import numpy as np

from pathlib import Path
from io import BytesIO

from sklearn.linear_model import LinearRegression
from sklearn.metrics import mean_absolute_error, mean_squared_error, r2_score
from scipy import stats


# ==========================================
# 1. FLASK CONFIGURATION
# ==========================================

app = Flask(__name__)

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"

DATA_PATH = DATA_DIR / "household_power_consumption.txt"

DATASET_URL = (
    "https://archive.ics.uci.edu/static/public/235/"
    "individual+household+electric+power+consumption.zip"
)


# ==========================================
# 2. AUTOMATIC DATASET DOWNLOAD
# ==========================================

def download_dataset():

    DATA_DIR.mkdir(parents=True, exist_ok=True)

    # Use existing dataset if already downloaded
    if DATA_PATH.exists() and DATA_PATH.stat().st_size > 0:
        print("Using existing electricity dataset.")
        return

    zip_path = DATA_DIR / "household_power_consumption.zip"
    temp_path = DATA_DIR / "household_power_consumption.tmp"

    print("Downloading electricity dataset from UCI...")

    try:

        # Download ZIP file
        with urlopen(DATASET_URL, timeout=180) as response:
            with open(zip_path, "wb") as file:
                shutil.copyfileobj(response, file)

        print("Download completed. Extracting dataset...")

        # Extract the required TXT file
        with zipfile.ZipFile(zip_path, "r") as archive:

            member = next(
                (
                    name for name in archive.namelist()
                    if Path(name).name == "household_power_consumption.txt"
                ),
                None
            )

            if member is None:
                raise FileNotFoundError(
                    "Dataset file not found inside UCI ZIP archive."
                )

            with archive.open(member) as source:
                with open(temp_path, "wb") as destination:
                    shutil.copyfileobj(source, destination)

        if temp_path.stat().st_size == 0:
            raise ValueError("Downloaded dataset is empty.")

        # Replace the final dataset only after successful extraction
        temp_path.replace(DATA_PATH)

        print("Electricity dataset is ready!")

    except Exception as exc:

        if temp_path.exists():
            temp_path.unlink()

        raise RuntimeError(
            f"Unable to download or extract UCI dataset: {exc}"
        ) from exc

    finally:

        if zip_path.exists():
            zip_path.unlink()


# ==========================================
# 3. LOAD AND CLEAN DATASET
# ==========================================

def load_data():

    download_dataset()

    df = pd.read_csv(
        DATA_PATH,
        sep=";",
        low_memory=False,
        na_values=["?", ""]
    )

    required = {
        "Date",
        "Time",
        "Global_active_power"
    }

    if not required.issubset(df.columns):

        raise ValueError(
            "Dataset format is not recognized. "
            "Expected Date, Time and Global_active_power columns."
        )

    # Convert date and time into datetime
    df["datetime"] = pd.to_datetime(
        df["Date"].astype(str) + " " + df["Time"].astype(str),
        dayfirst=True,
        errors="coerce"
    )

    # Convert numerical columns
    numeric_cols = [
        "Global_active_power",
        "Global_reactive_power",
        "Voltage",
        "Global_intensity",
        "Sub_metering_1",
        "Sub_metering_2",
        "Sub_metering_3"
    ]

    for col in numeric_cols:

        if col in df.columns:

            df[col] = pd.to_numeric(
                df[col],
                errors="coerce"
            )

    # Remove invalid records
    df = df.dropna(
        subset=["datetime", "Global_active_power"]
    )

    # Sort records by datetime
    df = df.sort_values(
        "datetime"
    ).reset_index(drop=True)

    return df


# ==========================================
# 4. STATISTICAL ANALYSIS
# ==========================================

def analyze(df):

    power = df["Global_active_power"].dropna()

    if power.empty:

        raise ValueError(
            "No valid power readings in the selected date range."
        )

    # Descriptive statistics
    mean = float(power.mean())

    std = (
        float(power.std(ddof=1))
        if len(power) > 1 else 0.0
    )

    sem = (
        stats.sem(power)
        if len(power) > 1 else 0.0
    )

    # 95% Confidence Interval
    ci_low, ci_high = (
        stats.t.interval(
            0.95,
            len(power) - 1,
            loc=mean,
            scale=sem
        )
        if len(power) > 1
        else (mean, mean)
    )

    # IQR Outlier Detection
    q1, q3 = power.quantile([0.25, 0.75])

    lower = q1 - 1.5 * (q3 - q1)
    upper = q3 + 1.5 * (q3 - q1)

    outliers = power[
        (power < lower) | (power > upper)
    ]

    # Empirical Probability
    threshold = mean

    prob_above_mean = float(
        (power > threshold).mean()
    )

    # Daily average consumption
    daily = (
        df.set_index("datetime")["Global_active_power"]
        .resample("D")
        .mean()
        .dropna()
    )

    # Regression initialization
    regression = {
        "slope": None,
        "intercept": None,
        "mae": None,
        "rmse": None,
        "r2": None
    }

    prediction = []

    # Linear Regression
    if len(daily) >= 3:

        x = np.arange(
            len(daily)
        ).reshape(-1, 1)

        split = max(
            2,
            int(len(x) * 0.8)
        )

        split = min(split, len(x) - 1)

        model = LinearRegression()

        model.fit(
            x[:split],
            daily.values[:split]
        )

        pred = model.predict(x[split:])

        actual = daily.values[split:]

        regression = {

            "slope": float(model.coef_[0]),

            "intercept": float(model.intercept_),

            "mae": (
                float(mean_absolute_error(actual, pred))
                if len(actual) else 0
            ),

            "rmse": (
                float(np.sqrt(mean_squared_error(actual, pred)))
                if len(actual) else 0
            ),

            "r2": (
                float(r2_score(actual, pred))
                if len(actual) > 1 else None
            )
        }

        # Forecast next 7 days
        future_x = np.arange(
            len(daily),
            len(daily) + 7
        ).reshape(-1, 1)

        prediction = [
            round(float(v), 3)
            for v in model.predict(future_x)
        ]

    # Daily values for charts
    by_day = (
        df.set_index("datetime")["Global_active_power"]
        .resample("D")
        .mean()
        .dropna()
    )

    # Return analytics
    return {

        "count": int(power.count()),

        "mean": round(mean, 4),

        "median": round(float(power.median()), 4),

        "std": round(std, 4),

        "min": round(float(power.min()), 4),

        "max": round(float(power.max()), 4),

        "ci": [
            round(float(ci_low), 4),
            round(float(ci_high), 4)
        ],

        "prob_above_mean": round(
            prob_above_mean, 4
        ),

        "outlier_count": int(len(outliers)),

        "outlier_pct": round(
            float(len(outliers) / len(power) * 100),
            2
        ),

        "dates": [
            d.strftime("%Y-%m-%d")
            for d in by_day.index
        ],

        "daily_mean": [
            round(float(v), 4)
            for v in by_day.values
        ],

        "regression": regression,

        "forecast": prediction,

        "columns": list(df.columns)
    }


# ==========================================
# 5. HOME DASHBOARD ROUTE
# ==========================================

@app.route("/")
def index():

    return render_template("index.html")


# ==========================================
# 6. ANALYTICS API
# ==========================================

@app.route("/api/analytics")
def analytics():

    try:

        df = load_data()

        start = request.args.get("start")
        end = request.args.get("end")

        # Start date filtering
        if start:

            df = df[
                df["datetime"] >= pd.to_datetime(start)
            ]

        # End date filtering
        if end:

            df = df[
                df["datetime"] <
                pd.to_datetime(end) + pd.Timedelta(days=1)
            ]

        # Perform statistical analysis
        result = analyze(df)

        result["start_date"] = (
            df["datetime"].min().strftime("%Y-%m-%d")
        )

        result["end_date"] = (
            df["datetime"].max().strftime("%Y-%m-%d")
        )

        return jsonify(result)

    except Exception as exc:

        return jsonify({
            "error": str(exc)
        }), 400


# ==========================================
# 7. CSV EXPORT API
# ==========================================

@app.route("/api/export")
def export():

    try:

        df = load_data()

        start = request.args.get("start")
        end = request.args.get("end")

        # Apply date filters
        if start:

            df = df[
                df["datetime"] >= pd.to_datetime(start)
            ]

        if end:

            df = df[
                df["datetime"] <
                pd.to_datetime(end) + pd.Timedelta(days=1)
            ]

        # Generate CSV file
        output = BytesIO()

        df.to_csv(
            output,
            index=False
        )

        output.seek(0)

        return send_file(
            output,
            mimetype="text/csv",
            as_attachment=True,
            download_name="VoltIQ_filtered_consumption.csv"
        )

    except Exception as exc:

        return jsonify({
            "error": str(exc)
        }), 400


# ==========================================
# 8. RENDER DEPLOYMENT CONFIGURATION
# ==========================================

if __name__ == "__main__":

    port = int(os.environ.get("PORT", 5000))

    app.run(
        host="0.0.0.0",
        port=port,
        debug=False
    )
