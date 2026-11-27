# 文件作用：为移动端适配层测试提供本机模型接口响应，不调用付费模型。
# 阅读边界：此文件负责构建或测试流程，不在应用运行时解释为业务逻辑。
"""Local-only deterministic model fixture for iOS/Android UI tests; never a product server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import time

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def event(self, response):
        self.wfile.write(('data: ' + json.dumps(response) + '\n\n').encode())
        self.wfile.flush()

    def completed(self, output):
        self.event({'type': 'response.completed', 'response': {
            'status': 'completed', 'output': output}})

    def do_POST(self):
        if self.path != '/responses':
            self.send_error(404)
            return
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        # Continue after function results without replaying the original tool request.
        item = body['input'][-1]
        content = item.get('content', '')
        prompt = (content if isinstance(content, str) else ''.join(
            part.get('text', '') for part in content if part.get('type') == 'input_text'))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        try:
            if prompt == 'write':
                call = {'call_id': 'call_mobile_write', 'type': 'function_call',
                    'name': 'file_write',
                    'arguments': json.dumps({'path': 'ui-result.txt', 'content': 'approved'})}
                self.completed([call])
                return
            if prompt == 'media cards':
                calls = [{'call_id': 'call_resources', 'type': 'function_call',
                          'name': 'system_resources', 'arguments': '{}'}]
                for index, name in enumerate(('parity-one.mp4', 'parity-two.mp4'), start=1):
                    calls.append({'call_id': 'call_media_' + str(index),
                                  'type': 'function_call',
                                      'name': 'agent_send_media',
                                      'arguments': json.dumps({'file_path': name, 'type': 'video'})})
                self.completed(calls)
                return
            if prompt == 'audio card':
                call = {'call_id': 'call_audio', 'type': 'function_call',
                        'name': 'agent_send_media', 'arguments': json.dumps({
                            'file_path': 'parity-audio.wav', 'type': 'audio'})}
                self.completed([call])
                return
            answer = ('Long message paragraph.\n\n' * 45 + 'Tail marker'
                      if prompt == 'long' else 'Mobile response ready')
            reasoning = 'Checking the mobile interface.'
            self.event({'type': 'response.reasoning_summary_text.delta', 'output_index': 0,
                        'summary_index': 0, 'delta': reasoning})
            time.sleep(3 if prompt == 'stop' else .25)
            for delta in (answer[:len(answer)//2], answer[len(answer)//2:]):
                self.event({'type': 'response.output_text.delta', 'output_index': 1,
                            'content_index': 0, 'delta': delta})
                time.sleep(3 if prompt == 'stop' else .25)
            self.completed([
                {'type': 'reasoning', 'summary': [{'type': 'summary_text', 'text': reasoning}]},
                {'type': 'message', 'role': 'assistant',
                 'content': [{'type': 'output_text', 'text': answer}]}])
        except (BrokenPipeError, ConnectionResetError):
            pass

if __name__ == '__main__':
    port = int(os.environ.get('MAICHAT_TEST_MODEL_PORT', '18189'))
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
