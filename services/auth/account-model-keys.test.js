// 文件作用：验证账号服务只经带服务令牌的本机回环读取主聊天 Key。
// 测试使用假 Key，不调用供应商或产生付费模型请求。
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createAccountModelKeyIssuer } = require('./account-model-keys');

test('account service reads only main chat keys over authenticated loopback', async () => {
  const token = 'abcdef0123456789abcdef0123456789';
  let requests = 0;
  const issue = createAccountModelKeyIssuer({ MAICHAT_AUTH_SERVICE_TOKEN: token },
    async (url, options) => {
      ++requests;
      assert.equal(url, 'http://127.0.0.1:9200/account-model-keys');
      assert.equal(options.headers.Authorization, `Bearer ${token}`);
      assert.deepEqual(JSON.parse(options.body), { action: 'account_models' });
      return { ok: true, async text() { return JSON.stringify({
        api_keys: { glm: 'glm-model-key', deepseek: 'deepseek-model-key',
          minimax: 'minimax-model-key' }
      }); } };
    });
  assert.deepEqual(await issue(), {
    api_keys: { glm: 'glm-model-key', deepseek: 'deepseek-model-key',
      minimax: 'minimax-model-key', openrouter: '' }
  });
  assert.equal(requests, 1);
  await assert.rejects(createAccountModelKeyIssuer({})());
});

// 注入的是凭据进程的合成响应，不读取环境文件或联系供应商。
function issuerForRaw(raw) {
  return createAccountModelKeyIssuer({
    MAICHAT_AUTH_SERVICE_TOKEN: 'abcdef0123456789abcdef0123456789'
  }, async () => ({ ok: true, async text() { return raw; } }));
}

test('OpenRouter credentials preserve existing providers and allow OpenRouter alone', async () => {
  for (const keys of [
    { glm: 'glm-key', deepseek: 'deepseek-key', minimax: 'minimax-key', openrouter: 'openrouter-key' },
    { glm: '', deepseek: '', minimax: '', openrouter: 'openrouter-key' },
    { glm: 'glm-key', deepseek: '', minimax: '', openrouter: '' }
  ]) {
    assert.deepEqual(await issuerForRaw(JSON.stringify({ api_keys: keys }))(), { api_keys: keys });
  }
});

test('malformed optional OpenRouter credentials and all-empty responses are rejected', async () => {
  for (const openrouter of [null, 42, {}, [], 'bad\nkey', 'bad\x7fkey', 'x'.repeat(4097)]) {
    await assert.rejects(issuerForRaw(JSON.stringify({ api_keys: {
      glm: 'glm-key', deepseek: '', minimax: '', openrouter
    } }))(), /response is invalid/);
  }
  await assert.rejects(issuerForRaw(JSON.stringify({ api_keys: {
    glm: '', deepseek: '', minimax: '', openrouter: ''
  } }))(), /response is invalid/);
  await assert.rejects(issuerForRaw(JSON.stringify({ api_keys: {
    glm: '', deepseek: '', minimax: ''
  } }))(), /response is invalid/);
});

test('four bounded keys fit with UTF-8 and JSON escaping overhead', async () => {
  for (const character of ['x', '\u20ac', '\\', '"']) {
    const key = character.repeat(4096);
    const keys = { glm: key, deepseek: key, minimax: key, openrouter: key };
    const raw = JSON.stringify({ api_keys: keys });
    assert.ok(Buffer.byteLength(raw) > 16_384);
    assert.ok(Buffer.byteLength(raw) < 65_536);
    assert.deepEqual(await issuerForRaw(raw)(), { api_keys: keys });
  }
});

test('account model key responses have a 64 KiB raw byte limit', async () => {
  const keys = { glm: '', deepseek: '', minimax: '', openrouter: 'openrouter-key' };
  const raw = JSON.stringify({ api_keys: keys });
  const atLimit = raw + ' '.repeat(65_536 - Buffer.byteLength(raw));
  assert.deepEqual(await issuerForRaw(atLimit)(), { api_keys: keys });
  await assert.rejects(issuerForRaw(atLimit + ' ')(), /response is too large/);
});
