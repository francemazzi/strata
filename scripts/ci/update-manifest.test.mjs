import { test } from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, verify } from 'node:crypto';
import { createUpdateManifest, signUpdateManifest, validateUpdateAcceptance } from './update-manifest.mjs';
const files = ['Strata-1.6.1-win64.exe', 'Strata-1.6.1-win64.zip', 'Strata-Installer.dmg', 'Strata-1.6.1-x86_64.AppImage'].map(name => ({ name, sha256: 'a'.repeat(64), size: 100 }));
const input = { tag: 'strata-v1.6.1', sourceSha: 'b'.repeat(40), files };
test('publishes all supported installations with exact asset hashes', () => {
  const manifest = JSON.parse(createUpdateManifest(input));
  assert.equal(manifest.packages.length, 5);
  assert.ok(manifest.packages.every(pkg => pkg.sha256 === 'a'.repeat(64) && pkg.url.includes('/strata-v1.6.1/')));
});
test('rejects incomplete, ambiguous, prerelease and unsafe packages', () => {
  for (const files of [input.files.slice(1), [...input.files, input.files[0]], input.files.map(file => ({ ...file, name: '../' + file.name }))]) assert.throws(() => createUpdateManifest({ ...input, files }));
  assert.throws(() => createUpdateManifest({ ...input, tag: 'strata-v1.6.1-rc1' }));
});
test('signature covers every manifest byte; trust root must match', () => {
  const { privateKey, publicKey } = generateKeyPairSync('rsa', { modulusLength: 3072, privateKeyEncoding: { type: 'pkcs8', format: 'pem' }, publicKeyEncoding: { type: 'spki', format: 'pem' } });
  const data = createUpdateManifest(input);
  const sig = Buffer.from(signUpdateManifest(data, privateKey, publicKey).toString().trim(), 'base64');
  assert.equal(verify('sha256', data, publicKey, sig), true);
  assert.equal(verify('sha256', Buffer.concat([data, Buffer.from(' ')]), publicKey, sig), false);
  assert.throws(() => signUpdateManifest(data, privateKey, 'invalid'));
});
test('publication refuses missing real platform acceptance', () => {
  assert.throws(() => validateUpdateAcceptance({}, { tag: input.tag, sourceSha: input.sourceSha }));
  assert.throws(() => validateUpdateAcceptance({ ...input, platforms: {} }, { tag: input.tag, sourceSha: input.sourceSha }));
});
