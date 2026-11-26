#!/usr/bin/env bash
#
# Prints the CHANGELOG.md section for a version, without its heading.
#
#   scripts/release-notes.sh [VERSION] [--plain]
#
# VERSION defaults to the version in package.json. --plain drops Markdown
# bold markers, for the app stores, which show release notes as plain text.
# Exits with an error when the version has no section or an empty one.

set -euo pipefail

cd "$(dirname "$0")/.."

version=""
plain=false
for arg in "$@"; do
  case "$arg" in
    --plain) plain=true ;;
    *) version="$arg" ;;
  esac
done
if [[ -z "$version" ]]; then
  version="$(jq -r .version package.json)"
fi

# The lines between "## <version>" and the next "## " heading, with leading
# and trailing blank lines removed.
notes="$(awk -v version="$version" '
  /^## / { in_section = ($2 == version); next }
  in_section { print }
' CHANGELOG.md | sed -e '/./,$!d' | sed -e ':a' -e '/^\n*$/{$d;N;ba' -e '}')"

if [[ -z "$notes" ]]; then
  echo "CHANGELOG.md has no notes for version $version. Add a \"## $version\" section." >&2
  exit 1
fi

if [[ "$plain" == true ]]; then
  notes="${notes//\*\*/}"
fi
printf '%s\n' "$notes"
