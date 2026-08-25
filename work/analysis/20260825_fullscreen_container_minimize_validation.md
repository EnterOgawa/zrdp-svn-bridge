# R-010 全画面connection bar最小化 ローカル検証

日時: 2026-08-25

## 対象

- Version: `0.7.16.0`
- Client: `out/ZRdpSvnBridge.exe`
- Agent: `out/ZRdpSvnAgent.exe`
- 変更対象: ClientのRDP ActiveX eventとSession Window最小化処理
- 非変更対象: Agent runtime、Virtual Channel、Application Protocol、allowlist、Proxy

## 実装確認

- `ContainerHandledFullScreen=1`を接続前に設定し、非0 readbackを必須とした。
- `OnRequestContainerMinimize`のDISPID 14を引数なし所有eventへ変換した。
- COM callbackからSession Windowへ標準`SC_MINIMIZE`を非同期送信した。
- `SIZE_MINIMIZED`ではActiveX再配置と動的解像度timerを停止した。
- 復元後は既存`WM_SIZE`／`LayoutControls`／debounce経路だけを使用する。
- 固定log event `rdp.event.request_container_minimize`だけを追加した。

## 自動検証

- Debug build: 成功、warning 0
- Debug CTest: 30／30成功
- Release build: 成功、warning 0
- Release CTest: 30／30成功
- resource／portable binary contract: Debug／Releaseとも成功
- source comment contract: Debug／Releaseとも成功
- `git diff --check`: 成功

## 未署名成果物

| File | size | Version | Authenticode | SHA-256 |
|---|---:|---|---|---|
| `ZRdpSvnBridge.exe` | 677,888 byte | `0.7.16.0` | `NotSigned` | `E99C0108A4F9D690DA41A8540CA172CA24964780D3C19BC300B17458F717CA80` |
| `ZRdpSvnAgent.exe` | 417,280 byte | `0.7.16.0` | `NotSigned` | `DF11B687288AAB6778E5096C74936DBC157A5763427DD12B8876732ADB936220` |

Client／Agentは配布時の組を明確にするためVersion resourceだけを同じ`0.7.16.0`へ
揃えた。Agentのchannel／Socket処理は変更していない。

## 署名済み成果物

ユーザーの明示指示により、Client／Agentを既存Releaseと同じ署名者で一括署名した。
両方ともSHA-256 Authenticode署名、RFC 3161 timestamp、Windows trust `Valid`、
SignTool検証成功、Version `0.7.16.0`維持を確認した。

| File | size | Version | Authenticode | SHA-256 |
|---|---:|---|---|---|
| `ZRdpSvnBridge.exe` | 690,248 byte | `0.7.16.0` | `Valid` | `E6C203DFC65E468F363BC12052FB4063898F4FE4178DB323FB6D6164F6717AC6` |
| `ZRdpSvnAgent.exe` | 429,640 byte | `0.7.16.0` | `Valid` | `701113D6FCEDC7BF0D323B20D034AE61D39F50954B9B75C2C8F63F66A61A37AE` |

署名後のresource contractとportable binary contractは2／2成功した。署名前正本は
Git管理外の`work/tmp/unsigned-0.7.16.0-fullscreen-minimize`へ保全した。

## 残る実機Gate

- 全画面connection barの最小化でSession Window全体がtaskbarへ移ること
- 白いSession Windowが画面上に残らないこと
- taskbarから全画面RDP表示へ復元できること
- 動的解像度が復元後のmonitorへ追従すること
- RDP、Agent、Proxy、既存HTTP stream、別sessionへ影響しないこと

実機Gateを通過するまで本成果物は署名済み試験候補として扱う。
