#!/usr/bin/env python3
"""Stage 2B eval analyzer - merges the SB3 eval DRIVER log with the UE ENGINE log.

Why this exists (Stage 2B contract, per user instruction):

  * The SB3 eval driver prints one `Episode reward: R, Episode length: L` line per
    episode. Those lines are the AUTHORITATIVE episode count and the catch/timeout
    classification (reward is high on CAUGHT, low on TIMEOUT). Python side = authority
    on "how many episodes ran" and on the result tally.
  * The UE engine log (VALIDATION + EVADER lines) carries the BEHAVIOR metrics
    (distances, steps, dot(vel,target), jumps, wall hits, the target's own motion).
  * The two are aligned 1:1 by episode index (both are sequential, deterministic).
  * A UE log line lost on flush at process shutdown is NOT assumed TIMEOUT: the
    driver still carries that episode's reward, which is the real signal.

Warm-up: episode 1 is excluded from the formal tally; the formal window is
episode (warmup+1) .. N. The warm-up episode's own result is reported separately
so the reader can see whether excluding it changed anything.

Usage:
    python tools/stage2b_eval_analyze.py <driver_log> <ue_log> \
        [--warmup 1] [--json out.json]
"""

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stage0_measure import parse as parse_ue, percentile, stats  # reuse the UE parser

RE_REWARD = re.compile(r"Episode reward:\s*([-\d.eE+]+),\s*Episode length:\s*(\d+)")


def err_scan(*paths):
    """Count NaN / Inf / space-mismatch / gRPC error lines across both logs."""
    counts = {"NaN": 0, "Inf": 0, "space_mismatch": 0, "gRPC_error": 0, "gRPC": 0}
    pats = {
        "NaN": re.compile(r"\bNaN\b"),
        "Inf": re.compile(r"\b[-+]?Inf(?:inity)?\b"),
        "space_mismatch": re.compile(r"space\s*mismatch", re.IGNORECASE),
        "gRPC_error": re.compile(r"gRPC\s*(error|failure|exception)", re.IGNORECASE),
        "gRPC": re.compile(r"gRPC"),
    }
    for p in paths:
        if not os.path.isfile(p):
            continue
        with open(p, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                for key, rx in pats.items():
                    if rx.search(line):
                        counts[key] += 1
    return counts


def main():
    ap = argparse.ArgumentParser(description="Stage 2B eval analyzer (driver + UE)")
    ap.add_argument("driver_log")
    ap.add_argument("ue_log")
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    # --- Python-side rewards (authoritative count + catch classification) ---
    with open(args.driver_log, "r", encoding="utf-8", errors="replace") as fh:
        dlines = fh.readlines()
    rewards = []
    lengths = []
    for ln in dlines:
        m = RE_REWARD.search(ln)
        if m:
            rewards.append(float(m.group(1)))
            lengths.append(int(m.group(2)))
    n_python = len(rewards)

    # --- UE-side behavior ---
    ue = parse_ue(args.ue_log)
    ue_ep = {e["index"]: e for e in ue["episodes"]}
    ue_ev = {e["index"]: e for e in ue["evader_entries"]}
    ue_by_index = {i: e["result"] for i, e in ue_ep.items()}

    # --- threshold from UE-observed episodes (reward separates CAUGHT vs TIMEOUT) ---
    caught_r = [rewards[i - 1] for i, r in ue_by_index.items() if r == "CAUGHT" and 1 <= i <= n_python]
    timeout_r = [rewards[i - 1] for i, r in ue_by_index.items() if r == "TIMEOUT" and 1 <= i <= n_python]
    if caught_r and timeout_r:
        threshold = (min(caught_r) + max(timeout_r)) / 2.0
    elif caught_r:
        threshold = min(caught_r) - 1.0
    elif timeout_r:
        threshold = max(timeout_r) + 1.0
    else:
        threshold = 8.0

    cls = ["CAUGHT" if r >= threshold else "TIMEOUT" for r in rewards]

    # cross-validation: where UE text exists, does reward-class agree?
    mismatches = []
    for i, res in ue_by_index.items():
        if 1 <= i <= n_python and cls[i - 1] != res:
            mismatches.append((i, cls[i - 1], res))

    total = n_python
    formal_idx = list(range(args.warmup + 1, total + 1))
    formal_caught = sum(1 for i in formal_idx if cls[i - 1] == "CAUGHT")
    formal_total = len(formal_idx)
    rate = formal_caught / formal_total if formal_total else None

    warmup_caught = sum(1 for i in range(1, args.warmup + 1) if cls[i - 1] == "CAUGHT")

    # behavior metrics over formal episodes present in UE
    present = [i for i in formal_idx if i in ue_ep]
    missing_tail = [i for i in formal_idx if i not in ue_ep]
    sub = [ue_ep[i] for i in present]
    ev_sub = [ue_ev[i] for i in present if i in ue_ev]

    def st(values):
        return stats(values)

    beh = {}
    if sub:
        beh["start_d"] = st([e["start_d"] for e in sub])
        beh["end_d"] = st([e["end_d"] for e in sub])
        beh["closest_d"] = st([e["closest_d"] for e in sub])
        beh["steps_sampled"] = st([float(e["samples"]) for e in sub])
        beh["dot_vel_target"] = st([e["dot_vel_target"] for e in sub])
        beh["grounded_fraction"] = st([e["grounded"] for e in sub])
        beh["mean_speed_cm_s"] = st([e["mean_speed"] for e in sub])
        beh["jumps_total"] = sum(e["jumps"] for e in sub)
        beh["jumps_per_episode"] = st([float(e["jumps"]) for e in sub])
        beh["wall_hits_counted_total"] = sum(e["wall_hits_counted"] for e in sub)
        beh["wall_hits_counted_per_episode"] = st([float(e["wall_hits_counted"]) for e in sub])
    evader = {}
    if ev_sub:
        evader["disp_cm"] = st([e["disp"] for e in ev_sub])
        evader["path_cm"] = st([e["path"] for e in ev_sub])
        evader["mean_speed_cm_s"] = st([e["mean_speed"] for e in ev_sub])
        evader["max_speed_cm_s"] = st([e["max_speed"] for e in ev_sub])
        evader["moved_episodes"] = sum(1 for e in ev_sub if e["disp"] > 1.0)
        evader["moved_fraction"] = round(evader["moved_episodes"] / len(ev_sub), 4)
        evader["ratios_baked"] = sorted({round(e["ratio"], 4) for e in ev_sub})
        evader["static_flags"] = sorted({e["static"] for e in ev_sub})

    errs = err_scan(args.driver_log, args.ue_log)

    summary = {
        "n_episodes_python": n_python,
        "n_episodes_ue_present": len(ue_ep),
        "formal_window": "episode %d..%d" % (args.warmup + 1, total),
        "formal_total": formal_total,
        "warmup_episodes": list(range(1, args.warmup + 1)),
        "warmup_caught": warmup_caught,
        "warmup_total": args.warmup,
        "reward_threshold": round(threshold, 4),
        "caught_formal": formal_caught,
        "timeout_formal": formal_total - formal_caught,
        "catch_rate_formal": round(rate, 4) if rate is not None else None,
        "catch_rate_formal_pct": round(100.0 * rate, 2) if rate is not None else None,
        "classification_mismatches_vs_ue": mismatches,
        "ue_present_in_formal": len(present),
        "ue_missing_in_formal": missing_tail,
        "behavior": beh,
        "evader": evader,
        "interface_spaces": ue["interface_spaces"],
        "obs_dims_seen": sorted(set(ue["obs_dims"])),
        "obs_contract_violations": ue["obs_contract_violations"],
        "wall_class_breakdown": {
            name: sum(1 for s in ue["wall_class_samples"] if s["class"] == name)
            for name in sorted({s["class"] for s in ue["wall_class_samples"]})
        },
        "error_scan": errs,
    }

    gate = "PASS" if (rate is not None and rate >= 0.95) else "FAIL(<95%)"
    summary["gate"] = gate
    summary["train_triggered"] = bool(rate is not None and rate < 0.95)

    out = args.json or (args.driver_log + ".stage2b.json")
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2, ensure_ascii=False)

    print("=" * 74)
    print("STAGE 2B EVAL ANALYSIS")
    print("  python episodes (authoritative count) : %d" % n_python)
    print("  ue episodes present                    : %d" % len(ue_ep))
    print("  reward threshold (CAUGHT>=)            : %.4f" % threshold)
    print("  warm-up ep1 caught/total               : %d/%d" % (warmup_caught, args.warmup))
    print("-" * 74)
    print("  FORMAL window %s" % summary["formal_window"])
    print("  CAUGHT / TIMEOUT                       : %d / %d" % (formal_caught, formal_total - formal_caught))
    print("  catch rate (formal, excluded warm-up) : %.2f%%" % (100.0 * rate if rate is not None else float("nan")))
    print("  classification mismatches vs UE text   : %d" % len(mismatches))
    print("  ue behavior present / missing (formal): %d / %s" % (len(present), missing_tail))
    print("-" * 74)
    print("  behavior (formal, UE-present):")
    for k in ("start_d", "end_d", "closest_d", "steps_sampled", "dot_vel_target",
              "grounded_fraction", "mean_speed_cm_s", "jumps_per_episode",
              "wall_hits_counted_per_episode"):
        if k in beh and beh[k]:
            e = beh[k]
            print("    %-26s mean %.2f median %.2f p90 %.2f min %.2f max %.2f"
                  % (k, e["mean"], e["median"], e["p90"], e["min"], e["max"]))
    print("    jumps_total                          : %s" % beh.get("jumps_total"))
    print("    wall_hits_counted_total              : %s" % beh.get("wall_hits_counted_total"))
    print("  wall class breakdown                   : %s" % summary["wall_class_breakdown"])
    if evader:
        print("  evader (formal, UE-present):")
        for k in ("disp_cm", "path_cm", "mean_speed_cm_s", "max_speed_cm_s"):
            if k in evader and evader[k]:
                e = evader[k]
                print("    %-26s mean %.2f median %.2f min %.2f max %.2f"
                      % (k, e["mean"], e["median"], e["min"], e["max"]))
        print("    moved episodes                     : %s/%s (%.1f%%)"
              % (evader["moved_episodes"], len(ev_sub),
                 100.0 * evader["moved_fraction"] if evader["moved_fraction"] is not None else float("nan")))
        print("    ratio baked / static flags        : %s / %s" % (evader["ratios_baked"], evader["static_flags"]))
    print("  interface spaces                       : %s" % summary["interface_spaces"])
    print("  obs dims seen                         : %s" % summary["obs_dims_seen"])
    print("  obs contract violations               : %s" % (summary["obs_contract_violations"] or "none"))
    print("  error scan                            : %s" % errs)
    print("-" * 74)
    print("  GATE (>=95%% => no training): %s" % gate)
    print("  TRAIN_TRIGGERED: %s" % summary["train_triggered"])
    print("=" * 74)
    print("JSON written: %s" % os.path.abspath(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
