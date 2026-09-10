import { mkdtemp, readFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, join } from 'node:path';
import { assertDraft, downloadRelease, readRelease, repository, run, sha256, tagSha, uploadFile } from './release-github.mjs';
import { platformAssets, platformPatterns, readJson, sealedAssetNames, validateAcceptance, validateReleaseSnapshot, validateWindowsReport } from './release-integrity.mjs';

const [tag, acceptancePath, notesPath] = process.argv.slice(2);
if (!acceptancePath || !notesPath) throw new Error('Usage: publish-release.mjs TAG acceptance.json release-notes.md');
if (basename(acceptancePath) !== 'windows-acceptance.json') throw new Error('Acceptance must be named windows-acceptance.json.');
assertDraft(readRelease(tag));
const directory = await mkdtemp(join(tmpdir(), 'strata-publish-'));
downloadRelease(tag, directory);
const manifestPath = join(directory, 'release-manifest.json');
const manifest = await readJson(manifestPath);
if (manifest.tag !== tag || manifest.sourceSha !== tagSha(tag)) throw new Error('Release manifest/tag mismatch.');
if (!/^https:\/\/github\.com\/francemazzi\/strata\/\.github\/workflows\/sign-release-assets\.yml@refs\/(heads\/master|tags\/strata-v[^/]+)$/.test(manifest.signerIdentity)) throw new Error('Untrusted sealing identity.');
run('cosign', ['verify-blob', '--bundle', `${manifestPath}.sigstore.json`, '--certificate-identity', manifest.signerIdentity, '--certificate-oidc-issuer', 'https://token.actions.githubusercontent.com', manifestPath], { stdio: 'inherit' });
const sealedNames = sealedAssetNames(manifest);
for (const file of manifest.artifacts) {
  if (file.name !== file.name.replace(/[^A-Za-z0-9._-]/g, '')) throw new Error('Invalid manifest filename.');
  if (await sha256(join(directory, file.name)) !== file.sha256) throw new Error(`Release changed after sealing: ${file.name}`);
}
for (const platform of Object.keys(platformPatterns)) {
  for (const file of platformAssets(manifest.artifacts, platform)) {
    const path = join(directory, file.name);
    const checksum = (await readFile(`${path}.sha256`, 'utf8')).trim();
    if (checksum !== `${file.sha256}  ${file.name}`) throw new Error('Checksum file mismatch.');
    run('cosign', ['verify-blob', '--bundle', `${path}.sigstore.json`, '--certificate-identity', manifest.signerIdentity, '--certificate-oidc-issuer', 'https://token.actions.githubusercontent.com', path], { stdio: 'inherit' });
  }
}
validateWindowsReport(await readJson(join(directory, 'windows-verification.json')), platformAssets(readRelease(tag).assets, 'windows'), manifest.sourceSha);
const acceptance = await readJson(acceptancePath);
validateAcceptance(acceptance, manifest);
const expected = await Promise.all(sealedNames.map(async name => ({ name, sha256: await sha256(join(directory, name)) })));
expected.push({ name: 'windows-acceptance.json', sha256: await sha256(acceptancePath) });
await uploadFile(tag, acceptancePath);
// Re-read remote hashes immediately before making the reviewed candidate public.
const current = readRelease(tag);
assertDraft(current);
validateReleaseSnapshot(current.assets, expected);
if (tagSha(tag) !== manifest.sourceSha) throw new Error('Remote release tag moved during acceptance.');
run('gh', ['release', 'edit', tag, '--repo', repository, '--draft=false', '--latest', '--notes-file', notesPath]);
console.log(`Published verified release ${tag}.`);
