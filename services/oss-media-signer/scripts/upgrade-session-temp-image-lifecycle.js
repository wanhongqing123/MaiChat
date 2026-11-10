#!/usr/bin/env node
'use strict';

// PutBucketLifecycle replaces every rule. This reviewed temporary prefix has
// separate completed-object and unfinished-Multipart actions; it never changes
// the persistent attachment rules. OSS applies both actions asynchronously.
const { open } = require('node:fs/promises');
const BUCKET = 'maichat-private-cloud-drive-gz-20261025-f346b709';
const REGION = 'oss-cn-guangzhou';
const PREFIX = 'session-temp-images/';
const RULE_ID = 'maichat-session-temp-images-expire-abort-1d';

function plan(rules) {
  if (!Array.isArray(rules)) throw new Error('Invalid lifecycle rule list');
  let configured = false;
  for (const rule of rules) {
    if (!rule || typeof rule !== 'object') throw new Error('Invalid lifecycle rule');
    if (rule.id === RULE_ID && rule.prefix !== PREFIX)
      throw new Error('Temporary lifecycle rule identifier is already in use');
    if (rule.prefix === PREFIX) {
      if (configured || rule.id !== RULE_ID || rule.status !== 'Enabled' ||
          Number(rule.expiration?.days) !== 1 ||
          Number(rule.abortMultipartUpload?.days) !== 1 ||
          !Object.keys(rule).every(key =>
            ['id','prefix','status','expiration','abortMultipartUpload'].includes(key)) ||
          Object.keys(rule.expiration).some(key => key !== 'days') ||
          Object.keys(rule.abortMultipartUpload).some(key => key !== 'days'))
        throw new Error('Temporary image lifecycle rule differs from the reviewed rule');
      configured = true;
    } else {
      // An absent/empty prefix may apply to the whole bucket. Refuse an
      // ambiguous overlap rather than hide an existing retention action.
      const prefix = rule.prefix || '';
      if (typeof prefix !== 'string' || PREFIX.startsWith(prefix) || prefix.startsWith(PREFIX))
        throw new Error('An overlapping temporary image lifecycle rule already exists');
    }
  }
  if (configured) return { status:'already-configured',rules };
  return { status:'ready',rules:[...rules,{
    id:RULE_ID,prefix:PREFIX,status:'Enabled',
    expiration:{ days:1 },abortMultipartUpload:{ days:1 }
  }] };
}

function fingerprint(value) {
  // OSS XML parsing can return day counts as strings. Compare every field,
  // tolerating only that representation change during the readback.
  if (Array.isArray(value)) return value.map(fingerprint);
  if (value && typeof value === 'object')
    return Object.fromEntries(Object.keys(value).sort().map(key =>
      [key,key === 'days' && /^\d+$/.test(String(value[key]))
        ? Number(value[key]) : fingerprint(value[key])]));
  return value;
}

function lifecycleRules(response) {
  // The SDK uses null for a verified empty configuration. A missing or invalid
  // field is not deletion evidence and must never become an empty PUT base.
  if (!response || !Object.hasOwn(response,'rules') ||
      (response.rules !== null && !Array.isArray(response.rules)))
    throw new Error('Invalid lifecycle rule list response');
  return response.rules || [];
}

async function requireUnversioned(storage) {
  if (typeof storage.getBucketVersioning !== 'function')
    throw new Error('Bucket versioning cannot be verified');
  const response = await storage.getBucketVersioning(BUCKET);
  // The pinned SDK exposes an explicit undefined versionStatus for a bucket
  // that has never enabled versioning. Enabled/Suspended may retain historical
  // image versions after current-object Expiration; this rule cannot clear them.
  if (!response || !Object.hasOwn(response,'versionStatus') ||
      (response.versionStatus !== undefined && response.versionStatus !== ''))
    throw new Error('Temporary image lifecycle requires an unversioned bucket');
}

async function upgrade(storage, { apply = false,backup } = {}) {
  await requireUnversioned(storage);
  const first = lifecycleRules(await storage.getBucketLifecycle(BUCKET));
  const prepared = plan(first);
  if (prepared.status === 'already-configured') return prepared.status;
  if (!apply) return 'ready-to-upgrade';
  if (typeof backup !== 'function') throw new Error('A durable lifecycle backup is required');
  await backup(structuredClone(first));
  await requireUnversioned(storage);
  const second = lifecycleRules(await storage.getBucketLifecycle(BUCKET));
  if (JSON.stringify(second) !== JSON.stringify(first))
    throw new Error('Lifecycle rules changed during preflight');
  await storage.putBucketLifecycle(BUCKET,structuredClone(prepared.rules));
  const after = lifecycleRules(await storage.getBucketLifecycle(BUCKET));
  if (JSON.stringify(fingerprint(after)) !== JSON.stringify(fingerprint(prepared.rules)))
    throw new Error('Lifecycle full rule-list readback did not match');
  return 'updated';
}

async function main(args = process.argv.slice(2), env = process.env) {
  const apply = args.includes('--apply');
  const confirmation = `--confirm-bucket=${BUCKET}`;
  const backups = args.filter(arg => arg.startsWith('--backup='));
  if (!args.includes(confirmation) || backups.length !== (apply ? 1 : 0) ||
      args.length !== (apply ? 3 : 1) ||
      args.some(arg => arg !== confirmation && arg !== '--apply' && !backups.includes(arg)) ||
      (apply && backups[0].length === '--backup='.length))
    throw new Error('Use --confirm-bucket=<reviewed-private-bucket> [--apply --backup=<new-file>]');
  if (apply && process.getuid?.() !== 0)
    throw new Error('Apply must run as the authorized ECS administrator');
  if (!env.ALIBABA_CLOUD_ACCESS_KEY_ID || !env.ALIBABA_CLOUD_ACCESS_KEY_SECRET)
    throw new Error('Temporary administrator OSS credentials are missing');
  const OSS = require('ali-oss');
  const storage = new OSS({ accessKeyId:env.ALIBABA_CLOUD_ACCESS_KEY_ID,
    accessKeySecret:env.ALIBABA_CLOUD_ACCESS_KEY_SECRET,
    bucket:BUCKET,region:REGION,authorizationV4:true,secure:true });
  const backup = apply ? async rules => {
    const file = await open(backups[0].slice('--backup='.length),'wx',0o600);
    try {
      await file.writeFile(`${JSON.stringify({ bucket:BUCKET,
        captured_at:new Date().toISOString(),rules },null,2)}\n`);
      await file.sync();
    } finally { await file.close(); }
  } : undefined;
  process.stdout.write(`${await upgrade(storage,{ apply,backup })}\n`);
}

if (require.main === module) {
  main().catch(error => {
    process.stderr.write(`Temporary image lifecycle upgrade failed: ${error.message}\n`);
    process.exitCode = 1;
  });
}

module.exports = { plan,upgrade,main,BUCKET,PREFIX };
