"""
NTONIX Integration Test Configuration

Pytest fixtures and configuration for integration testing.
Tests are designed to run against a live NTONIX proxy with mock backends.

Usage:
    # Start the proxy stack first:
    docker-compose up -d

    # Run tests:
    pytest tests/integration/ -v

    # Or with custom proxy URL:
    NTONIX_PROXY_URL=http://localhost:8080 pytest tests/integration/
"""

import os
import time
import pytest
import requests
from typing import Generator

# Default URLs - can be overridden via environment variables
DEFAULT_PROXY_URL = "http://localhost:8080"
DEFAULT_PROXY_SSL_URL = "https://localhost:8443"


@pytest.fixture(scope="session")
def proxy_url() -> str:
    """Get the proxy URL from environment or use default."""
    return os.getenv("NTONIX_PROXY_URL", DEFAULT_PROXY_URL)


@pytest.fixture(scope="session")
def proxy_ssl_url() -> str:
    """Get the proxy SSL URL from environment or use default."""
    return os.getenv("NTONIX_PROXY_SSL_URL", DEFAULT_PROXY_SSL_URL)


@pytest.fixture(scope="session", autouse=True)
def wait_for_proxy(proxy_url: str) -> Generator[None, None, None]:
    """
    Wait for the proxy to be healthy before running tests.

    This fixture runs once per test session and ensures the proxy
    is ready to accept connections.
    """
    max_retries = 30
    retry_interval = 1  # seconds

    for i in range(max_retries):
        try:
            response = requests.get(f"{proxy_url}/health", timeout=5)
            if response.status_code == 200:
                print(f"\nProxy is healthy at {proxy_url}")
                yield
                return
        except requests.exceptions.ConnectionError:
            pass
        except requests.exceptions.Timeout:
            pass

        if i < max_retries - 1:
            print(f"Waiting for proxy... ({i + 1}/{max_retries})")
            time.sleep(retry_interval)

    pytest.fail(f"Proxy at {proxy_url} is not responding after {max_retries} retries")


@pytest.fixture
def chat_completion_request() -> dict:
    """Standard chat completion request payload."""
    return {
        "model": "test-model",
        "messages": [
            {"role": "user", "content": "Hello, how are you?"}
        ],
        "stream": False
    }


@pytest.fixture
def streaming_chat_request() -> dict:
    """Streaming chat completion request payload."""
    return {
        "model": "test-model",
        "messages": [
            {"role": "user", "content": "Hello, how are you?"}
        ],
        "stream": True
    }


@pytest.fixture
def unique_chat_request() -> dict:
    """
    Generate a unique chat request to ensure cache miss.
    Uses timestamp to make the request unique.
    """
    return {
        "model": "test-model",
        "messages": [
            {"role": "user", "content": f"Unique request at {time.time()}"}
        ],
        "stream": False
    }


@pytest.fixture
def reset_cache(proxy_url: str):
    """
    Fixture to help tests that need a fresh cache state.

    Note: Currently there's no cache reset endpoint, so tests
    needing fresh cache should use unique requests instead.
    """
    # This is a placeholder for potential future cache reset functionality
    yield


def pytest_configure(config):
    """Configure custom pytest markers."""
    config.addinivalue_line(
        "markers", "slow: marks tests as slow (deselect with '-m \"not slow\"')"
    )
    config.addinivalue_line(
        "markers", "ssl: marks tests that require SSL configuration"
    )
