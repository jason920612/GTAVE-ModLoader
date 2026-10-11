#!/bin/bash
# Evaluate a policy from several start frames (WSL). usage: eval_starts.sh <model.xml> <clip.npz> <run dir> <frame> [frame ...]
cd "$(dirname "$0")/../.."
model=$1 clip=$2 run=$3
shift 3
for f in "$@"; do
    XLA_PYTHON_CLIENT_PREALLOCATE=false ~/bball-env/bin/python -u tools/train/imitate.py eval "$model" "$clip" "$run" "$f" 2>&1 | grep "rollout from"
done
