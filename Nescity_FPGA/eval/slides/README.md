# Nescity FPGA高速化の発表資料

[hkaomua/slide-template](https://github.com/hkaomua/slide-template) のBeamerテーマを使った資料です。
白背景、青い見出し線、濃いグレーのフッター、LINE Seed JPの書体を上流の設定で使用しています。
テンプレートの取得コミットとライセンスは [template/README.md](template/README.md) に記録しています。
見出しは普段の発表資料 `hamano_meeting_20260605.pdf` を参考に、短い話題名と、
その下に置く「このページで伝えたいこと」の一文に分けています。
16:9、全9枚（**本編7枚・約5分＋質問用の付録2枚**）。

- [発表用PDF](output/pdf/nescity_fpga_optimization.pdf)
- [発表メモ](talk_notes.md)：本編の時間配分と話す内容
- [Quartus結果](quartus_results.md)：提供レポートの出典・資源・タイミング
- [編集用TeX](nescity_fpga_optimization.tex)：表・数式・グラフも編集可能
- [build.sh](build.sh)：PDFの再生成
- [generate_metrics.py](generate_metrics.py)：固定した評価記録から表示数値を生成

本編は結果、課題、読出しの再利用、パイプライン、計算時間、リソース、検証の7枚です。
開発段階の比較と2セル並列化の案は付録にまとめ、本編5分には含めません。
並列化の約1.8～2倍は目標値です。追加M9K 2個で収める構成を検討していますが、ロジック使用量とタイミングは未検証です。
本編末尾に[FPGA実装PR #4](https://github.com/hkaomua/MICS_CUP_2026-public/pull/4)の案内を掲載しています。
配布版の `main` に対する `fpga/max10-opt` の実装・evalの差分を確認できます。

## 再生成

初回は [LINE Seed公式サイト](https://seed.line.me/index_jp.html) から日本語版のZIPを取得し、展開します。
リポジトリのルートで、実際の展開先を指定してフォントを配置します。

```sh
node Nescity_FPGA/eval/slides/template/scripts/setup-fonts.mjs /path/to/LINESeedJP_20241105
python3 -m venv Nescity_FPGA/eval/.build/slide-fonts-venv
Nescity_FPGA/eval/.build/slide-fonts-venv/bin/python -m pip install fonttools==4.66.1
Nescity_FPGA/eval/.build/slide-fonts-venv/bin/python Nescity_FPGA/eval/slides/template/scripts/setup-uptex-fonts.py
```

フォント本体と字幅データはGitに含めません。Windowsでは `bin/python` を `Scripts/python` に置き換えます。
フォントの配置・更新後に字幅データを生成してください。上流と同じ相対パス構成を維持しています。

以降はリポジトリのルートから実行します。

```sh
sh Nescity_FPGA/eval/slides/build.sh
```

Python 3.9以上、upLaTeX、dvipdfmx、latexmk、pltotf、
beamer・otf・pxchfon・pxjahyper・ly1・TikZ・lmodern・colortbl・pgfplots・listingsが必要です。
フォント配置にはNode.js、字幅データの生成にはfonttoolsを使います。TeX Live 2026で確認しました。
RTL評価ツールは資料のビルドには不要です。
中間ファイルはGit対象外の `eval/.build/slides/` に出力し、PDFを配布用成果物としてGitに含めます。

RTL評価の数値は作業中の測定ファイルではなく、元の固定コミット `229aed0` から転記した `data/229aed0.json` を読みます。
RTLのSHA-256と記録中のSHA-256を照合し、速度比・時間・削減率を再計算します。
段階比較には `data/` 内の `473ef32`、`24171f4`、`93d2113` の記録も使います。元のGit履歴は不要です。
評価対象を更新する際は `generate_metrics.py` の参照先と検査値、スライドの説明、発表メモを合わせて更新してください。

## 数値の読み方

評価対象は `229aed0`、比較元の配布回路は `a614553` です。
RTL評価日は2026年9月6日、Quartusレポートと資料更新は2026年10月3日です。
この資料修正ではRTL試験・合成を再実行していません。保存済みの検証記録と提供レポートを要約しています。
Quartus結果にはソースのコミットIDがなく、RTL評価対象との厳密な同一性は未照合です。
根拠と検証条件は [評価README](../README.md)、[results.json](../results.json)、同じコミットのRTLと評価スクリプトにあります。

- **69.63倍**はMMIO STARTからBUSY解除までの計算クロック比です。
  10 MHzで76,804から1,103クロック、7.6804から0.1103 msになります。
  描画・CRC・NES命令による待機などを含む実機FPSではありません。
- **NES全体は6,020／8,064 LE（75%）、2,573 FF（32%）、40／42 M9K（95%）**です。
  Quartus Fitterの結果に置き換えました。M9Kの空きは2個、DSPの9-bit要素は0／48です。
  アクセラレータは1,830 Logic Cells、849 FF、M9K 3個（盤面2個＋行バッファ1個）です。
  配布回路のQuartus結果はないため、以前のYosys推定値と混ぜた増分比較は行いません。
- **全体の配置配線・SOF生成は成功、タイミング違反は残っています。**
  Slow 1200 mV 85℃のSetup slackは計算側10 MHzで+4.040 ns、100 MHz側で−2.246 nsです。
  未制約クロック・I/Oもあり、全体のタイミングを満たしたとは言えません。
  モデルはPreliminaryです。実機でのハッシュ一致は、タイミング違反が解消したことを意味しません。
- **実機でも0500（16進表記、1,280世代）までハッシュ一致**を確認しています。
  2026年10月3日の実施者からの確認報告に基づきます。実機FPSは未測定です。
- **355配置・2,058世代**は変更していないC参照モデルと全960セルを比較した結果です。
  通常の値域に加え、全バイト値・乱数・周期境界を含みます。
  詳細な検査範囲と再現コマンドは評価READMEを参照してください。
- 開発段階の棒グラフは配布回路に対する累積倍率です。
  各版には複数の変更が含まれるため、棒の差を単一手法の寄与とは解釈しません。
