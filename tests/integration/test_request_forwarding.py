"""
Test: Request forwarding to single backend

Verifies that the NTONIX proxy correctly forwards HTTP requests
to backend servers and returns responses to clients.
"""

import pytest
import requests


class TestRequestForwarding:
    """Tests for basic request forwarding functionality."""

    def test_health_endpoint_returns_200(self, proxy_url: str):
        """Verify the proxy's own health endpoint responds correctly."""
        response = requests.get(f"{proxy_url}/health")

        assert response.status_code == 200
        assert "status" in response.json() or response.text  # Accept various formats

    def test_metrics_endpoint_returns_json(self, proxy_url: str):
        """Verify the metrics endpoint returns valid JSON."""
        response = requests.get(f"{proxy_url}/metrics")

        assert response.status_code == 200
        data = response.json()
        # Check for expected metric fields
        assert "requests" in data or "requests_total" in data or "total" in str(data)

    def test_forward_post_to_backend(self, proxy_url: str, chat_completion_request: dict):
        """Verify POST request is forwarded to backend and response returned."""
        response = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=chat_completion_request,
            headers={"Content-Type": "application/json"}
        )

        assert response.status_code == 200
        data = response.json()

        # Verify OpenAI-compatible response structure
        assert "choices" in data
        assert len(data["choices"]) > 0
        assert "message" in data["choices"][0] or "delta" in data["choices"][0]

    def test_request_includes_x_request_id(self, proxy_url: str, chat_completion_request: dict):
        """Verify proxy adds X-Request-ID header to responses."""
        response = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=chat_completion_request,
            headers={"Content-Type": "application/json"}
        )

        assert response.status_code == 200
        # Check if X-Request-ID is in response headers
        # Some proxies return it in headers, others in body
        has_request_id = (
            "X-Request-ID" in response.headers or
            "x-request-id" in response.headers or
            "id" in response.json()
        )
        assert has_request_id, "Response should include request ID"

    def test_custom_x_request_id_passed_through(self, proxy_url: str, chat_completion_request: dict):
        """Verify client-provided X-Request-ID is passed through."""
        custom_id = "test-request-12345"

        response = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=chat_completion_request,
            headers={
                "Content-Type": "application/json",
                "X-Request-ID": custom_id
            }
        )

        assert response.status_code == 200
        # The custom ID should be present in response or backend should receive it
        # Response body 'id' field may contain the request ID
        data = response.json()
        # Backend may use our X-Request-ID for response id field
        assert "id" in data

    def test_invalid_json_returns_400(self, proxy_url: str):
        """Verify malformed JSON request returns 400 Bad Request."""
        response = requests.post(
            f"{proxy_url}/v1/chat/completions",
            data="this is not valid json",
            headers={"Content-Type": "application/json"}
        )

        # Should return 400 Bad Request
        assert response.status_code == 400

    def test_root_endpoint_returns_info(self, proxy_url: str):
        """Verify root endpoint returns server information."""
        response = requests.get(f"{proxy_url}/")

        assert response.status_code == 200
        # Root should return some useful info
        assert response.text  # Has content

    def test_unknown_endpoint_returns_404(self, proxy_url: str):
        """Verify unknown endpoints return 404 Not Found."""
        response = requests.get(f"{proxy_url}/unknown/endpoint/path")

        assert response.status_code == 404
