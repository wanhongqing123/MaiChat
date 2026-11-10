'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createServer:createAuthServer,sessionAttachmentAuthorizerFromEnv } = require('./server');
const { openSessionSyncStore } = require('./session-sync-store');
const { createSessionSyncService } = require('./session-sync-service');
const { createServer:createSyncServer,createAccountAuthorizer } = require('./session-sync-server');

test('account HTTP verifies pending and ready image against Sync DB and reads with offline owner',async t => {
  const db = openStore(':memory:'),syncDB = openSessionSyncStore(':memory:');
  let verify;
  const objects = new Map();
  const options = {
    authorizeAttachment:(binding,headers) => verify(binding,headers),
    startUpload:async () => 'upload_a',abortUpload:async () => {},listUploads:async () => [],
    listParts:async () => [{part_number:1,size_bytes:1024,etag:'etag_a'}],
    completeUpload:async (_account,key) => {objects.set(key,true);},
    verifyObject:async (_account,key) => objects.has(key) ?
      {exists:true,size_bytes:1024,content_type:'image/png'} : {exists:false},
    deleteObject:async (_account,key) => {objects.delete(key);},
    signRead:async () => 'https://oss.example/direct-image'
  };
  const auth = createAuthService({db,sms:{send:async () => {}},
    secret:'fake-local-auth-secret-at-least-32-characters',
    imSecret:'fake-local-im-secret-at-least-32-characters',sdkAppId:123456789,
    sessionAttachments:options});
  const authServer = createAuthServer(auth);
  await new Promise(resolve => authServer.listen(0,'127.0.0.1',resolve));
  const base = `http://127.0.0.1:${authServer.address().port}`;
  const authorize = createAccountAuthorizer(base);
  const sync = createSessionSyncService({db:syncDB,authorize});
  const syncServer = createSyncServer(sync,authorize);
  await new Promise(resolve => syncServer.listen(0,'127.0.0.1',resolve));
  const syncBase = `http://127.0.0.1:${syncServer.address().port}`;
  verify = sessionAttachmentAuthorizerFromEnv({MAICHAT_SESSION_SYNC_PORT:String(syncServer.address().port)});
  t.after(async () => {
    await new Promise(resolve => syncServer.close(resolve));
    await new Promise(resolve => authServer.close(resolve));
    syncDB.close();db.close();
  });
  async function request(origin,path,token,body) {
    const response = await fetch(origin+path,{method:body === undefined ? 'GET' : 'POST',
      headers:{Authorization:`Bearer ${token}`,'Content-Type':'application/json'},
      ...(body !== undefined ? {body:JSON.stringify(body)} : {})});
    return {status:response.status,body:await response.json()};
  }
  await auth.createTestAccount('temp_owner','temp-owner-password-123456');
  async function login(slot) {
    const reply = await request(base,'/v1/auth/password/login','',{
      account_id:'temp_owner',password:'temp-owner-password-123456',device_slot:slot,age_confirmed_14_plus:true});
    assert.equal(reply.status,200);return reply.body.access_token;
  }
  const owner = await login('mac'),viewer = await login('ios_phone');
  const me = await request(base,'/v1/auth/me',owner);
  const binding = {owner_slot:'mac',owner_login_session_id:me.body.login_session_id,
    session_id:'session_a',message_id:'message_a',part_id:'part_a'};
  await request(syncBase,'/v1/session-sync/presence',owner,{device_id:'install_mac',name:'Mac',
    sessions:[{id:'session_a',title:'Session',updated_at:Date.now()}]});
  const image = {purpose:'temporary_session_image',name:'screenshot.png',size_bytes:1024,
    content_type:'image/png',...binding};
  assert.equal((await request(base,'/v1/session-attachments/uploads',owner,image)).status,404);
  const publish = remoteImage => request(syncBase,'/v1/session-sync/publish',owner,{
    session_id:'session_a',messages:[{id:'message_a',role:'assistant',parts:[{
      id:'part_a',kind:'image',remotePartId:'part_a',mimeType:'image/png',remoteImage}]}]});
  assert.equal((await publish({state:'pending',retention:'temporary'})).status,200);
  const uploaded = await request(base,'/v1/session-attachments/uploads',owner,image);
  assert.equal(uploaded.status,201);
  // Cleanup must not reset idempotency while the authoritative pending part
  // remains published: otherwise a second init would allocate a fresh object.
  assert.equal((await request(base,'/v1/session-attachments/cleanup-room',owner,binding)).status,409);
  const repeated = await request(base,'/v1/session-attachments/uploads',owner,image);
  assert.equal(repeated.body.attachment_id,uploaded.body.attachment_id);
  const completed = await request(base,`/v1/session-attachments/uploads/${uploaded.body.attachment_id}/complete`,owner,{});
  assert.equal(completed.status,200);
  const receipt = completed.body;
  assert.equal((await publish({state:'ready',retention:receipt.retention,attachment_id:receipt.attachment_id,
    owner_login_session_id:receipt.owner_login_session_id,room_key:receipt.room_key,name:receipt.name,
    media_type:receipt.media_type,content_type:receipt.content_type,size_bytes:receipt.size_bytes,
    expires_at:receipt.expires_at})).status,200);
  syncDB.prepare('UPDATE sync_devices SET last_seen=0').run();
  const path = `/v1/session-attachments/${receipt.attachment_id}/play-link?${new URLSearchParams(binding)}`;
  const read = await request(base,path,viewer);
  assert.equal(read.status,200);
  assert.equal(read.body.read_url,'https://oss.example/direct-image');
  assert.equal(read.body.expires_at,receipt.expires_at);
  // Owner deletion first publishes the new directory, then removes room data,
  // matching DesktopSessionSyncCoordinator's normal cleanup order.
  assert.equal((await request(syncBase,'/v1/session-sync/presence',owner,{device_id:'install_mac',name:'Mac',sessions:[]})).status,200);
  const deleted = await request(syncBase,'/v1/session-sync/room-delete',owner,{session_id:'session_a'});
  assert.equal(deleted.status,200);
  assert.equal((await request(base,path,viewer)).status,404);
  assert.equal((await request(base,'/v1/session-attachments/cleanup-room',owner,binding)).status,202);
  await auth.cleanupSessionAttachments();
  assert.equal(db.prepare('SELECT COUNT(*) AS n FROM session_image_publications').get().n,0);
});
