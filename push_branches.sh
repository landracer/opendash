#!/bin/bash
# Script to push the synchronized feature branches to remote repository
# Run this script with appropriate GitHub credentials

set -e

echo "Pushing synchronized feature branches..."
echo ""

# Define branches to push (other than Center which comes from main)
feature_branches=("GPS" "Left" "Right")

# Verify main branch exists locally (will be pushed to Center)
if ! git rev-parse --verify "main" &>/dev/null; then
    echo "Error: Branch 'main' does not exist locally"
    exit 1
fi

# Verify feature branches exist locally
for branch in "${feature_branches[@]}"; do
    if ! git rev-parse --verify "$branch" &>/dev/null; then
        echo "Error: Branch '$branch' does not exist locally"
        exit 1
    fi
done

# Push main branch to Center
main_commit_hash=$(git rev-parse --short "main")
echo "Pushing main branch to Center (commit: $main_commit_hash)..."
if git push origin "main:Center"; then
    echo "✓ main branch pushed to Center successfully"
else
    echo "✗ Failed to push main branch to Center"
    exit 1
fi
echo ""

# Push each feature branch
for branch in "${feature_branches[@]}"; do
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
