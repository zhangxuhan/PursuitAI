# Evaluation provenance

`summary.csv` and `../figures/training_comparison.svg` are a compact public view of four formal evaluations. `formal_episodes.csv` contains all 400 formal reward/step rows parsed from the four driver logs; outcomes were classified with each analysis JSON's recorded reward threshold, and the resulting catch counts were checked against those JSONs. The four JSON files are copies of the local analysis output, not hand-entered replacements. Full engine and driver logs plus checkpoint ZIPs remain outside Git because they are larger operational artifacts; their identities are recorded below. These tables permit an audit of the reported counts but do not, by themselves, rerun a historical checkpoint.

| JSON here | Local source | Map SHA256 | Checkpoint SHA256 | Formal result |
|---|---|---|---|---|
| `moving025_before.json` | `logs/stage2b_moving025_zeroshot_eval_analysis.json` | `797f7861609108e4a013494785e067446e13e1d7e4c3586ae4fd80debc3328ed` | static 61,440: `3c6ec76d860df0b610e930db188776ece718398854b0b548b2d988e3351733e3` | 55 caught, 45 timeout |
| `moving025_after.json` | `logs/stage2b_moving025_plus50k_eval_analysis.json` | same Moving025 map | Moving025 112,640: `b6bd4519ff14b59bc7a4d17d244686e10b0d9264b54639e058643e71edda5b76` | 100 caught, 0 timeout |
| `moving050_before.json` | `logs/stage2c_moving050_zeroshot_eval_analysis.json` | `7a7e713280de1b40702a5e20c5250474de78b124e14c7347ae77617bf2471b23` | same Moving025 112,640 checkpoint | 84 caught, 16 timeout |
| `moving050_selected.json` | `logs/stage2f/rep3_formal_analysis.json` | same Moving050 map | rep3 132,880: `0dbcfe74b3c37926ce454216ca7534b2b8432531a09856467a27cea829893562` | 98 caught, 2 timeout |

Protocol: 101 deterministic episodes for each evaluation; episode 1 was excluded as warm-up, leaving episode 2–101 (100 formal episodes). Python-side episode rewards/classification were authoritative. One UE result line at process shutdown is absent, so behavior means use 99 UE episodes. The independent runs did not share a controlled spawn sequence and are not a paired trial.

The Moving025 “before” model was trained on a *static* target. The Moving050 “before” model was trained on the slower Moving025 target. Neither is an untrained random policy. The selected Moving050 result is not monotonic-learning evidence: separate 10k continuations from the same 122,640-step point screened at 23.33%, 73.33%, and 96.67% over 30 formal episodes. Only the last continuation was promoted after its independent 100-episode formal result of 98%. Longer Stage 2C continuations also regressed; no general claim that additional steps improve this task is warranted.

The included `models/rep3_sb3_exact.onnx` is for the selected rep3 policy. `onnx_parity.txt` records action-level agreement on 400 real observations (max absolute error `1.192e-7`). This verifies that particular model's deterministic ONNX action path; it does not validate arbitrary exports.
