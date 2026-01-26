# Mock LLM Backend Dockerfile
FROM python:3.11-slim

WORKDIR /app

# Install dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy application
COPY llm_backend.py .

# Default environment variables
ENV BACKEND_ID=backend-1
ENV PORT=8001
ENV RESPONSE_DELAY_MS=100
ENV TOKEN_DELAY_MS=50

EXPOSE ${PORT}

# Health check
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://localhost:${PORT}/health')" || exit 1

CMD ["python", "llm_backend.py"]
