# Version 0.7.13 Documentation／Release候補確認

確認日: 2026-07-31

## 1. ユーザー方針

Version `0.7.13.0`を社内で試行利用し、通常利用中に判明した不具合を項目単位で
報告・修正する。従来の実機観察項目は隠さず、既知の制約とリリースノートへ記録する。

## 2. 作成文書

- `README.md`
- `docs/specification.md`
- `docs/user-guide.md`
- `docs/release-notes-0.7.13.md`
- `docs/release-checklist.md`

既存のarchitecture、protocol、RDP、Proxy、Agent、configuration、security、
diagnostics、test planをVersion `0.7.13.0`へ集約した。

## 3. 文書監査

- 対象文書: 5 file
- Token／certificate／利用者／実endpointの識別marker: 0件
- 許可していないIPv4 literal: 0件
- 相対Markdown link切れ: 0件
- Version `0.7.13` marker: 全文書で確認
- 設定例: loopbackまたはRFC 5737文書用addressだけ

USB token、certificate subject、thumbprint、provider情報、PIN／PUKは文書、log、
scriptへ記録していない。

## 4. Build／test baseline

- Debug build: warningなし
- Debug CTest: 30／30成功
- Release build: warningなし
- Release CTest: 30／30成功
- Client／Agent: Version `0.7.13.0`

## 5. 未署名正本

標準`out`と同じ未署名成果物を
`work/tmp/release-unsigned-final-0.7.13/`へ保全した。

| File | SHA-256 | Authenticode |
|---|---|---|
| `ZRdpSvnBridge.exe` | `9DA3ED367BFAD9FEBEB153C016FA40B2085E0AACE9290CEF3EF0B6A1A3B2C9F3` | `NotSigned` |
| `ZRdpSvnAgent.exe` | `4A4258932A64749E2905476B3D989028B05635AF4BA5C2690F30D79D2465954D` | `NotSigned` |

以前の一時Release候補は同じVersionでも現在の標準`out`とhashが異なるため、署名前
正本として使用しない。上表の二fileだけを今回の署名対象とする。

## 6. 署名境界

`AGENTS.md`に従い、USB token、certificate、PINをCodexは操作しない。組織から権限を
付与されたユーザーがClient／Agentを署名し、完了連絡後にCodexが次をread-onlyで
検証する。

- Authenticode status
- TimestampとWindows trust validation
- File／Product Version
- 署名済みSHA-256
- 配布file一覧
- 署名後の最小RDP／Agent／Proxy回帰結果

署名自動化script、batch、certificate export、秘密鍵fileは作成しない。
