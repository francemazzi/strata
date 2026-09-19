import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
const root = resolve(import.meta.dirname, '../..');
async function fixture(nsis) {
  const source = await mkdtemp(join(tmpdir(), 'strata cpack-'));
  await mkdir(join(source, 'cmake'));
  for (const name of ['StrataWindowsCodeSignPreBuild.cmake.in', 'StrataWindowsCodeSignPostBuild.cmake.in']) await copyFile(join(root, 'cmake', name), join(source, 'cmake', name));
  await writeFile(join(source, 'COPYING'), 'Test fixture license');
  await writeFile(join(source, 'README.md'), 'Packaging fixture');
  if (nsis) await writeFile(join(source, 'makensis.exe'), 'configuration-only stub');
  await writeFile(join(source, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.20)
project(StrataPackageTest NONE)
set(WIN32 TRUE)
set(UNIX FALSE)
set(CMAKE_SYSTEM_NAME Windows)
add_custom_target(qgis)
set(QGIS_APP_NAME Strata)
set(COMPLETE_VERSION 4.3.1)
set(CPACK_PACKAGE_VERSION_MAJOR 4)
set(CPACK_PACKAGE_VERSION_MINOR 3)
set(CPACK_PACKAGE_VERSION_PATCH 1)
set(STRATA_VERSION 1.4.4)
set(CREATE_NSIS TRUE CACHE BOOL "" FORCE)
set(CREATE_ZIP TRUE CACHE BOOL "" FORCE)
set(STRATA_WINDOWS_CODE_SIGN TRUE CACHE BOOL "" FORCE)
install(FILES README.md DESTINATION .)
include("${root.replaceAll('\\', '/')}/cmake/Bundle.cmake")
`);
  return source;
}
test('CPack uses the product version while retaining installation and ABI identity', async () => {
  const source = await fixture(true);
  try {
    const result = spawnSync('cmake', ['-S', source, '-B', join(source, 'build')], { encoding: 'utf8', env: { ...process.env, STRATA_NSIS_EXECUTABLE: join(source, 'makensis.exe') } });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    const config = await readFile(join(source, 'build/BundleConfig.cmake'), 'utf8');
    for (const [key, value] of [['CPACK_PACKAGE_VERSION', '1.4.4'], ['CPACK_PACKAGE_FILE_NAME', 'Strata-1.4.4-win64'], ['CPACK_PACKAGE_INSTALL_DIRECTORY', 'Strata 4.3.1'], ['CPACK_PACKAGE_INSTALL_REGISTRY_KEY', 'Strata 4.3.1'], ['CPACK_NSIS_DISPLAY_NAME', 'Strata 1.4.4']]) {
      assert.ok(config.includes(`set(${key} "${value}")`), `${key} does not match`);
    }
    assert.match(config, /!uninstfinalize/);
    assert.match(config, /sign-nsis-uninstaller\.cmd/);
    assert.match(config, /%1/);
    assert.doesNotMatch(config, /'"powershell/);
    assert.match(config, /CPACK_NSIS_EXECUTABLE_PRE_ARGUMENTS "\/DNSISDIR=/);
    const script = join(source, 'read-config.cmake');
    const restored = join(source, 'restored-command.txt');
    await writeFile(script, `include("${source.replaceAll('\\', '/')}/build/BundleConfig.cmake")
file(WRITE "${restored.replaceAll('\\', '/')}" "\${CPACK_NSIS_DEFINES}")
`);
    const reloaded = spawnSync('cmake', ['-P', script], { encoding: 'utf8' });
    assert.equal(reloaded.status, 0, reloaded.stdout + reloaded.stderr);
    assert.equal(await readFile(restored, 'utf8'), `!uninstfinalize '"${source.replaceAll('\\', '/')}/scripts/ci/sign-nsis-uninstaller.cmd" "%1"' = 0`);
  } finally { await rm(source, { recursive: true, force: true }); }
});
test('NSIS executes the finalizer with a complete uninstaller path', { skip: process.platform !== 'win32' }, async () => {
  assert.ok(process.env.STRATA_TEST_NSIS, 'Windows packaging tests require a real NSIS compiler');
  const source = await fixture(true);
  try {
    await mkdir(join(source, 'scripts/ci'), { recursive: true });
    await writeFile(join(source, 'scripts/ci/sign-nsis-uninstaller.cmd'), `@echo off\r\nif not exist "%~1" exit /b 42\r\n> "%~dp0finalizer-ran.txt" echo %~1\r\nexit /b 0\r\n`);
    const configure = spawnSync('cmake', ['-S', source, '-B', join(source, 'build')], { encoding: 'utf8', env: { ...process.env, STRATA_NSIS_EXECUTABLE: process.env.STRATA_TEST_NSIS } });
    assert.equal(configure.status, 0, configure.stdout + configure.stderr);
    const config = join(source, 'test-package.cmake');
    await writeFile(config, `include("${source.replaceAll('\\', '/')}/build/BundleConfig.cmake")
# Exercise the generated NSIS finalizer without Azure or production signatures.
set(CPACK_PRE_BUILD_SCRIPTS "")
set(CPACK_POST_BUILD_SCRIPTS "")
`);
    const packaged = spawnSync('cpack', ['--config', config, '-G', 'NSIS'], { cwd: join(source, 'build'), encoding: 'utf8', timeout: 120000 });
    assert.equal(packaged.status, 0, packaged.stdout + packaged.stderr);
    const invokedPath = (await readFile(join(source, 'scripts/ci/finalizer-ran.txt'), 'utf8')).trim();
    assert.match(invokedPath, /\.exe$/i);
    assert.doesNotMatch(invokedPath, /%1|[";]/);
  } finally { await rm(source, { recursive: true, force: true }); }
});
test('release packaging refuses a missing signed NSIS toolchain', async () => {
  const source = await fixture(false);
  try {
    const result = spawnSync('cmake', ['-S', source, '-B', join(source, 'build')], { encoding: 'utf8', env: { ...process.env, STRATA_NSIS_EXECUTABLE: '' } });
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /Signed NSIS plugin toolchain is required/);
  } finally { await rm(source, { recursive: true, force: true }); }
});
