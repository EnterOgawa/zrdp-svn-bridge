# RDPドメインとユーザー名の分離入力

## 目的

ユーザー名欄へ`DOMAIN\user`形式を入力しても、Microsoft標準credential UIへ
ドメインが期待どおり反映されない環境がある。ZRdpの入力欄を「ドメイン」と
「ユーザー名」へ分離し、Remote Desktop ActiveX Controlの標準propertyへ
それぞれ設定する。

## UIと入力規則

- 接続先、ドメイン、ユーザー名を別々の編集可能combo boxとする。
- ドメインは任意入力とする。
- ドメインが空で、ユーザー名が`DOMAIN\user`形式の場合は、既存入力との
  互換性のため接続前に一度だけ分離して各欄へ反映する。
- ドメイン欄が入力済みの場合、ユーザー名欄に`\`を含む曖昧な指定は拒否する。
- ドメインだけが入力され、ユーザー名が空の場合は接続を開始しない。
- UPN形式の`user@example.test`は、ドメイン欄が空ならユーザー名として
  そのまま扱う。

実在するドメイン名、ユーザー名、接続先はsource、test、文書、logへ記録しない。

## 変更対象

- `src/client/app/rdp_user_identity.h/.cpp`
  - ドメインとユーザー名の検証、旧`DOMAIN\user`入力の分離、履歴用の
    再結合を一つの境界へ集約する。
- `src/client/app/main_window.h/.cpp`
  - ドメインlabel／combo boxを追加する。
  - 接続前に入力を正規化し、`RdpConnectionSettings`へ分離して渡す。
  - 接続先履歴選択時に、対応するドメインとユーザー名を復元する。
- `src/client/rdp/rdp_control_host.h/.cpp`
  - `RdpConnectionSettings::domain`を追加する。
  - 接続前に`IMsTscAx::put_Domain`と`put_UserName`を別々に呼ぶ。
- `src/client/app/connection_history.h/.cpp`
  - process内entryを「接続先＋ドメイン＋ユーザー名」の3項目とする。
  - 重複排除も3項目の大小文字を無視した完全一致で行う。
- resource、test、設計文書、roadmap、version情報を更新する。

## 接続履歴互換性

暗号化fileのmagic、version、DPAPI entropy、field数は変更しない。永続化時だけ、
ドメインが存在する場合は従来のユーザー名fieldへ`DOMAIN\user`形式で格納する。
読込み時は同じfieldをドメインとユーザー名へ分離する。

この方式により次を満たす。

- 既存`0.1.9.0`までの履歴を`0.1.10.0`で読める。
- `0.1.10.0`で保存した履歴を旧版へ戻しても、旧版は従来どおり
  `DOMAIN\user`形式のユーザー名として読める。
- passwordは引き続き保存しない。

## ActiveX／COMへの影響

既存のCOM STA UI thread上で、接続前に次を設定する。

```cpp
client_->put_Domain(domain);
client_->put_UserName(user_name);
```

空ドメインも毎回設定し、同じActiveX instanceで前回接続のドメインが残らない
ようにする。password入力、NLA、CredSSP、証明書警告は従来どおりMicrosoft
標準UIへ委譲し、`ClearTextPassword`は使用しない。

## Security boundary

- ドメインとユーザー名を診断logへ出力しない。
- 接続履歴は従来どおりcurrent-user DPAPIで暗号化する。
- password、credential、認証tokenをZRdpへ取り込まない。
- 入力文字数、埋込みNUL、曖昧な`\`指定を接続前に拒否する。
- ドメイン名からDNS探索、network列挙、接続先探索を行わない。

## 失敗時動作

- 入力の分離または検証に失敗した場合はActiveX設定前に接続を拒否し、
  固定resource messageだけを表示する。
- `put_Domain`または`put_UserName`が失敗した場合は接続前設定全体を失敗させ、
  `Connect`を呼ばない。
- 履歴保存失敗は成立済みRDP接続を切断せず、固定eventとHRESULTだけを記録する。

## Protocol compatibility

Static Virtual Channel `ESVNBRG`、Application Protocol、localhost HTTP Proxy、
Agent、SVN通信には影響しない。RDP認証前設定とClient UIだけの変更である。

## Test

- 空ドメイン＋通常ユーザー名。
- ドメイン＋ユーザー名。
- 空ドメイン＋旧`DOMAIN\user`入力の分離。
- 空ドメイン＋UPN形式の維持。
- ドメイン入力時の`\`付きユーザー名拒否。
- ドメインだけの入力拒否。
- 文字数上限、埋込みNUL、複数`\`の拒否。
- 履歴の3項目重複排除、候補重複排除、暗号化、旧形式互換。
- x64 Debug／Release build、CTest、portable EXE起動。
- 実機でMicrosoft標準password画面にドメイン付きアカウントが表示されること。

## Rollback

UI、`RdpConnectionSettings::domain`、`put_Domain`、identity helperを削除し、
接続履歴entryを接続先と結合済みユーザー名の2項目へ戻す。永続化formatを
変更しないため、履歴fileの削除やdata migrationは不要である。
