# VoltIQ - Electricity Analytics
#
# The 127 MiB dataset is baked into the image at build time and converted to the
# compact Parquet cache, so the container starts in <1 s instead of parsing the
# raw text file on every boot.
#
#   docker build -t voltiq .
#   docker run -p 5000:5000 voltiq
#
# Build without internet? Put data/household_power_consumption.txt in the build
# context and run:  docker build --build-arg FETCH_DATA=0 -t voltiq .
FROM python:3.11-slim AS base

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PIP_NO_CACHE_DIR=1 \
    PORT=5000 \
    VOLTIQ_DATA_DIR=/app/data

WORKDIR /app

# System deps kept minimal; pandas/numpy ship manylinux wheels.
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app.py gunicorn.conf.py ./
COPY templates ./templates
COPY static ./static
COPY scripts ./scripts
COPY tests ./tests

# ---- dataset: fetched once, then cached as Parquet inside the image ----------
ARG FETCH_DATA=1
RUN mkdir -p /app/data \
 && if [ "$FETCH_DATA" = "1" ]; then python scripts/fetch_data.py --out-dir /app/data; fi \
 && python app.py build || echo "No dataset available at build time; it will be loaded at runtime."

# Run as an unprivileged user; /app/data must stay writable for the cache.
RUN groupadd --system voltiq && useradd --system --gid voltiq --uid 10001 voltiq \
 && chown -R voltiq:voltiq /app
USER voltiq

EXPOSE 5000

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS "http://127.0.0.1:${PORT}/api/health" || exit 1

# 2 workers x 2 threads; preload_app means the 70 MB dataset is parsed once and
# shared copy-on-write between workers (~200 MB PSS per worker).
CMD ["gunicorn", "-c", "gunicorn.conf.py", "app:app"]
