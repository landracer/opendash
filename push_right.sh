#!/bin/bash
# Script to push Right display folder to Right branch
# This script syncs only the right/ folder to the Right remote branch

set -e

echo "Syncing Right display folder..."
echo ""

# Verify right folder exists
if [ ! -d "right" ]; then
    echo "Error: 'right' folder does not exist"
    exit 1
fi

# Verify Right branch exists
if ! git rev-parse --verify "Right" &>/dev/null; then
    echo "Error: 'Right' branch does not exist locally"
    echo "Try: git fetch origin Right:Right"
    exit 1
fi

# Get current branch
current_branch=$(git rev-parse --abbrev-ref HEAD)

# Create a temporary split branch from right folder
echo "Preparing Right branch for sync..."
git subtree split --prefix=right -b right-sync-temp --quiet

# Force push to remote Right branch
echo "Pushing right folder to origin/Right..."
if git push origin right-sync-temp:Right --force --quiet; then
    echo "✓ Successfully pushed to Right branch"
else
    echo "✗ Failed to push to Right branch"
    git branch -D right-sync-temp 2>/dev/null || true
    exit 1
fi

# Cleanup
git branch -D right-sync-temp 2>/dev/null || true

echo ""
echo "Right sync complete!"
echo "Remote Right branch updated with contents from right/"
git log --oneline -1 origin/Right | sed 's/^/  Latest: /'
