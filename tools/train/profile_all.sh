#!/bin/bash
# Profile a model at several batch sizes (WSL, ~/bball-env). usage: profile_all.sh <model.xml> <n> [n ...]
cd "$(dirname "$0")/../.."
model=$1
shift
for n in "$@"; do
    XLA_PYTHON_CLIENT_PREALLOCATE=false timeout 1200 ~/bball-env/bin/python -u tools/train/profile_step.py "$model" "$n" 2>&1 | grep -v -i warp
done
