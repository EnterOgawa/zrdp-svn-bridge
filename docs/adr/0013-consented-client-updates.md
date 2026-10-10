# ADR 0013 Clientの同意付き自動更新

- Status: Accepted
- Date: 2026-10-07

2026-10-10の追加指示により、Clientだけの取得・置換・単独backup方針は
[ADR 0023](0023-paired-client-agent-updates.md)の同一版Client／Agentセット更新で置き換える。
同意、有限取得、署名、全session終了待ち、process非強制停止と公開制限は維持する。
以下は当初の決定記録である。

## Context

新しいReleaseを通知し、利用者が許可した場合だけClientを自動更新する。
更新対象はClientのみ、配布元は公開用GitHub Releasesとする。既存Private
リポジトリの公開、Release作成、署名、uploadはこの実装に含めない。

## Decision

- `client.ini`の`[update]`で公開リポジトリを`owner/repository`形式で指定する。
  既定sourceはユーザーが指定した`EnterOgawa/zrdp-svn-bridge`とする。
  `enabled=false`または不正な設定の場合は外部通信をしない。認証tokenを扱わない。
- 起動時と稼働中の24時間ごとに更新情報を取得する。2026-10-10の追加指示で
  managerメニューと手動確認入口を廃止する。確認はworker上で行い、
  同時確認と結果queueを各一件に制限する。
- 最新Releaseの`zrdp-client-update.ini`だけを通知前に取得する。EXEは同意後に
  ダウンロードし、保留／取消時には入れ替えない。
- manifestの形式、四要素version、tag、固定asset名、size、SHA-256、Bridge
  protocol互換性を有限上限で検証する。古い版、同じ版、protocol非互換版は
  自動適用しない。
- Windows標準WinHTTPでHTTPSを使用する。GitHubと指定したRelease asset
  配信用hostだけを許可し、redirectごとに宛先とHTTPSを再検証する。証明書
  検証、OS通信policy、Zscalerを変更せず、HTTPや別経路へfallbackしない。
- WindowsのAuthenticode検証、SHA-256、x64 PE、正式製品名、OriginalFilename、
  version、更新bootstrap形式を検証する。署名者は実行中Clientの公開鍵と
  比較し、別publisherの正規署名を持つEXEも拒否する。証明書識別情報を
  設定、source、診断logへ保存しない。
- 未署名の開発buildからの自動適用は拒否する。署名鍵変更とBridge protocol
  非互換更新は、確認済み配布物の手動適用で行う。
- ダウンロード済み候補はEXE隣接の専用stageへ置く。対象はClient EXEだけで、
  `client.ini`、`agent.ini`、履歴、log、Agent EXEを変更しない。
- 同意後でもsession Windowが一つ以上あれば適用を保留する。全session終了後に
  適用を開始し、適用中の新規session開始を拒否する。RDPやSVN／Trac streamを
  自動切断せず、別processを強制終了しない。
- 実行中の同じClient EXEをstageへ複写し、内部maintenance modeで起動する。
  永続helper、service、installer、管理者昇格、追加配布EXEは導入しない。
- helperは継承した親process handleで終了を待ち、固定Client pathと検証済み候補
  だけを同一volume内で置換する。署名・hashと旧版の不変を適用直前にも確認する。
  update mutexで同じClient配置への競合を防ぐ。
- 旧版EXEをbackupし、差替え後の検証失敗や新processの起動失敗では旧版へ戻す。
  再起動したprocessを強制終了してrollbackする処理は行わない。
- 正常再起動後は旧版を`ZRdpSvnBridge.previous.exe`へ一世代保全する。復旧後も
  同一配置mutexでstage回収を直列化する。未知のbackupを上書きしない。
  残存stageが2件以上の場合は新しい更新を止め、無制限に蓄積しない。
- helperのfile pathは自身のstage配置から導出し、command lineから任意の
  差替え先、URL、実行commandを受け付けない。継承HANDLEも専用一覧に限定する。
- ローカルの実行ユーザーが書込み権限を持つClient配置を前提とする。
  同じユーザー自身や管理者による既存EXE改変への完全防御は保証しない。

## Consequences

Clientの更新取得だけが新たなHTTPS用途となる。Agent、HTTP SVN／Trac中継、
`ESVNBRG`とwire formatは変更しない。ClientとAgentの製品versionが異なる場合も、
同じBridge protocolで互換な組だけを自動更新対象とする。

初回は自動更新機能を含む署名済みClientを手動適用する必要がある。公開配布元、
署名済みEXEとmanifestの公開が揃うまで、本機能だけでReleaseを公開することはない。
更新先の404、通信制限、署名検証失敗、書込み権限不足は更新だけを停止し、RDPを維持する。

## Validation

- version、manifest、設定、repository／redirectの形式・長さ・overflowを単体testする。
- 同意前にEXEを取得せず、取消時とsession生存中に適用しないことをfake backendで検証する。
- unsigned、hash不一致、別署名者、metadata不一致、downgradeを拒否する。
- 正常差替え、差替え失敗、backup復元、新process起動失敗を隔離fixtureで検証する。
- manager UI、既存RDP、Proxy、AgentのDebug／Release全CTestを再実行する。
- 実設定とPrivate設定を検証作業で変更しない。検証だけのbuildと成果物の更新を区別する。
  利用者指定の成果物更新では`D:\develop\zrdp\out`へ最新版を出力し、旧署名済みEXEを
  上書き前に保全する。未署名buildを署名済み配布版とは扱わない。

隔離Debug／Releaseで各37 CTestを通過した。実Windowsのfile置換・復元と
既存配布Clientの署名検証も確認した。公開Releaseを使う署名済み新版への一連の
受入は、初回bootstrap版の署名・手動配布と次のReleaseを用意した後に行う。
詳細は[Clientの自動更新](../client-updates.md)を参照する。

## Rollback

更新設定を無効化すると通知・downloadを止められる。適用済み版の問題は保存した
旧版EXEを手動復元する。実装撤回時は本機能の変更だけを逆patchで戻し、既存の
C++規約整備とHTTP境界修正を保持する。

## References

- [GitHub Release assetへのリンク](https://docs.github.com/en/repositories/releasing-projects-on-github/linking-to-releases)
- [Windowsの署名検証](https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust)
- [同一volume内のfile置換とbackup](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)
