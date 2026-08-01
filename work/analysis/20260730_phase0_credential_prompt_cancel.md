# Phase 0 credential promptキャンセル処理の実装・検証記録

## 対象

- version: `0.1.11.0`
- 実施日: 2026-07-30
- 対象: Microsoft標準credential UIを利用者が閉じた場合の`OnDisconnected`

実環境の接続先、ドメイン、ユーザー名、画面内容は本記録へ転記していない。

## 原因

credential UIのcloseはActiveXから`OnDisconnected`として通知される。従来のUIは
ZRdp自身の切断要求以外をすべて異常として扱っていたため、利用者がcredential UIを
閉じた後にも追加のRDP接続error dialogを表示していた。

## 修正

次の完全一致だけをcredential promptの利用者取消として分類する。

- 切断直前stateが`Connecting`または`Connected`
- disconnect reasonが`7943 (0x1F07)`
- extended reasonが`0`
- ZRdp自身が要求した切断ではない

一致時は固定の未接続表示へ戻し、接続入力を再び有効化して、追加error dialogを
作成しない。数値診断はInformation levelで
`credential_prompt_cancelled=1`として記録する。

`LoggedIn`後、別reason、拡張reasonあり、local切断はこの分類から除外した。
credential内容、接続先、ドメイン、ユーザー名は判定にもlogにも使用しない。

## 自動検証

### Debug

- configure: 成功
- build: 成功、warning 0
- CTest: 5/5成功

### Release

- build: 成功、warning 0
- CTest: 5/5成功

分類testでは、`Connecting`／`Connected`の正例と、`LoggedIn`、別reason、
拡張reasonあり、local切断の負例を確認した。診断log testでは固定数値fieldだけが
出力されることを確認した。実RDP接続は自動testで行っていない。

## 配布物

- path: `C:\develop\zrdp\out\ZRdpSvnBridge.exe`
- size: 450,560 bytes
- file／product version: `0.1.11.0`
- SHA-256:
  `312D324482382DF0EA5580541E12D917FAD94E3728C47086FA17A870504D0344`
- Authenticode: 未署名
- architecture: x64／PE32+
- High Entropy VA、ASLR、DEP、Control Flow Guard: 有効
- import: Windows標準DLLのみ

## 未完了Gate

実機でcredential UIをcloseし、追加error dialogが表示されず、ZRdpの接続buttonが
再び利用できることを確認する。結果確認まではユーザー環境Gateを未完了とする。
