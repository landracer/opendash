#!/bin/bash
# Script to push the synchronized feature branches to remote repository
# Run this script with appropriate GitHub credentials

set -e

echo "Pushing synchronized feature branches..."

# Push Center branch
echo "Pushing Center branch (commit: 2b45429)..."
git push origin Center:Center

# Push GPS branch  
echo "Pushing GPS branch (commit: 11dbbe8)..."
git push origin GPS:GPS

# Push Left branch
echo "Pushing Left branch (commit: 49bb856)..."
git push origin Left:Left

# Push Right branch
echo "Pushing Right branch (commit: 9585267)..."
git push origin Right:Right

echo "All feature branches have been pushed successfully!"
echo ""
echo "Verification:"
git --no-pager log --all --decorate --oneline --graph -15
