# ZRdpアプリアイコン

更新日: 2026-07-31

## デザイン

外周の角丸タイルやbadgeを持たないfree-standing symbolとした。コーラルと
royal purpleの二つのdesktop endpointを、sky blueからturquoiseへ変化する
Z字状data pathで接続し、ZRdpとRDP内のSVN Bridgeを表現する。黄色、gold、
amber、limeは使用しない。

黒、濃紺、charcoalの背景を使用せず、明背景と暗背景の双方で色が残る
明るい配色とした。文字や細かな回路表現を使わず、taskbarやExplorerの小さい
表示でもZ型connectionのsilhouetteが残る。透明余白は左右約2%、上下約4%に
限定し、subjectはmaster幅の約96%、高さの約92%を占める。ClientとAgentは
一つの製品familyなので同一iconを使用し、EXE名とwindow titleで役割を区別する。

## ファイル

| File | 用途 |
|---|---|
| `zrdp-icon.png` | 1024×1024 RGBAの透過master |
| `zrdp.ico` | 16、20、24、32、40、48、64、128、256pxのWindows icon |
| `zrdp-icon.rc` | Client／Agentへ同じICOを埋め込む共通resource |

透過masterは四隅のalphaが0、alpha範囲が0～255であり、chroma key由来の
green edgeと黄色系endpointが残っていないことを確認した。16、24、32、48pxを
明背景と暗背景へ合成し、外周tileがなくendpointとZ型pathを識別できることを
確認した。

## 生成方法

Codexのbuilt-in image generationを使用し、flatな`#00ff00` chroma-key背景で
原版を生成した。導入した外部logo、商標、stock assetはない。背景除去後のPNGから
Pillowでsubjectを横約96%まで拡大し、Windows用の各sizeを作成した。

最終prompt:

```text
Use case: precise-object-edit
Asset type: Windows desktop application icon master for ZRdp
Input image: Image 1 is the edit target. Preserve its core identity: two
simplified desktop endpoints connected by one rounded geometric Z-shaped
bridge.
Primary request: Make the existing free-standing symbol substantially larger
and bolder for Windows taskbar and title-bar use. The combined symbol must
occupy about 90–94% of the square canvas in both width and height, leaving only
3–5% clear background margin on each outer side. Make the Z ribbon and endpoint
shapes about 20% thicker and slightly more compact so they remain clearly
readable at 16x16 pixels.
Color change: Replace every yellow, gold, and amber part of the lower-right
endpoint with a saturated royal purple around #6847E8. Do not use any yellow,
gold, amber, mustard, or lime anywhere. Keep the upper-left endpoint coral,
keep the bridge bright sky blue to turquoise, and retain small white screen
highlights.
Scene/backdrop: perfectly flat solid #00ff00 chroma-key background for local
background removal; exactly one uniform color with no shadow, gradient,
texture, reflection, or floor plane.
Style/medium: polished vector-like flat icon, friendly and pop, crisp bold
geometry.
Invariants: keep the icon free-standing with no outer tile, badge, container,
enclosing frame, shield, circle, black background, navy background, charcoal
background, text, watermark, trademark, external shadow, or extra object.
Do not use #00ff00 in the subject. Preserve clear contrast on both light and
dark Windows backgrounds.
```
