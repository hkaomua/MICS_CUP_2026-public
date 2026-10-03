# テンプレートの出典

[hkaomua/slide-template](https://github.com/hkaomua/slide-template) のBeamer版を利用しています。
取得したコミットは [`3d26f4fe802aba71afe31bc47bb19c589fd726df`](https://github.com/hkaomua/slide-template/tree/3d26f4fe802aba71afe31bc47bb19c589fd726df) です。

次のファイルは上流から変更せずに取り込んでいます。

- `beamer/beamerthemelineSeed.sty`：レイアウト、書体、配色、表紙、フッター
- `beamer/.latexmkrc`：upLaTeX・dvipdfmxとフォント参照の設定
- `scripts/setup-fonts.mjs`：公式配布フォントの配置
- `scripts/setup-uptex-fonts.py`：欧文の字幅データ生成
- `fonts/README.md`、`fonts/OFL.txt`、`LICENSE`：上流の説明とライセンス

テーマとスクリプトは[MIT License](LICENSE)です。
LINE Seed JPには[同梱のSIL Open Font License 1.1](fonts/OFL.txt)が適用されます。
フォント本体と生成する字幅データはGit管理から除外し、PDFには表示に必要な字形を埋め込みます。

FPGA用の本文・表・グラフは [../nescity_fpga_optimization.tex](../nescity_fpga_optimization.tex) にあります。
表紙は上流の `\seedtitleframe[36]`、本文は見出し24 bp・本文18 bpの設定を使います。
表は上流の記入例と同じ16 bpで、補足14 bp・出典9 bpを加えています。
表・グラフのあるページでも見出し線が表示されるよう、本文側で同じ位置・色の線を前景にも描画しています。
5分版の構成と数値の根拠は [資料README](../README.md) を参照してください。
