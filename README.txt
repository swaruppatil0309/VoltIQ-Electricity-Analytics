VoltIQ - Electricity Consumption Analytics
============================================

PROJECT FILES
- app.py                         Flask backend and statistical calculations
- requirements.txt               Python dependencies
- templates/index.html           Dashboard structure
- static/style.css               Dark navy/electric-blue design
- static/app.js                  Interactive charts and filters
- data/                          Put the dataset here

DATASET REQUIRED
Place household_power_consumption.txt inside the data folder:
data/household_power_consumption.txt

RUN ON WINDOWS
1. Extract this ZIP.
2. Open the extracted project folder in VS Code.
3. Open Terminal in that folder.
4. Run:
   py -m pip install -r requirements.txt
5. Run:
   py app.py
6. Open:
   http://127.0.0.1:5000

FEATURES
- Descriptive statistics: mean, median, standard deviation, minimum, maximum
- Empirical probability: P(power reading > sample mean)
- 95% t-confidence interval for the mean
- IQR-based outlier detection
- Linear regression on daily average consumption
- MAE, RMSE and R-squared evaluation
- Seven-day forecast
- Date filters
- Filtered CSV export

NOTES
- The project expects the UCI household power consumption text file, semicolon-separated,
  with Date, Time and Global_active_power columns.
- Internet is required once to load Chart.js and Google Fonts in the dashboard.
- Regression uses a chronological 80/20 split for the daily average series.
