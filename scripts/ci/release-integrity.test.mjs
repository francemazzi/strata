import test from 'node:test';
import assert from 'node:assert/strict';
import { assertDraft, assertUpload, releaseApiUrl, validateTag } from './release-github.mjs';
import { acceptanceScenarios, platformAssets, sealedAssetNames, validateAcceptance, validateReleaseSnapshot, validateWindowsReport } from './release-integrity.mjs';
const hash = 'a'.repeat(64);
const sourceSha = 'b'.repeat(40);
const artifacts = ['Strata-1.4.4-win64.exe', 'Strata-1.4.4-win64.zip'].map(name => ({ name, sha256: hash, digest: `sha256:${hash}` }));
function windowsReport() {
  const files = [{ path: 'bin/OpenCL.dll', sha256: hash, machine: '0x8664', signer: 'CN=Vendor', status: 'valid' }];
  return { result: 'passed', sourceSha, packages: artifacts, portable: structuredClone(files),
    installed: [...structuredClone(files), { path: 'Uninstall.exe', signer: 'CN=Company', status: 'valid' }],
    scenarios: ['signatures', 'payload-match', 'opencl-x64', 'portable-runtime', 'installed-runtime', 'uninstall'] };
}
function acceptance() {
  return { tag: 'strata-v1.4.4', sourceSha, tester: 'Tester', testedAt: '2026-09-10T12:00:00Z', windowsBuild: 26100,
    smartAppControl: 'On', strataCodeIntegrityBlocks: 0, packages: artifacts,
    colleagueConfirmed: true, scenarios: Object.fromEntries(acceptanceScenarios.map(name => [name, 'passed'])) };
}
const manifest = { tag: 'strata-v1.4.4', sourceSha, artifacts };
test('published releases cannot be changed even with identical bytes', () => {
  assert.throws(() => assertDraft({ draft: false }), /draft/);
  assert.throws(() => assertUpload({ draft: false, assets: artifacts }, artifacts[0].name, hash), /immutable/);
});
test('draft retry is idempotent only for identical bytes', () => {
  assert.equal(assertUpload({ draft: true, assets: artifacts }, artifacts[0].name, hash), false);
  assert.equal(assertUpload({ draft: true, assets: [] }, artifacts[0].name, hash), true);
  assert.throws(() => assertUpload({ draft: true, assets: artifacts }, artifacts[0].name, 'c'.repeat(64)), /replace/);
});
test('implicit, malformed and branch references cannot become release tags', () => {
  for (const tag of ['', undefined, 'master', 'latest', 'strata-v1.4.4/../master', 'strata-v1.4.4;echo']) assert.throws(() => validateTag(tag));
  assert.equal(validateTag('strata-v1.4.4'), 'strata-v1.4.4');
});
test('missing or ambiguous platform assets fail sealing', () => {
  assert.equal(platformAssets(artifacts, 'windows').length, 2);
  assert.throws(() => platformAssets([artifacts[0]], 'windows'));
  assert.throws(() => platformAssets([...artifacts, { name: 'Another-win64.exe' }], 'windows'));
});
test('Windows report requires the tested source and package bytes', () => {
  validateWindowsReport(windowsReport(), artifacts, sourceSha);
  assert.throws(() => validateWindowsReport(windowsReport(), artifacts, 'x'), /stale/);
  const report = windowsReport(); report.packages = [];
  assert.throws(() => validateWindowsReport(report, artifacts, sourceSha), /cover/);
});
test('OpenCL, every signature and uninstaller are mandatory', () => {
  for (const mutate of [r => r.portable[0].status = 'failed', r => r.portable[0].machine = '0x014c',
    r => r.installed.pop(), r => r.installed[0].signer = '', r => r.scenarios.pop()]) {
    const report = windowsReport(); mutate(report);
    assert.throws(() => validateWindowsReport(report, artifacts, sourceSha));
  }
});
test('publication accepts only complete Windows 11 SAC evidence', () => {
  validateAcceptance(acceptance(), manifest);
  for (const mutate of [r => r.smartAppControl = 'Off', r => r.windowsBuild = 19045,
    r => r.colleagueConfirmed = false, r => r.packages = [], r => r.strataCodeIntegrityBlocks = 1,
    r => r.scenarios.opencl = 'pending', r => r.sourceSha = 'old', r => r.tester = '']) {
    const report = acceptance(); mutate(report);
    assert.throws(() => validateAcceptance(report, manifest));
  }
});

test('publication checks the entire sealed inventory including signatures', () => {
  const full = { artifacts: [...artifacts, ...['Strata.dmg', 'Strata.AppImage', 'windows-verification.json',
    'build-windows.json', 'build-macos.json', 'build-linux.json'].map(name => ({ name, sha256: hash }))] };
  const names = sealedAssetNames(full);
  assert.equal(names.length, 19);
  const expected = names.map(name => ({ name, sha256: hash }));
  const remote = names.map(name => ({ name, digest: `sha256:${hash}` }));
  validateReleaseSnapshot(remote, expected);
  assert.throws(() => validateReleaseSnapshot([...remote, { name: 'untested.exe', digest: `sha256:${hash}` }], expected));
  const changed = structuredClone(remote);
  changed.find(file => file.name === 'release-manifest.json.sigstore.json').digest = `sha256:${'c'.repeat(64)}`;
  assert.throws(() => validateReleaseSnapshot(changed, expected));
  for (const files of [full.artifacts.slice(1), [...full.artifacts, full.artifacts[0]],
    [...full.artifacts, { name: 'unexpected.exe', sha256: hash }]]) {
    assert.throws(() => sealedAssetNames({ artifacts: files }));
  }
});

test('pending draft tags resolve through GitHub CLI and authentication failures stay fatal', () => {
  const url = 'https://api.github.com/repos/francemazzi/strata/releases/386172503';
  assert.equal(releaseApiUrl('strata-v1.4.4', (command, args) => {
    assert.equal(command, 'gh');
    assert.deepEqual(args.slice(0, 3), ['release', 'view', 'strata-v1.4.4']);
    return { status: 0, stdout: `${url}\n` };
  }), url);
  assert.equal(releaseApiUrl('strata-v1.4.4', () => ({ status: 1, stderr: 'release not found\n' })), null);
  assert.throws(() => releaseApiUrl('strata-v1.4.4', () => ({ status: 1, stderr: 'HTTP 401: Bad credentials' })), /Cannot inspect/);
  assert.throws(() => releaseApiUrl('strata-v1.4.4', () => ({ status: 0, stdout: 'https://example.com/release' })), /Unexpected/);
});
