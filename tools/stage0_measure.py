#!/usr/bin/env python3
"""Stage 0 / Stage 2A acceptance parser - ONE self-contained file, no third-party imports.

Why this file exists and why it is the only new measurement tool:

    The previous rounds judged runs from the TensorBoard reward curve and from ad-hoc
    scripts, and both were wrong in the same direction: reward is not catch rate, and a
    metric that no one re-derives goes stale the moment the reward coefficients change.
    This parser reads the ENGINE LOG only - the numbers the simulation itself printed,
    including the ones added in the Stage 0 patch and the evader census added for the
    Stage 2A moving-target stage - and derives the acceptance items from them. Nothing
    here talks to the trainer, so a run cannot "pass" because a policy looked good on a
    chart.

Items it reports (the acceptance list, in the order the reports use them):

    episode count        exactly N completed episodes (the -PursuitCharMaxEpisodes exit)
    catch rate           CAUGHT / (CAUGHT + TIMEOUT), plus the timeout split
    distance             start_d / end_d / closest_d: mean, median, min, max
    steps                per-episode step count (the VALIDATION `samples` field): mean,
                         median, p90, min, max
    locomotion           grounded fraction, mean speed cm/s, mean cos(vel, target)
    evader               the TARGET's own motion: end-to-start displacement, path length,
                         mean/peak speed, the baked EvaderSpeedRatio, and the fraction of
                         episodes whose evader actually moved (Stage 2A's hard evidence)
    jumps                actuator jumps per episode (0 when the gate is doing its job)
    wall hits            counted hits, the ignored tallies by class, sample class lines
    dims                 observation ndim set, action dims from the interface print
    walls                probe_summary non_clear / blocked readings
    reproducibility      sha256 of the per-episode spawn signature (same seed -> same digest)

Usage:

    python tools/stage0_measure.py <engine log> [--json out.json] [--label greedy]

    exit code 0 = parsed and at least one episode completed, 1 = no episode was parsed
    (either a failed run or a log this parser no longer understands - the two are
    distinguished by the PARSE WARNING line).

The JSON is written next to the log (or to --json) so the report can quote exact numbers
instead of transcribing them by hand.
"""

import argparse
import hashlib
import json
import os
import re
import statistics
import sys

# --- the log lines this parser understands -------------------------------------------
# Every pattern searches the whole line: UE prefixes them with [timestamp][frame]Logger:.

RE_VALIDATION = re.compile(
    r"episode (\d+) VALIDATION result=(CAUGHT|TIMEOUT) "
    r"start_d=(-?[\d.]+) end_d=(-?[\d.]+) closest_d=(-?[\d.]+) "
    r"grounded=([\d.]+) mean_speed=([\d.]+) dot_vel_target=(-?[\d.]+) jumps=(\d+) "
    r"wall_hits_counted=(\d+) "
    # the field was renamed ignored[...] -> ignored_total[...]; accept both so an
    # older log is never silently parsed as "zero episodes" (that failure mode is
    # indistinguishable from a genuinely empty run).
    r"ignored(?:_total)?\[floor=(\d+) support=(\d+) target=(\d+) self=(\d+)\] samples=(\d+)"
    r"(?: probe_non_clear=(\d+) probe_blocked=(\d+) probe_closest=([\d.]+))?"
)

# The evader's own motion, printed on its own line by LogEpisodeValidationSummary.
# `static=yes` with disp=path=0 is the Stage 0/1 result; `static=no` with a non-zero path
# is the Stage 2A one. Anchored loosely at the end so a future added field does not break
# the parse of the existing ones.
RE_EVADER = re.compile(
    r"episode (\d+) EVADER disp=(-?[\d.]+) path=(-?[\d.]+) mean_speed=(-?[\d.]+) "
    r"max_speed=(-?[\d.]+) ratio=(-?[\d.]+) static=(\w+) "
    r"start=\((-?[\d.]+), (-?[\d.]+)\) end=\((-?[\d.]+), (-?[\d.]+)\)"
)

RE_EPISODE_RESULT = re.compile(
    r"episode (\d+) (CAUGHT|TIMEOUT)(?: by (\w+))? in (\d+) steps \(([\d.]+) sim-s\)"
)

RE_STARTED = re.compile(
    r"episode (\d+) started\s+chaser=\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)\s+"
    r"evader=\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)\s+gap=(-?[\d.]+) cm(.*)$"
)

RE_PROBE_SUMMARY = re.compile(
    r"episode (\d+) probe_summary non_clear_readings=(\d+) blocked_readings=(\d+) "
    r"closest=([\d.]+) \((\d+) steps sampled\)"
)

RE_INTERFACE = re.compile(r"INTERFACE SPACES obs_dims=(\d+) preflatten action_dims=(\d+)")

RE_ACTION_PROBE = re.compile(
    r"ACTION PROBE step (\d+) shape=(\S+?)(?: \[(\d+) values:([^\]]*)\])? resolved=(\d+) "
    r"jump_requested=(\d+) threshold=([\d.]+)"
)

RE_OBS_DIM = re.compile(r"episode (\d+) obs = \[.*?\] walls=\[.*?\] \(\d+/\d+ clear\) ndim=(\d+)")

RE_MAX_EPISODES = re.compile(r"validation mode - will exit after exactly (\d+) completed episode")

RE_VALIDATION_COMPLETE = re.compile(
    r"VALIDATION COMPLETE - (\d+) of (\d+) requested episodes finished \(last=(\w+)\)"
)

RE_WALL_CLASS = re.compile(
    r"wall-hit CLASS=(\w+) other=(\S+) comp=(\S+) "
    r"normal=\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\) \|normal.Z\|=([\d.]+)"
)

RE_JUMPED = re.compile(r"PursuitJumpActuator: (\S+) jumped")

RE_OBS_CONTRACT_BROKEN = re.compile(r"TargetSensor emitted (\d+) dims, expected 15")

RE_QUIT_AFTER = re.compile(r"QuitAfter reached \(([\d.]+) s\), exiting")


def percentile(values, fraction):
    """Nearest-rank percentile - stdlib only, and defined for any list length."""
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = fraction * (len(ordered) - 1)
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def stats(values):
    """mean/median/min/max/p90 for a list; None for an empty list."""
    if not values:
        return None
    return {
        "n": len(values),
        "mean": round(statistics.fmean(values), 2),
        "median": round(statistics.median(values), 2),
        "p90": round(percentile(values, 0.90), 2),
        "min": round(min(values), 2),
        "max": round(max(values), 2),
    }


def parse(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    result = {
        "log": os.path.abspath(path),
        "episodes": [],
        "evader_entries": [],
        "episode_results": [],
        "spawn_entries": [],
        "probe_summaries": [],
        "interface_spaces": None,
        "action_probes": [],
        "obs_dims": [],
        "obs_contract_violations": [],
        "max_episodes_requested": None,
        "validation_complete": None,
        "wall_class_samples": [],
        "jump_log_lines": 0,
        "quit_after_line": None,
        "run_flags": {},
    }

    for line in lines:
        match = RE_VALIDATION.search(line)
        if match:
            result["episodes"].append({
                "index": int(match.group(1)),
                "result": match.group(2),
                "start_d": float(match.group(3)),
                "end_d": float(match.group(4)),
                "closest_d": float(match.group(5)),
                "grounded": float(match.group(6)),
                "mean_speed": float(match.group(7)),
                "dot_vel_target": float(match.group(8)),
                "jumps": int(match.group(9)),
                "wall_hits_counted": int(match.group(10)),
                "wall_ignored_floor": int(match.group(11)),
                "wall_ignored_support": int(match.group(12)),
                "wall_ignored_target": int(match.group(13)),
                "wall_ignored_self": int(match.group(14)),
                "samples": int(match.group(15)),
                "probe_non_clear": int(match.group(16)) if match.group(16) else None,
                "probe_blocked": int(match.group(17)) if match.group(17) else None,
                "probe_closest": float(match.group(18)) if match.group(18) else None,
            })
            continue

        match = RE_EVADER.search(line)
        if match:
            result["evader_entries"].append({
                "index": int(match.group(1)),
                "disp": float(match.group(2)),
                "path": float(match.group(3)),
                "mean_speed": float(match.group(4)),
                "max_speed": float(match.group(5)),
                "ratio": float(match.group(6)),
                "static": match.group(7).lower(),
                "start_x": float(match.group(8)),
                "start_y": float(match.group(9)),
                "end_x": float(match.group(10)),
                "end_y": float(match.group(11)),
            })
            continue

        match = RE_EPISODE_RESULT.search(line)
        if match:
            result["episode_results"].append({
                "index": int(match.group(1)),
                "result": match.group(2),
                "caught_by": match.group(3) or "",
                "steps": int(match.group(4)),
                "sim_seconds": float(match.group(5)),
            })
            continue

        match = RE_STARTED.search(line)
        if match:
            flags = match.group(9)
            result["spawn_entries"].append([
                float(match.group(2)), float(match.group(3)), float(match.group(4)),
                float(match.group(5)), float(match.group(6)), float(match.group(7)),
                float(match.group(8)),
            ])
            for flag in ("[static target]", "[flat arena]", "[jump disabled]"):
                if flag in flags:
                    result["run_flags"][flag] = True
            continue

        match = RE_PROBE_SUMMARY.search(line)
        if match:
            result["probe_summaries"].append({
                "index": int(match.group(1)),
                "non_clear_readings": int(match.group(2)),
                "blocked_readings": int(match.group(3)),
                "closest": float(match.group(4)),
                "steps_sampled": int(match.group(5)),
            })
            continue

        match = RE_INTERFACE.search(line)
        if match:
            result["interface_spaces"] = {
                "obs_dims": int(match.group(1)),
                "action_dims": int(match.group(2)),
            }
            continue

        match = RE_ACTION_PROBE.search(line)
        if match:
            values = []
            if match.group(4):
                for token in match.group(4).replace(":", " ").split():
                    try:
                        values.append(float(token))
                    except ValueError:
                        pass
            result["action_probes"].append({
                "step": int(match.group(1)),
                "shape": match.group(2),
                "value_count": int(match.group(3)) if match.group(3) else None,
                "values": values,
                "resolved": int(match.group(5)),
                "jump_requested": int(match.group(6)),
                "threshold": float(match.group(7)),
            })
            continue

        match = RE_OBS_DIM.search(line)
        if match:
            result["obs_dims"].append(int(match.group(2)))
            continue

        match = RE_OBS_CONTRACT_BROKEN.search(line)
        if match:
            result["obs_contract_violations"].append(int(match.group(1)))
            continue

        match = RE_MAX_EPISODES.search(line)
        if match:
            result["max_episodes_requested"] = int(match.group(1))
            continue

        match = RE_VALIDATION_COMPLETE.search(line)
        if match:
            result["validation_complete"] = {
                "completed": int(match.group(1)),
                "requested": int(match.group(2)),
                "last": match.group(3),
            }
            continue

        match = RE_WALL_CLASS.search(line)
        if match:
            result["wall_class_samples"].append({
                "class": match.group(1),
                "other": match.group(2),
                "component": match.group(3),
                "normal_z": float(match.group(6)),
            })
            continue

        if RE_JUMPED.search(line):
            result["jump_log_lines"] += 1

        match = RE_QUIT_AFTER.search(line)
        if match:
            result["quit_after_line"] = float(match.group(1))

    return result


def summarise(result):
    episodes = result["episodes"]
    caught = [e for e in episodes if e["result"] == "CAUGHT"]
    timeout = [e for e in episodes if e["result"] == "TIMEOUT"]
    total = len(episodes)

    spawn_digest = hashlib.sha256(
        ";".join(
            "%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f" % tuple(entry)
            for entry in result["spawn_entries"]
        ).encode("utf-8")
    ).hexdigest()[:16] if result["spawn_entries"] else None

    # Probe liveness, from ONE source only. The VALIDATION line carries the per-episode
    # probe tally on both paths; probe_summary is the older training-only line, kept as a
    # fallback for logs from before the tally moved into the shared summary. Summing both
    # would double count a training run.
    probe_from_validation = [e for e in episodes if e.get("probe_non_clear") is not None]
    if probe_from_validation:
        probe_non_clear_total = sum(e["probe_non_clear"] for e in probe_from_validation)
        probe_blocked_total = sum(e["probe_blocked"] or 0 for e in probe_from_validation)
        probe_source = "VALIDATION lines (%d)" % len(probe_from_validation)
    else:
        probe_non_clear_total = sum(p["non_clear_readings"] for p in result["probe_summaries"])
        probe_blocked_total = sum(p["blocked_readings"] for p in result["probe_summaries"])
        probe_source = "probe_summary lines (%d)" % len(result["probe_summaries"])

    # --- evader census (Stage 2A) -----------------------------------------------------
    # `path` is the sum of per-sample displacements, so it is the metric that cannot be
    # faked by an evader that circles back to where it started; `disp` is the chord. A
    # "moved" episode is judged on the chord so the threshold is a physical statement
    # (the target ended up somewhere else) rather than a noise floor on float addition.
    evader = result["evader_entries"]
    evader_disp = [e["disp"] for e in evader]
    evader_path = [e["path"] for e in evader]
    evader_speed = [e["mean_speed"] for e in evader]
    evader_max_speed = [e["max_speed"] for e in evader]
    evader_moved = [e for e in evader if e["disp"] > 1.0]
    evader_ratios = sorted({round(e["ratio"], 4) for e in evader})
    evader_static_flags = sorted({e["static"] for e in evader})

    # Step counts: prefer the engine's own per-episode step line when the level had
    # bLogEpisodes on, otherwise fall back to the sampled-frame count the VALIDATION line
    # carries - that field exists on every path, which is what makes the two runs
    # comparable at all.
    steps_from_results = [float(e["steps"]) for e in result["episode_results"]]
    steps_from_validation = [float(e["samples"]) for e in episodes]

    summary = {
        "log": result["log"],
        "run_flags": sorted(result["run_flags"].keys()),
        "episodes_completed": total,
        "max_episodes_requested": result["max_episodes_requested"],
        "episode_count_exact": (
            result["max_episodes_requested"] is not None
            and total == result["max_episodes_requested"]
        ),
        "caught": len(caught),
        "timeout": len(timeout),
        "catch_rate": round(len(caught) / total, 4) if total else None,
        "caught_by": sorted({e.get("caught_by", "") for e in result["episode_results"]}),
        "start_d": stats([e["start_d"] for e in episodes]),
        "end_d": stats([e["end_d"] for e in episodes]),
        "closest_d": stats([e["closest_d"] for e in episodes]),
        "steps_sampled": stats(steps_from_validation),
        "steps_engine_line": stats(steps_from_results),
        "grounded_fraction": stats([e["grounded"] for e in episodes]),
        "mean_speed_cm_s": stats([e["mean_speed"] for e in episodes]),
        "dot_vel_target": stats([e["dot_vel_target"] for e in episodes]),
        "jumps_per_episode": stats([float(e["jumps"]) for e in episodes]),
        "jumps_total": sum(e["jumps"] for e in episodes),
        "wall_hits_counted_total": sum(e["wall_hits_counted"] for e in episodes),
        "wall_hits_counted_per_episode": stats([float(e["wall_hits_counted"]) for e in episodes]),
        "wall_ignored_floor_total": sum(e["wall_ignored_floor"] for e in episodes),
        "wall_ignored_support_total": sum(e["wall_ignored_support"] for e in episodes),
        "wall_ignored_target_total": sum(e["wall_ignored_target"] for e in episodes),
        "wall_ignored_self_total": sum(e["wall_ignored_self"] for e in episodes),
        "wall_class_samples": result["wall_class_samples"],
        "wall_class_breakdown": {
            name: sum(1 for s in result["wall_class_samples"] if s["class"] == name)
            for name in sorted({s["class"] for s in result["wall_class_samples"]})
        },
        "evader_reports": len(evader),
        "evader_disp_cm": stats(evader_disp),
        "evader_path_cm": stats(evader_path),
        "evader_mean_speed_cm_s": stats(evader_speed),
        "evader_max_speed_cm_s": stats(evader_max_speed),
        "evader_moved_episodes": len(evader_moved),
        "evader_moved_fraction": (
            round(len(evader_moved) / len(evader), 4) if evader else None
        ),
        "evader_ratios_baked": evader_ratios,
        "evader_static_flags": evader_static_flags,
        "evader_rows": evader,
        "interface_spaces": result["interface_spaces"],
        "action_probe_shapes": sorted(
            {"%s(%s)" % (p["shape"], p["value_count"]) for p in result["action_probes"]}
        ),
        "action_probe_values": [p["values"] for p in result["action_probes"][:2]],
        "obs_dims_seen": sorted(set(result["obs_dims"])),
        "obs_dim_samples": len(result["obs_dims"]),
        "obs_contract_violations": result["obs_contract_violations"],
        "probe_non_clear_total": probe_non_clear_total,
        "probe_blocked_total": probe_blocked_total,
        "probe_source": probe_source,
        "probe_summary_lines": len(result["probe_summaries"]),
        "jump_actuator_log_lines": result["jump_log_lines"],
        "quit_after_line": result["quit_after_line"],
        "validation_complete_line": result["validation_complete"],
        "spawn_signature_sha256_16": spawn_digest,
        "spawn_count": len(result["spawn_entries"]),
        "episode_rows": episodes,
    }
    return summary


def print_summary(summary):
    print("=" * 74)
    print("ACCEPTANCE SUMMARY")
    print("  log                    : %s" % summary["log"])
    print("  run flags              : %s" % (", ".join(summary["run_flags"]) or "(none)"))
    print("-" * 74)
    print("  episodes completed     : %s" % summary["episodes_completed"])
    print("  requested (MaxEpisodes): %s  -> exact: %s"
          % (summary["max_episodes_requested"], summary["episode_count_exact"]))
    print("  CAUGHT / TIMEOUT       : %s / %s" % (summary["caught"], summary["timeout"]))
    print("  catch rate             : %s" % (
        "n/a" if summary["catch_rate"] is None else "%.1f%%" % (100.0 * summary["catch_rate"])))
    print("  caught by              : %s" % (", ".join(x for x in summary["caught_by"] if x) or "-"))
    print("-" * 74)
    for key, label in (("start_d", "start_d cm"), ("end_d", "end_d cm"), ("closest_d", "closest_d cm")):
        entry = summary[key]
        if entry:
            print("  %-22s : mean %.1f median %.1f min %.1f max %.1f (n=%d)"
                  % (label, entry["mean"], entry["median"], entry["min"], entry["max"], entry["n"]))
    entry = summary["steps_sampled"]
    if entry:
        print("  %-22s : mean %.1f median %.1f p90 %.1f min %.1f max %.1f"
              % ("steps (sampled)", entry["mean"], entry["median"], entry["p90"],
                 entry["min"], entry["max"]))
    entry = summary["steps_engine_line"]
    if entry:
        print("  %-22s : mean %.1f median %.1f p90 %.1f"
              % ("steps (engine line)", entry["mean"], entry["median"], entry["p90"]))
    for key, label, scale in (
        ("grounded_fraction", "grounded fraction", 1.0),
        ("mean_speed_cm_s", "mean speed cm/s", 1.0),
        ("dot_vel_target", "dot(vel,target)", 1.0),
        ("jumps_per_episode", "jumps/episode", 1.0),
        ("wall_hits_counted_per_episode", "wall hits/episode", 1.0),
    ):
        entry = summary[key]
        if entry:
            print("  %-22s : mean %.2f median %.2f min %.2f max %.2f"
                  % (label, entry["mean"] * scale, entry["median"] * scale,
                     entry["min"] * scale, entry["max"] * scale))
    print("-" * 74)
    if summary["evader_reports"]:
        print("  EVADER reports         : %d  (static flags: %s, ratio baked: %s)"
              % (summary["evader_reports"], ", ".join(summary["evader_static_flags"]),
                 ", ".join(str(r) for r in summary["evader_ratios_baked"])))
        for key, label in (
            ("evader_disp_cm", "evader disp cm"),
            ("evader_path_cm", "evader path cm"),
            ("evader_mean_speed_cm_s", "evader mean speed"),
            ("evader_max_speed_cm_s", "evader max speed"),
        ):
            entry = summary[key]
            if entry:
                print("  %-22s : mean %.1f median %.1f min %.1f max %.1f"
                      % (label, entry["mean"], entry["median"], entry["min"], entry["max"]))
        print("  %-22s : %s / %s (%s)"
              % ("evader moved episodes", summary["evader_moved_episodes"],
                 summary["evader_reports"],
                 "n/a" if summary["evader_moved_fraction"] is None
                 else "%.1f%%" % (100.0 * summary["evader_moved_fraction"])))
    else:
        print("  EVADER reports         : none (log predates the evader census line)")
    print("-" * 74)
    print("  jumps total            : %s (actuator log lines: %s)"
          % (summary["jumps_total"], summary["jump_actuator_log_lines"]))
    print("  wall hits counted      : %s" % summary["wall_hits_counted_total"])
    print("  wall ignored           : floor=%s support=%s target=%s self=%s"
          % (summary["wall_ignored_floor_total"], summary["wall_ignored_support_total"],
             summary["wall_ignored_target_total"], summary["wall_ignored_self_total"]))
    for sample in summary["wall_class_samples"]:
        print("    class sample           : %-16s other=%-24s comp=%-24s normal.Z=%+.2f"
              % (sample["class"], sample["other"], sample["component"], sample["normal_z"]))
    print("-" * 74)
    print("  interface spaces       : %s" % (summary["interface_spaces"],))
    print("  action probe shapes    : %s" % (summary["action_probe_shapes"],))
    print("  action probe values    : %s" % (summary["action_probe_values"],))
    print("  obs dims seen          : %s over %s samples"
          % (summary["obs_dims_seen"], summary["obs_dim_samples"]))
    print("  obs contract violations: %s" % (summary["obs_contract_violations"] or "none"))
    print("  probe non_clear total  : %s (blocked %s) from %s"
          % (summary["probe_non_clear_total"], summary["probe_blocked_total"],
             summary["probe_source"]))
    print("-" * 74)
    print("  spawn signature        : %s (%s episodes)"
          % (summary["spawn_signature_sha256_16"], summary["spawn_count"]))
    print("  VALIDATION COMPLETE    : %s" % (summary["validation_complete_line"],))
    print("  QuitAfter safety net   : %s" % (
        "FIRED at %.1f s - the episode exit did not happen" % summary["quit_after_line"]
        if summary["quit_after_line"] is not None else "not fired (correct)"))
    print("=" * 74)


def main():
    parser = argparse.ArgumentParser(description="Stage 0 / Stage 2A acceptance parser")
    parser.add_argument("log", help="engine log to parse")
    parser.add_argument("--json", dest="json_path", default=None,
                        help="where to write the JSON summary (default: <log>.stage0.json)")
    parser.add_argument("--label", default=None, help="free-form label stored in the JSON")
    args = parser.parse_args()

    if not os.path.isfile(args.log):
        print("MISSING_LOG %s" % args.log)
        return 1

    parsed = parse(args.log)
    summary = summarise(parsed)
    if args.label:
        summary["label"] = args.label

    # A log whose episodes started but whose per-episode result rows did not parse means
    # the VALIDATION line format changed under us. Say so loudly: "0 episodes" otherwise
    # reads like a run that never happened.
    if summary["spawn_count"] and not summary["episodes_completed"]:
        print("PARSE WARNING: %d episode start line(s) found but 0 VALIDATION result rows "
              "parsed - the VALIDATION format in this log differs from what this parser "
              "expects. Do not read the zeros below as a real result."
              % summary["spawn_count"])

    json_path = args.json_path or (args.log + ".stage0.json")
    with open(json_path, "w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, ensure_ascii=False)

    print_summary(summary)
    print("JSON written: %s" % os.path.abspath(json_path))

    return 0 if summary["episodes_completed"] else 1


if __name__ == "__main__":
    sys.exit(main())
