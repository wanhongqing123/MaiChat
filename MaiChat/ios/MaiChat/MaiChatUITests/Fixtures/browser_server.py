#!/usr/bin/env python3
"""Loopback-only browser UI fixture; no accounts, external websites or model requests."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs
import json
import time


class BrowserFixture(BaseHTTPRequestHandler):
    retry_allowed = False

    def do_GET(self):
        if self.path == "/test/slow-image":
            time.sleep(8)
            self.send_response(204)
            self.end_headers()
            return
        if self.path in {"/form", "/slow-form", "/form-with-loading-image", "/form-timeout"}:
            if self.path == "/slow-form":
                time.sleep(1.2)
            if self.path == "/form-timeout":
                time.sleep(7)
            body = Path(__file__).with_name("httpbin-form.html").read_text()
            # Keep the original controls and form semantics, with readable mobile fixture presentation.
            body = body.replace("<head>", '''<head><meta name="viewport" content="width=device-width,initial-scale=1">
                <title>Form test</title><style>body{font:17px -apple-system;padding:14px;color:#172337}
                input:not([type=checkbox]):not([type=radio]),textarea{font-size:17px;display:block;max-width:95%;padding:5px}
                button{font-size:17px;padding:8px}fieldset{border:1px solid #ccd7e1;border-radius:10px}</style>''')
            extra = '''<label for="file">Upload a file</label><input id="file" type="file">
            <label for="password">Account password</label><input id="password" type="password">
            <span id="aria-name">Accessible first name</span><input name="aria" aria-labelledby="aria-name">
            <label>Locked input<input name="locked" readonly></label>'''
            if self.path == "/form-with-loading-image":
                extra += '<img src="/test/slow-image" alt="Slow resource">'
            self.respond(body.replace("</body>", extra + "</body>").encode())
            return
        if self.path in {"/test/retry-ready", "/test/retry-reset"}:
            type(self).retry_allowed = self.path.endswith("ready")
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"OK")
            return
        if self.path == "/slow":
            time.sleep(20)
        if self.path == "/failure" and not type(self).retry_allowed:
            self.connection.shutdown(2)
            self.connection.close()
            return
        password = '<input aria-label="网页密码" type="password">' if self.path == "/login" else ''
        rows = ''.join(f'<p>浏览内容 {i} · 这段内容使用本机素材，不请求外部服务。</p>' for i in range(20))
        body = f'''<!doctype html><html lang="zh"><head>
        <meta name="viewport" content="width=device-width,initial-scale=1">
        <title>网页任务演示</title><style>
        body{{font-family:-apple-system,sans-serif;margin:0;padding:22px;color:#172337;background:#fff}}
        h1{{font-size:27px;line-height:1.3;margin:14px 0}}p{{font-size:16px;line-height:1.7}}
        .tag{{color:#008bd1;font-size:13px;font-weight:600}}input{{box-sizing:border-box;width:100%;
        border:1px solid #d8e2ea;border-radius:12px;padding:14px;font:17px -apple-system}}
        button{{border:0;border-radius:12px;background:#e8f5ff;color:#007cbd;padding:14px 18px;
        font:17px -apple-system;margin:14px 0}}section{{margin-top:38px}}#result{{color:#526673}}
        </style></head><body><div class="tag">MaiChat · 网页任务</div><h1>网页和对话，各自有空间</h1>
        <p>返回对话后，这里的输入内容和阅读位置都会保留。</p>
        <input aria-label="网页备注" placeholder="写一条备注">
        <button onclick="document.querySelector('#result').textContent='备注已保存：'+document.querySelector('input').value">保存备注</button>
        <p id="result">页面已准备好</p>{password}<section>{rows}</section>
        <h2>已经读到网页末尾</h2><button aria-label="网页末尾按钮" onclick="this.textContent='已确认到达末尾'">确认读完</button>
        </body></html>'''.encode()
        self.respond(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        data = parse_qs(self.rfile.read(min(length, 65536)).decode(), keep_blank_values=True)
        # Only synthetic fixture values are submitted; no external network or accounts.
        self.respond(("<!doctype html><title>Submitted order</title><pre>" +
                      json.dumps({"form": data}, ensure_ascii=True) + "</pre>").encode())

    def respond(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


if __name__ == '__main__':
    ThreadingHTTPServer(('127.0.0.1', 18680), BrowserFixture).serve_forever()
