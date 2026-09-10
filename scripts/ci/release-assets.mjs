import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, join } from 'node:path';
import { checkoutSha, ensureDraft, sha256, uploadFile } from './release-github.mjs';
import { platformAssets, platformPatterns, readJson, validateWindowsReport } from './release-integrity.mjs';

const [command, tag, platform, ...files] = process.argv.slice(2);
if (!['draft', 'upload'].includes(command)) throw new Error('Expected draft or upload command.');
ensureDraft(tag);
if (command === 'draft') {
  console.log(`Draft ${tag} is ready for verified build assets.`);
} else if (command === 'upload') {
  if (!platformPatterns[platform] || !files.length) throw new Error('Platform and explicit package paths required.');
  const entries = await Promise.all(files.map(async file => ({ name: basename(file), sha256: await sha256(file) })));
  if (new Set(entries.map(file => file.name)).size !== entries.length) throw new Error('Duplicate asset names.');
  const assets = entries.map(file => ({ ...file, digest: `sha256:${file.sha256}` }));
  const binaries = platformAssets(assets, platform);
  if (platform === 'windows') {
    const reportPath = files.find(file => basename(file) === 'windows-verification.json');
    if (!reportPath) throw new Error('Windows installed-package verification report required.');
    validateWindowsReport(await readJson(reportPath), binaries, checkoutSha());
  }
  const expectedCount = platform === 'windows' ? 3 : 1;
  if (files.length !== expectedCount) throw new Error('Unexpected files in platform upload.');
  const receipt = { platform, sourceSha: checkoutSha(), files: entries };
  const directory = await mkdtemp(join(tmpdir(), 'strata-receipt-'));
  const receiptPath = join(directory, `build-${platform}.json`);
  await writeFile(receiptPath, `${JSON.stringify(receipt, null, 2)}\n`);
  for (const file of files) await uploadFile(tag, file);
  // A receipt is published last, so incomplete platform uploads cannot be sealed.
  await uploadFile(tag, receiptPath);
} else {
  throw new Error('Expected draft or upload command.');
}
