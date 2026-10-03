# Nescity CPU高速化の発表資料

FPGA版と同じ [hkaomua/slide-template](https://github.com/hkaomua/slide-template) のBeamerテーマを利用しています。
短い見出しと、そのページで伝えたいことを一文で示す構成です。
16:9、全9枚（**本編7枚・約5分＋質疑用の付録2枚**）。

- [発表用PDF](output/pdf/nescity_optimization.pdf)
- [発表原稿](talk_notes.md)
- [編集用TeX](nescity_optimization.tex)
- [テンプレートの取得元・ライセンス](template/README.md)

本編は、表紙、課題、最適化2枚、評価2枚、検証です。
開発経過と処理時間の内訳は付録にまとめました。
末尾に[公開CPU実装PR #5](https://github.com/hkaomua/MICS_CUP_2026-public/pull/5)を案内しています。

## 評価の範囲

数値は [../RESULTS.md](../RESULTS.md) の2026年9月6日の検証記録に基づきます。
付属配置から1,280世代の平均599,899サイクル、配布版比70.615倍です。
実ROMの `sim_step` 先頭からRTSまでをCPUシミュレータで計測し、呼出元JSR・CRC・描画・フレーム待ち・NMIを除いています。
実機FPSは未測定で、倍率は盤面に依存します。CPU版の実機動作も未検証です。
今回の編集では性能測定を再実行せず、保存された結果を資料と原稿に反映しました。

## 再生成

初回は [LINE Seed公式サイト](https://seed.line.me/index_jp.html) から日本語版ZIPを取得して展開し、リポジトリのルートで次を実行します。

```sh
node Nescity_CPU/eval/slides/template/scripts/setup-fonts.mjs /path/to/LINESeedJP_20241105
python3 -m venv Nescity_CPU/eval/build/slide-fonts-venv
Nescity_CPU/eval/build/slide-fonts-venv/bin/python -m pip install fonttools==4.66.1
Nescity_CPU/eval/build/slide-fonts-venv/bin/python Nescity_CPU/eval/slides/template/scripts/setup-uptex-fonts.py
```

フォント本体・字幅データはGit対象外です。Windowsでは `bin/python` を `Scripts/python` に置き換えます。
以降は次のコマンドでPDFを更新します。

```sh
sh Nescity_CPU/eval/slides/build.sh
```

upLaTeX・dvipdfmx・latexmk・pltotfと、beamer・otf・pxchfon・pxjahyper・ly1・TikZ・lmodern・colortbl・pgfplots・listingsが必要です。
フォント設定にはNode.js・Python 3.9以上・fonttoolsを使います。TeX Live 2026でビルドし、全9ページを表示確認しています。
中間ファイルは `eval/build/slides/uptex/` に出力します。CPU評価ツールや元の非公開Git履歴は資料のビルドに不要です。
