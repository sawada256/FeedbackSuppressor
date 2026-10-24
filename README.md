# Feedback Suppressor V3

*English summary: Automatic acoustic feedback (howling) suppressor for live vocal mics — detects feedback in a background thread and places narrow notch filters, with zero audio latency by default. VST3 / AU / Standalone, macOS universal. AGPL-3.0. Documentation is in Japanese.*

ライブでボーカルマイクを使うときに起きるハウリング（アコースティック・フィードバック）を自動で見つけ、その周波数だけを細いノッチフィルタで削るプラグインです。形式は VST3／AU／Standalone です。

- **音声経路の遅延は 0 サンプルです**（初期設定の場合）。イヤモニで歌っても違和感はありません。
- macOS Universal Binary（Apple Silicon と Intel）に対応しています。Cubase 13 での使用を想定しています。
- 開発中の旧版 V2 とは別のプラグインとして登録されます（プラグインコード `Fdb3`）。

検出方式等の詳細は [docs/v3-design-ja.md](docs/v3-design-ja.md) を参照してください。

---

## 1. 何をするプラグインか

1. マイクの音を別スレッドで常時分析し、次の条件を満たす音を探します。
   - **周波数がまったく揺れない**
   - **他の音と関係なく単独で大きくなる**
   - **倍音を伴わない**
2. 見つけた周波数に、幅の狭いノッチを最初は 3〜12 dB の深さで入れます。ハウリングが残っている間は、3 dB ずつ深くします。
3. 一定時間（Lift Timer）が過ぎると、ノッチは少しずつ解除されます。同じ周波数で再びハウリングが起きた場合は、前回の深さに近いところからやり直します。
4. ノッチが入るまでの間に音が急に大きくなっても、**パニックリミッター**（−0.5 dBFS）が耳とスピーカーを守ります。

歌声のビブラートやピッチの揺れ、倍音のある音、音の立ち上がり、クレッシェンドは、ハウリングと区別して削らないように設計しています。

---

## 2. インストール

### 配布 ZIP から（ビルド不要）

1. [Releases](../../releases) から `FeedbackSuppressorV3-<版>-macOS-universal.zip` をダウンロードして展開します。
2. `Feedback Suppressor V3.vst3` を `~/Library/Audio/Plug-Ins/VST3/` へ、AU を使う場合は `Feedback Suppressor V3.component` を `~/Library/Audio/Plug-Ins/Components/` へコピーします（Finder で `Cmd + Shift + G` → パスを入力）。
3. **ターミナルで次を実行します**（理由は下記）。

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Feedback Suppressor V3.vst3" ~/Library/Audio/Plug-Ins/Components/"Feedback Suppressor V3.component"
   ```

   Standalone 版（`Feedback Suppressor V3.app`）を使う場合も、同様に `xattr -dr com.apple.quarantine "Feedback Suppressor V3.app"` を実行します。

> **なぜ 3. が必要？** このプラグインは Apple の有料開発者署名・公証（notarization）を受けていません（「アドホック署名」のみ）。インターネットからダウンロードしたファイルには macOS が「隔離（quarantine）」の印を付け、公証のないプラグインは DAW での読み込みを拒否されます。上のコマンドはその印だけを外し、中身は変更しません。信頼できる入手元のファイルにだけ実行してください。

### ソースからビルド

```bash
./scripts/build-macos.sh    # ビルド + テスト（初回は JUCE 取得のため数分）
./install_plugins.sh        # ~/Library/Audio/Plug-Ins へコピー
```

自分でビルドしたものには隔離の印が付かないため、`xattr` は不要です。

コピーしたら Cubase を再起動します。「Studio > VST プラグインマネージャー」に **LiveAudioLab / Feedback Suppressor V3** が表示されます。表示されない場合は、プラグインマネージャーで再スキャンしてください。

---

## 3. クイックスタート（Cubase の例）

1. ボーカルのトラック（モノラルでもステレオでも可）の **インサートの最初の段** に「Feedback Suppressor V3」を挿します。
2. 最初はすべて初期設定のまま使います。
   - Engine Mode：**Advanced (Music Aware)**
   - Sensitivity：0 dB
   - Notch Q：35
   - Max Cut：−20 dB
   - Lift Timer：30 s
   - Latency：**Zero Latency**
3. **サウンドチェック（推奨）**
   1. Mode を **Learn (Soundcheck)** に切り替えます。
   2. モニターの音量を本番より少し上げて、マイクに向かって話したり歌ったりします。
   3. ハウリングしかけた周波数に **FIXED** のノッチが入ります。
   4. 終わったら Mode を **Live Auto** に戻します。
   - FIXED のノッチはプロジェクトと一緒に保存されます。Lock を押すと深さも固定されます。
4. **本番**：Live Auto のままにしておきます。新しく出たハウリングには一時的な **LIVE** ノッチが入り、Lift Timer が過ぎると自動で解除されます。
5. 声がこもる、削られすぎると感じたら、Sensitivity を下げます（−6〜−12 dB）。ハウリングの止まりが遅いと感じたら、Sensitivity を上げるか、Engine Mode を Aggressive にします。

---

## 4. 画面とパラメータ

| 項目 | 内容 |
|---|---|
| **Mode** | Live Auto（通常。一時的なノッチ）／Learn (Soundcheck)（新しいノッチを FIXED として残す）／Bypass（すべてのノッチをなめらかに外し、検出も止める） |
| **Latency** | Zero Latency (0 ms)／Lookahead Limiter (3 ms)：出力を 3 ms 遅らせ、パニックリミッターが先にゲインを下げるので、急なピークもはみ出しません。遅延はホストに報告されます。ハウリングの検出が速くなるわけではありません |
| **Engine Mode** | **Advanced (Music Aware)**（推奨）／**Aggressive (Fast)**（しきい値が低く確認時間が短い。そのぶん音楽での誤検出が増える）／**Classic (Legacy)**（V1／V2 の音量ベースの方式。歌への誤検出が多いため非推奨） |
| **Sensitivity** | −60〜+18 dB。上げるほど弱いハウリングを早く捉えますが、誤検出も増えます。1 dB 上げるごとに、突出量のしきい値が 0.35 dB 下がります |
| **Notch Q** | 新しく入れるノッチの幅。35 ≒ 1/24 オクターブです。低域では自動的に 4 Hz より狭くならないよう調整します |
| **Max Cut** | 1つのノッチの最大の深さ（−6〜−40 dB） |
| **Profile** | Speech／Music／Flat：Classic モードの基準カーブ。High Focus（2 kHz 以上）／Low Focus（1.2 kHz 以下）：Advanced と Aggressive では、その帯域のしきい値を 2 dB 下げます |
| **Growth Rate** | ハウリングとみなす成長の速さ。Low は 50 dB/s、Medium は 25 dB/s、High は 14 dB/s（速い経路）。遅い経路はそれぞれ 12／6／3 dB/s |
| **Lift Timer** | Live ノッチを解除するまでの時間（5 s〜5 min／Hold）。解除は +4 dB/s で進みます |
| **Panic Protection** | 緊急用リミッターのオン／オフ |
| スペクトル表示 | 水色が入力スペクトル、黄色の破線が背景の推定、白い線が全ノッチを合わせた特性です。マーカーは赤が今まさに検出中、水色が LIVE、オレンジが FIXED を表します |
| フィルタ一覧 | 周波数、カット量、Q、解除までの残り時間（`releasing` は解除中）、検出回数を表示します。Lock／Bypass／Delete ボタンと、一括操作の Clear Live／Lock All to Fixed／Clear All があります。外す操作はすべてなめらかに行うので、クリック音は出ません |

各つまみやメニューにマウスを重ねると、英語の短い説明が表示されます。

---

## 5. 制限事項

- **倍音の少ない持続音**（まっすぐ伸ばした高い声、フルート、シンセ）は、ハウリングと区別しきれないことがあります。誤って削られる場合は、Sensitivity を下げるか、サウンドチェックで固定フィルタを作って Live の感度を下げてください。
- **成長しないハウリング**（一定の音量で鳴り続けるもの）は、確認に 0.7 秒（Aggressive は 0.45 秒）かかります。大きくなっていくハウリングには、それより速く反応します。
- **ハウリングの周波数が歌の倍音とちょうど重なった場合**は、その音を歌っている間、検出が遅れることがあります。
- 評価は合成信号によるものです。実際の会場、スピーカー、モニターでの効果は試聴して確認してください。
- 3 ms の Lookahead はリミッターのためのものです。ハウリングの検出を先回りするものではありません。
- ノッチによる抑制は「ゲインを下げる」方式です。システムの限界を超えてゲインを上げることはできません。根本的な対策は、マイクとスピーカーの配置とゲインの設定です。

---

## 6. ビルドとテスト（開発者向け）

```bash
cmake -S . -B build-v3 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-v3 --parallel 8
ctest --test-dir build-v3 --output-on-failure
build-v3/fs_benchmark        # 検出時間・誤検出・周波数誤差・CPU を測定
```

| テスト | 内容 |
|---|---|
| `dsp_test` | 従来の12項目（遅延0、無音、倍音、検出、先読みリミッター、Classic の感度・プロファイル） |
| `music_detection_test` | 音楽の中の弱いハウリング、逆相ステレオ、44.1／48／96 kHz、ビブラート、短い音、閉ループの模擬 |
| `v3_test` | クリックなしの削除と Bypass、再発メモリ、低域のノッチ幅、NaN からの復帰、高サンプルレートの間引き、誤検出・検出時間・周波数誤差のしきい値 |
| `processor_test` | 実際のスレッドで動かしたときのノッチ配置、遅延の報告、状態の保存と復元、エディタの生成 |

VST3 はビルド後に ad-hoc 署名し直されます（`codesign --verify --deep --strict` で検証済み）。CMake 3.20 以上と Xcode Command Line Tools が必要です。JUCE 8.0.6 は自動で取得されます（`-DFETCHCONTENT_SOURCE_DIR_JUCE=...` で既存のソースを指定することもできます）。

ソース構成：`source/dsp/FeedbackDetector.*`（検出）、`FilterManager.*`（ノッチの管理。音声スレッドではロックを使わない）、`BiquadNotchFilter.h`、`PanicLimiter.h`（先読み対応）、`source/PluginProcessor.*`（スレッドと状態の管理）、`source/ui/*`。

---

## 7. ライセンス

AGPL-3.0-only（[LICENSE](LICENSE)）。JUCE 8.0.6 を AGPLv3 の下で使用しています（[licenses/JUCE.md](licenses/JUCE.md)）。依存関係は [THIRD_PARTY.md](THIRD_PARTY.md) を参照してください。
