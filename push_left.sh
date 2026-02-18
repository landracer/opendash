#!/bin/bash
# Script to push Left display folder to Left branch
# This script syncs only the left/ folder to the Left remote branch

set -e

echo "Syncing Left display folder..."
echo ""

# Verify left folder exists
if [ ! -d "left" ]; then
    echo "Error: 'left' folder does not exist"
    exit 1
fi

# Verify Left branch exists
if ! git rev-parse --verify "Left" &>/dev/null; then
    echo "Error: 'Left' branch does not exist locally"
    echo "Try: git fetch origin Left:Left"
    exit 1
fi

# Get current branch
current_branch=$(git rev-parse --abbrev-ref HEAD)

# Create a temporary split branch from left folder
echo "Preparing Left branch for sync..."
git subtree split --prefix=left -b left-sync-temp --quiet

# Force push to remote Left branch
echo "Pushing left folder to origin/Left..."
if git push origin left-sync-temp:Left --force --quiet; then
    echo "✓ Successfully pushed to Left branch"
else
    echo "✗ Failed to push to Left branch"
    git branch -D left-sync-temp 2>/dev/null || true
    exit 1
fi

# Cleanup
git branch -D left-sync-temp 2>/dev/null || true

echo ""
echo "Left sync complete!"
echo "Remote Left branch updated with contents from left/"
git log --oneline -1 origin/Left | sed 's/^/  Latest: /'
