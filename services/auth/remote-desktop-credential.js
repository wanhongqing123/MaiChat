'use strict';

const { createHash } = require('node:crypto');
const { createUserSig } = require('./usersig');

// IM 仍按账号登录。TRTC 按账号+设备派生独立身份，避免同账号两端进同房互踢。
// 身份格式与 Qt/iOS 共用固定测试向量；不可接受客户端自报的任意 UserID。
function remoteDesktopUserId(ownerUserId, deviceId) {
  if (typeof ownerUserId !== 'string' || !/^[A-Za-z0-9_-]{1,32}$/.test(ownerUserId) ||
      typeof deviceId !== 'string' || !/^[A-Za-z0-9_-]{16,128}$/.test(deviceId))
    throw new TypeError('Invalid remote desktop device');
  return 'rd_' + createHash('sha256')
    .update('maichat-remote-v2\0' + ownerUserId + '\0' + deviceId).digest('hex').slice(0, 24);
}

function remoteDesktopCredential(ownerUserId, body, sdkAppId, secret, issuedAt) {
  if (!body || typeof body !== 'object' || Array.isArray(body) ||
      Object.keys(body).length !== 1 || !Object.hasOwn(body, 'device_id')) return null;
  let userId;
  try { userId = remoteDesktopUserId(ownerUserId, body.device_id); } catch { return null; }
  const lifetime = 7 * 24 * 60 * 60; // 沿用现有 IM 凭据有效期，不额外缩短长时间远控会话的重连窗口。
  return { sdk_app_id: sdkAppId, owner_user_id: ownerUserId, device_id: body.device_id,
    user_id: userId, user_sig: createUserSig(sdkAppId, userId, secret, issuedAt, lifetime),
    expires_at: issuedAt + lifetime };
}

module.exports = { remoteDesktopUserId, remoteDesktopCredential };
