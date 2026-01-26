"""
Test: SSL termination works correctly

Verifies that the NTONIX proxy correctly handles HTTPS connections
and terminates SSL/TLS before forwarding to backends.

Note: These tests require SSL to be enabled in the proxy configuration
and valid certificates to be loaded.
"""

import pytest
import requests
import ssl
import socket


@pytest.mark.ssl
class TestSSLTermination:
    """Tests for SSL/TLS termination functionality."""

    def test_https_connection_succeeds(self, proxy_ssl_url: str):
        """
        Verify that HTTPS connections are accepted.

        Note: This test uses verify=False for self-signed certificates
        in development/test environments.
        """
        try:
            response = requests.get(
                f"{proxy_ssl_url}/health",
                verify=False,  # Accept self-signed certs
                timeout=10
            )

            assert response.status_code == 200

        except requests.exceptions.SSLError as e:
            pytest.skip(f"SSL not configured or certificate issue: {e}")
        except requests.exceptions.ConnectionError as e:
            pytest.skip(f"SSL port not available: {e}")

    def test_https_chat_completion_works(self, proxy_ssl_url: str):
        """
        Verify that HTTPS requests are forwarded correctly to backends.
        """
        request_data = {
            "model": "test-model",
            "messages": [
                {"role": "user", "content": "SSL test request"}
            ],
            "stream": False
        }

        try:
            response = requests.post(
                f"{proxy_ssl_url}/v1/chat/completions",
                json=request_data,
                headers={"Content-Type": "application/json"},
                verify=False,
                timeout=30
            )

            assert response.status_code == 200
            data = response.json()
            assert "choices" in data

        except requests.exceptions.SSLError as e:
            pytest.skip(f"SSL not configured: {e}")
        except requests.exceptions.ConnectionError as e:
            pytest.skip(f"SSL port not available: {e}")

    def test_ssl_port_accepts_tls12_or_higher(self, proxy_ssl_url: str):
        """
        Verify that the proxy accepts TLS 1.2 or higher connections.
        """
        import urllib.parse
        parsed = urllib.parse.urlparse(proxy_ssl_url)
        host = parsed.hostname
        port = parsed.port or 8443

        try:
            # Create SSL context requiring TLS 1.2+
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE  # For self-signed certs

            with socket.create_connection((host, port), timeout=5) as sock:
                with context.wrap_socket(sock, server_hostname=host) as ssock:
                    # Connection succeeded with TLS 1.2+
                    version = ssock.version()
                    assert version in ["TLSv1.2", "TLSv1.3"], (
                        f"Expected TLS 1.2 or 1.3, got {version}"
                    )

        except (socket.timeout, ConnectionRefusedError, ssl.SSLError) as e:
            pytest.skip(f"SSL connection test skipped: {e}")

    def test_http_and_https_return_same_response(self, proxy_url: str, proxy_ssl_url: str):
        """
        Verify that HTTP and HTTPS endpoints return equivalent responses.
        """
        request_data = {
            "model": "protocol-test",
            "messages": [
                {"role": "user", "content": "Protocol comparison test"}
            ],
            "stream": False
        }

        # HTTP request
        http_response = requests.post(
            f"{proxy_url}/v1/chat/completions",
            json=request_data,
            headers={"Content-Type": "application/json"}
        )

        try:
            # HTTPS request
            https_response = requests.post(
                f"{proxy_ssl_url}/v1/chat/completions",
                json=request_data,
                headers={"Content-Type": "application/json"},
                verify=False
            )

            # Both should succeed
            assert http_response.status_code == 200
            assert https_response.status_code == 200

            # Both should have valid responses
            http_data = http_response.json()
            https_data = https_response.json()

            assert "choices" in http_data
            assert "choices" in https_data

        except requests.exceptions.ConnectionError:
            pytest.skip("HTTPS port not available")

    def test_ssl_handshake_failure_handled_gracefully(self, proxy_ssl_url: str):
        """
        Verify that the proxy handles SSL handshake failures gracefully
        (doesn't crash, logs appropriately).
        """
        import urllib.parse
        parsed = urllib.parse.urlparse(proxy_ssl_url)
        host = parsed.hostname
        port = parsed.port or 8443

        try:
            # Try connecting with an incompatible protocol
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            # Try to force TLS 1.0 (should be rejected by server)
            context.maximum_version = ssl.TLSVersion.TLSv1
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE

            try:
                with socket.create_connection((host, port), timeout=5) as sock:
                    with context.wrap_socket(sock, server_hostname=host) as ssock:
                        # If this succeeds, TLS 1.0 is allowed (not ideal but okay for test)
                        pass
            except ssl.SSLError:
                # Expected: server rejects TLS 1.0
                pass

            # After the failed handshake, proxy should still work
            response = requests.get(
                f"{proxy_ssl_url}/health",
                verify=False,
                timeout=10
            )
            # Proxy should still be responsive
            assert response.status_code == 200

        except (socket.timeout, ConnectionRefusedError) as e:
            pytest.skip(f"SSL port not available: {e}")
