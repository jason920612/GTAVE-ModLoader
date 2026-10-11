#!/bin/bash
# Long training run in the background with MuJoCo Warp's per-step solver notices filtered out of the log (WSL).
# usage: train_bg.sh <out dir> <steps> <clip.npz> [clip ...]
cd "$(dirname "$0")/../../.."
out=$1 steps=$2
shift 2
XLA_PYTHON_CLIENT_PREALLOCATE=false ~/loco-env/bin/python -u tools/train/loco/train.py "$@" --out "$out" --steps "$steps" --envs 2048 2>&1 \
    | grep --line-buffered -v "solver iterations limit\|warn_overflow" > "$out.log"
