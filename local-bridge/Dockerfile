FROM python:3.12-slim

# libopus is required by the opuslib Python package (STT/TTS Opus codec)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libopus0 \
    libopus-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

# Bridge listens on PORT env var (Fly.io sets this automatically, default 8000)
ENV BRIDGE_PORT=8080

EXPOSE 8080

CMD ["python", "-m", "bridge.server"]
