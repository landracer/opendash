#!/bin/bash
# Script to push Center display folder to Center branch
# This script syncs the entire repository to the Center remote branch
# (Center branch mirrors main branch)

set -e

echo "Syncing Center display folder..."
echo ""

# Verify center folder exists
if [ ! -d "center" ]; then
    echo "Error: 'center' folder does not exist"
    exit 1
fi

# Verify Center branch exists
if ! git rev-parse --verify "Center" &>/dev/null; then
    echo "Error: 'Center' branch does not exist locally"
    echo "Try: git fetch origin Center:Center"
    exit 1
fi

# Get current branch
current_branch=$(git rev-parse --abbrev-ref HEAD)

# Create a temporary split branch from center folder
echo "Preparing Center branch for sync..."
git subtree split --prefix=center -b center-sync-temp --quiet

# Force push to remote Center branch
echo "Pushing center folder to origin/Center..."
if git push origin center-sync-temp:Center --force --quiet; then
    echo "✓ Successfully pushed to Center branch"
else
    echo "✗ Failed to push to Center branch"
    git branch -D center-sync-temp 2>/dev/null || true
    exit 1
fi

# Cleanup
git branch -D center-sync-temp 2>/dev/null || true

echo ""
echo "Center sync complete!"
echo "Remote Center branch updated with contents from center/"
git log --oneline -1 origin/Center | sed 's/^/  Latest: /'
