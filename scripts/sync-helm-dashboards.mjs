import { mkdir, readdir, readFile, rm, writeFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const sourceDir = path.join(repoRoot, 'deploy', 'grafana')
const targetDir = path.join(repoRoot, 'deploy', 'helm', 'fibron', 'dashboards')

async function listJsonFiles(dir) {
  try {
    const entries = await readdir(dir, { withFileTypes: true })
    return entries
      .filter((entry) => entry.isFile() && entry.name.endsWith('.json'))
      .map((entry) => entry.name)
      .sort()
  } catch (error) {
    if (error && typeof error === 'object' && 'code' in error && error.code === 'ENOENT') {
      return []
    }
    throw error
  }
}

async function main() {
  const checkOnly = process.argv.includes('--check')
  const sourceFiles = await listJsonFiles(sourceDir)
  if (sourceFiles.length === 0) {
    throw new Error(`No Grafana dashboard JSON files found under ${sourceDir}`)
  }

  const targetFiles = await listJsonFiles(targetDir)
  const drift = []

  if (!checkOnly) {
    await mkdir(targetDir, { recursive: true })
  }

  for (const filename of sourceFiles) {
    const srcPath = path.join(sourceDir, filename)
    const dstPath = path.join(targetDir, filename)
    const expected = await readFile(srcPath, 'utf8')
    let actual = null
    try {
      actual = await readFile(dstPath, 'utf8')
    } catch (error) {
      if (!(error && typeof error === 'object' && 'code' in error && error.code === 'ENOENT')) {
        throw error
      }
    }
    if (actual !== expected) {
      drift.push(filename)
      if (!checkOnly) {
        await writeFile(dstPath, expected, 'utf8')
      }
    }
  }

  const sourceSet = new Set(sourceFiles)
  for (const filename of targetFiles) {
    if (!sourceSet.has(filename)) {
      drift.push(`extra:${filename}`)
      if (!checkOnly) {
        await rm(path.join(targetDir, filename))
      }
    }
  }

  if (checkOnly && drift.length > 0) {
    console.error(
      `Helm Grafana dashboards are out of sync with deploy/grafana/: ${drift.join(', ')}\n` +
        'Run `npm run helm:sync-dashboards` to update them.',
    )
    process.exitCode = 1
    return
  }

  const mode = checkOnly ? 'verified' : 'synced'
  console.log(`Helm Grafana dashboards ${mode} (${sourceFiles.length} files).`)
}

await main()
