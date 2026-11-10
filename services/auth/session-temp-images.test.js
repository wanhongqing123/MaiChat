'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { openStore } = require('./store');
const { createCloudDriveService } = require('./cloud-drive-service');

const DAY = 24*60*60_000;
const ROOT = '/v1/session-attachments';
const binding = { owner_slot:'mac',owner_login_session_id:'login_a',
  session_id:'session_a',message_id:'message_a',part_id:'part_a' };
const image = { purpose:'temporary_session_image',name:'screenshot.png',
  size_bytes:1024,content_type:'image/png',...binding };

function fixture(t) {
  const db = openStore(':memory:');
  t.after(() => db.close());
  let now = Date.parse('2026-11-10T00:00:00Z');
  const owner = { user_id:'user-a',account_id:'account_a',device_slot:'mac',id:'login_a' };
  const viewer = { ...owner,device_slot:'ios_phone',id:'login_phone' };
  const other = { user_id:'user-b',account_id:'account_b',device_slot:'mac',id:'other_login' };
  for (const user of [owner,other]) db.prepare(`INSERT INTO users
    (id,account_id,im_user_id,created_at,updated_at) VALUES(?,?,?,?,?)`)
    .run(user.user_id,user.account_id,user.account_id,now,now);
  const objects = new Map(), uploads = new Map(), authorizations = [], reads = [];
  let starts = 0, authorized = true, failDelete = false,completionHook = null,readHook = null,partHook = null;
  const service = createCloudDriveService({ db,mode:'session',now:() => now,
    authorizeAttachment:async (request,headers) => {
      authorizations.push({ request,headers }); return authorized;
    },
    startUpload:async (_account,key) => { ++starts; uploads.set('upload_a',key); return 'upload_a'; },
    signPart:async (_account,_key,_upload,_number,bytes,md5) => {
      if (partHook) await partHook();
      return {upload_url:'https://oss.example/part',upload_headers:{'Content-Length':String(bytes),'Content-MD5':md5}};
    },
    listParts:async () => [{ part_number:1,size_bytes:1024,etag:'etag_a' }],
    listUploads:async (_account,key) => [...uploads].filter(([,value]) => value === key).map(([id]) => id),
    completeUpload:async (_account,key,id) => {
      objects.set(key,true); uploads.delete(id); if (completionHook) await completionHook();
    },
    abortUpload:async (_account,_key,id) => { uploads.delete(id); },
    verifyObject:async (_account,key) => objects.has(key) ?
      { exists:true,size_bytes:1024,content_type:'image/png' } : { exists:false },
    signRead:async (_account,key,expires) => {
      reads.push({key,expires});if (readHook) await readHook();return 'https://oss.example/image';
    },
    deleteObject:async (_account,key) => {
      if (failDelete) throw new Error('fake OSS unavailable'); objects.delete(key);
    }
  });
  const headers = { authorization:'Bearer fixture-account-token' };
  const call = (method,path,body = {},principal = owner,query) =>
    service.handle(method,path,body,principal,query,headers);
  const begin = (body = image,principal = owner) => call('POST',`${ROOT}/uploads`,body,principal);
  const complete = id => call('POST',`${ROOT}/uploads/${id}/complete`);
  const signPart = id => call('POST',`${ROOT}/uploads/${id}/parts/1/sign`,{
    content_md5_base64:Buffer.alloc(16,1).toString('base64')});
  const read = (id,principal = viewer,expected = binding) =>
    call('GET',`${ROOT}/${id}/play-link`,{},principal,new URLSearchParams(expected));
  return { db,owner,viewer,other,objects,uploads,authorizations,reads,begin,complete,read,signPart,service,
    starts:() => starts,now:() => now,advance:ms => { now += ms; },
    authorize:value => { authorized = value; },failDelete:value => { failDelete = value; },
    onComplete:callback => { completionHook = callback; },onRead:callback => { readHook = callback; },
    onPart:callback => { partHook = callback; } };
}

test('temporary image publication reserves one fixed lifetime and complete owner identity', async t => {
  const f = fixture(t);
  const first = await f.begin();
  assert.equal(first.statusCode,201);
  assert.equal(first.body.retention,'temporary');
  assert.equal(first.body.expires_at,f.now()+DAY);
  for (const [key,value] of Object.entries(binding)) assert.equal(first.body[key],value);
  const row = f.db.prepare('SELECT * FROM session_attachments WHERE id=?').get(first.body.attachment_id);
  assert.match(row.object_key,/^session-temp-images\/account_a\//);
  assert.equal(row.object_expires_at,f.now()+DAY);
  f.advance(1000);
  const repeat = await f.begin();
  assert.equal(repeat.body.attachment_id,first.body.attachment_id);
  assert.equal(repeat.body.expires_at,first.body.expires_at);
  assert.equal(f.starts(),1);
  assert.equal((await f.begin({...image,size_bytes:2048})).statusCode,409);
  const completed = await f.complete(first.body.attachment_id);
  assert.equal(completed.statusCode,200);
  assert.equal(completed.body.expires_at,first.body.expires_at);
  assert.equal(completed.body.part_id,binding.part_id);
  const readyRepeat = await f.begin();
  assert.equal(readyRepeat.statusCode,201);
  assert.equal(readyRepeat.body.upload_id,first.body.upload_id);
  assert.equal(readyRepeat.body.attachment_id,first.body.attachment_id);
  assert.equal(readyRepeat.body.expires_at,first.body.expires_at);
});

test('temporary creation requires actual owner authorization and never accepts media paths or caller expiry', async t => {
  const f = fixture(t);
  f.authorize(false);
  assert.equal((await f.begin()).statusCode,404);
  assert.equal(f.starts(),0);
  f.authorize(true);
  assert.equal((await f.begin(image,f.viewer)).statusCode,403);
  assert.equal((await f.begin({...image,path:'/private/photo.png'})).statusCode,400);
  assert.equal((await f.begin({...image,expires_at:f.now()+DAY})).statusCode,400);
  assert.equal((await f.begin({...image,name:'clip.mp4',content_type:'video/mp4'})).statusCode,400);
  assert.equal(f.starts(),0);
});

test('ready image readers require exact room and part while owner presence is irrelevant', async t => {
  const f = fixture(t);
  const created = await f.begin();
  await f.complete(created.body.attachment_id);
  const read = await f.read(created.body.attachment_id);
  assert.equal(read.statusCode,200);
  assert.equal(read.body.retention,'temporary');
  assert.equal(read.body.expires_at,created.body.expires_at);
  for (const [key,value] of Object.entries(binding)) assert.equal(read.body[key],value);
  assert.equal(f.reads[0].expires,created.body.expires_at);
  assert.equal((await f.read(created.body.attachment_id,f.other)).statusCode,404);
  assert.equal((await f.read(created.body.attachment_id,f.viewer,{...binding,part_id:'wrong'})).statusCode,404);
  f.authorize(false);
  assert.equal((await f.read(created.body.attachment_id)).statusCode,404);
  assert.equal(f.reads.length,1);
  assert.equal(f.authorizations[0].request.operation,'create');
  assert.equal(f.authorizations[0].headers.authorization,'Bearer fixture-account-token');
  assert.equal(f.authorizations[1].request.operation,'read');
});

test('expiry denies reads before sweep and deletion is proved before metadata removal without resurrection', async t => {
  const f = fixture(t);
  const created = await f.begin();
  const id = created.body.attachment_id;
  await f.complete(id);
  f.advance(DAY);
  assert.equal((await f.read(id)).statusCode,410);
  assert.equal(f.reads.length,0);
  f.failDelete(true);
  await f.service.sweep();
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM session_attachments WHERE id=?').get(id).cleanup_pending,1);
  assert.equal(f.objects.size,1);
  f.failDelete(false);
  await f.service.sweep();
  assert.equal(f.objects.size,0);
  assert.equal(f.uploads.size,0);
  assert.equal(f.db.prepare('SELECT id FROM session_attachments WHERE id=?').get(id),undefined);
  assert.equal((await f.read(id)).statusCode,410);
  assert.equal((await f.begin()).statusCode,410);
  assert.equal(f.starts(),1);
  assert.equal(f.db.prepare('SELECT COUNT(*) AS n FROM session_image_publications').get().n,1);
});

test('room removal retains publication identity until OSS absence is verified',async t => {
  const f = fixture(t);
  const created = await f.begin();await f.complete(created.body.attachment_id);
  f.failDelete(true);
  await f.service.handle('POST',`${ROOT}/cleanup-room`,binding,f.owner);
  await f.service.sweep();
  assert.equal(f.db.prepare('SELECT COUNT(*) AS n FROM session_image_publications').get().n,1);
  f.failDelete(false);await f.service.sweep();
  assert.equal(f.objects.size,0);
  assert.equal(f.db.prepare('SELECT COUNT(*) AS n FROM session_image_publications').get().n,0);
});

test('a completion that crosses fixed expiry cannot publish ready or leave its late object',async t => {
  const f = fixture(t);
  const created = await f.begin();
  f.onComplete(() => { f.advance(DAY); });
  assert.equal((await f.complete(created.body.attachment_id)).statusCode,410);
  assert.equal(f.objects.size,0);
  const row = f.db.prepare('SELECT status,cleanup_pending FROM session_attachments WHERE id=?')
    .get(created.body.attachment_id);
  assert.equal(row.status,'deleted');
  assert.equal(row.cleanup_pending,1);
  f.advance(DAY);
  await f.service.sweep();
  assert.equal(f.db.prepare('SELECT id FROM session_attachments WHERE id=?').get(created.body.attachment_id),undefined);
  assert.equal((await f.begin()).statusCode,410);
  assert.equal(f.starts(),1);
});

for (const failure of [false,true]) test(`read expiry during signing returns 410 rather than ${failure ? '502' : 'a zero lifetime'}`,async t => {
  const f = fixture(t);
  const created = await f.begin();await f.complete(created.body.attachment_id);
  f.onRead(() => {f.advance(DAY-(failure ? 0 : 500));if (failure) throw new Error('signing expired');});
  const reply = await f.read(created.body.attachment_id);
  assert.equal(reply.statusCode,410);assert.equal(reply.body.error_code,'image_expired');
});

test('part signing expiry never returns an invalid signature failure or zero lifetime',async t => {
  const f = fixture(t);
  const created = await f.begin();
  f.onPart(() => {f.advance(DAY);throw new Error('signing expired');});
  assert.equal((await f.signPart(created.body.attachment_id)).statusCode,410);
});
