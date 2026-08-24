#!/usr/bin/env bash
# Shallow-clones a pinned tag of a third-party library into vendor/<name>,
# matching how vendor/cxxopt was added (see .agents/skills/vendor-import/SKILL.md).
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <name> <git-url> <tag>" >&2
    exit 2
fi

name="$1"
url="$2"
tag="$3"

repo_root="$(git rev-parse --show-toplevel)"
dest="$repo_root/vendor/$name"

if [ -e "$dest" ]; then
    echo "error: $dest already exists" >&2
    exit 1
fi

git clone --depth 1 --branch "$tag" "$url" "$dest"
echo "cloned $url@$tag into vendor/$name"
echo "next: write build/secondary/vendor/$name/BUILD.gn (see SKILL.md Step 3)"
