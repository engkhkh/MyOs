import http.server, sys, os
os.chdir(sys.argv[1])
class H(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def do_GET(self):
        if self.path == '/redirect':
            self.send_response(302); self.send_header('Location', '/page2.html'); self.send_header('Content-Length', '0'); self.end_headers(); return
        if self.path == '/chunked':
            self.send_response(200); self.send_header('Content-Type', 'text/html'); self.send_header('Transfer-Encoding', 'chunked'); self.end_headers()
            for part in [b'<title>Chunked</title><h1>Chunked page</h1>', b'<p>This arrived in several ', b'pieces and was put back together.</p>']:
                self.wfile.write(b'%x\r\n' % len(part) + part + b'\r\n')
            self.wfile.write(b'0\r\n\r\n'); return
        self.close_connection = True
        return super().do_GET()
    def log_message(self, f, *a): open(sys.argv[2], 'a').write((f % a) + '\n')
http.server.ThreadingHTTPServer(('127.0.0.1', 80), H).serve_forever()
