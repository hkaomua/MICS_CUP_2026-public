# フォントの配置

フォント本体はGit管理していません。
[LINE Seed公式サイト](https://seed.line.me/index_jp.html)から日本語版を取得してください。

展開した配布フォルダを指定すると、3形式に必要なファイルをコピーできます。
リポジトリのルートで実行してください。

```sh
node scripts/setup-fonts.mjs /path/to/LINESeedJP_20241105
```

Node.jsを使わない場合は、次のファイルをこの`fonts`フォルダに手動でコピーしてください。
BeamerまたはTypstだけを使用する場合はOTFの3ファイルとOFL、Marpだけの場合はWOFF2の3ファイルとOFLを使います。

| 配布フォルダ内の場所 | コピーするファイル |
| --- | --- |
| `Desktop/OTF/` | `LINESeedJP_OTF_Rg.otf`、`LINESeedJP_OTF_Bd.otf`、`LINESeedJP_OTF_Eb.otf` |
| `Web/WOFF2/` | `LINESeedJP_OTF_Rg.woff2`、`LINESeedJP_OTF_Bd.woff2`、`LINESeedJP_OTF_Eb.woff2` |
| 配布フォルダの直下 | `OFL.txt` |

スクリプトは`LINESeedJP_20241105`のフォルダ構成で確認しています。
公式配布の構成が変わった場合は、上記のファイル名で手動配置してください。
フォントにはSIL Open Font License 1.1が適用されます。
このリポジトリのMITライセンスはフォントには適用されません。

Beamer版では、共通セットアップ後に`scripts/setup-uptex-fonts.py`も実行します。
各ウェイトの`seed-*.tfm`と`seed-*-cmap`をこのフォルダに生成します。
生成物もGit管理から除外されます。手順はルートのREADMEを参照してください。
