"""Gunicorn config for VoltIQ.

    gunicorn -c gunicorn.conf.py app:app

Sizing notes for the 2.05 M-row dataset (~75 MB float32 in memory per worker):
* `preload_app` loads and caches the dataset once in the master, then forks -
  workers share the read-only pages instead of each parsing 133 MB of text.
* 2 workers x 2 threads keeps RAM well inside small (512 MB-1 GB) containers
  while still handling concurrent dashboard + export requests.
* `timeout` is generous because a full 150 MB CSV download streams for a while.
"""
import multiprocessing
import os

bind = f"0.0.0.0:{os.getenv('PORT', '5000')}"
workers = int(os.getenv("WEB_CONCURRENCY", "2"))
threads = int(os.getenv("WEB_THREADS", "2"))
worker_class = "gthread"
preload_app = True
timeout = int(os.getenv("WEB_TIMEOUT", "180"))
graceful_timeout = 30
keepalive = 5
accesslog = "-"
errorlog = "-"
loglevel = os.getenv("LOG_LEVEL", "info")
max_requests = 2000
max_requests_jitter = 200
