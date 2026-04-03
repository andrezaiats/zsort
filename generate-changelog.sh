#!/usr/bin/env bash
#
# generate-changelog.sh — builds CHANGELOG.md from Conventional Commits
#
# Usage:
#   ./generate-changelog.sh              # writes CHANGELOG.md
#   ./generate-changelog.sh --stdout     # prints to stdout
#

set -euo pipefail

output_file="CHANGELOG.md"
to_stdout=false

if [[ "${1:-}" == "--stdout" ]]; then
    to_stdout=true
fi

# Section order and commit type mapping
declare -A type_labels=(
    [feat]="Features"
    [fix]="Bug Fixes"
    [perf]="Performance"
    [refactor]="Refactoring"
    [docs]="Documentation"
    [build]="Build"
    [ci]="CI"
    [test]="Tests"
    [style]="Style"
    [chore]="Chores"
    [revert]="Reverts"
)

section_order=(feat fix perf refactor docs build ci test style chore revert)

# Collect all tags sorted by version, oldest first
mapfile -t tags < <(git tag --sort=version:refname 2>/dev/null)

# Build list of ranges: tag1..tag2, ..., lastTag..HEAD, and untagged (all commits if no tags)
ranges=()
range_labels=()

if [[ ${#tags[@]} -eq 0 ]]; then
    ranges+=("HEAD")
    range_labels+=("Unreleased")
else
    # Unreleased: latest tag..HEAD
    latest_tag="${tags[-1]}"
    unreleased_count=$(git rev-list --count "${latest_tag}..HEAD" 2>/dev/null || echo 0)
    if [[ "$unreleased_count" -gt 0 ]]; then
        ranges+=("${latest_tag}..HEAD")
        range_labels+=("Unreleased")
    fi

    # Tag ranges, newest first
    for ((i = ${#tags[@]} - 1; i >= 0; i--)); do
        tag="${tags[$i]}"
        tag_date=$(git log -1 --format='%Y-%m-%d' "$tag" 2>/dev/null)
        if [[ $i -eq 0 ]]; then
            ranges+=("$tag")
            range_labels+=("${tag} — ${tag_date}")
        else
            prev_tag="${tags[$((i - 1))]}"
            ranges+=("${prev_tag}..${tag}")
            range_labels+=("${tag} — ${tag_date}")
        fi
    done
fi

{
    echo "# Changelog"
    echo ""
    echo "All notable changes to this project will be documented in this file."
    echo "This file is auto-generated from [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/)."
    echo ""

    for idx in "${!ranges[@]}"; do
        range="${ranges[$idx]}"
        label="${range_labels[$idx]}"

        # Get commits for this range
        if [[ "$range" == *..* ]]; then
            mapfile -t commits < <(git log --format='%H' "$range" 2>/dev/null)
        else
            # Single tag or HEAD with no tags — all commits up to that ref
            mapfile -t commits < <(git log --format='%H' "$range" 2>/dev/null)
        fi

        [[ ${#commits[@]} -eq 0 ]] && continue

        # Collect breaking changes and typed commits
        declare -A sections=()
        breaking_lines=()

        for sha in "${commits[@]}"; do
            subject=$(git log -1 --format='%s' "$sha")
            body=$(git log -1 --format='%b' "$sha")
            short=$(git log -1 --format='%h' "$sha")

            # Check for BREAKING CHANGE in footer or ! in type
            is_breaking=false
            if echo "$subject" | grep -qE '^[a-z]+(\([^)]*\))?!:'; then
                is_breaking=true
            fi
            if echo "$body" | grep -qE '^BREAKING[ -]CHANGE:'; then
                is_breaking=true
            fi

            # Parse type and description
            if echo "$subject" | grep -qE '^(feat|fix|build|chore|ci|docs|style|refactor|perf|test|revert)(\([^)]*\))?!?: .+'; then
                type=$(echo "$subject" | sed -E 's/^([a-z]+)(\([^)]*\))?!?: .+/\1/')
                scope=$(echo "$subject" | sed -E 's/^[a-z]+(\(([^)]*)\))?!?: .+/\2/')
                desc=$(echo "$subject" | sed -E 's/^[a-z]+(\([^)]*\))?!?: (.+)/\2/')

                if [[ -n "$scope" ]]; then
                    line="- **${scope}:** ${desc} (\`${short}\`)"
                else
                    line="- ${desc} (\`${short}\`)"
                fi

                sections[$type]+="${line}"$'\n'
            fi

            if $is_breaking; then
                breaking_desc=$(echo "$body" | grep -oP '(?<=^BREAKING[ -]CHANGE: ).*' || true)
                if [[ -z "$breaking_desc" ]]; then
                    breaking_desc=$(echo "$subject" | sed -E 's/^[a-z]+(\([^)]*\))?!?: (.+)/\2/')
                fi
                breaking_lines+=("- ${breaking_desc} (\`${short}\`)")
            fi
        done

        # Only print section if there's content
        has_content=false
        if [[ ${#breaking_lines[@]} -gt 0 ]]; then
            has_content=true
        fi
        for type in "${section_order[@]}"; do
            if [[ -n "${sections[$type]:-}" ]]; then
                has_content=true
                break
            fi
        done

        if ! $has_content; then
            unset sections
            declare -A sections=()
            breaking_lines=()
            continue
        fi

        echo "## ${label}"
        echo ""

        if [[ ${#breaking_lines[@]} -gt 0 ]]; then
            echo "### BREAKING CHANGES"
            echo ""
            for bl in "${breaking_lines[@]}"; do
                echo "$bl"
            done
            echo ""
        fi

        for type in "${section_order[@]}"; do
            if [[ -n "${sections[$type]:-}" ]]; then
                echo "### ${type_labels[$type]}"
                echo ""
                echo -n "${sections[$type]}"
                echo ""
            fi
        done

        # Clean up for next iteration
        unset sections
        declare -A sections=()
        breaking_lines=()
    done
} > /tmp/changelog_output.md

if $to_stdout; then
    cat /tmp/changelog_output.md
else
    cp /tmp/changelog_output.md "$output_file"
    echo "CHANGELOG.md updated."
fi

rm -f /tmp/changelog_output.md
