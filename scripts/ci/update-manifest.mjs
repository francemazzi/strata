import { createPrivateKey, createPublicKey, sign, verify } from 'node:crypto';

const keyId = 'update-2026';
export function supportsInAppUpdates(tag) {
  const match = /^strata-v(\d+)\.(\d+)\.(\d+)$/.exec(tag ?? '');
  if (!match) return false;
  const [major, minor, patch] = match.slice(1).map(Number);
  return major > 1 || (major === 1 && (minor > 6 || (minor === 6 && patch >= 1)));
}
export function createUpdateManifest({ tag, sourceSha, files }) {
  const version = tag.replace(/^strata-v/, '');
  if (!/^\d+\.\d+\.\d+$/.test(version)) throw new Error('Only stable versions support updates.');
  const packages = [];
  const routes = [
    [/win64\.exe$/, 'windows', 'x86_64', 'exe', '10.0'],
    [/win64\.zip$/, 'windows-portable', 'x86_64', 'zip', '10.0'],
    [/\.dmg$/, 'macos', 'arm64', 'dmg', '11.0'],
    [/\.dmg$/, 'macos', 'x86_64', 'dmg', '10.15'],
    [/x86_64\.AppImage$/, 'linux', 'x86_64', 'AppImage', ''],
  ];
  for (const [pattern, platform, architecture, format, minimumOsVersion] of routes) {
    const matches = files.filter(file => pattern.test(file.name));
    if (matches.length !== 1) throw new Error(`Missing or ambiguous package: ${platform}/${architecture}`);
    const file = matches[0];
    if (!/^[A-Za-z0-9._-]+$/.test(file.name) || !/^[a-f0-9]{64}$/.test(file.sha256) || !Number.isSafeInteger(file.size) || file.size <= 0) throw new Error('Invalid package metadata.');
    packages.push({ platform, architecture, format, minimumOsVersion, size: file.size, sha256: file.sha256,
      url: `https://github.com/francemazzi/strata/releases/download/${tag}/${file.name}` });
  }
  return Buffer.from(`${JSON.stringify({ schemaVersion: 1, keyId, version, sourceSha, packages }, null, 2)}\n`);
}
export function signUpdateManifest(manifest, privatePem, pinnedPublicPem) {
  const key = createPrivateKey(privatePem);
  if (key.asymmetricKeyType !== 'rsa' || key.asymmetricKeyDetails.modulusLength < 3072) throw new Error('A dedicated RSA key of at least 3072 bits is required.');
  if (createPublicKey(key).export({ type: 'spki', format: 'pem' }) !== createPublicKey(pinnedPublicPem).export({ type: 'spki', format: 'pem' })) throw new Error('Signing key does not match the desktop trust root.');
  const signature = sign('sha256', manifest, key);
  if (!verify('sha256', manifest, pinnedPublicPem, signature)) throw new Error('Update signature verification failed.');
  return Buffer.from(`${signature.toString('base64')}\n`);
}
export function validateUpdateAcceptance(report, manifest) {
  if (report.sourceSha !== manifest.sourceSha || report.tag !== manifest.tag) throw new Error('Updater acceptance has a different source revision.');
  for (const platform of ['windows', 'windows-portable', 'macos', 'linux']) {
    const result = report.platforms?.[platform];
    if (!Number.isFinite(Date.parse(result?.testedAt)) || !result?.tester?.trim() || result.result !== 'passed') throw new Error(`Missing updater acceptance: ${platform}`);
    for (const scenario of ['upgrade', 'restart', 'preserve-data', 'invalid-signature', 'cancel', 'failed-install-recovery']) {
      if (result.scenarios?.[scenario] !== 'passed') throw new Error(`Missing updater scenario: ${platform}/${scenario}`);
    }
  }
}
