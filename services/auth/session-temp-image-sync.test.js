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
  const call = (token,method,path,body = {}) => service.handle({method,path,body,headers:{authorization:token}});
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
  const publish = remoteImage => call('mac','POST',`${ROOT}/publish`,{session_id:binding.session_id,
    messages:[message(remoteImage)]});
  const authorize = (token,operation,patch = {}) => call(token,'POST',`${ROOT}/authorize-attachment`,{
    operation,...binding,...(operation === 'read' ? {attachment_id:attachmentId,expires_at:ready.expires_at} : {}),...patch});
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
