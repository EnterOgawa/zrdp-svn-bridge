# Phase 0 RDPドメイン分離入力の実装・検証記録

## 対象

- version: `0.1.10.0`
- 実施日: 2026-07-30
- 対象phase: Phase 0
- 対象機能: RDP接続前のドメイン／ユーザー名入力と接続履歴

実環境の接続先、ドメイン、ユーザー名、画面内容は本記録へ転記していない。

## 実装結果

- 接続先、ドメイン、ユーザー名を別々の編集可能combo boxとして配置した。
- ActiveXへ`IMsTscAx::put_Domain`と`put_UserName`を別々に適用した。
- ドメインを空に戻した接続では、前回値を持ち越さないよう空値も明示的に
  `put_Domain`へ適用した。
- ドメイン欄が空の場合に限り、旧`DOMAIN\user`入力を分離して正規化した。
- UPN形式はドメインを推測せず、ユーザー名として維持した。
- 分離ドメインと結合済みユーザー名の同時指定、複数区切り、埋込みNUL、
  上限超過を接続前に拒否した。
- process内の履歴entryを「接続先＋ドメイン＋ユーザー名」の3項目とした。
- 暗号化履歴fileはversion 1の2 field形式を維持し、保存時だけ
  `DOMAIN\user`へ結合することで旧版との双方向互換を維持した。
- passwordの入力、保存、log出力は追加していない。

## 自動検証

### Debug

- `cmake --preset vs2022-x64`: 成功
- `cmake --build --preset debug --parallel`: 成功、warning 0
- `ctest --preset debug --output-on-failure`: 5/5成功

### Release

- `cmake --build --preset release --parallel`: 成功、warning 0
- `ctest --preset release --output-on-failure`: 5/5成功

成功したtest:

- `unit.build_contract`
- `unit.resource_contract`
- `unit.rdp_host_smoke`
- `unit.diagnostic_log`
- `unit.connection_history`

`unit.rdp_host_smoke`は実RDP接続を行わず、インストール済みMicrosoft RDP
ActiveXへ分離したDomain／UserNameを設定できること、空domainへの戻し、
不正入力の拒否を確認した。

## 配布物

- path: `C:\develop\zrdp\out\ZRdpSvnBridge.exe`
- size: 450,560 bytes
- file version: `0.1.10.0`
- product version: `0.1.10.0`
- SHA-256:
  `4E4CBC783A030D3F0B4299A02F85883B2141AAD4995F238713879CBDC4B8BB44`
- Authenticode: 未署名

PE検査:

- x64 / PE32+
- Windows GUI
- High Entropy VA: 有効
- ASLR Dynamic Base: 有効
- DEP NX Compatible: 有効
- Control Flow Guard: 有効
- importはWindows標準DLLのみ

非表示起動probeではprocessの起動継続を確認した。非表示windowへ
`CloseMainWindow`を送れなかったため、probeで生成したprocessだけをPID指定で
終了し、残存processがないことを確認した。

## 未完了Gate

Microsoft標準credential UIにドメイン指定が反映されることは、実際の接続先と
認証画面を必要とするため自動testでは確定できない。ユーザー実機で次を確認する。

1. ドメイン欄へドメインだけを入力する。
2. ユーザー名欄へドメインを含まないユーザー名だけを入力する。
3. 接続し、Microsoft標準password入力画面で対象accountのドメインを確認する。
4. 認証完了後、再起動して3項目の履歴候補と重複排除を確認する。

この確認が完了するまで、Phase 0のドメイン反映Gateは未完了とする。SVN Bridgeの
Phase 1実装は本変更に含まない。
