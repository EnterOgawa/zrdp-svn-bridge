# R-012 全画面復帰経路 ローカル検証

日時: 2026-08-25

## 実機現象とlog

Version `0.7.17.0`ではZRdpのborderless container全画面が成立し、title bar、frame、
表示mode欄は消えた。一方、Microsoft connection barが描画されず、mouse操作による
最小化とWindow復帰経路が失われた。

固定診断logでは次を確認した。

- `rdp.event.request_go_full_screen`
- `rdp.full_screen_call_returned hresult=0x00000000`
- 復帰までに`request_leave_full_screen`は記録されていない

## 原因

ZRdpは`ConnectionBarText`だけを設定し、connection barの表示状態、raised状態、
最小化button、復帰buttonを明示していなかった。またcontainer-handled全画面で
Control側barが描画されない場合の可視fallbackを持っていなかった。

## 修正

- `DisplayConnectionBar=VARIANT_TRUE`
- `PinConnectionBar=VARIANT_FALSE`（raised状態）
- `ConnectionBarShowMinimizeButton=VARIANT_TRUE`
- `ConnectionBarShowRestoreButton=VARIANT_TRUE`
- 全propertyの接続前readback
- 全画面右上へZRdp `最小化`／`ウィンドウ`buttonをoverlay
- fallback最小化を既存`SC_MINIMIZE`経路へ接続
- fallback Window復帰を既存ActiveX／container rollback経路へ接続
- 表示mode欄は全画面中に非表示のまま維持

Virtual Channel、Protocol、Proxy、Agent runtime、allowlistは変更していない。

## 自動検証

- Debug build: 成功、warning 0
- Debug CTest: 30／30成功
- Release build: 成功、warning 0
- Release CTest: 30／30成功
- RDP ActiveX接続前smoke test: 成功
- connection bar property setter／readback: 成功
- fallback button右上配置policy: 成功
- source comment／resource／portable binary contract: 成功

## 未署名成果物

| File | size | Version | Authenticode | SHA-256 |
|---|---:|---|---|---|
| `ZRdpSvnBridge.exe` | 682,496 byte | `0.7.18.0` | `NotSigned` | `C18956FA3B366664109318A287CFB9AC5EE921CB7345DBC8908396B4A7301197` |
| `ZRdpSvnAgent.exe` | 417,280 byte | `0.7.18.0` | `NotSigned` | `4D572EECDBB50A3CDAA1EC4036AAF5206A97A63A3B49ADDA6F74F9BAA8E7DF05` |

標準`out`へ配置済みである。検証時にClient／Agent processは起動していない。

## 残る実機Gate

- 全画面でMicrosoft connection barまたはZRdp fallbackが操作できること
- `ウィンドウ`で全画面前の通常／最大化配置へ戻ること
- `最小化`でlocal desktopへ戻り、taskbar復元後も全画面を維持すること
- `Ctrl+Alt+Break`、三表示mode、動的解像度が従来どおり機能すること
- RDP、Agent、Proxy、HTTP stream、別sessionへ回帰がないこと
