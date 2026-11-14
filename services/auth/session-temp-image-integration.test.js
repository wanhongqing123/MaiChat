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

async function renewalFixture(t) {
  const db = openStore(':memory:'),syncDB = openSessionSyncStore(':memory:');
  let now = Date.now(),verify,legacy = false,oldSync = false,starts = 0;
  const objects = new Map(),uploads = new Map();
  const options = {authorizeAttachment:(binding,headers) => verify(binding,headers),
    startUpload:async (_account,key) => {const id = `upload_${++starts}`;uploads.set(id,key);return id;},
    abortUpload:async (_account,_key,id) => {uploads.delete(id);},
    listUploads:async (_account,key) => [...uploads].filter(([,value]) => value === key).map(([id]) => id),
    listParts:async () => [{part_number:1,size_bytes:1024,etag:'fixture-etag'}],
    completeUpload:async (_account,key,id) => {objects.set(key,true);uploads.delete(id);},
    verifyObject:async (_account,key) => objects.has(key)
      ? {exists:true,size_bytes:1024,content_type:'image/png'} : {exists:false},
    deleteObject:async (_account,key) => {objects.delete(key);},
    signRead:async () => 'https://oss.example/direct-renewed-image'};
  const auth = createAuthService({db,now:() => now,sms:{send:async () => {}},
    secret:'fake-local-auth-secret-at-least-32-characters',
    imSecret:'fake-local-im-secret-at-least-32-characters',sdkAppId:123456789,sessionAttachments:options});
  const authServer = createAuthServer(auth);
  await new Promise(resolve => authServer.listen(0,'127.0.0.1',resolve));
  const base = `http://127.0.0.1:${authServer.address().port}`;
  const authorize = createAccountAuthorizer(base);
  const sync = createSessionSyncService({db:syncDB,authorize,now:() => now});
  // Frozen generation-blind create branch from 38480181, served over real HTTP.
  // Preserve its account/current-device/current-session/exact pending-part checks;
  // its missing typed generation is the old-binary behavior under test.
  const legacySyncCreate = async ({headers,body}) => {
    const reply = (statusCode,response) => ({statusCode,body:response});
    const principal = await authorize(headers);
    if (!principal) return reply(401,{error:'Authentication required'});
    const valid = value => typeof value === 'string' && /^[A-Za-z0-9_-]{1,128}$/.test(value);
    if (!valid(body.owner_slot) || !valid(body.owner_login_session_id) || !valid(body.session_id)
        || !valid(body.message_id) || !valid(body.part_id)) return reply(400,{error:'Invalid image binding'});
    const device = syncDB.prepare('SELECT login_session_id FROM sync_devices WHERE account_id=? AND slot=?')
      .get(principal.accountId,principal.slot);
    if (principal.slot !== body.owner_slot || principal.loginSessionId !== body.owner_login_session_id
        || device?.login_session_id !== principal.loginSessionId)
      return reply(403,{error:'Only the current owner can publish the image'});
    const session = syncDB.prepare(`SELECT 1 FROM sync_sessions
      WHERE account_id=? AND slot=? AND login_session_id=? AND session_id=?`)
      .get(principal.accountId,body.owner_slot,body.owner_login_session_id,body.session_id);
    if (!session) return reply(404,{error:'Session image not found'});
    const row = syncDB.prepare(`SELECT payload FROM sync_room_messages WHERE account_id=?
      AND owner_slot=? AND session_id=? AND message_id=?`)
      .get(principal.accountId,body.owner_slot,body.session_id,body.message_id);
    const part = row ? JSON.parse(row.payload).parts.find(item => item.id === body.part_id) : null;
    if (part?.kind !== 'image' || part.remoteImage?.retention !== 'temporary')
      return reply(404,{error:'Session image not found'});
    return part.remoteImage.state === 'pending' ? reply(200,{ok:true})
      : reply(409,{error:'Image publication already settled'});
  };
  const routed = {...sync,handle:request => oldSync
    && request.path === '/v1/session-sync/authorize-attachment' && request.body.operation === 'create'
    ? legacySyncCreate(request) : sync.handle(request)};
  const syncServer = createSyncServer(routed,authorize);
  await new Promise(resolve => syncServer.listen(0,'127.0.0.1',resolve));
  const syncBase = `http://127.0.0.1:${syncServer.address().port}`;
  const current = sessionAttachmentAuthorizerFromEnv({MAICHAT_SESSION_SYNC_PORT:String(syncServer.address().port)});
  // This is the former connector behavior: no capability header and generation
  // collapsed to a boolean. It must never authorize a new-generation upload.
  const oldBooleanConnector = async (binding,headers) => {
    const response = await fetch(syncBase+'/v1/session-sync/authorize-attachment',{
      method:'POST',redirect:'error',headers:{'Content-Type':'application/json',Authorization:headers.authorization},
      body:JSON.stringify(binding)});
    return response.status === 200 && (await response.json()).ok === true;
  };
  verify = (binding,headers) => (legacy ? oldBooleanConnector : current)(binding,headers);
  t.after(async () => {
    await new Promise(resolve => syncServer.close(resolve));
    await new Promise(resolve => authServer.close(resolve));syncDB.close();db.close();
  });
  async function request(origin,path,token,body,extraHeaders = {}) {
    const response = await fetch(origin+path,{method:body === undefined ? 'GET' : 'POST',
      headers:{Authorization:`Bearer ${token}`,'Content-Type':'application/json',...extraHeaders},
      ...(body !== undefined ? {body:JSON.stringify(body)} : {})});
    return {status:response.status,body:await response.json()};
  }
  await auth.createTestAccount('renewal_owner','renewal-owner-password-123456');
  const login = async slot => {
    const reply = await request(base,'/v1/auth/password/login','',{
      account_id:'renewal_owner',password:'renewal-owner-password-123456',device_slot:slot,age_confirmed_14_plus:true});
    assert.equal(reply.status,200);
    const me = await request(base,'/v1/auth/me',reply.body.access_token);
    assert.equal(me.status,200);return {...reply.body,login_session_id:me.body.login_session_id};
  };
  let owner = await login('mac'),viewer = await login('ios_phone');
  const binding = {owner_slot:'mac',owner_login_session_id:owner.login_session_id,
    session_id:'renewal_session',message_id:'renewal_message',part_id:'renewal_part'};
  const presence = () => request(syncBase,'/v1/session-sync/presence',owner.access_token,{
    device_id:'renewal_install_mac',name:'Owned Mac',
    sessions:[{id:binding.session_id,title:'Owned Session',updated_at:now}]});
  assert.equal((await presence()).status,200);
  const publish = remoteImage => request(syncBase,'/v1/session-sync/publish',owner.access_token,{
    session_id:binding.session_id,messages:[{id:binding.message_id,role:'assistant',
      parts:[{id:binding.part_id,kind:'image',remotePartId:binding.part_id,mimeType:'image/png',remoteImage}]}]});
  const begin = patch => request(base,'/v1/session-attachments/uploads',owner.access_token,{
    purpose:'temporary_session_image',name:'renewal.png',size_bytes:1024,content_type:'image/png',...binding,...patch});
  const complete = id => request(base,`/v1/session-attachments/uploads/${id}/complete`,owner.access_token,{});
  const ready = receipt => ({state:'ready',retention:'temporary',attachment_id:receipt.attachment_id,
    owner_login_session_id:receipt.owner_login_session_id,room_key:receipt.room_key,name:receipt.name,
    media_type:'image',content_type:receipt.content_type,size_bytes:receipt.size_bytes,expires_at:receipt.expires_at,
    ...(receipt.generation > 0 ? {generation:receipt.generation} : {})});
  return {db,syncDB,binding,publish,begin,complete,ready,presence,starts:() => starts,
    advance:async ms => {
      now += ms;
      const refresh = async credential => {
        const reply = await request(base,'/v1/auth/refresh','',{refresh_token:credential.refresh_token});
        assert.equal(reply.status,200);return {...credential,...reply.body};
      };
      owner = await refresh(owner);viewer = await refresh(viewer);assert.equal((await presence()).status,200);
    },
    reloginOwner:async () => {owner = await login('mac');binding.owner_login_session_id = owner.login_session_id;
      assert.equal((await presence()).status,200);},
    legacy:value => {legacy = value;},
    rollbackSync:value => {oldSync = value;},
    legacyGrant:() => oldBooleanConnector({operation:'create',...binding},{authorization:`Bearer ${owner.access_token}`}),
    read:id => request(base,`/v1/session-attachments/${id}/play-link?${new URLSearchParams(binding)}`,viewer.access_token),
    unsupportedGrant:() => request(syncBase,'/v1/session-sync/authorize-attachment',owner.access_token,{operation:'create',...binding})};
}

test('normal Auth and Sync HTTP renew only an acknowledged next generation and preserve the expired original',async t => {
  const f = await renewalFixture(t);
  assert.equal((await f.publish({state:'pending',retention:'temporary'})).status,200);
  const original = await f.begin();assert.equal(original.status,201);
  const completed = await f.complete(original.body.attachment_id);assert.equal(completed.status,200);
  assert.equal((await f.publish(f.ready(completed.body))).status,200);
  const old = f.db.prepare('SELECT * FROM session_image_publications').get();
  const pending = {state:'pending',retention:'temporary',generation:1,owner_login_session_id:f.binding.owner_login_session_id};
  assert.equal((await f.publish(pending)).status,409);
  assert.equal((await f.begin({generation:1})).status,400);
  assert.equal(f.starts(),1);
  await f.advance(24*60*60_000);
  assert.equal((await f.begin()).status,410);
  assert.equal((await f.publish(pending)).status,200);
  const renewed = await f.begin();assert.equal(renewed.status,201);
  assert.equal(renewed.body.generation,1);
  assert.notEqual(renewed.body.attachment_id,original.body.attachment_id);
  assert.equal(renewed.body.expires_at-original.body.expires_at,24*60*60_000);
  assert.equal((await f.begin()).body.attachment_id,renewed.body.attachment_id);
  assert.equal(f.starts(),2);
  const ready = await f.complete(renewed.body.attachment_id);assert.equal(ready.status,200);
  assert.equal((await f.publish(f.ready(ready.body))).status,200);
  f.syncDB.prepare('UPDATE sync_devices SET last_seen=0').run();
  assert.equal((await f.read(renewed.body.attachment_id)).status,200);
  assert.equal((await f.read(original.body.attachment_id)).status,410);
  assert.deepEqual(f.db.prepare('SELECT * FROM session_image_publications').get(),old);
  assert.equal(f.db.prepare('SELECT COUNT(*) n FROM session_image_publication_generations').get().n,1);
});

test('an actual old boolean connector cannot create generation zero for a relogged owner with no original receipt',async t => {
  const f = await renewalFixture(t);
  assert.equal((await f.publish({state:'failed',retention:'temporary'})).status,200);
  await f.reloginOwner();
  assert.equal((await f.publish({state:'pending',retention:'temporary',generation:1,
    owner_login_session_id:f.binding.owner_login_session_id})).status,200);
  assert.equal((await f.unsupportedGrant()).status,409);
  assert.equal(await f.legacyGrant(),false);
  f.rollbackSync(true);
  assert.equal((await f.begin()).status,404);
  assert.equal(f.starts(),0);
  assert.equal(f.db.prepare('SELECT COUNT(*) n FROM session_image_publications').get().n,0);
  assert.equal(f.db.prepare('SELECT COUNT(*) n FROM session_image_publication_generations').get().n,0);
  f.rollbackSync(false);
  f.legacy(true);
  assert.equal((await f.begin()).status,404);
  assert.equal(f.starts(),0);
  assert.equal(f.db.prepare('SELECT COUNT(*) n FROM session_image_publications').get().n,0);
  f.legacy(false);
  const created = await f.begin();assert.equal(created.status,201);assert.equal(created.body.generation,1);
  assert.equal(f.starts(),1);
  f.legacy(true);
  assert.equal((await f.begin()).body.attachment_id,created.body.attachment_id);
  assert.equal(f.starts(),1);
  assert.equal(f.db.prepare('SELECT COUNT(*) n FROM session_image_publications').get().n,0);
});
