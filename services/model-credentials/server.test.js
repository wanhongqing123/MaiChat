// 文件作用：验证模型凭据 HTTP 入口只接受账号服务持有的内部令牌。
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createServer } = require('./server');
const { createHandler } = require('./index');

test('account service receives model keys over loopback only', async () => {
  const token = 'fedcba9876543210fedcba9876543210';
  const server = createServer(createHandler({ env: {
    MAICHAT_AUTH_SERVICE_TOKEN: token,
    MAICHAT_GLM_API_KEY: 'glm-test-key',
    MAICHAT_DEEPSEEK_API_KEY: 'deepseek-test-key',
    MAICHAT_MINIMAX_API_KEY: 'minimax-test-key',
    MAICHAT_OPENROUTER_API_KEY: 'openrouter-test-key',
    MAICHAT_OPENAI_API_KEY: 'openai-test-key'
  } }));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const url = `http://127.0.0.1:${server.address().port}/account-model-keys`;
    const body = JSON.stringify({ action: 'account_models' });
    const denied = await fetch(url, { method: 'POST', body,
      headers: { Authorization: 'Bearer wrong' } });
    assert.equal(denied.status, 401);
    assert.equal(denied.headers.get('cache-control'), 'no-store');
    const issued = await fetch(url, { method: 'POST', body,
      headers: { Authorization: `Bearer ${token}` } });
    assert.equal(issued.status, 200);
    assert.equal(issued.headers.get('cache-control'), 'no-store');
    assert.deepEqual((await issued.json()).api_keys, {
      glm: 'glm-test-key', deepseek: 'deepseek-test-key', minimax: 'minimax-test-key',
      openrouter: 'openrouter-test-key', openai: 'openai-test-key'
    });
    const modelRequest = await fetch(`http://127.0.0.1:${server.address().port}/responses`, {
      method: 'POST', body: '{}', headers: { Authorization: `Bearer ${token}` }
    });
    assert.equal(modelRequest.status, 404);
  } finally {
    await new Promise(resolve => server.close(resolve));
  }
});
