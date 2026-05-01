FROM python:3.11-slim

# libopus0: native Opus codec required by opuslib (STT/TTS audio encoding)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libopus0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY bridge/ bridge/

ENV BRIDGE_HOST=0.0.0.0
ENV BRIDGE_PORT=8080

EXPOSE 8080

CMD ["python", "-m", "bridge.server"]
