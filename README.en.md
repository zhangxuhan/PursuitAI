# PursuitAI

[中文](README.md) · [日本語](README.ja.md)

PursuitAI is a character-pursuit reinforcement-learning experiment in Unreal Engine 5.7. A C++ environment supplies observations, actions, rewards, and episode termination. AMD Schola connects the Unreal simulation to Stable-Baselines3 PPO; the selected policy is exported to ONNX and run inside Unreal through NNE.

“Reusable training” refers to the **environment–training–evaluation–inference loop**, not an automatic map-and-rules-to-model platform. A new task still needs a solvable scenario, suitable observations and actions, a checked reward, retraining, and an independent evaluation.

## What is implemented

- One trained chaser (`ACharacter`); the evader follows a scripted fleeing rule.
- A 15-dimensional observation and a 3-dimensional action contract.
- Static-target and moving-target curriculum maps, isolated checkpoints, evaluation parsing, and ONNX inference.
- A separate circular demo map with a collidable boundary, lighting, two character roles, and a live upper-left status display.

The default policy is used for **moving-target pursuit on flat ground without internal obstacles**, inside a circular arena with a collidable boundary wall. The city backdrop, internal obstacles, the jump actuator and extra chasers are added step by step in the later stages (see v2 / v3 below).

## Four stages: v0 → v3

| Stage | Goal | What it adds |
|---|---|---|
| **v0 · Ball pursuit prototype** | Close the training loop | Simple spheres stand in for characters. It only proves the chain "Unreal emits observations → Schola/gRPC → PPO emits actions → catch rate rises with training". No character animation, chase camera, or internal obstacles. |
| **v1 · Character pursuit** | Real character + curriculum | The chaser becomes an `ACharacter` (skeletal mesh + character movement): 15 observation dimensions (target direction, distance, own and target speed, plus five forward ray probes) and a 3-dimensional continuous action. Curriculum: static target → Moving025 → Moving050; numbers in the next section. |
| **v2 · Obstacles and city stage** | Scene extension and transfer test | The arena gains internal obstacles, walls, and a city backdrop; the observation is extended with wall distance and clearance channels, to test how an already-learned pursuit policy behaves in a scene with obstacles. |
| **v3 · Jump dimension and multi-agent chase** | Actuation and coordination extension | The environment gains a jump actuator (enabled per level via `bEnableAgentJump`) and a second chaser, `SupportAgent`: the character can clear low walls, and two chasers can coordinate to corner the evader. |

<table>
  <tr>
    <td align="center"><b>v0 · Ball pursuit prototype</b><br><img src="docs/figures/stage_v0_ball.gif" width="340" alt="v0 ball pursuit prototype"></td>
    <td align="center"><b>v1 · Character pursuit</b><br><img src="docs/figures/stage_v1_character.gif" width="340" alt="v1 character pursuit"></td>
  </tr>
  <tr>
    <td align="center"><b>v2 · Obstacles and city stage</b><br><img src="docs/figures/stage_v2_obstacles.gif" width="340" alt="v2 obstacles and city stage"></td>
    <td align="center"><b>v3 · Jump dimension and multi-agent chase</b><br><img src="docs/figures/stage_v3_jump_multi.gif" width="340" alt="v3 jump dimension and multi-agent chase"></td>
  </tr>
</table>

## Before and after task-specific training

![Catch rates from same-map evaluations](docs/figures/training_comparison.svg)

| Evaluation map | Before adaptation | After adaptation | Meaning |
|---|---:|---:|---|
| Moving025, target speed 0.25× | 55/100 | 100/100 | Static-task policy zero-shot; then 51,200 actual additional steps on Moving025 |
| Moving050, target speed 0.50× | 84/100 | 98/100 | Moving025 policy zero-shot; then the selected Moving050 continuation |

Each value is from a separate 101-episode deterministic evaluation with episode 1 excluded as warm-up. The runs are **not paired by spawn seed**. “Before” does not mean random weights: it is a policy trained on the preceding curriculum stage. Moving050 training was highly variable; 98/100 is a selected continuation that passed a formal re-evaluation, not a promise that more PPO steps always help.

See the [summary CSV](docs/results/summary.csv), [per-episode CSV](docs/results/formal_episodes.csv), four [evaluation summaries](docs/results/), and the [chart](docs/figures/training_comparison.svg). The success counts cover 100 Python-reported formal episodes; behavior means cover the 99 episodes with complete Unreal result lines. The original checkpoint ZIPs are not distributed here; their identities are in the [provenance notes](docs/results/PROVENANCE.md).

## Setup and use

Use Windows, Unreal Engine 5.7, Python 3.12, and [AMD Schola](https://github.com/GPUOpen-LibrariesAndSDKs/Schola) tag `v2.1.1` at `Plugins/Schola/` (`git clone --branch v2.1.1 --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/Schola.git Plugins/Schola`). The third-party `RPGHeroSquad` and `Cartoon_City_Free` Fab assets are deliberately excluded. Install the Schola SB3 Python extra in a virtual environment:

```powershell
py -3.12 -m venv .venv
& .\.venv\Scripts\python.exe -m pip install -e ".\Plugins\Schola\Resources\python[sb3]"
```

For a first run from scratch, train on `/Game/Maps/L_PursuitCharCurriculum`:

```powershell
& .\.venv\Scripts\schola.exe sb3 train ppo project PursuitAI.uproject `
  --map /Game/Maps/L_PursuitCharCurriculum --headless `
  --build-dir Build/Staging --timesteps 60000 `
  --save-final-policy --checkpoint-dir checkpoints/static `
  --enable-tensorboard --log-dir logs/tb/static --disable-eval --no-pbar
```

`tools/run_training_char_curriculum.sh` is the guarded **resume** entry point for the moving-target maps; it requires an explicit input checkpoint, a run tag, and a map from its allowlist. `tools/gen_char_curriculum_level.py` generates the curriculum maps, and `tools/stage0_measure.py` / `tools/stage2b_eval_analyze.py` parse evaluations. These are project workflows, not a claim of identical results on every PC.

To run the included ONNX demo, set `UE_ENGINE_ROOT` to your UE 5.7 installation and use `tools/run_demo_circle.ps1 -View Arena` or `-View Follow`. The demo uses a 450–600 cm opening and a 0.50× moving target. With seed `20260923`, a separate 20-episode screen caught 19 targets and timed out once. Do not substitute that number for the formal Moving050 result. The included [ONNX parity record](docs/results/onnx_parity.txt) compares 400 real observations against the SB3 deterministic policy (maximum absolute action error `1.192e-7`). The older `tools/export_policy.bat` applies an incorrect Tanh to this checkpoint; other exported models also need their own parity test.

No automatic arbitrary-map training platform, multi-agent training, world model, or learned jumping is claimed.
