# 全画面接続バーへの接続先表示

## 目的

全画面表示ではZRdp本体の接続先コンボボックスが見えないため、複数のRDP接続を
識別しにくい。Microsoft Remote Desktop ActiveXの全画面connection barへ、
現在の接続先PC名またはIPアドレスを製品名と併記する。

表示形式は次とする。

```text
ZRdp SVN Bridge - <接続先>
```

## 変更対象

- `src/client/rdp/rdp_control_host.cpp`
  - 検証済み`RdpConnectionSettings::server`から有限長の全画面表示を構築する。
  - `IMsTscAx::FullScreenTitle`と
    `IMsRdpClientNonScriptable3::ConnectionBarText`へ同じ値を設定する。
  - `FullScreenTitle`の`MAX_PATH - 1`文字制約を超える場合は、接続先設定を
    変更せず、表示末尾だけを`...`として省略する。
  - `ConnectionBarText`をreadbackし、設定した有限長表示との完全一致を確認する。
- `tests/unit/rdp_host_smoke_test.cpp`
  - 文書用IPv4 literalで全画面表示を含む接続前設定が成功することを確認する。
  - 空、上限超過、埋め込みNULを拒否する。
- `AGENTS.md`、`docs/rdp-activex.md`、`docs/test-plan.md`、`docs/roadmap.md`
  - 全画面接続バーの識別要件と実機確認項目を記録する。
- `CMakeLists.txt`、resource、manifest
  - versionを`0.1.9.0`へ更新する。

## ActiveX／COMへの影響

既に使用中の`put_FullScreenTitle`と`put_ConnectionBarText`へ渡すBSTR内容だけを
変更する。新しいCOM interface、event、thread、network処理は追加しない。
設定は従来どおり接続前にCOM STA UI threadで適用する。

## Security boundary

- 接続先は利用者がZRdpへ入力済みで、通常windowでも表示している値だけを使う。
- DNS解決、接続先探索、実際のremote IP取得は追加しない。hostname入力時は
  hostnameをそのまま表示する。
- 接続先を診断log、resource、文書、追加設定fileへ出力しない。
- 接続履歴のDPAPI保護、件数、保存形式は変更しない。
- password、ユーザー名、credential、payloadは全画面表示へ含めない。

## 失敗時動作

有限bufferで表示文字列を構築できない場合、BSTR生成に失敗した場合、または
ActiveX setterまたはreadbackが失敗した場合は接続前設定全体を失敗とし、
曖昧な全画面表示のまま接続を開始しない。HRESULTだけを既存の固定eventで
記録する。

## Protocol compatibility

Static Virtual Channel `ESVNBRG`とApplication Protocolに影響しない。
wire format、SVN Bridge、localhost Proxy、Agentは変更しない。

## Test

- x64 Debug／Release buildとCTest。
- 空、上限超過、埋め込みNULの接続先拒否を維持する。
- 文書用IPv4 literalで接続前設定と`ConnectionBarText` readbackが成功する。
- 実RDP全画面接続バーに、入力した接続先が製品名とともに表示される。
- 全画面からwindowへ戻った後も接続状態と3表示modeが維持される。

## Rollback

全画面titleとconnection bar textを固定の`ZRdp SVN Bridge`へ戻し、version、
test、文書を直前版へ戻す。接続履歴formatを変更しないためdata migrationは
不要である。
