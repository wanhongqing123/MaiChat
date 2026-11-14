'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { openSessionSyncStore } = require('./session-sync-store');
const { createSessionSyncService } = require('./session-sync-service');

const ROOT = '/v1/session-sync';
const binding = { owner_slot:'mac',owner_login_session_id:'login_mac',session_id:'session_a',
  message_id:'message_a',part_id:'part_image' };
const attachmentId = '123e4567-e89b-42d3-a456-426614174000';

async function fixture(t,role = 'assistant') {
  const db = openSessionSyncStore(':memory:');
  t.after(() => db.close());
  let now = Date.parse('2026-11-10T00:00:00Z');
  const principals = { mac:{ accountId:'account_a',slot:'mac',loginSessionId:'login_mac' },
    phone:{ accountId:'account_a',slot:'ios_phone',loginSessionId:'login_phone' },
    replacement:{ accountId:'account_a',slot:'mac',loginSessionId:'login_new' },
    other:{ accountId:'account_b',slot:'ios_phone',loginSessionId:'login_other' } };
  const service = createSessionSyncService({ db,now:() => now,authorize:async headers => principals[headers.authorization] });
  const call = (token,method,path,body = {},extraHeaders = {}) => service.handle({method,path,body,headers:{authorization:token,...extraHeaders}});
  const presence = token => call(token,'POST',`${ROOT}/presence`,{device_id:'install_mac',name:'Mac',
    sessions:[{id:binding.session_id,title:'Session',updated_at:now}]});
  await presence('mac');
  const pending = {state:'pending',retention:'temporary'};
  const ready = {state:'ready',retention:'temporary',attachment_id:attachmentId,
    owner_login_session_id:binding.owner_login_session_id,room_key:'mac:session_a',
    name:'screenshot.png',media_type:'image',content_type:'image/png',size_bytes:1024,expires_at:now+86400000};
  const message = remoteImage => ({id:binding.message_id,role,parts:[
    {id:'part_before',kind:'text',text:'Before'},
    {id:binding.part_id,kind:'image',mimeType:'image/png',remotePartId:binding.part_id,
      ...(remoteImage ? {remoteImage} : {})},
    {id:'part_after',kind:'text',text:'After'}]});
  const publish = (remoteImage,token = 'mac') => call(token,'POST',`${ROOT}/publish`,{session_id:binding.session_id,
    messages:[message(remoteImage)]});
  const authorize = (token,operation,patch = {},headers = {}) => call(token,'POST',`${ROOT}/authorize-attachment`,{
    operation,...binding,...(operation === 'read' ? {attachment_id:attachmentId,expires_at:ready.expires_at} : {}),...patch},headers);
  return {db,call,presence,publish,authorize,pending,ready,message,advance:ms => {now += ms;}};
}

for (const role of ['user','assistant']) test(`only exact published pending ${role} image authorizes first upload`,async t => {
  const f = await fixture(t,role);
  assert.equal((await f.publish(f.pending)).statusCode,200);
  assert.equal((await f.authorize('mac','create')).statusCode,200);
  assert.equal((await f.authorize('phone','create')).statusCode,403);
  assert.equal((await f.authorize('other','create')).statusCode,403);
  for (const patch of [{session_id:'wrong'},{message_id:'wrong'},{part_id:'part_before'},
    {owner_login_session_id:'old_login'}])
    assert.equal((await f.authorize('mac','create',patch)).statusCode === 200,false);
});

test('ready reference authorizes offline reads and survives assistant text updates without reorder',async t => {
  const f = await fixture(t);
  await f.publish(f.pending);
  await f.publish(f.ready);
  assert.equal((await f.publish()).statusCode,200);
  const history = await f.call('phone','GET',`${ROOT}/history?owner_slot=mac&session_id=session_a`);
  assert.deepEqual(history.body.messages[0].parts.map(part => part.id),['part_before','part_image','part_after']);
  assert.deepEqual(history.body.messages[0].parts[1].remoteImage,f.ready);
  f.advance(60_000);
  assert.equal((await f.authorize('phone','read')).statusCode,200);
  await f.presence('replacement');
  assert.equal((await f.authorize('phone','read')).statusCode,200);
  assert.equal((await f.authorize('other','read')).statusCode,404);
  assert.equal((await f.authorize('phone','read',{attachment_id:'123e4567-e89b-42d3-a456-426614174099'})).statusCode,404);
  f.advance(86400000);
  assert.equal((await f.authorize('phone','read')).statusCode,410);
});

test('ready image cannot change object identity or regain pending state on republish',async t => {
  const f = await fixture(t);
  await f.publish(f.ready);
  assert.equal((await f.publish({...f.ready,attachment_id:'123e4567-e89b-42d3-a456-426614174099'})).statusCode,409);
  assert.equal((await f.publish(f.pending)).statusCode,200);
  const history = await f.call('phone','GET',`${ROOT}/history?owner_slot=mac&session_id=session_a`);
  assert.deepEqual(history.body.messages[0].parts[1].remoteImage,f.ready);
});

test('explicit generation renews only settled or server-expired image and authorizes its pending upload',async t => {
  const f = await fixture(t);
  await f.publish(f.ready);
  const next = {...f.pending,generation:1,owner_login_session_id:'login_mac'};
  assert.equal((await f.publish(next)).statusCode,409); // Healthy ready must retain its original object.
  f.advance(86400000);
  assert.equal((await f.publish(next)).statusCode,200);
  assert.equal((await f.authorize('mac','create',{}, {'X-MaiChat-Image-Generation':'1'})).statusCode,200);
  const ready = {...f.ready,generation:1,attachment_id:'123e4567-e89b-42d3-a456-426614174001',
    expires_at:f.ready.expires_at+86400000};
  assert.equal((await f.publish(ready)).statusCode,200);
  assert.equal((await f.publish(f.ready)).statusCode,200); // Late generation 0 cannot roll back generation 1.
  const history = await f.call('phone','GET',`${ROOT}/history?owner_slot=mac&session_id=session_a`);
  assert.deepEqual(history.body.messages[0].parts[1].remoteImage,{...next,...ready});
  assert.equal((await f.authorize('phone','read')).statusCode,404);
  assert.equal((await f.authorize('phone','read',{attachment_id:ready.attachment_id,expires_at:ready.expires_at})).statusCode,200);
});

test('generation rejects skips, malformed values, pending restarts and expired new receipts',async t => {
  const f = await fixture(t);
  await f.publish({...f.pending,state:'failed'});
  for (const generation of [-1,0.5,Number.MAX_SAFE_INTEGER+1,'1'])
    assert.equal((await f.publish({...f.pending,generation})).statusCode,400);
  assert.equal((await f.publish({...f.pending,generation:2})).statusCode,409);
  assert.equal((await f.publish({...f.pending,generation:1,owner_login_session_id:'wrong'})).statusCode,409);
  assert.equal((await f.publish({...f.pending,generation:1,owner_slot:'windows'})).statusCode,409);
  assert.equal((await f.publish({...f.pending,generation:1,part_id:'part_before'})).statusCode,409);
  assert.equal((await f.publish({...f.pending,generation:1})).statusCode,200);
  assert.equal((await f.publish({...f.pending,generation:2})).statusCode,409);
  assert.equal((await f.publish({...f.ready,generation:1,expires_at:1})).statusCode,409);
  assert.equal((await f.publish({...f.ready,generation:1,owner_login_session_id:'wrong'})).statusCode,409);
});

test('only current owner can renew after relogin and legacy no-generation semantics stay unchanged',async t => {
  const f = await fixture(t,'user');
  await f.publish({...f.pending,state:'failed'});
  await f.presence('replacement');
  const next = {...f.pending,generation:1};
  assert.equal((await f.publish(next)).statusCode,409);
  assert.notEqual((await f.publish(next,'phone')).statusCode,200);
  assert.notEqual((await f.publish(next,'other')).statusCode,200);
  assert.equal((await f.publish(next,'replacement')).statusCode,200);
  const history = await f.call('phone','GET',`${ROOT}/history?owner_slot=mac&session_id=session_a`);
  assert.equal(history.body.messages[0].parts[1].remoteImage.owner_login_session_id,'login_new');
  assert.equal((await f.authorize('replacement','create',{owner_login_session_id:'login_new'}, {'x-maichat-image-generation':'1'})).statusCode,200);
  assert.equal((await f.publish({...f.ready,generation:1,owner_login_session_id:'login_mac'},'replacement')).statusCode,409);
});

test('new image generations require the fixed connector capability, not a caller body hint',async t => {
  const f = await fixture(t);
  await f.publish({...f.pending,state:'failed'});
  await f.presence('replacement');
  await f.publish({...f.pending,generation:1},'replacement');
  for (const value of [undefined,'0','01','1, 1',1,['1']]) {
    const headers = value === undefined ? {} : {'x-maichat-image-generation':value};
    const denied = await f.authorize('replacement','create',{owner_login_session_id:'login_new'},headers);
    assert.equal(denied.statusCode,409);
    assert.equal(denied.body.errorCode,'image_generation_unsupported');
  }
  assert.equal((await f.authorize('replacement','create',{owner_login_session_id:'login_new',generation:1})).statusCode,409);
  assert.equal((await f.authorize('replacement','create',{owner_login_session_id:'login_new'}, {'X-MaiChat-Image-Generation':'1'})).statusCode,200);
});
