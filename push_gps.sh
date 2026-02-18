#!/bin/bash
# Script to push GPS display folder to GPS branch
# This script syncs only the gps/ folder to the GPS remote branch

set -e

echo "Syncing GPS display folder..."
echo ""

# Verify gps folder exists
if [ ! -d "gps" ]; then
    echo "Error: 'gps' folder does not exist"
    exit 1
fi

# Verify GPS branch exists
if ! git rev-parse --verify "GPS" &>/dev/null; then
    echo "Error: 'GPS' branch does not exist locally"
    echo "Try: git fetch origin GPS:GPS"
    exit 1
fi

# Get current branch
current_branch=$(git rev-parse --abbrev-ref HEAD)

# Create a temporary split branch from gps folder
echo "Preparing GPS branch for sync..."
git subtree split --prefix=gps -b gps-sync-temp --quiet

# Force push to remote GPS branch
echo "Pushing gps folder to origin/GPS..."
if git push origin gps-sync-temp:GPS --force --quiet; then
    echo "✓ Successfully pushed to GPS branch"
else
    echo "✗ Failed to push to GPS branch"
    git branch -D gps-sync-temp 2>/dev/null || true
    exit 1
fi

# Cleanup
git branch -D gps-sync-temp 2>/dev/null || true

echo ""
echo "GPS sync complete!"
echo "Remote GPS branch updated with contents from gps/"
git log --oneline -1 origin/GPS | sed 's/^/  Latest: /'
