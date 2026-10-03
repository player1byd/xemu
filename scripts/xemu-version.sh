#!/bin/bash

set -eu

dir="$1"
XEMU_DATE=$(date -u)
XEMU_COMMIT=$( \
  cd "$dir"; \
  if test -e .git; then \
    git rev-parse HEAD 2>/dev/null | tr -d '\n'; \
  elif test -e XEMU_COMMIT; then \
    cat XEMU_COMMIT; \
  fi)
XEMU_VERSION=$( \
  cd "$dir"; \
  if test -e .git; then \
    git describe --tags --match 'v[0-9]*' 2>/dev/null | cut -c 2- | tr -d '\n'; \
  fi) || :

# A test prerelease tag can be an ancestor of a later source commit. Its
# git-describe suffix is not a product version; use the checked-in base then.
if [[ ! "$XEMU_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+-g[0-9a-f]+|-0-unofficial-[0-9a-f]+)?$ ]]; then
  XEMU_VERSION=""
fi

# Release worktrees may contain the exact source commit without its upstream
# version tags. Keep the upstream base version in the source tree so such a
# build cannot silently identify itself as 0.0.0.
version_from_file=false
if [[ -z "$XEMU_VERSION" && -f "$dir/XEMU_VERSION" ]]; then
  XEMU_VERSION=$(cat "$dir/XEMU_VERSION")
  version_from_file=true
fi
if [[ ! "$XEMU_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+-g[0-9a-f]+|-0-unofficial-[0-9a-f]+)?$ ]]; then
  echo "xemu: no valid version tag or XEMU_VERSION file" >&2
  exit 1
fi

# A plain base version from the file needs the product-code commit in its
# visible version string. The numeric fourth Windows version field uses the
# first 16 commit bits when a tag-relative commit count is unavailable.
commit_component=0
if $version_from_file && [[ "$XEMU_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] &&
   [[ "$XEMU_COMMIT" =~ ^[0-9a-f]{12,}$ ]]; then
  XEMU_VERSION="${XEMU_VERSION}-0-g${XEMU_COMMIT:0:12}"
  commit_component=$(printf '%d' "0x${XEMU_COMMIT:0:4}")
fi

get_version_field() {
  echo ${XEMU_VERSION}-0 | cut -d- -f$1
}

get_version_dot () {
  echo $(get_version_field 1) | cut -d. -f$1
}

XEMU_VERSION_MAJOR=$(get_version_dot 1)
XEMU_VERSION_MINOR=$(get_version_dot 2)
XEMU_VERSION_PATCH=$(get_version_dot 3)
if (( commit_component == 0 )); then
  commit_component=$(get_version_field 2)
fi

cat <<EOF
#define XEMU_VERSION       "$XEMU_VERSION"
#define XEMU_VERSION_MAJOR $XEMU_VERSION_MAJOR
#define XEMU_VERSION_MINOR $XEMU_VERSION_MINOR
#define XEMU_VERSION_PATCH $XEMU_VERSION_PATCH
#define XEMU_VERSION_COMMIT $commit_component
#define XEMU_COMMIT        "$XEMU_COMMIT"
#define XEMU_DATE          "$XEMU_DATE"
EOF
