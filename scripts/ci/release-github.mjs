import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { basename } from 'node:path';

export const repository = process.env.GITHUB_REPOSITORY || 'francemazzi/strata';
if (repository !== 'francemazzi/strata') throw new Error('Release commands only support francemazzi/strata.');

export function run(command, args, options = {}) {
  const result = spawnSync(command, args, { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024, ...options });
  if (result.error || result.status !== 0) throw new Error(`${command} failed: ${result.error?.message || result.stderr}`);
  return result.stdout?.trim();
}

export function validateTag(tag) {
  if (!/^strata-v\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$/.test(tag || '')) throw new Error('Explicit Strata release tag required.');
  return tag;
}

export function tagSha(tag) {
  validateTag(tag);
  return run('gh', ['api', `repos/${repository}/commits/${tag}`, '--jq', '.sha']);
}

export function checkoutSha() { return run('git', ['rev-parse', 'HEAD']); }

export function readRelease(tag) {
  validateTag(tag);
  const result = spawnSync('gh', ['api', `repos/${repository}/releases/tags/${tag}`], { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
  if (result.status === 0) return JSON.parse(result.stdout);
  if (result.stderr?.includes('HTTP 404')) return null;
  throw new Error(`Cannot inspect release: ${result.stderr || result.error}`);
}

export function assertDraft(release) {
  if (!release?.draft) throw new Error('Release must exist and remain a draft. Published assets are immutable.');
}

export function ensureDraft(tag) {
  if (tagSha(tag) !== checkoutSha()) throw new Error('Checkout does not match the remote release tag.');
  let release = readRelease(tag);
  if (!release) {
    try {
      run('gh', ['release', 'create', tag, '--repo', repository, '--verify-tag', '--draft', '--title', `Strata ${tag.slice(8)}`, '--notes', 'Release candidate: verification and Windows acceptance pending.']);
    } catch (error) {
      // Concurrent platform builds may have created the same draft.
      if (!readRelease(tag)) throw error;
    }
    release = readRelease(tag);
  }
  assertDraft(release);
  return release;
}

export async function sha256(file) {
  const hash = createHash('sha256');
  for await (const chunk of createReadStream(file)) hash.update(chunk);
  return hash.digest('hex');
}

export function assertUpload(release, name, digest) {
  assertDraft(release);
  const existing = release.assets.find(asset => asset.name === name);
  if (!existing) return true;
  if (existing.digest !== `sha256:${digest}`) throw new Error(`Refusing to replace changed asset: ${name}`);
  return false;
}

export async function uploadFile(tag, file) {
  const digest = await sha256(file);
  if (assertUpload(readRelease(tag), basename(file), digest)) {
    run('gh', ['release', 'upload', tag, file, '--repo', repository]);
  }
}

export function downloadRelease(tag, directory) {
  assertDraft(readRelease(tag));
  run('gh', ['release', 'download', tag, '--repo', repository, '--dir', directory]);
}
