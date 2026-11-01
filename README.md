# Feedback Suppressor V2.1 (Live Acoustic Defender)

*English summary: Automatic acoustic feedback (howling) suppressor for live vocal mics with zero-latency notch filtering. V2.1 keeps V2's detection results bit-identical while fixing real-time-safety and click bugs. VST3/AU/Standalone for macOS, VST3/Standalone for Windows x64. AGPL-3.0. Documentation is in Japanese.*

> **V2.1 について**：V2 と検出結果が同じ（検証済み）まま、不具合を修正して処理を軽くした版です。
> - 音声スレッドでロックを使わないようにしました。
> - ノッチの削除・Bypass でクリック音が出ないようにしました。
> - 遅延の報告と、状態の保存・復元を修正しました。
> - 解析の CPU 負荷を 24〜86 % 減らしました。
>
> プラグイン名は「Feedback Suppressor V2.1」（コード `Fd21`）で、V2 などの他の版と同時にインストールできます。
> 詳しくは [docs/v2.1-notes-ja.md](docs/v2.1-notes-ja.md) を参照してください。
> 以下の機能説明は V2 と共通です。

ライブ・ステージでのボーカル・マイク使用を想定した、超低遅延（**0サンプル・アルゴリズム遅延**）アコースティック・フィードバック（ハウリング）自動検知・抑制 VST3 / AU / Standalone プラグインです。
初代 Feedback Suppressor (v1) と同一環境で共存（同時インストール・同時起動）可能です。

macOS（Apple Silicon `arm64` と Intel `x86_64` の Universal Binary、VST3 / AU / Standalone）と Windows 10 / 11 x64（VST3 / Standalone）に対応しています。

---

## 1. 特徴 & 採用アルゴリズム

### (1) 超低遅延（0サンプル遅延）アーキテクチャ
* **メイン音声パス**: FFT による周波数領域変換を行わず、時間領域の **Direct Form II Transposed IIR Biquad ノッチフィルタ直列カスケード** で直接処理します。
  * プラグインによるアルゴリズム遅延は **完全 0 サンプル（< 0.001 ms）**。
  * インイヤーモニター（IEM）やウェッジモニターで歌うボーカリストが違和感を感じない極限の低遅延を実現。
* **サイドチェイン並列解析**: 音声はロックフリー FIFO 経由で並列スレッドに送られ、高精度複素数 STFT 解析を常時バックグラウンド実行。
* **Lookahead モード（先読みオプション）**:
  * 「0 ms (Zero Latency)」と「3 ms」を選択可能。3 msは音声遅延のオプションであり、90〜350 ms以上を要する検出処理全体の先読みではありません。

### (2) 音楽中のピークを検出する3つのモード
* **`Advanced (Music Aware)` [新規インスタンスのデフォルト]**: 周辺帯域との相対差、時間方向の持続、周波数の安定、位相の一貫性、倍音構造を組み合わせます。音楽全体より小さいハウリングも候補にできます。
* **`Aggressive (Mix Hunter)`**: 相対差の条件を緩め、確認時間を短くします。弱いハウリングに対応しやすくなる一方、楽器の持続音への反応も増え得ます。
* **`Standard (Safe)`**: 従来の固定音量・1/f基準を使う保守的な検出を残します。保存済みセッションのモード番号は維持します。

### (3) 改善した検出・抑制処理
1. 左右のスペクトルを独立に解析し、逆相でピークが消える問題を防ぎます。
2. ピーク周辺の線形パワーから背景を推定し、音楽全体の音量を必須条件にしません。
3. 補間した周波数の倍音関係とビブラートを確認し、音楽由来の候補を保護します。
4. 16フレームの正規化した時間的持続と、約85 ms分の位相差を確認します。確認時間は成長中130／90 ms、定常350／220 ms（Advanced／Aggressive）を最小条件とし、窓・履歴準備の時間が別途加わります。
5. EQ後の残存信号を再解析し、初期カット3〜9 dBから必要に応じて150 ms以上の間隔で3 dBずつ追加します。約10 msの平滑化で変化を滑らかにします。

倍音が少ない安定したシンセ音やフルート音とハウリングを、音声だけで完全に区別することはできません。実会場での無誤検出は保証していません。市販製品3例・研究3例の比較、採用理由、数式、検証範囲は [アルゴリズム調査・設計資料](docs/algorithm-review-ja.md) を参照してください。

### (4) 固定／動的フィルタの管理
* **Fixed（固定）フィルタ & Live（動的）フィルタ**:
  * **Live Auto モード**: 本番中、突発的に発生した周波数を最大 16 基の動的ノッチで即座に追従抑制。
  * **Learn / Soundcheck モード**: リハーサル時にマイクゲインを上げて部屋の定在波を鳴らし、固有共振周波数を Fixed フィルタとして固定配置。
* **Filter Lift（自動復帰タイマー）**:
  * マイクがスピーカーから離れるなどしてハウリング原因が解消した場合、設定時間（5s〜5分）後にノッチゲインを徐々に 0 dB へ戻し、スロットを再利用可能にします。
* **ロック / Fixed 化**: 頻繁に発生する周波数をリストからワンクリックで「Fixed」に昇格可能。
* **Panic Limiter（突発音保護）**:
  * 想定外の大音量ハウリングが起きた瞬間、-1 dBFS の高速ソフトリミッターが耳（イヤモニ）やPAスピーカーを緊急保護。

---

## 2. UI / パラメータ仕様

* **Real-time Spectrum Analyzer (RTA)**:
  * 20 Hz 〜 20 kHz 対数スペクトル表示。
  * 背景エンベロープ（Standardでは1/f基準を含む黄色の破線）と、適用中のノッチカーブ（白色/シアンの下向きディップ）を重ねて描画。
  * ハウリング検知時に、該当周波数の上部に警告バッジ（例: `2.20 kHz (-24dB)`）が光るアニメーション。
* **Filter Table / List**:
  * 検知された周波数、カット量（dB）、Q値、Lift 残り時間（秒）、発生回数をリアルタイム表示。
  * 個別フィルタの [Lock/Unlock], [Bypass], [Delete] 操作が可能。
  * 一括操作: 「Clear Live」「Lock All to Fixed」「Clear All」。
* **操作パラメータ**:
  * `Sensitivity`: 検出感度オフセット（**-60 dB** 〜 +18 dB。低くすると条件を厳しくします。クリップ寸前限定の設定はStandardの場合）
  * `Engine Mode`: Standard (Safe) / Advanced (Music Aware) / Aggressive (Mix Hunter)
  * `Notch Q`: ノッチ帯域幅（5.0 〜 120.0、デフォルト 35.0：ボーカルの音色を崩さない約 1/24 オクターブ幅）
  * `Max Cut`: 1つのノッチが下げる最大深度（-40 dB 〜 -6 dB、デフォルト -20 dB）
  * `Profile`: Speech（声向け）/ Music（音楽向け）/ Flat (1/f) / High Focus（高音ハウリング抑制重視）/ Low Focus（低音ブーミング共振抑制重視）
  * `Growth Rate`: ハウリング加速度の感度（Low / Medium / High）
  * `Lift Timer`: 5s / 15s / 30s / 60s / 5m / Hold (解除なし)
  * `Mode`: Live Auto / Learn (Soundcheck) / Bypass
  * `Latency`: Zero Latency (0ms) / Lookahead (3ms)
  * `Panic Protection`: オン/オフ

---

## 3. インストール

### Windows（インストーラー）

1. [Releases](../../releases) の **v2.1.0** から `FeedbackSuppressorV2.1-<版>-Windows-x64-Setup.exe` をダウンロードして実行します。
2. 「Windows によって PC が保護されました」と表示された場合は **詳細情報 → 実行** を押します（有料のコード署名がないため表示されます。信頼できる入手元のファイルにだけ実行してください）。
3. 管理者の確認に「はい」で答えます。VST3 は `C:\Program Files\Common Files\VST3`、Standalone 版は `C:\Program Files\Feedback Suppressor V2.1` に入ります。
4. DAW でプラグインを再スキャンします。アンインストールは「設定 → アプリ」から行えます。

インストーラーを使わない場合は、`FeedbackSuppressorV2.1-<版>-Windows-x64.zip` の `Feedback Suppressor V2.1.vst3` フォルダーを `C:\Program Files\Common Files\VST3` へコピーします。Visual C++ ランタイムの別途インストールは不要です。

### macOS（配布 ZIP）

1. [Releases](../../releases) の **v2.1.0** から `FeedbackSuppressorV2.1-<版>-macOS-universal.zip` をダウンロードして展開します。
2. `Feedback Suppressor V2.1.vst3` を `~/Library/Audio/Plug-Ins/VST3/` へ、AU を使う場合は `Feedback Suppressor V2.1.component` を `~/Library/Audio/Plug-Ins/Components/` へコピーします。
3. ターミナルで次を実行します。

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Feedback Suppressor V2.1.vst3" ~/Library/Audio/Plug-Ins/Components/"Feedback Suppressor V2.1.component"
   ```

   Apple の有料署名・公証（notarization）を受けていない（アドホック署名のみの）ため、ダウンロードしたファイルに付く「隔離」の印を外さないと DAW が読み込みを拒否します。コマンドは印を外すだけで、中身は変更しません。
4. DAW を再起動するか、プラグインを再スキャンします。

---

## 4. ソースからビルド

macOS（Xcode Command Line Tools、CMake 3.20 以上、Ninja）:

```bash
./scripts/build-macos.sh     # ビルド + テスト（JUCE 8.0.6 は自動で取得）
./install_plugins.sh         # ~/Library/Audio/Plug-Ins へコピー（隔離の解除は不要）
```

Windows（Visual Studio 2022 の C++、CMake、Inno Setup 6）: PowerShell で `./scripts/package-windows.ps1` を実行すると、`dist\` に ZIP とインストーラーができます。

テストは既存の12項目の DSP テスト、合成音楽中のハウリング検出・誤検出の回帰テスト、V2.1 のクリック音・NaN テスト、プロセッサーの状態保存テストです。Release ビルドでもテストの assert は有効です。

---

## 5. ライセンス

AGPL-3.0-only（[LICENSE](LICENSE)）。JUCE 8.0.6 を AGPLv3 の下で使用しています（[licenses/JUCE.md](licenses/JUCE.md)、[THIRD_PARTY.md](THIRD_PARTY.md)）。

検出方式を変更した実験版 V3 のソースは、タグ `v3.0.0` に残してあります（配布は終了しています）。
