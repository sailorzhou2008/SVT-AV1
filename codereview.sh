#!/bin/bash

# git diff --name-only <commit compare1> <compare2>
# checkFiles=$(git diff --name-only HEAD~ HEAD)
# 
echo "ghprbActualCommit: ${ghprbActualCommit}";
echo "ghprbTargetBranch: ${ghprbTargetBranch}";

checkFiles=$(git diff --name-only ${ghprbActualCommit} ${ghprbTargetBranch})
echo $checkFiles

# cpplint check file type
CPPLINT_EXTENS=cc,cpp,h

# cpplint filter;  -xxx remove, +xxx add
CPPLINT_FILTER=-whitespace/line_length,-build/include_what_you_use,-readability/todo,-build/include,-build/header_guard

cpplint --extensions=$CPPLINT_EXTENS --filter=$CPPLINT_FILTER $checkFiles 2>&1 | tee cpplint-result.xml