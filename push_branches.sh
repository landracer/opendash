#!/bin/bash
# Script to push the synchronized feature branches to remote repository
# Run this script with appropriate GitHub credentials

set -e

echo "Pushing synchronized feature branches..."
echo ""

# Define branches to push
branches=("Center" "GPS" "Left" "Right")

# Verify branches exist locally
for branch in "${branches[@]}"; do
    if ! git rev-parse --verify "$branch" &>/dev/null; then
        echo "Error: Branch '$branch' does not exist locally"
        exit 1
    fi
done

# Push each branch
for branch in "${branches[@]}"; do
    commit_hash=$(git rev-parse --short "$branch")
    echo "Pushing $branch branch (commit: $commit_hash)..."
    if git push origin "$branch:$branch"; then
        echo "✓ $branch branch pushed successfully"
    else
        echo "✗ Failed to push $branch branch"
        exit 1
    fi
    echo ""
done

echo "All feature branches have been pushed successfully!"
echo ""
echo "Verification:"
git --no-pager log --all --decorate --oneline --graph -15
