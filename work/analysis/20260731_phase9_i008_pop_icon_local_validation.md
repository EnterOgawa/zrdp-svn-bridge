# Phase 9改善I-008 枠なしpopカラーiconのローカル検証

更新日: 2026-07-31

## 1. 対象

- Client／Agent version: `0.7.8.0`
- 対象要望: 濃紺または黒いtile状の外枠を廃止し、明るくpopなiconへ変更する
- ClientとAgentは一製品familyとして同じiconを使用する

## 2. デザイン境界

- 外周の角丸tile、badge、container、shieldを持たない
- sky blueからturquoiseへ変化するZ型connection pathをBridgeとして使用する
- coralとsunshine yellowの二つのdesktop endpointを配置する
- black、charcoal、navyの背景を使用しない
- 文字、商標、watermark、細かな回路、外部shadowを持たない
- transparent backgroundとし、明背景と暗背景の双方で色を維持する

## 3. 生成と透明化

- Codex built-in image generationを使用した
- flatな`#ff00ff` chroma-key背景で生成した
- imagegen skillの`remove_chroma_key.py`によりlocalでalpha化した
- 透過subjectを正方形へcropし、1024×1024 RGBA masterを生成した
- 16、20、24、32、40、48、64、128、256pxのICOを生成した
- 最終promptは`assets/icons/README.md`へ記録した
- 導入した外部logo、商標、stock assetはない

## 4. 画像検証

- master alpha範囲: 0～255
- master四隅alpha: すべて0
- magenta相当の不透明pixel: 0
- 不透明pixelの最小luminance: 121
- 16、32、48pxを明背景と暗背景へ合成し、外周tileがないことを確認した
- Debug／ReleaseのClient／Agent EXEから32px iconを抽出した
- Client／Agentから抽出したiconのSHA-256が一致した

| file | size | SHA-256 |
|---|---:|---|
| `assets/icons/zrdp-icon.png` | 205,607 byte | `E9C2385A1EDCE7D9C70D4751330314CC746096361C53CFF430F33A3692DA3B43` |
| `assets/icons/zrdp.ico` | 45,362 byte | `381411C3941B0EDE5D3FDE5A3E86603861B89C4AED61EC2081EA973AF22CFC23` |
| Release Client抽出icon | 1,327 byte | `7F59493D824FCDF546CD7E0776771C12E4297A9F4C2F806177B5B242BC455642` |
| Release Agent抽出icon | 1,327 byte | `7F59493D824FCDF546CD7E0776771C12E4297A9F4C2F806177B5B242BC455642` |

## 5. 自動検証

- Debug build: 成功
- Debug CTest: `28 / 28`成功
- Release build: 成功
- Release CTest: `28 / 28`成功
- `unit.resource_contract`: Client／Agent共通icon resource確認
- `unit.portable_binary_contract`: x64とVersion `0.7.8.0`確認
- RDP ActiveX、Agent、Proxy、HTTP／SVN、Trac、Hardeningの既存testも回帰成功

## 6. Portable成果物

| file | size | version | SHA-256 | signature |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 655,872 byte | `0.7.8.0` | `E7E6A493D48F6B3967415AEE4EB8E1A3C51D2F0221E4E7FA92959676BBAB5179` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 400,896 byte | `0.7.8.0` | `CCC76795806EA6CA3779776E6765F1723DE2F2EC1ADD3E8BBE75A7F11320C16A` | NotSigned |

## 7. 残りGate

- ExplorerでClient／Agentが枠なしの明るいiconとして表示される
- 接続管理Window、session Window、Agent console titleで同じiconが表示される
- taskbarのsmall iconでもZ型Bridgeと二つのendpointを識別できる
- 明るいthemeと暗いthemeで黒いtile状に見えない

Windows icon cacheに旧意匠が残る場合、system cacheを自動削除せず、新しいEXEの
title barまたは別folderへコピーしたExplorer表示を正本として確認する。
実機Gate通過までは改善I-008を完了扱いにしない。
