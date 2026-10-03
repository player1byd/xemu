#!/bin/bash

set -eu

generator=$1
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

fail()
{
    echo "test-xemu-version: $*" >&2
    exit 1
}

expect_define()
{
    output=$1
    name=$2
    value=$3

    awk -v name="$name" -v value="$value" \
        '$1 == "#define" && $2 == name && $3 == value { found = 1 }
         END { exit !found }' <<<"$output" ||
        fail "missing $name $value"
}

make_repo()
{
    repo=$1
    mkdir -p "$repo"
    git -C "$repo" init -q
    git -C "$repo" config user.name "xemu version test"
    git -C "$repo" config user.email "xemu-version-test@example.invalid"
    printf 'source\n' > "$repo/source"
    git -C "$repo" add source
    git -C "$repo" commit -q -m initial
}

repo=$tmpdir/repo
make_repo "$repo"
printf '1.2.3\n' > "$repo/XEMU_VERSION"
commit=$(git -C "$repo" rev-parse HEAD)
output=$("$generator" "$repo")
expect_define "$output" XEMU_VERSION "\"1.2.3-0-g${commit:0:12}\""
expect_define "$output" XEMU_COMMIT "\"$commit\""

git -C "$repo" tag v2.3.4
output=$("$generator" "$repo")
expect_define "$output" XEMU_VERSION '"2.3.4"'

printf 'next\n' >> "$repo/source"
git -C "$repo" commit -qam next
commit=$(git -C "$repo" rev-parse HEAD)
output=$("$generator" "$repo")
expect_define "$output" XEMU_VERSION "\"2.3.4-1-g${commit:0:7}\""

worktree=$tmpdir/worktree
git -C "$repo" worktree add -q --detach "$worktree"
output=$("$generator" "$worktree")
expect_define "$output" XEMU_COMMIT "\"$commit\""

git -C "$repo" tag v0.0.0-pr264-example
printf 'another change\n' >> "$repo/source"
git -C "$repo" commit -qam after-prerelease
commit=$(git -C "$repo" rev-parse HEAD)
output=$("$generator" "$repo")
expect_define "$output" XEMU_VERSION "\"1.2.3-0-g${commit:0:12}\""

archive=$tmpdir/archive
mkdir "$archive"
printf '3.4.5\n' > "$archive/XEMU_VERSION"
printf '%s\n' 1234567890abcdef1234567890abcdef12345678 > "$archive/XEMU_COMMIT"
output=$("$generator" "$archive")
expect_define "$output" XEMU_VERSION '"3.4.5-0-g1234567890ab"'
expect_define "$output" XEMU_VERSION_COMMIT 4660

rm "$archive/XEMU_COMMIT"
output=$("$generator" "$archive")
expect_define "$output" XEMU_VERSION '"3.4.5"'
expect_define "$output" XEMU_COMMIT '""'

printf '4.5.6' > "$archive/XEMU_VERSION"
output=$("$generator" "$archive")
expect_define "$output" XEMU_VERSION '"4.5.6"'

printf 'invalid\n' > "$archive/XEMU_VERSION"
if "$generator" "$archive" >"$tmpdir/invalid.out" 2>"$tmpdir/invalid.err"; then
    fail "invalid metadata succeeded"
fi
grep -Fq 'no valid version tag or XEMU_VERSION file' "$tmpdir/invalid.err" ||
    fail "invalid metadata did not report its error"

rm "$archive/XEMU_VERSION"
if "$generator" "$archive" >"$tmpdir/missing.out" 2>"$tmpdir/missing.err"; then
    fail "missing metadata succeeded"
fi
grep -Fq 'no valid version tag or XEMU_VERSION file' "$tmpdir/missing.err" ||
    fail "missing metadata did not report its error"
