import { createUpdateManifest, signUpdateManifest, validateUpdateAcceptance } from './update-manifest.mjs';
import { mkdtemp, writeFile, access, readFile, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { checkoutSha, downloadRelease, ensureDraft, run, sha256, uploadFile } from './release-github.mjs';
import { platformAssets, platformPatterns, readJson, sealedAssetNames, validateWindowsReport } from './release-integrity.mjs';

const [tag] = process.argv.slice(2);
const release = ensureDraft(tag);
const platforms = Object.keys(platformPatterns);
if (!platforms.every(platform => release.assets.some(asset => asset.name === `build-${platform}.json`))) {
  console.log('Platform verification receipts are incomplete; release remains a draft.');
  process.exit(0);
}
const directory = await mkdtemp(join(tmpdir(), 'strata-seal-'));
downloadRelease(tag, directory);
const files = new Map();
for (const platform of platforms) {
  const name = `build-${platform}.json`;
  const receipt = await readJson(join(directory, name));
  if (receipt.sourceSha !== checkoutSha() || receipt.platform !== platform) throw new Error('Build receipt source mismatch.');
  platformAssets(receipt.files, platform);
  for (const file of receipt.files) {
    if (file.name !== file.name.replace(/[^A-Za-z0-9._-]/g, '')) throw new Error('Invalid receipt filename.');
    if (await sha256(join(directory, file.name)) !== file.sha256) throw new Error(`Receipt hash mismatch: ${file.name}`);
    if (files.has(file.name)) throw new Error('Conflicting platform receipts.');
    files.set(file.name, file);
  }
  files.set(name, { name, sha256: await sha256(join(directory, name)) });
}
validateWindowsReport(await readJson(join(directory, 'windows-verification.json')), platformAssets(release.assets, 'windows'), checkoutSha());
const updateAcceptancePath = join(directory, 'updater-acceptance.json');
if (!await access(updateAcceptancePath).then(() => true, () => false)) {
  console.log('Updater acceptance is incomplete; release remains an unsealed draft.');
  process.exit(0);
}
validateUpdateAcceptance(await readJson(updateAcceptancePath), { tag, sourceSha: checkoutSha() });
files.set('updater-acceptance.json', { name: 'updater-acceptance.json', sha256: await sha256(updateAcceptancePath) });
const updateFiles = await Promise.all([...files.values()].filter(file => /\.(exe|zip|dmg|AppImage)$/.test(file.name)).map(async file => ({ ...file, size: (await stat(join(directory, file.name))).size })));
const updateManifest = createUpdateManifest({ tag, sourceSha: checkoutSha(), files: updateFiles });
const publicHeader = await readFile('src/app/updates/update-public-key.h', 'utf8');
const publicPem = publicHeader.match(/-----BEGIN PUBLIC KEY-----[\s\S]+?-----END PUBLIC KEY-----/)[0];
const updateSignature = signUpdateManifest(updateManifest, process.env.STRATA_UPDATE_PRIVATE_KEY, publicPem);
for (const [name, data] of [['update-manifest.json', updateManifest], ['update-manifest.sig', updateSignature]]) {
  await writeFile(join(directory, name), data);
  await uploadFile(tag, join(directory, name));
  files.set(name, { name, sha256: await sha256(join(directory, name)) });
}
const identity = `https://github.com/${process.env.GITHUB_WORKFLOW_REF || ''}`;
if (!/^https:\/\/github\.com\/francemazzi\/strata\/\.github\/workflows\/sign-release-assets\.yml@refs\/(heads\/master|tags\/strata-v[^/]+)$/.test(identity)) {
  throw new Error('Sealing requires the trusted GitHub signing workflow.');
}
const manifest = { tag, sourceSha: checkoutSha(), signerIdentity: identity, artifacts: [...files.values()].sort((a, b) => a.name.localeCompare(b.name)) };
sealedAssetNames(manifest);
const manifestPath = join(directory, 'release-manifest.json');
await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
await uploadFile(tag, manifestPath);
const targets = [...files.keys()].filter(name => /\.(exe|zip|dmg|AppImage)$/.test(name));
targets.push('release-manifest.json');
for (const name of targets) {
  const file = join(directory, name);
  const checksum = `${file}.sha256`;
  const bundle = `${file}.sigstore.json`;
  await writeFile(checksum, `${await sha256(file)}  ${name}\n`);
  const exists = await access(bundle).then(() => true, () => false);
  if (!exists) run('cosign', ['sign-blob', '--yes', '--bundle', bundle, file], { stdio: 'inherit' });
  run('cosign', ['verify-blob', '--bundle', bundle, '--certificate-identity', identity, '--certificate-oidc-issuer', 'https://token.actions.githubusercontent.com', file], { stdio: 'inherit' });
  await uploadFile(tag, checksum);
  await uploadFile(tag, bundle);
}
console.log(`Sealed ${tag} at ${checkoutSha()}; Windows 11 acceptance is still required before publication.`);
