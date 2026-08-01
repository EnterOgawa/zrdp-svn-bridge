# Phase 9改善I-004 RDP音声リダイレクト ローカル検証

検証日: 2026-07-31

## 1. 対象

- Client／Agent version: `0.7.4.0`
- Platform: Windows x64
- Build: Visual Studio 2022、C++20、`/W4`、`/WX`、Release `/MT`
- wire protocol: Version `1`のまま
- Agent／Proxy／allowlist: 変更なし

## 2. 実装結果

- 接続管理Windowへ、既定OFFの`マイクを接続先で使用する`を追加した。
- リモート音声の再生先として、次の3項目を追加した。
  - このPCで再生する
  - 接続先PCで再生する
  - 再生しない
- 音声設定はsession生成時に所有copy化し、Connect前だけActiveXへ適用する。
- `AudioRedirectionMode`と`AudioCaptureRedirectionMode`はsetter後にreadbackする。
- 接続履歴をVersion 3へ更新し、音声設定をcurrent-user DPAPI保護対象へ追加した。
- Version 1／2履歴はマイクOFF／このPCで再生する既定値で読み込む。
- password、音声data、device名、実接続先情報を設定fileまたはlogへ追加していない。

## 3. 自動テスト

```text
Debug:   26 / 26 passed
Release: 26 / 26 passed
```

重点確認:

- マイクON／OFFの接続前設定とreadback
- 3種類の音声再生先と未知enum拒否
- Version 3履歴の保存／復元
- Version 1／2履歴の後方互換読込
- 不正音声値の保存拒否
- Client resource文字列
- Client／Agent x64、version resource一致
- 既存Protocol、Bridge、Proxy、Agent、Phase 1～6結合testの回帰

実RDP、支給PC、Zscaler、実SVN／Tracへの自動接続は行っていない。

## 4. Portable成果物

| File | Size | Version | SHA-256 | Authenticode |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 673,792 byte | 0.7.4.0 | `50CE86CF2E9003447E26D31B5F70EAC26FC1E936C031CF9D655841146B269E61` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 423,424 byte | 0.7.4.0 | `EC8D195B0C76781774EA6404F72D892BC5ACBBA2CEE652D74131B77E45FBA84C` | NotSigned |

未署名はRelease Gate前の想定状態である。署名処理はユーザーが実施する。

## 5. 実機確認待ち

- 接続管理Windowの追加行がDPI変更時にも欠けず表示される。
- マイクOFFで従来接続に回帰がない。
- マイクONでクライアントPCの既定マイクを接続先sessionから利用できる。
- リモート音声をこのPCで再生できる。
- 名前付きprofileから音声設定を復元できる。
- 設定変更が新しいsessionまたは再接続後に反映される。
- Agent、Proxy、SVN／Trac操作に回帰がない。

GPOまたはWindows privacy設定が転送を禁止する場合は、ZRdpから変更または回避せず、
適用中の制約として報告する。
