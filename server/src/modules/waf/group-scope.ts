// Which projects a WAF group reaches, and how that lands on the wire. Shared by
// validation (selector caps) and the generator (mount + mode gates).

import { type WafGroup, type WafPolicy } from './model.js'

// Mirrors star_glob in native/access-server/src/routing/EdgeFunctionsSnapshot.cpp:
// '*' matches any run, with backtracking; every other character is literal.
export function projectGlobMatches(pattern: string, project: string): boolean {
  let patternIndex = 0
  let projectIndex = 0
  let starIndex = -1
  let starProjectIndex = 0
  while (projectIndex < project.length) {
    if (pattern[patternIndex] === '*') {
      starIndex = patternIndex
      starProjectIndex = projectIndex
      patternIndex += 1
    } else if (patternIndex < pattern.length && pattern[patternIndex] === project[projectIndex]) {
      patternIndex += 1
      projectIndex += 1
    } else if (starIndex >= 0) {
      starProjectIndex += 1
      projectIndex = starProjectIndex
      patternIndex = starIndex + 1
    } else {
      return false
    }
  }
  while (pattern[patternIndex] === '*') {
    patternIndex += 1
  }
  return patternIndex === pattern.length
}

export function projectInGroup(group: WafGroup, project: string): boolean {
  return (
    group.projects.include.some((glob) => projectGlobMatches(glob, project)) &&
    !(group.projects.exempt ?? []).some(({ project: glob }) => projectGlobMatches(glob, project))
  )
}

export interface WafModeSets {
  readonly off: readonly string[]
  readonly log: readonly string[]
  readonly block: readonly string[]
}

// Overrides that reach this group, so changing a project's mode only touches
// the scripts of the groups it actually belongs to.
export function groupModeSets(policy: WafPolicy, group: WafGroup): WafModeSets {
  const sets: Record<keyof WafModeSets, string[]> = { off: [], log: [], block: [] }
  for (const [project, mode] of Object.entries(policy.projectModes)) {
    if (projectInGroup(group, project)) {
      sets[mode].push(project)
    }
  }
  return { off: sets.off.sort(), log: sets.log.sort(), block: sets.block.sort() }
}

export interface WafMountSelector {
  readonly include: readonly string[]
  readonly exclude: readonly string[]
}

export function groupMountSelector(group: WafGroup, sets: WafModeSets): WafMountSelector {
  const exempt = (group.projects.exempt ?? []).map(({ project }) => project)
  return {
    include: [...group.projects.include].sort(),
    exclude: [...new Set([...exempt, ...sets.off])].sort(),
  }
}
