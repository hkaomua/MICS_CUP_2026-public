# Quartus配置配線結果（2026年10月3日）

提供された `output_files.zip` のレポートを転記した記録です。資料側でQuartusを再実行した結果ではありません。
RTLシミュレーションの評価対象は `229aed0` ですが、ZIPにはソースのコミットID・QSF・SDC本体がありません。
そのため、このコンパイルとの厳密なソース同一性は未照合です。

## 条件と全体結果

- Quartus Prime 22.1std.1 Build 917、SC Lite Edition
- リビジョン／トップ：`nes_top_max10`
- デバイス：MAX10 `10M08SAE144C8GES`、Timing Models：Preliminary
- Fitter終了：2026年10月3日13:55:14（レポート表記）
- Fitter成功、フルコンパイル成功、SOF生成。タイミング要件は未達

| 資源（NES全体） | 使用数／総数 | 使用率 |
| --- | ---: | ---: |
| LE | 6,020／8,064 | 75% |
| 組合せ関数 | 5,460／8,064 | 68% |
| FF | 2,573／8,064 | 32% |
| LAB | 444／504 | 88% |
| M9K | 40／42 | 95% |
| 論理メモリbit | 314,368／387,072 | 81% |
| M9K割当容量bit | 368,640／387,072 | 95% |
| DSP（9-bit要素） | 0／48 | 0% |
| PLL | 1／1 | 100% |

出典：`nes_top_max10.fit.summary`、`nes_top_max10.fit.rpt` の Fitter Resource Usage Summary。
FFと組合せ関数を足してLE使用数とはしません。空きメモリは論理bitの81%ではなくM9Kブロック数で判断します。

アクセラレータ階層はLogic Cells 1,830、FF 849、M9K 3個、論理メモリ17,408 bitです。
盤面RAMは1,024×8 bitが2個、行履歴は32×32 bitが1個で、それぞれM9Kを1個使います。
出典：fit.rpt の Resource Utilization by Entity と RAM Summary。
配布回路のFitter結果は未提供なので、実装前後の資源増分を実測したとはしません。

## タイミング

| クロック | 周波数（レポート表記） | Setup slack | 同一クロック内Fmax |
| --- | ---: | ---: | ---: |
| PLL clk[0]（計算側） | 10.0 MHz | +4.040 ns | 35.88 MHz |
| PLL clk[1] | 100.01 MHz | −2.246 ns | 81.67 MHz |

出典：`nes_top_max10.sta.rpt` の Clocks、Slow 1200mV 85C Model Fmax Summary／Setup Summary。
本編ではclk[1]を公称100 MHzと表記しています。

- 100 MHz側のSetup TNSは−300.263 ns。Slow 0℃でもSetup slackは−1.673 nsです。
- Fmaxは同一クロック内の経路のみです。クロック間転送を含む動作周波数の保証には使いません。
- 未制約クロック `joypad_count[15]`、未制約入力11ポート・出力16ポートが残っています。
- 受領レポートには違反経路の詳細がなく、原因モジュールは未特定です。
- 実機で0500（16進表記、1,280世代）までハッシュ一致を確認したとの報告を2026年10月3日に受領しました。
  これは実施者による動作確認結果です。実機FPSの測定値は未受領です。
  ハッシュ一致をもって、レポート上のタイミング違反を解消済みとはしません。
- 約69.63倍・0.1103 msは既存のRTLサイクル評価／10 MHz換算値です。実機測定値への置換はしません。

20 MHz化・2セル同時処理は今後の計画です。今回のコンパイルによる達成結果には含めません。

## 出典ファイルの識別

元ZIPと掲載値の出典レポートのSHA-256です。

- `output_files.zip`
  `6d35d0f5819367bfc378ea45c858a72d49143f6d5b23453af84bf37fa810090c`
- `nes_top_max10.fit.summary`
  `74d8fb595026d9fd7d1b60f3bcb8afca5c9687389175644d925a36e5b800ea91`
- `nes_top_max10.fit.rpt`
  `bef3dec09a8d86a85b9c45c6d570c9096b437910217083d5bf382ee92652f658`
- `nes_top_max10.sta.rpt`
  `000181545f90a58fb1edec83e90c6177f56b480ae913128106c23a6382653122`
- `nes_top_max10.flow.rpt`
  `ea3388ffa0137ed3084c8ff50d3fbebb9c4af2433d2d92b72a3c5c07d7846f2e`
