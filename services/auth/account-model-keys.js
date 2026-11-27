// 文件作用：账号服务从本机模型凭据提供者领取账号可用的供应商 Key。
// 此接口只传少量凭据 JSON，不承载模型请求、流式输出或媒体文件。
'use strict';

function createAccountModelKeyIssuer(env, fetcher = fetch) {
  const token = env.MAICHAT_AUTH_SERVICE_TOKEN || '';
  const port = Number(env.MAICHAT_MODEL_CREDENTIAL_PORT || 9200);
  return async function issueAccountModelKeys() {
    if (token.length < 32 || !Number.isInteger(port) || port < 1 || port > 65535)
      throw new Error('Account model key issuer is not configured');
    const response = await fetcher(`http://127.0.0.1:${port}/account-model-keys`, {
      method: 'POST', redirect: 'error',
      headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'account_models' }),
      signal: AbortSignal.timeout(8000)
    });
    if (!response.ok) throw new Error('Account model key issuer rejected the request');
    const raw = await response.text();
    // 五个各 4096 字符的 Key 还会产生 UTF-8 与 JSON 转义开销；保持整包有界。
    if (Buffer.byteLength(raw) > 65_536)
      throw new Error('Account model key response is too large');
    const keys = JSON.parse(raw).api_keys;
    // 旧凭据进程没有新增供应商字段，视为未配置；显式 null 或其他类型仍须拒绝。
    const openrouter = keys?.openrouter === undefined ? '' : keys.openrouter;
    const openai = keys?.openai === undefined ? '' : keys.openai;
    if (!keys || typeof keys !== 'object' || Array.isArray(keys) ||
        ![keys.glm, keys.deepseek, keys.minimax, openrouter, openai].every(key =>
          typeof key === 'string' && key.length <= 4096 && !/[\x00-\x1f\x7f]/.test(key)) ||
        (!keys.glm && !keys.deepseek && !keys.minimax && !openrouter && !openai))
      throw new Error('Account model key response is invalid');
    return { api_keys: { glm: keys.glm, deepseek: keys.deepseek,
      minimax: keys.minimax, openrouter, openai } };
  };
}

module.exports = { createAccountModelKeyIssuer };
