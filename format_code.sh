#!/usr/bin/env bash

set -euo pipefail

FORMAT_ALL=0
CLANG_FORMAT_BIN="${CLANG_FORMAT_BIN:-}"
CODE_PATHS=(
    "*.c"
    "*.cc"
    "*.cpp"
    "*.cxx"
    "*.h"
    "*.hh"
    "*.hpp"
    "*.hxx"
    ":(glob,exclude)**/third_party/**"
    ":(glob,exclude)**/3rdparty/**"
    ":(glob,exclude)**/vendor/**"
    ":(glob,exclude)**/external/**"
    ":(glob,exclude)**/deps/**"
)

usage() {
    cat <<'EOF'
Usage: ./format_code.sh [-a]

Formats changed repository-owned C/C++ files by default.

Options:
  -a    Format all repository-owned C/C++ files, including unchanged files.

Environment:
  CLANG_FORMAT_BIN    Explicit clang-format executable. When unset, the
                      newest clang-format available on PATH is used, matching
                      the pinned submodule's format_code.sh.
EOF
}

while getopts ":ah" opt; do
    case "${opt}" in
        a)
            FORMAT_ALL=1
            ;;
        h)
            usage
            exit 0
            ;;
        \?)
            echo "format_code.sh: unknown option: -${OPTARG}" >&2
            usage >&2
            exit 2
            ;;
    esac
done
shift $((OPTIND - 1))

if (( $# != 0 )); then
    echo "format_code.sh: unexpected argument: $1" >&2
    usage >&2
    exit 2
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "${REPO_ROOT}"

# Resolve the clang-format binary, mirroring the pinned submodule's
# format_code.sh: prefer the newest version-suffixed binary on PATH and fall
# back to a bare `clang-format`. Candidates are probed so a broken binary (for
# example a node_modules binary built for a newer glibc) is skipped instead of
# failing startup. CLANG_FORMAT_BIN overrides the search.
declare -a candidates=()
if [[ -n "${CLANG_FORMAT_BIN}" ]]; then
    candidates+=("${CLANG_FORMAT_BIN}")
fi
if [[ -x "${REPO_ROOT}/node_modules/.bin/clang-format" ]]; then
    candidates+=("${REPO_ROOT}/node_modules/.bin/clang-format")
fi
for version in 22 21 20 19 18 17; do
    if command -v "clang-format-${version}" >/dev/null 2>&1; then
        candidates+=("clang-format-${version}")
    fi
done
if command -v clang-format >/dev/null 2>&1; then
    candidates+=("clang-format")
fi

CLANG_FORMAT_BIN=""
version_output=""
for candidate in "${candidates[@]}"; do
    if output="$("${candidate}" --version 2>&1)"; then
        CLANG_FORMAT_BIN="${candidate}"
        version_output="${output}"
        break
    fi
done

if [[ -z "${CLANG_FORMAT_BIN}" ]]; then
    echo "format_code.sh: no clang-format binary found on PATH; run npm install or set CLANG_FORMAT_BIN" >&2
    exit 1
fi

STYLE_FILE="${REPO_ROOT}/third_party/fiber-gateway-cpp/.clang-format"
if [[ ! -f "${STYLE_FILE}" ]]; then
    echo "format_code.sh: missing pinned Fiber style: ${STYLE_FILE}" >&2
    exit 1
fi

echo "format_code.sh: using ${version_output}"

declare -A seen=()
files=()

add_file() {
    local path="$1"

    if [[ -f "${path}" ]] && [[ -z "${seen[${path}]+x}" ]]; then
        seen["${path}"]=1
        files+=("${path}")
    fi
}

if (( FORMAT_ALL == 1 )); then
    while IFS= read -r -d '' path; do
        add_file "${path}"
    done < <(git ls-files -z -- "${CODE_PATHS[@]}")
else
    while IFS= read -r -d '' path; do
        add_file "${path}"
    done < <(git diff --name-only -z --diff-filter=ACMRT HEAD -- "${CODE_PATHS[@]}")
fi

while IFS= read -r -d '' path; do
    add_file "${path}"
done < <(git ls-files --others --exclude-standard -z -- "${CODE_PATHS[@]}")

if (( ${#files[@]} == 0 )); then
    if (( FORMAT_ALL == 1 )); then
        echo "format_code.sh: no repository-owned C/C++ files to format"
    else
        echo "format_code.sh: no changed repository-owned C/C++ files to format"
    fi
    exit 0
fi

if (( FORMAT_ALL == 1 )); then
    printf 'format_code.sh: formatting %d repository-owned C/C++ file(s)\n' "${#files[@]}"
else
    printf 'format_code.sh: formatting %d changed repository-owned C/C++ file(s)\n' "${#files[@]}"
fi
printf '  %s\n' "${files[@]}"

# Record content hashes before formatting so the run can report exactly which
# files clang-format actually modified (many files pass through unchanged).
declare -A hashes=()
for path in "${files[@]}"; do
    hashes["${path}"]="$(sha256sum "${path}" | awk '{print $1}')"
done

"${CLANG_FORMAT_BIN}" -i --style="file:${STYLE_FILE}" -- "${files[@]}"

modified=()
for path in "${files[@]}"; do
    if [[ "$(sha256sum "${path}" | awk '{print $1}')" != "${hashes["${path}"]}" ]]; then
        modified+=("${path}")
    fi
done
if (( ${#modified[@]} > 0 )); then
    printf 'format_code.sh: %d file(s) changed by clang-format\n' "${#modified[@]}"
    printf '  %s\n' "${modified[@]}"
else
    echo 'format_code.sh: no files needed reformatting'
fi
