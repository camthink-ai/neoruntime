#!/usr/bin/env node
/**
 * Sync version from version.mk to package.json
 * This ensures package.json version stays in sync with the project version
 */

const fs = require('fs');
const path = require('path');

function readVersionFromMk() {
  const versionMkPath = path.resolve(__dirname, '../../version.mk');

  if (!fs.existsSync(versionMkPath)) {
    console.warn('Warning: version.mk not found, skipping version sync');
    return null;
  }

  try {
    const content = fs.readFileSync(versionMkPath, 'utf-8');

    // Try to read WEB_VERSION_OVERRIDE first
    const webVersionOverride = content
      .match(/WEB_VERSION_OVERRIDE\s*:?=\s*(\S+)/)?.[1]
      ?.trim();

    let version;
    if (
      webVersionOverride &&
      webVersionOverride !== '' &&
      webVersionOverride !== '$(VERSION)'
    ) {
      // Use WEB component override version
      version = webVersionOverride;
    } else {
      // Use main version
      const major = content.match(/VERSION_MAJOR\s*:?=\s*(\d+)/)?.[1] || '0';
      const minor = content.match(/VERSION_MINOR\s*:?=\s*(\d+)/)?.[1] || '0';
      const patch = content.match(/VERSION_PATCH\s*:?=\s*(\d+)/)?.[1] || '0';
      const build = content.match(/VERSION_BUILD\s*\??=\s*(\d+)/)?.[1] || '0';
      version = `${major}.${minor}.${patch}.${build}`;
    }

    // Handle suffix: check WEB_SUFFIX first, then VERSION_SUFFIX
    const webSuffix = content.match(/WEB_SUFFIX\s*:?=\s*(\S+)/)?.[1]?.trim();
    const mainSuffix = content
      .match(/VERSION_SUFFIX\s*:?=\s*(\S+)/)?.[1]
      ?.trim();

    let suffix = '';
    if (webSuffix !== undefined) {
      // WEB_SUFFIX is explicitly set
      suffix = webSuffix === 'NONE' || webSuffix === '' ? '' : webSuffix;
    } else if (mainSuffix && mainSuffix !== '') {
      // Use main suffix if WEB_SUFFIX not set
      suffix = mainSuffix;
    }

    // Append suffix if present
    return suffix ? `${version}-${suffix}` : version;
  } catch (error) {
    console.error('Error reading version.mk:', error.message);
    return null;
  }
}

function updatePackageJsonVersion(version) {
  const packageJsonPath = path.resolve(__dirname, '../package.json');

  if (!fs.existsSync(packageJsonPath)) {
    console.error('Error: package.json not found');
    return false;
  }

  try {
    const packageJson = JSON.parse(fs.readFileSync(packageJsonPath, 'utf-8'));
    const oldVersion = packageJson.version;

    if (oldVersion === version) {
      console.log(`✓ package.json version already up to date: ${version}`);
      return true;
    }

    packageJson.version = version;
    fs.writeFileSync(
      packageJsonPath,
      JSON.stringify(packageJson, null, 2) + '\n',
      'utf-8'
    );

    console.log(`✓ Updated package.json version: ${oldVersion} → ${version}`);
    return true;
  } catch (error) {
    console.error('Error updating package.json:', error.message);
    return false;
  }
}

function main() {
  console.log('Syncing version from version.mk to package.json...');

  const version = readVersionFromMk();
  if (!version) {
    console.warn('Warning: Could not read version from version.mk');
    process.exit(0); // Don't fail the build
  }

  const success = updatePackageJsonVersion(version);
  process.exit(success ? 0 : 1);
}

// Run if called directly
if (require.main === module) {
  main();
}

module.exports = { readVersionFromMk, updatePackageJsonVersion };
