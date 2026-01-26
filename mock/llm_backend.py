"""
NTONIX Mock LLM Backend

A FastAPI server that simulates an OpenAI-compatible LLM API with streaming support.
Used for testing and demonstrating the NTONIX proxy.

Features:
- OpenAI-compatible /v1/chat/completions endpoint
- Server-Sent Events (SSE) streaming responses
- Configurable response delay to simulate inference time
- Health check endpoint for load balancer monitoring
- Unique backend identification for testing load balancing
"""

import asyncio
import json
import os
import time
import uuid
from typing import AsyncGenerator

from fastapi import FastAPI, Request
from fastapi.responses import StreamingResponse, JSONResponse

# Configuration from environment variables
BACKEND_ID = os.getenv("BACKEND_ID", "backend-1")
RESPONSE_DELAY_MS = int(os.getenv("RESPONSE_DELAY_MS", "100"))
TOKEN_DELAY_MS = int(os.getenv("TOKEN_DELAY_MS", "50"))

app = FastAPI(title=f"Mock LLM Backend ({BACKEND_ID})")

# Sample response tokens for streaming
SAMPLE_RESPONSE = [
    "Hello", "!", " ", "I", "'m", " ", "a", " ", "mock", " ",
    "LLM", " ", "backend", " ", "running", " ", "on", " ",
    f"{BACKEND_ID}", ".", " ", "How", " ", "can", " ", "I", " ",
    "help", " ", "you", " ", "today", "?"
]


@app.get("/health")
async def health_check():
    """Health check endpoint for load balancer monitoring."""
    return JSONResponse(
        content={
            "status": "healthy",
            "backend_id": BACKEND_ID,
            "timestamp": time.time()
        },
        status_code=200
    )


@app.get("/")
async def root():
    """Root endpoint with backend info."""
    return {
        "name": "Mock LLM Backend",
        "backend_id": BACKEND_ID,
        "endpoints": [
            "/health",
            "/v1/chat/completions"
        ]
    }


async def generate_streaming_response(
    model: str,
    request_id: str
) -> AsyncGenerator[str, None]:
    """
    Generate SSE streaming response mimicking OpenAI's format.

    Yields Server-Sent Events with partial completion chunks.
    """
    # Initial delay to simulate model loading/processing
    await asyncio.sleep(RESPONSE_DELAY_MS / 1000.0)

    for i, token in enumerate(SAMPLE_RESPONSE):
        # Simulate token generation delay
        await asyncio.sleep(TOKEN_DELAY_MS / 1000.0)

        chunk = {
            "id": request_id,
            "object": "chat.completion.chunk",
            "created": int(time.time()),
            "model": model,
            "choices": [
                {
                    "index": 0,
                    "delta": {
                        "content": token
                    } if i > 0 else {
                        "role": "assistant",
                        "content": token
                    },
                    "finish_reason": None
                }
            ]
        }
        yield f"data: {json.dumps(chunk)}\n\n"

    # Send final chunk with finish_reason
    final_chunk = {
        "id": request_id,
        "object": "chat.completion.chunk",
        "created": int(time.time()),
        "model": model,
        "choices": [
            {
                "index": 0,
                "delta": {},
                "finish_reason": "stop"
            }
        ]
    }
    yield f"data: {json.dumps(final_chunk)}\n\n"
    yield "data: [DONE]\n\n"


def generate_non_streaming_response(
    model: str,
    request_id: str,
    prompt_tokens: int
) -> dict:
    """Generate a complete non-streaming response."""
    content = "".join(SAMPLE_RESPONSE)
    completion_tokens = len(SAMPLE_RESPONSE)

    return {
        "id": request_id,
        "object": "chat.completion",
        "created": int(time.time()),
        "model": model,
        "choices": [
            {
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": content
                },
                "finish_reason": "stop"
            }
        ],
        "usage": {
            "prompt_tokens": prompt_tokens,
            "completion_tokens": completion_tokens,
            "total_tokens": prompt_tokens + completion_tokens
        },
        "backend_id": BACKEND_ID  # Extra field for debugging
    }


@app.post("/v1/chat/completions")
async def chat_completions(request: Request):
    """
    OpenAI-compatible chat completions endpoint.

    Supports both streaming and non-streaming responses based on
    the 'stream' parameter in the request body.
    """
    try:
        body = await request.json()
    except json.JSONDecodeError:
        return JSONResponse(
            content={"error": "Invalid JSON in request body"},
            status_code=400
        )

    # Extract request parameters
    model = body.get("model", "mock-gpt")
    messages = body.get("messages", [])
    stream = body.get("stream", False)

    # Generate request ID (use X-Request-ID if provided by proxy)
    request_id = request.headers.get("X-Request-ID", f"chatcmpl-{uuid.uuid4().hex[:24]}")

    # Estimate prompt tokens (rough approximation)
    prompt_text = " ".join(m.get("content", "") for m in messages)
    prompt_tokens = len(prompt_text.split())

    if stream:
        # Return streaming SSE response
        return StreamingResponse(
            generate_streaming_response(model, request_id),
            media_type="text/event-stream",
            headers={
                "Cache-Control": "no-cache",
                "Connection": "keep-alive",
                "X-Accel-Buffering": "no",  # Disable nginx buffering
                "X-Backend-ID": BACKEND_ID
            }
        )
    else:
        # Simulate processing delay for non-streaming
        await asyncio.sleep(RESPONSE_DELAY_MS / 1000.0)

        return JSONResponse(
            content=generate_non_streaming_response(model, request_id, prompt_tokens),
            headers={"X-Backend-ID": BACKEND_ID}
        )


if __name__ == "__main__":
    import uvicorn

    port = int(os.getenv("PORT", "8001"))
    print(f"Starting Mock LLM Backend ({BACKEND_ID}) on port {port}")
    print(f"Response delay: {RESPONSE_DELAY_MS}ms, Token delay: {TOKEN_DELAY_MS}ms")

    uvicorn.run(app, host="0.0.0.0", port=port)
