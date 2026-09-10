import { readFile } from 'node:fs/promises';

export const platformPatterns = {
  windows: [/-win64\.exe$/, /-win64\.zip$/],
  macos: [/\.dmg$/],
  linux: [/\.AppImage$/],
};

export function platformAssets(assets, platform) {
  return platformPatterns[platform].map(pattern => {
    const matches = assets.filter(asset => pattern.test(asset.name));
    if (matches.length !== 1) throw new Error(`Expected one ${platform} asset matching ${pattern}.`);
    return matches[0];
  });
}

export function validateWindowsReport(report, assets, sourceSha) {
  if (report.result !== 'passed' || report.sourceSha !== sourceSha) throw new Error('Windows package verification is missing or stale.');
  for (const asset of assets) {
    if (!report.packages?.some(file => file.name === asset.name && `sha256:${file.sha256}` === asset.digest)) {
      throw new Error(`Windows report does not cover ${asset.name}.`);
    }
  }
  for (const tree of ['portable', 'installed']) {
    if (!report[tree]?.length || report[tree].some(file => file.status !== 'valid' || !file.signer)) throw new Error(`Invalid ${tree} signatures.`);
    if (!report[tree].some(file => /(^|\/)bin\/OpenCL\.dll$/i.test(file.path) && file.machine === '0x8664')) throw new Error(`Missing x64 OpenCL in ${tree}.`);
  }
  if (!report.installed.some(file => /(^|\/)Uninstall\.exe$/i.test(file.path))) throw new Error('Uninstaller verification missing.');
  for (const scenario of ['signatures', 'payload-match', 'opencl-x64', 'portable-runtime', 'installed-runtime', 'uninstall']) {
    if (!report.scenarios?.includes(scenario)) throw new Error(`Missing Windows scenario: ${scenario}`);
  }
}

export const acceptanceScenarios = ['install', 'launch', 'restart', 'portable', 'uninstall', 'upgrade-preserves-data', 'project', 'raster-vector', 'processing', 'pyqgis', 'opencl', 'opencl-gpu', 'opencl-no-platform'];

export function validateAcceptance(report, manifest) {
  if (report.tag !== manifest.tag || report.sourceSha !== manifest.sourceSha) throw new Error('Acceptance refers to another release.');
  if (!report.tester?.trim() || !report.testedAt || !Number.isFinite(Date.parse(report.testedAt))) throw new Error('Acceptance must identify the tester and date.');
  if (!(report.windowsBuild >= 22000) || report.smartAppControl !== 'On' || report.strataCodeIntegrityBlocks !== 0) {
    throw new Error('Windows 11 with Smart App Control On and no Strata blocks is required.');
  }
  for (const scenario of acceptanceScenarios) {
    if (report.scenarios?.[scenario] !== 'passed') throw new Error(`Acceptance scenario incomplete: ${scenario}`);
  }
  for (const artifact of platformAssets(manifest.artifacts, 'windows')) {
    if (!report.packages?.some(file => file.name === artifact.name && file.sha256 === artifact.sha256)) throw new Error('Acceptance package hash mismatch.');
  }
  if (report.colleagueConfirmed !== true) throw new Error('The affected colleague has not confirmed resolution.');
}

export function sealedAssetNames(manifest) {
  if (!Array.isArray(manifest.artifacts)) throw new Error('Missing sealed artifact inventory.');
  const names = manifest.artifacts.map(file => file.name);
  if (new Set(names).size !== names.length || manifest.artifacts.some(file =>
    !/^[A-Za-z0-9._-]+$/.test(file.name) || !/^[a-f0-9]{64}$/.test(file.sha256))) {
    throw new Error('Invalid sealed artifact inventory.');
  }
  const binaries = Object.keys(platformPatterns).flatMap(platform => platformAssets(manifest.artifacts, platform)).map(file => file.name);
  const receipts = ['windows-verification.json', ...Object.keys(platformPatterns).map(platform => `build-${platform}.json`)];
  if (names.length !== binaries.length + receipts.length || receipts.some(name => !names.includes(name))) {
    throw new Error('Unexpected or missing sealed artifacts.');
  }
  const targets = [...binaries, 'release-manifest.json'];
  return [...names, 'release-manifest.json', ...targets.flatMap(name => [`${name}.sha256`, `${name}.sigstore.json`])];
}

export function validateReleaseSnapshot(assets, expected) {
  if (assets.length !== expected.length || new Set(assets.map(asset => asset.name)).size !== assets.length ||
      expected.some(file => !assets.some(asset => asset.name === file.name && asset.digest === `sha256:${file.sha256}`))) {
    throw new Error('Remote release assets changed or include unverified files.');
  }
}

export async function readJson(path) {
  return JSON.parse((await readFile(path, 'utf8')).replace(/^\uFEFF/, ''));
}
