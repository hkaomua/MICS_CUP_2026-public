# Nescity CPU高速化の発表資料

Beamer製、16:9、全21枚（本編19枚＋付録2枚）。約18分を想定し、発表時間は要リハーサル。

- `nescity_optimization.tex`: 編集用ソース。グラフ・表・数式もTeX内で編集可能
- `talk_notes.md`: 発表時の補足、詳細資源量、検証条件、固定依存とSHA-256
- `build.sh`: PDFの再生成
- `output/pdf/nescity_optimization.pdf`: 1756db3／70.615倍の評価に合わせて公開用に再生成したPDF

## 測定対象

資料ソースは1756db3、2026年9月6日の[RESULTS.md](../RESULTS.md)に合わせています。今回の資料更新では性能測定を再実行していません。

付属初期配置から1280世代の平均599,899サイクル、合計767,870,293サイクル。同一固定ツールチェーンで再ビルドした配布版（合計54,222,785,215）に対し70.615倍、サイクル削減98.58%です。比較する3版はbaseline・07b8126・1756db3。旧eb9c478は56.989倍の開発段階として残します。

段階比較は7f2c8ae・453dfb1・63e14f3・eb9c478・07b8126・1756db3の各RESULTS.mdに基づきます。各版は複数変更を含み、棒の差は単独手法の独立した寄与率ではありません。

CPUシミュレーションで実ROMのsim_step先頭からRTSまでを測定した値です。呼出元JSR・CRC・描画・フレーム待ち・NMIを除きます。従来の6フレーム待ち・再描画頻度を維持し、実機FPSやアプリ全体の速度倍率とは区別します。倍率は盤面に依存し、初期5世代の配布版比は41.686倍です。

実機表示・処理速度、QuartusのLE/FF/BRAM使用率、タイミング収束は未評価です。簡易PPUによる起動試験と、ソフトウェアROM・RAM・スタックの検査を区別します。

## PDFの再生成

リポジトリのルートで実行します。

```sh
sh Nescity_CPU/eval/slides/build.sh
```

XeLaTeXとbeamer・xeCJK・pgfplots・listings・lmodern、TeX Gyre Heros、原ノ味ゴシックが必要です。既存手順はTeX Live 2026で確認されたものです。中間ファイルはGit対象外のeval/build/slides/に出力し、PDFは配布用成果物としてGitに含めます。

公開用の資料はTeX Live環境でPDFを再生成しています。旧Git履歴はPDF生成に不要です。
