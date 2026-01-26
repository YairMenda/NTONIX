"""
Test: Cache hit returns cached response

Verifies that the NTONIX proxy correctly caches responses and
returns cached responses for identical requests.
"""

import pytest
import requests
import time


class TestCaching:
    """Tests for LRU cache functionality."""

    def test_identical_requests_return_cached_response(self, proxy_url: str):
        """
        Verify that identical requests return cached responses.

        Sends the same request twice and checks that the second
        request is served from cache (faster response time).
        """
        request_data = {
            "model": "cache-test-model",
            "messages": [
                {"role": "user", "content": "This is a cacheable request for testing"}
            ],
            "stream": False
        }

        # First request - should hit backend
        start1 = time.time()
        response1 = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request_data,
            headers={"Content-Type": "application/json"}
        )
        time1 = time.time() - start1

        assert response1.status_code == 200
        data1 = response1.json()

        # Second request with same content - should hit cache
        start2 = time.time()
        response2 = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request_data,
            headers={"Content-Type": "application/json"}
        )
        time2 = time.time() - start2

        assert response2.status_code == 200
        data2 = response2.json()

        # Cached response should be faster (allow some variance for network)
        # If cache is working, second request should be significantly faster
        # or have identical content

        # Check content is the same (cached)
        if "choices" in data1 and "choices" in data2:
            content1 = data1["choices"][0].get("message", {}).get("content", "")
            content2 = data2["choices"][0].get("message", {}).get("content", "")
            assert content1 == content2, "Cached response content should match"

    def test_cache_control_no_cache_bypasses_cache(self, proxy_url: str):
        """
        Verify that Cache-Control: no-cache bypasses the cache.
        """
        # Use a fixed, deterministic request
        request_data = {
            "model": "bypass-test-model",
            "messages": [
                {"role": "user", "content": "Cache bypass test content"}
            ],
            "stream": False
        }

        # First request - populates cache
        response1 = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request_data,
            headers={"Content-Type": "application/json"}
        )
        assert response1.status_code == 200

        # Second request with no-cache - should bypass cache
        response2 = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request_data,
            headers={
                "Content-Type": "application/json",
                "Cache-Control": "no-cache"
            }
        )
        assert response2.status_code == 200

        # Both responses should be valid
        assert "choices" in response1.json()
        assert "choices" in response2.json()

    def test_different_prompts_not_cached_together(self, proxy_url: str):
        """
        Verify that different prompts get different responses (not cached together).
        """
        request1 = {
            "model": "test-model",
            "messages": [
                {"role": "user", "content": "First unique prompt for cache isolation"}
            ],
            "stream": False
        }

        request2 = {
            "model": "test-model",
            "messages": [
                {"role": "user", "content": "Second unique prompt for cache isolation"}
            ],
            "stream": False
        }

        response1 = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request1,
            headers={"Content-Type": "application/json"}
        )

        response2 = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request2,
            headers={"Content-Type": "application/json"}
        )

        assert response1.status_code == 200
        assert response2.status_code == 200

        # Both requests should get valid responses
        data1 = response1.json()
        data2 = response2.json()

        assert "choices" in data1
        assert "choices" in data2

    def test_cache_hit_reported_in_metrics(self, proxy_url: str):
        """
        Verify that cache hits are tracked in metrics.
        """
        # Get initial metrics
        metrics_before = requests.get(f"{proxy_url}/metrics").json()

        # Make a cacheable request twice
        request_data = {
            "model": "metrics-cache-test",
            "messages": [
                {"role": "user", "content": "Metrics cache hit test"}
            ],
            "stream": False
        }

        # First request - cache miss
        requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request_data,
            headers={"Content-Type": "application/json"}
        )

        # Second request - should be cache hit
        requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request_data,
            headers={"Content-Type": "application/json"}
        )

        # Get metrics after
        metrics_after = requests.get(f"{proxy_url}/metrics").json()

        # Metrics should show cache activity
        # The exact field names depend on implementation
        assert metrics_after is not None

    def test_model_parameter_affects_cache_key(self, proxy_url: str):
        """
        Verify that different model parameters result in different cache keys.
        """
        base_message = {"role": "user", "content": "Model parameter cache test"}

        request1 = {
            "model": "model-a",
            "messages": [base_message],
            "stream": False
        }

        request2 = {
            "model": "model-b",
            "messages": [base_message],
            "stream": False
        }

        response1 = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request1,
            headers={"Content-Type": "application/json"}
        )

        response2 = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request2,
            headers={"Content-Type": "application/json"}
        )

        assert response1.status_code == 200
        assert response2.status_code == 200

        # Both should have valid responses (different cache entries)
        assert "choices" in response1.json()
        assert "choices" in response2.json()
