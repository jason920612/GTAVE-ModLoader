#!/bin/bash
# Retarget every clip of a CMU subject (WSL, ~/bball-env). usage: retarget_all.sh <model.xml> <mocap dir> <subject>
cd "$(dirname "$0")/../.."
model=$1 dir=$2 subject=$3
for amc in "$dir"/"$subject"_*.amc; do
    out="${amc%.amc}.npz"
    echo "== $(basename "$amc")"
    ~/bball-env/bin/python -u tools/train/retarget.py "$model" "$dir/$subject.asf" "$amc" "$out" 2>&1 | grep -v -i warp
done
