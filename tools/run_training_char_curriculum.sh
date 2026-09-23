#!/usr/bin/env bash
# Generic PursuitAI curriculum PPO trainer (Stage 2B et al.).
# MAP is a required positional arg, constrained to an allowlist so a typo or
# arbitrary string can NEVER point training at the city map or anything else.
# RUN_TAG alone decides every output root (isolation contract, same as the
# original smoke runner). Resume pre/post hard guards are preserved verbatim
# from run_training_char_stage1_smoke.bat.
#
# Usage:
#   bash tools/run_training_char_curriculum.sh <MAP> <STEPS> <SAVE_FREQ> <RUN_TAG> <RESUME_FROM> [--dry-run]
#
# Allowlist (curriculum only):
#   /Game/Maps/L_PursuitCharCurriculum
#   /Game/Maps/L_PursuitCharMoving025
set -u

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ" || exit 1
export NO_PROXY=127.0.0.1,localhost
export no_proxy=127.0.0.1,localhost
export PYTHONIOENCODING=utf-8

MAP="${1:-}"
STEPS="${2:-50000}"
SAVE_FREQ="${3:-10000}"
RUN_TAG="${4:-}"
RESUME_FROM="${5:-}"
DRY_RUN="${6:-}"

ALLOWED_MAPS="/Game/Maps/L_PursuitCharCurriculum /Game/Maps/L_PursuitCharMoving025 /Game/Maps/L_PursuitCharMoving050"
MAP_OK=0
for am in $ALLOWED_MAPS; do
  [ "$MAP" = "$am" ] && MAP_OK=1
done

SCHOLA="$PROJ/.venv/Scripts/schola.exe"
CKPT_DIR="$PROJ/checkpoints/$RUN_TAG"
TB_DIR="$PROJ/logs/tb/$RUN_TAG"
DRIVER="$PROJ/logs/${RUN_TAG}_driver.txt"
UELOG="$PROJ/logs/${RUN_TAG}_ue.log"
STAGED="$PROJ/Build/Staging/Windows/PursuitAI/Saved/Logs/PursuitAI.log"

echo "============================================================"
echo " PursuitAI curriculum PPO trainer"
echo " project     : $PROJ"
echo " map         : $MAP"
echo " run tag     : $RUN_TAG"
echo " steps       : $STEPS (additional when resuming)"
echo " save freq   : $SAVE_FREQ"
echo " resume from : $RESUME_FROM"
echo " checkpoint  : $CKPT_DIR"
echo " tensorboard : $TB_DIR"
echo " driver log  : $DRIVER"
echo " ue log copy : $UELOG"
echo "============================================================"

if [ "$MAP_OK" -ne 1 ]; then
  echo "[FATAL] MAP '$MAP' is not in the allowlist:" >&2
  echo "         $ALLOWED_MAPS" >&2
  echo "         Refusing to run (would point training at an unintended map)." >&2
  exit 4
fi

if [ -z "$RUN_TAG" ]; then
  echo "[FATAL] RUN_TAG is required (decides every output root)." >&2
  exit 1
fi

if [ -z "$RESUME_FROM" ]; then
  echo "[FATAL] RESUME_FROM is required. This runner only CONTINUES; it never" >&2
  echo "         trains from scratch (that would silently discard the static model)." >&2
  exit 1
fi

if [ ! -f "$RESUME_FROM" ]; then
  echo "[FATAL] resume target does not exist: $RESUME_FROM" >&2
  echo "         Refusing to run: schola would silently train from scratch." >&2
  exit 2
fi

if [ ! -f "$SCHOLA" ]; then
  echo "[FATAL] .venv schola missing: $SCHOLA" >&2
  exit 1
fi

# --- resolve the exact command so a dry-run shows what WOULD run ---
CMD=(
  "$SCHOLA" sb3 train ppo project "PursuitAI.uproject"
  --map "$MAP" --headless
  --build-dir "$PROJ/Build/Staging"
  --timesteps "$STEPS" --enable-tensorboard --log-dir "$TB_DIR"
  --save-final-policy --checkpoint-dir "$CKPT_DIR"
  --enable-checkpoints --save-freq "$SAVE_FREQ"
  --log-freq 1 --resume-from "$RESUME_FROM"
  --disable-eval --no-pbar
)

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "[DRY RUN] exact command:"
  printf '  %s\n' "${CMD[*]}"
  echo "[DRY RUN] nothing executed."
  exit 0
fi

mkdir -p "$CKPT_DIR" "$TB_DIR" "$PROJ/logs"

{
  echo "RUN_TAG=$RUN_TAG STEPS=$STEPS SAVE_FREQ=$SAVE_FREQ"
  echo "MAP=$MAP"
  echo "RESUME_FROM=$RESUME_FROM"
  echo "START_JST=$(date '+%Y-%m-%d %H:%M:%S')"
} > "$DRIVER"
T0=$(date +%s)

"${CMD[@]}" >> "$DRIVER" 2>&1
RC=$?

T1=$(date +%s)
{
  echo "TRAIN_EXIT=$RC"
  echo "WALL_SECONDS=$((T1 - T0))"
  echo "END_JST=$(date '+%Y-%m-%d %H:%M:%S')"
} >> "$DRIVER"

# --- resume post-guard: schola wraps load in try/except and only warns,
#     then silently trains from scratch. Fail hard so it cannot be missed. ---
if grep -q "Error loading model" "$DRIVER"; then
  echo "[FATAL] resume failed: driver log contains 'Error loading model'." >&2
  echo "         Schola fell back to Training-from-scratch. Discard this run." >&2
  exit 3
fi
if grep -q "Training from scratch" "$DRIVER"; then
  echo "[FATAL] resume failed: driver log contains 'Training from scratch'." >&2
  echo "         Discard this run." >&2
  exit 3
fi

if [ -f "$STAGED" ]; then
  cp "$STAGED" "$UELOG"
  echo "UE_LOG_COPIED=yes" >> "$DRIVER"
fi

echo "DONE" >> "$DRIVER"
echo "============================================================"
echo " Done. artifacts:"
echo "   final model : $CKPT_DIR/ppo_final.zip"
echo "   interval    : $CKPT_DIR/ppo_<N>_steps.zip"
echo "   tensorboard : $TB_DIR"
echo "   driver log  : $DRIVER"
echo "   ue log      : $UELOG"
echo "============================================================"
exit 0
