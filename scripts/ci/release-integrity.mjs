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

export const acceptanceScenarios = ['install', 'launch', 'restart', 'portable', 'uninstall', 'upgrade-preserves-data', 'project', 'raster-vector', 'processing', 'pyqgis', 'opencl'];

export function validateAcceptance(report, manifest) {
  if (report.tag !== manifest.tag || report.sourceSha !== manifest.sourceSha) throw new Error('Acceptance refers to another release.');
  if (!report.tester?.trim() || !report.testedAt || !Number.isFinite(Date.parse(report.testedAt))) throw new Error('Acceptance must identify the tester and date.');
  if (!(report.windowsBuild >= 22000) || report.smartAppControl !== 'On' || report.strataCodeIntegrityBlocks !== 0) {
    throw new Error('Windows 11 with Smart App Control On and no Strata blocks is required.');
  }
  for (const scenario of acceptanceScenarios) {
    if (report.scenarios?.[scenario] !== 'passed') throw new Error(`Acceptance scenario incomplete: ${scenario}`);
  }
  for (const artifact of manifest.artifacts.filter(file => /-win64\.(exe|zip)$/.test(file.name))) {
    if (!report.packages?.some(file => file.name === artifact.name && file.sha256 === artifact.sha256)) throw new Error('Acceptance package hash mismatch.');
  }
  if (report.colleagueConfirmed !== true) throw new Error('The affected colleague has not confirmed resolution.');
}

export async function readJson(path) {
  return JSON.parse((await readFile(path, 'utf8')).replace(/^\uFEFF/, ''));
}
