'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { plan,upgrade,BUCKET,PREFIX } = require('./upgrade-session-temp-image-lifecycle');

function fixture() {
  const original = [
    { id:'cloud-drive-abort',prefix:'cloud-drive/',status:'Enabled',
      abortMultipartUpload:{ days:'8' } },
    { id:'persistent-attachment-abort',prefix:'session-attachments/',status:'Enabled',
      abortMultipartUpload:{ days:'8' } },
    { id:'unrelated',prefix:'other/',status:'Enabled',expiration:{ days:'90' },
      transition:{ days:'30',storageClass:'IA' } }
  ];
  const state = { rules:structuredClone(original),writes:0,events:[],backups:[] };
  const storage = {
    async getBucketVersioning(bucket) {
      assert.equal(bucket,BUCKET); return { versionStatus:undefined };
    },
    async getBucketLifecycle(bucket) {
      assert.equal(bucket,BUCKET); state.events.push('get');
      return { rules:structuredClone(state.rules) };
    },
    async putBucketLifecycle(bucket,rules) {
      assert.equal(bucket,BUCKET); state.events.push('put');
      state.rules = structuredClone(rules); state.writes += 1;
    }
  };
  const backup = async rules => {
    state.events.push('backup'); state.backups.push(structuredClone(rules));
  };
  return { state,storage,backup,original };
}

test('temporary lifecycle backs up and replaces the full preserved list only on apply', async () => {
  const { state,storage,backup,original } = fixture();
  assert.equal(await upgrade(storage), 'ready-to-upgrade');
  assert.equal(state.writes,0);
  state.events.length = 0;
  assert.equal(await upgrade(storage,{ apply:true,backup }), 'updated');
  assert.deepEqual(state.events,['get','backup','get','put','get']);
  assert.deepEqual(state.backups,[original]);
  assert.deepEqual(state.rules.slice(0,original.length),original);
  assert.deepEqual(state.rules.at(-1),{
    id:'maichat-session-temp-images-expire-abort-1d',prefix:PREFIX,status:'Enabled',
    expiration:{ days:1 },abortMultipartUpload:{ days:1 }
  });
  assert.equal(await upgrade(storage,{ apply:true,backup }), 'already-configured');
  assert.equal(state.writes,1);
});

test('temporary lifecycle refuses overlap, action drift and reused rule identifiers', () => {
  const { original } = fixture();
  for (const rule of [
    { id:'global',prefix:'',status:'Enabled',expiration:{ days:30 } },
    { id:'parent',prefix:'session-',status:'Enabled',abortMultipartUpload:{ days:8 } },
    { id:'child',prefix:`${PREFIX}account_a/`,status:'Enabled',expiration:{ days:2 } },
    { id:'wrong',prefix:PREFIX,status:'Enabled',expiration:{ days:1 },
      abortMultipartUpload:{ days:8 } },
    { id:'maichat-session-temp-images-expire-abort-1d',prefix:'unrelated/',status:'Enabled',
      expiration:{ days:1 } }
  ]) assert.throws(() => plan([...original,rule]),/overlap|differs|identifier/);
});

test('temporary lifecycle detects preflight drift before writing', async () => {
  const { state,storage,backup } = fixture();
  const read = storage.getBucketLifecycle;
  let reads = 0;
  storage.getBucketLifecycle = async bucket => {
    if (++reads === 2) state.rules[2].expiration.days = '91';
    return read(bucket);
  };
  await assert.rejects(upgrade(storage,{ apply:true,backup }),/changed during preflight/);
  assert.equal(state.writes,0);
  assert.equal(state.backups.length,1);
});

test('temporary lifecycle verifies every rule after replacement', async () => {
  const { storage,backup } = fixture();
  const write = storage.putBucketLifecycle;
  storage.putBucketLifecycle = async (bucket,rules) => write(bucket,rules.slice(-1));
  await assert.rejects(upgrade(storage,{ apply:true,backup }),/readback/);
});

test('temporary lifecycle cannot apply without a durable backup', async () => {
  const { state,storage } = fixture();
  await assert.rejects(upgrade(storage,{ apply:true }),/backup/);
  await assert.rejects(upgrade(storage,{ apply:true,backup:async () => {
    throw new Error('backup disk failure');
  } }),/backup disk failure/);
  assert.equal(state.writes,0);
});

test('malformed lifecycle responses never become an empty replacement list', async () => {
  for (const response of [{},{ rules:false },{ rules:'not-a-list' }]) {
    const { state,storage,backup } = fixture();
    storage.getBucketLifecycle = async () => response;
    await assert.rejects(upgrade(storage,{ apply:true,backup }),/Invalid lifecycle rule list/);
    assert.equal(state.writes,0);
  }
});

test('temporary deletion refuses versioned or unverifiable buckets before any write', async () => {
  for (const response of [{ versionStatus:'Enabled' },{ versionStatus:'Suspended' },{},null]) {
    const { state,storage,backup } = fixture();
    storage.getBucketVersioning = async () => response;
    await assert.rejects(upgrade(storage,{ apply:true,backup }),/version/i);
    assert.equal(state.writes,0);
    assert.equal(state.backups.length,0);
  }
});

test('bucket versioning is checked again after backup before lifecycle replacement', async () => {
  const { state,storage,backup } = fixture();
  let reads = 0;
  storage.getBucketVersioning = async () => ({ versionStatus:++reads === 1 ? undefined : 'Enabled' });
  await assert.rejects(upgrade(storage,{ apply:true,backup }),/version/i);
  assert.equal(state.backups.length,1);
  assert.equal(state.writes,0);
});

test('pinned OSS SDK serializes both one-day actions and preserves unrelated rules', async () => {
  const OSS = require('ali-oss');
  const { state,storage,backup,original } = fixture();
  const sdk = new OSS({ bucket:BUCKET,region:'oss-cn-guangzhou',
    accessKeyId:'local-id',accessKeySecret:'local-secret',authorizationV4:true,secure:true });
  let xml = '';
  sdk.request = async params => {
    xml = params.content; return { res:{ status:200 } };
  };
  storage.putBucketLifecycle = async (bucket,rules) => {
    state.rules = structuredClone(rules);
    await sdk.putBucketLifecycle(bucket,rules);
  };
  assert.equal(await upgrade(storage,{ apply:true,backup }),'updated');
  assert.deepEqual(state.rules.slice(0,original.length),original);
  assert.match(xml,/<Prefix>session-temp-images\/<\/Prefix>/);
  assert.match(xml,/<Expiration><Days>1<\/Days><\/Expiration>/);
  assert.match(xml,/<AbortMultipartUpload><Days>1<\/Days><\/AbortMultipartUpload>/);
  assert.match(xml,/<ID>unrelated<\/ID>/);
  assert.match(xml,/<StorageClass>IA<\/StorageClass>/);
  assert.equal((xml.match(/<Rule>/g) || []).length,4);
});
