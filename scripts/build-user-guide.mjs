import { readFile, mkdir, writeFile } from 'node:fs/promises'
import path from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'

import { Marked } from 'marked'
import * as prettier from 'prettier'

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const guideRoot = path.join(repositoryRoot, 'docs', 'user-guide')
const outputRoot = path.join(guideRoot, 'html')
const checkOnly = process.argv.includes('--check')

const documents = [
  ['zh-CN', 'routing'],
  ['zh-CN', 'script-routing'],
  ['zh-CN', 'script-reference'],
  ['en', 'routing'],
  ['en', 'script-routing'],
  ['en', 'script-reference'],
]

function escapeAttribute(value) {
  return value
    .replaceAll('&', '&amp;')
    .replaceAll('"', '&quot;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
}

function slugify(value) {
  return (
    value
      .normalize('NFKC')
      .toLocaleLowerCase('en-US')
      .replaceAll(/[^\p{Letter}\p{Number}]+/gu, '-')
      .replaceAll(/^-+|-+$/gu, '') || 'section'
  )
}

async function renderDocument(source, sourceLabel) {
  const marked = new Marked({ gfm: true })
  const usedSlugs = new Map()

  marked.use({
    renderer: {
      heading({ tokens, text, depth }) {
        const baseSlug = slugify(text)
        const occurrence = (usedSlugs.get(baseSlug) ?? 0) + 1
        usedSlugs.set(baseSlug, occurrence)
        const slug = occurrence === 1 ? baseSlug : `${baseSlug}-${occurrence}`
        const content = this.parser.parseInline(tokens)
        const label = escapeAttribute(`Link to ${text}`)
        return `<h${depth} id="${escapeAttribute(slug)}">${content}<a class="documentation-heading-anchor" href="#${escapeAttribute(slug)}" aria-label="${label}">#</a></h${depth}>\n`
      },
      html({ text }) {
        return `<pre class="documentation-escaped-html"><code>${escapeAttribute(text)}</code></pre>\n`
      },
    },
    walkTokens(token) {
      if (token.type !== 'link') return
      const href = token.href.trim()
      if (/^(?:javascript|data|vbscript):/iu.test(href)) {
        throw new Error(`Unsafe link in ${sourceLabel}: ${href}`)
      }
    },
  })

  const rendered = await marked.parse(source)
  const notice = `<!-- Generated from ${sourceLabel}; run npm run docs:build. -->`
  return prettier.format(`${notice}\n${rendered}`, {
    parser: 'html',
    printWidth: 100,
    singleAttributePerLine: false,
  })
}

const staleFiles = []

for (const [language, name] of documents) {
  const sourcePath = path.join(guideRoot, language, `${name}.md`)
  const outputPath = path.join(outputRoot, language, `${name}.html`)
  const sourceLabel = path.relative(repositoryRoot, sourcePath)
  const source = await readFile(sourcePath, 'utf8')
  const rendered = await renderDocument(source, sourceLabel)

  if (checkOnly) {
    const existing = await readFile(outputPath, 'utf8').catch(() => null)
    if (existing !== rendered) staleFiles.push(path.relative(repositoryRoot, outputPath))
    continue
  }

  await mkdir(path.dirname(outputPath), { recursive: true })
  await writeFile(outputPath, rendered)
}

if (staleFiles.length > 0) {
  process.stderr.write(
    `Generated user-guide HTML is stale:\n${staleFiles.map((file) => `- ${file}`).join('\n')}\nRun npm run docs:build.\n`,
  )
  process.exitCode = 1
} else if (!checkOnly) {
  process.stdout.write(`Generated ${documents.length} user-guide HTML fragments.\n`)
}
