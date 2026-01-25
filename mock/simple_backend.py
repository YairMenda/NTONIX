#!/usr/bin/env python3
"""
Simple mock LLM backend for testing NTONIX load balancer.
Returns the port number in response to verify which backend handled the request.
"""

import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
import json

class MockBackendHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/health':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({'status': 'healthy', 'port': self.server.server_port}).encode())
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length) if content_length > 0 else b''

        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()

        response = {
            'id': 'chatcmpl-mock',
            'object': 'chat.completion',
            'backend_port': self.server.server_port,
            'choices': [{
                'index': 0,
                'message': {
                    'role': 'assistant',
                    'content': f'Response from backend on port {self.server.server_port}'
                },
                'finish_reason': 'stop'
            }]
        }
        self.wfile.write(json.dumps(response).encode())

    def log_message(self, format, *args):
        print(f"[Backend:{self.server.server_port}] {args[0]}")

if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8001
    server = HTTPServer(('0.0.0.0', port), MockBackendHandler)
    print(f"Mock backend running on port {port}")
    server.serve_forever()
