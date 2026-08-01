# Phase 9改善I-008 拡大・黄色除去iconのローカル検証

更新日: 2026-07-31

## 1. 対象

- Client／Agent version: `0.7.9.0`
- Version `0.7.8.0`の実機feedback:
  - taskbar相当の小表示でiconが小さく見える
  - 黄色endpointは淡色背景で視認性が不足する
- 実画面capture、接続先、ユーザー情報は記録しない

## 2. 修正

- free-standingの二desktop endpointとZ型Bridgeという構成を維持する
- Z型pathとendpointを太くし、全体をmaster幅約96%／高さ約92%へ拡大する
- 透明余白を左右約2%／上下約4%へ縮小する
- lower-right endpointの黄色、gold、amberをroyal purpleへ変更する
- yellow、gold、amber、mustard、limeをsubjectへ使用しない
- upper-left endpointのcoral、Bridgeのsky blue／turquoise、白highlightを維持する
- outer tile、badge、black／navy／charcoal背景を追加しない

## 3. 生成と透明化

- Codex built-in image generationで既存masterをtargeted editした
- flatな`#00ff00` chroma-key背景で生成した
- imagegen skillの`remove_chroma_key.py`によりlocalでalpha化した
- Pillowで安全余白を残してcropし、1024×1024 RGBA masterを生成した
- 16、20、24、32、40、48、64、128、256pxのICOを生成した
- 最終promptは`assets/icons/README.md`へ記録した

## 4. 画像検証

- master subject占有率: 幅約96.4%、高さ約92.1%
- master四隅alpha: すべて0
- 黄色endpoint相当pixel: 0
- chroma-key green相当の不透明pixel: 0
- 16、24、32、48pxを明背景と暗背景へ合成して確認した
- Release EXEから抽出した32px icon:
  - alpha 128以上pixel: Version `0.7.8.0`の185から339へ増加
  - alpha 128以上のbounding box: 28×24pxから30×30pxへ増加
  - 黄色系pixel: 26から0へ減少
- Client／Agentから抽出したiconのSHA-256が一致した

| file | size | SHA-256 |
|---|---:|---|
| `assets/icons/zrdp-icon.png` | 326,877 byte | `29BD53F9581A961B428E8073A718792E75348507DE756BD6FACF6F64A1CF0784` |
| `assets/icons/zrdp.ico` | 57,871 byte | `C6DC184663714FB8C4E7BC2147BA47FFF0B0F4F61A6FBF41853F0B485947E43D` |
| Release Client抽出icon | 1,736 byte | `49E6E9DB082CF6F295168D067CFE0BA62A80F73455D92F7688E0FB88D3E1DF24` |
| Release Agent抽出icon | 1,736 byte | `49E6E9DB082CF6F295168D067CFE0BA62A80F73455D92F7688E0FB88D3E1DF24` |

## 5. 自動検証

- Debug build: 成功
- Debug CTest: `28 / 28`成功
- Release build: 成功
- Release CTest: `28 / 28`成功
- `unit.resource_contract`: Client／Agent共通icon resource確認
- `unit.portable_binary_contract`: x64とVersion `0.7.9.0`確認
- RDP ActiveX、Agent、Proxy、HTTP／SVN、Trac、Hardeningの既存testも回帰成功

## 6. Portable成果物

| file | size | version | SHA-256 | signature |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 668,672 byte | `0.7.9.0` | `51170D17AE7D38A66834800F4C1CC32B0F057367F9620D314A066DA321D74B6B` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 413,696 byte | `0.7.9.0` | `0DEBCC66D96E147716A38B2A580013B3208C1964F94121127ED9C4B8287B4780` | NotSigned |

## 7. 残りGate

- Explorer、taskbar、title barでVersion `0.7.9.0`が旧版より大きく見える
- lower-right endpointがroyal purpleで表示され、黄色に見えない
- 明るいthemeと暗いthemeでZ型Bridgeと二つのendpointを識別できる
- Client／Agentで同じiconが表示される

Windows icon cacheに旧意匠が残る場合、system cacheを自動削除せず、新しいEXEの
title barまたは別folderへコピーしたExplorer表示を正本として確認する。
実機Gate通過までは改善I-008を完了扱いにしない。
