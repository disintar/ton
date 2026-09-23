#!/usr/bin/env python3
"""Exercise the actual C++ HTTP transport against a local server, without chain writes."""
import base64
import http.server
import json
import os
import subprocess
import sys
import threading
import time

requests = []
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append((body, self.headers.get('X-API-Key')))
        mode = self.path.strip('/')
        if mode == 'disconnect':
            self.connection.close()
            return
        if mode == 'timeout': time.sleep(3)
        status = int(mode) if mode.isdigit() else 200
        self.send_response(status)
        self.end_headers()
        if mode == 'malformed': data = b'not json'
        elif mode == 'wronghash': data = b'{"message_hash":"bad"}'
        elif mode == 'large': data = b'x' * 70000
        else: data = json.dumps({'message_hash': base64.b64encode(bytes(32)).decode()}).encode()
        try: self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError): pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
boc = base64.b64encode(b'test signed-message bytes').decode()
env = dict(os.environ, DTON_TONCENTER_API_KEY='test-secret')
try:
    for mode in ['200', '400', '429', '500', 'malformed', 'wronghash', 'large', 'disconnect', 'timeout']:
        before = len(requests)
        start = time.monotonic()
        result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}/{mode}'],
                                input=boc+'\n', text=True, capture_output=True, env=env, timeout=5)
        assert result.returncode == (0 if mode == '200' else 1), (mode, result.stdout, result.stderr)
        assert requests[before] == ({'boc': boc}, 'test-secret'), mode
        assert 'test-secret' not in result.stdout + result.stderr
        assert time.monotonic() - start < 4, mode
        print('PASS', mode)
finally:
    server.shutdown()
