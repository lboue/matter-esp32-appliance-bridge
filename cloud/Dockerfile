FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    DATABASE_PATH=/data/st-relay.db \
    PORT=8000

WORKDIR /app
COPY pyproject.toml README.md ./
COPY st_relay ./st_relay
RUN pip install --no-cache-dir . && \
    useradd --system --uid 10001 relay && \
    mkdir /data && chown relay /data

USER relay
VOLUME /data
EXPOSE 8000
CMD ["st-relay"]
