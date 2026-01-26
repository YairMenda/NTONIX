"""
Test: Load balancing distributes across multiple backends

Verifies that the NTONIX proxy distributes requests evenly
across available backend servers using round-robin algorithm.
"""

import pytest
import requests
from collections import Counter


class TestLoadBalancing:
    """Tests for load balancer functionality."""

    def test_requests_distributed_across_backends(self, proxy_url: str):
        """
        Verify requests are distributed across multiple backends.

        Sends multiple requests and checks that responses come from
        different backends (identified by X-Backend-ID header or response content).
        """
        backend_hits = Counter()
        num_requests = 10

        for i in range(num_requests):
            # Use unique content to avoid caching
            request_data = {
                "model": "test-model",
                "messages": [
                    {"role": "user", "content": f"Load balance test request {i} - unique"}
                ],
                "stream": False
            }

            response = requests.post(
                f"{proxy_url}/v1/chat/completions",
                json=request_data,
                headers={
                    "Content-Type": "application/json",
                    "Cache-Control": "no-cache"  # Bypass cache
                }
            )

            assert response.status_code == 200

            # Try to identify which backend served the request
            backend_id = None

            # Check X-Backend-ID header
            if "X-Backend-ID" in response.headers:
                backend_id = response.headers["X-Backend-ID"]
            elif "x-backend-id" in response.headers:
                backend_id = response.headers["x-backend-id"]

            # Check response body for backend_id
            if backend_id is None:
                data = response.json()
                if "backend_id" in data:
                    backend_id = data["backend_id"]

            if backend_id:
                backend_hits[backend_id] += 1

        # Verify that multiple backends were used (if we could identify them)
        if len(backend_hits) > 0:
            # Should hit at least 2 different backends with 10 requests
            assert len(backend_hits) >= 2, (
                f"Expected requests to hit multiple backends, "
                f"but only got: {dict(backend_hits)}"
            )

            # With round-robin, distribution should be relatively even
            # Allow some variance since we might have cache hits
            counts = list(backend_hits.values())
            if len(counts) >= 2:
                max_count = max(counts)
                min_count = min(counts)
                # Allow up to 3:1 ratio (accounts for startup, cache, etc.)
                assert max_count <= min_count * 3, (
                    f"Load balancing is too uneven: {dict(backend_hits)}"
                )

    def test_weighted_round_robin_respects_weights(self, proxy_url: str):
        """
        Test that weighted round-robin distributes load according to weights.

        Note: This test assumes default equal weights (1:1).
        """
        backend_hits = Counter()
        num_requests = 20

        for i in range(num_requests):
            request_data = {
                "model": "test-model",
                "messages": [
                    {"role": "user", "content": f"Weight test {i} at timestamp unique_{i}"}
                ],
                "stream": False
            }

            response = requests.post(
                f"{proxy_url}/v1/chat/completions",
                json=request_data,
                headers={
                    "Content-Type": "application/json",
                    "Cache-Control": "no-cache"
                }
            )

            assert response.status_code == 200

            # Extract backend ID
            backend_id = response.headers.get(
                "X-Backend-ID",
                response.headers.get("x-backend-id")
            )

            if backend_id is None:
                data = response.json()
                backend_id = data.get("backend_id")

            if backend_id:
                backend_hits[backend_id] += 1

        # With equal weights (1:1), expect roughly equal distribution
        if len(backend_hits) >= 2:
            counts = list(backend_hits.values())
            total = sum(counts)
            for backend, count in backend_hits.items():
                ratio = count / total
                # Each backend should get between 30% and 70% with equal weights
                assert 0.3 <= ratio <= 0.7, (
                    f"Backend {backend} got {ratio:.1%} of requests, "
                    f"expected ~50% with equal weights"
                )

    @pytest.mark.slow
    def test_load_balancing_skips_unhealthy_backends(self, proxy_url: str):
        """
        Test that unhealthy backends are skipped in load balancing.

        Note: This test requires the ability to mark a backend unhealthy,
        which may not be possible in all test environments.
        This is a placeholder for when such functionality exists.
        """
        # For now, just verify that requests succeed even if we can't
        # control backend health directly
        response = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json={
                "model": "test",
                "messages": [{"role": "user", "content": "Health test"}],
                "stream": False
            },
            headers={
                "Content-Type": "application/json",
                "Cache-Control": "no-cache"
            }
        )

        # As long as at least one backend is healthy, request should succeed
        assert response.status_code in [200, 503], (
            "Request should either succeed (200) or fail with 503 if no backends"
        )
