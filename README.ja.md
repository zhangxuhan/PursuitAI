# PursuitAI

[中文](README.md) · [English](README.en.md)

Unreal Engine 5.7 上で、キャラクターの追跡方策を強化学習する実験プロジェクトです。C++ の環境が観測・行動・報酬・エピソード終了条件を定義し、AMD Schola を介して Stable-Baselines3 の PPO で学習します。選定した方策は ONNX に変換し、UE 内では NNE で推論します。

ここで再利用できるのは**環境設計から学習、評価、推論までの流れ**です。任意のマップとルールを入力するだけでモデルが完成する汎用プラットフォームではありません。課題を変える場合は、到達可能性、観測・行動、報酬を見直し、再学習と別条件での評価が必要です。

## 実装と適用範囲

- 学習するのは追跡者の `ACharacter` 一体。逃走者はルールベースです。
- 観測は 15 次元、行動は 3 次元。静止目標から速度の異なる移動目標へ段階的に学習します。
- 学習用マップ、チェックポイントを分離した実行スクリプト、評価解析、ONNX 推論を用意しました。
- 展示用の円形マップには衝突判定のある外周壁、照明、二体のキャラクター、左上の実測値表示があります。

標準方策は**内部障害物のない平地での移動目標追跡**に使用します。外周壁には衝突判定があります。街の背景、内部障害物、ジャンプアクチュエータ、追加の追跡者は、後の段階で順に追加されます（下記 v2 / v3）。

## 4 つの段階：v0 → v3

| 段階 | 目的 | 内容 |
|---|---|---|
| **v0 · 球体追跡の試作** | 学習ループの確立 | キャラクターの代わりに単純な球を使用し、「UE が観測を出す → Schola/gRPC → PPO が行動を出す → 捕獲率が学習とともに上がる」という経路だけを確認します。キャラクターアニメーション、追跡カメラ、内部障害物はありません。 |
| **v1 · キャラクター追跡** | 実キャラクター + カリキュラム学習 | 追跡者を `ACharacter`（スケルタルメッシュ + CharacterMovement）に変更。観測は 15 次元（目標の方向・距離・自身と目標の速度＋前方 5 本のレイ）、行動は 3 次元の連続値です。静止目標 → Moving025 → Moving050 の順に学習し、数値は次の節にあります。 |
| **v2 · 障害物と街ステージ** | シーン拡張と転移の検証 | 場内に障害物・壁・街の背景を追加し、観測に壁までの距離とクリアランスを追加しました。学習済みの追跡方策が障害物のある場面でどう振る舞うかを検証します。 |
| **v3 · ジャンプ次元と複数追跡者** | 実行と連携の拡張 | 環境にジャンプアクチュエータ（レベルの `bEnableAgentJump` で有効化）と 2 体目の追跡者 `SupportAgent` を追加しました。キャラクターは低い壁を乗り越えられ、2 体の追跡者が連携して逃走者を追い込めます。 |

<table>
  <tr>
    <td align="center"><b>v0 · 球体追跡の試作</b><br><img src="docs/figures/stage_v0_ball.gif" width="340" alt="v0 ball pursuit prototype"></td>
    <td align="center"><b>v1 · キャラクター追跡</b><br><img src="docs/figures/stage_v1_character.gif" width="340" alt="v1 character pursuit"></td>
  </tr>
  <tr>
    <td align="center"><b>v2 · 障害物と街ステージ</b><br><img src="docs/figures/stage_v2_obstacles.gif" width="340" alt="v2 obstacles and city stage"></td>
    <td align="center"><b>v3 · ジャンプ次元と複数追跡者</b><br><img src="docs/figures/stage_v3_jump_multi.gif" width="340" alt="v3 jump dimension and multi-agent chase"></td>
  </tr>
</table>

## 学習前後の評価

![同じマップでの学習前後の捕獲率](docs/figures/training_comparison.svg)

| 評価マップ | このマップでの追加学習前 | 追加学習後 | 条件 |
|---|---:|---:|---|
| Moving025、目標速度 0.25× | 55/100 | 100/100 | 静止目標で学習した方策をゼロショット評価し、Moving025 で実際に 51,200 ステップ追加学習 |
| Moving050、目標速度 0.50× | 84/100 | 98/100 | Moving025 方策をゼロショット評価し、Moving050 で追加学習した選定モデルを評価 |

各数値は独立した 101 エピソードの決定論的評価から、ウォームアップの第 1 エピソードを除いた結果です。**同一の初期位置を使ったペア比較ではありません**。「学習前」は未学習のランダム方策ではなく、前段階で学習済みの方策です。Moving050 では追加学習の結果に大きなばらつきがあり、98/100 は正式な再評価に通った一つの選定結果です。「学習を続ければ必ず改善する」という意味ではありません。

[集計 CSV](docs/results/summary.csv)、[各エピソードの CSV](docs/results/formal_episodes.csv)、[評価解析 JSON](docs/results/)、[グラフ](docs/figures/training_comparison.svg)を同梱しています。捕獲数は Python 側の正式 100 回分、行動指標の平均は UE 側で結果行が残った 99 回分です。元の checkpoint ZIP は含めず、識別情報は[根拠一覧](docs/results/PROVENANCE.md)に残しました。

## セットアップと実行

Windows、Unreal Engine 5.7、Python 3.12 を使用します。[AMD Schola](https://github.com/GPUOpen-LibrariesAndSDKs/Schola) の `v2.1.1` を `Plugins/Schola/` に配置してください（`git clone --branch v2.1.1 --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/Schola.git Plugins/Schola`）。Fab の `RPGHeroSquad` と `Cartoon_City_Free` は再配布しません。Python 環境には Schola の SB3 オプションを入れます。

```powershell
py -3.12 -m venv .venv
& .\.venv\Scripts\python.exe -m pip install -e ".\Plugins\Schola\Resources\python[sb3]"
```

最初は `/Game/Maps/L_PursuitCharCurriculum` で学習を開始します。

```powershell
& .\.venv\Scripts\schola.exe sb3 train ppo project PursuitAI.uproject `
  --map /Game/Maps/L_PursuitCharCurriculum --headless `
  --build-dir Build/Staging --timesteps 60000 `
  --save-final-policy --checkpoint-dir checkpoints/static `
  --enable-tensorboard --log-dir logs/tb/static --disable-eval --no-pbar
```

`tools/run_training_char_curriculum.sh` は移動目標マップへの**継続学習**用で、入力 checkpoint、実行タグ、許可リスト内のマップを必須にしています。関係するコードは `Source/PursuitAI/`、マップ生成は `tools/gen_char_curriculum_level.py`、評価解析は `tools/stage0_measure.py` と `tools/stage2b_eval_analyze.py` です。

同梱の ONNX を試す場合は `UE_ENGINE_ROOT` に UE 5.7 のインストール先を設定し、`tools/run_demo_circle.ps1 -View Arena` または `-View Follow` を実行します。展示マップは開始距離 450–600 cm、目標速度 0.50×です。固定シード `20260923` の別途 20 回の確認では 19 回捕獲、1 回時間切れでした。上表の Moving050 正式評価とは別の数値です。[ONNX 対照記録](docs/results/onnx_parity.txt)では実観測 400 件に対する SB3 の決定論的行動との最大誤差が `1.192e-7` でした。既存の `tools/export_policy.bat` が加える Tanh はこの checkpoint には合わないため、そのまま再出力しないでください。

任意マップの自動学習、複数エージェントの同時学習、世界モデル、習得済みのジャンプは成果として主張しません。
