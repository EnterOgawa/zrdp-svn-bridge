# ZRdp SVN Bridge Client／Agentのセット自動更新

更新日: 2026-10-13

ClientはGitHubの公開Releaseを確認し、新版があれば利用者へ通知する。
EXEの取得は利用者の同意後、置換はすべてのRDP session WindowとAgent設定画面の
終了、および受付済み設定保存の完了後に行う。
Clientと同じdirectoryのAgent EXEも同じ版へ更新する。接続設定、履歴、INI、診断logは更新しない。

旧配布物`0.7.20.0`は、この更新機能を含まない。正式版`0.7.21.0`は、
同版・同署名者のClient／Agentとformat 2 manifestを持つ初回bootstrap版である。
最初はSetupまたはportable ZIPで手動導入し、次の新版からセット更新を利用する。旧updaterはformat 2を拒否する。
リポジトリはPrivateのままであり、匿名の自動確認・取得は利用できない。Release作成の承認をPublic化の承認とは扱わない。

## 利用者の操作

起動時と稼働中の24時間ごとに、最新Releaseに添付された更新manifestだけを確認する。
接続管理Windowにはメニューバーや手動確認の入口を置かない。確認と署名検証は
workerで行い、RDP画面や既存Proxyの処理を待機させない。

1. 新版通知で現在のversionと更新先のversionを確認する。
2. 更新する場合は「はい」を選ぶ。「いいえ」ではEXEを取得しない。
3. `更新: 取得・検証中`の後、接続中または設定画面が開いていれば
   `更新: RDP／設定ウィンドウ終了待ち`になる。
4. SVN／Trac操作とAgent設定の保存を完了し、利用者自身がすべてのRDP session Windowと
   Agent設定画面を閉じる。
5. Clientが終了し、検証済みClient／Agentセットへ置換して接続管理Windowを再起動する。
   次回RDP接続時、登録済みAgent設定がある接続だけで接続先のAgentも自動更新・起動する。

RDPを自動切断しない。接続管理Windowだけを閉じても、生存するsessionがあれば
適用を開始しない。同意済みの取得・適用中にすべてのWindowを閉じた場合は、
その処理の完了までprocessが継続する。適用中は新しいRDP sessionを開始できない。
終了したRDP sessionやSVN streamを、新版起動時に自動再開しない。

「いいえ」で見送った同じ新版は、そのClient起動中の周期確認では再通知しない。
同じ新版を再検討する場合は、次回のClient起動時の自動確認を待つ。
同意前の確認失敗ではdialogを出さず、同意後の取得・検証・適用失敗は固定のerror分類を表示する。
更新失敗だけでRDP接続を停止しない。

## 設定

EXEと同じdirectoryの`client.ini`を使う。fileがない場合、または`[update]`がない
場合も、指定済みの既定公開元`EnterOgawa/zrdp-svn-bridge`を確認する。
Private repositoryへ認証tokenを渡す機能は持たず、公開前は取得に失敗する。

```ini
[proxy]
port=18080

[update]
enabled=true
repository=EnterOgawa/zrdp-svn-bridge
```

外部への更新確認を止める場合は`enabled=false`にする。設定は確認のたびに読み、
同意後の取得前と適用前にも無効化・取得元変更を確認する。
更新で再起動した直後のlocal backup保全は、通信を伴わず、無効設定でも完了させる。

任意URL、認証token、HTTP source、独自CA、別portは設定できない。
不正な更新設定では更新だけを停止する。正常な`[proxy]`設定は維持する。
`[update]`だけのfileではProxy base portは既定の18080となるが、明示した
`[proxy]`ではportを省略できない。設定内容を診断logへ出力しない。

## 取得と適用の条件

HTTPSのGitHub Release assetだけを、Windows標準WinHTTPで取得する。
redirectは最大4回とし、毎回HTTPS・443・許可hostを確認する。許可hostは
`github.com`、`objects.githubusercontent.com`、`release-assets.githubusercontent.com`である。
manifestは16 KiB、Client EXEは64 MiB、Agent EXEは16 MiBを上限とし、二EXEはfileへstreamingする。
各通信operationは5秒、各EXE取得は2分の期限を持つ。二EXEは直列取得し、最大計4分である。OSの通信policy、Zscaler、
証明書store、Proxy設定を変更せず、HTTPや別経路へfallbackしない。

候補を実行する前に、次をすべて検証する。

- 完全なmanifestと、現在より新しい四要素version。
- 二EXEそれぞれのexact sizeとSHA-256。
- Windowsで有効なAuthenticode primary SHA-256署名と検証済みtimestamp。
- 二候補とも現行Clientと同じ署名者公開鍵。元Agentも同版・同署名者の通常fileであること。
- ClientはWindows x64 GUI PE、Agentはx64 console PE。正式製品名`ZRdp SVN Bridge`と各固定元file名。
- 二EXEのFileVersion・ProductVersionがmanifestの同じ四要素versionへ一致。
- Clientの`UpdaterProtocol=1`・`PairedUpdateProtocol=1`、Agentの`AgentSetupProtocol=1`、双方の`BridgeProtocol=1`。
- 取得前に確認した元の二EXEが変更されていないこと。Agentの不在・版違い・未署名では適用しない。

署名の失効確認はWindowsのcache-only方式を使う。Windowsが信頼を確認できない
場合は更新を拒否し、証明書取得や信頼設定の変更で補完しない。署名証明書の識別値や
Release assetの署名付きURLをlogへ記録しない。

未署名の開発build、同版・旧版、署名鍵の変更、wire protocol非互換変更は
自動適用しない。署名鍵変更とprotocol非互換更新には手動配布を用いる。
Client／Agentは同一製品版を必須にする。同じwire versionでも片方だけの更新を許可しない。

## 置換と復旧

書込み可能なlocal directoryへ、固定名`ZRdpSvnBridge.exe`で配置する。
network share、reparse point、複数hard linkなど、検証できない配置では自動更新しない。
書込み権限不足の場合、管理者昇格や別directoryへのfallbackは行わない。

取得候補はEXE隣接の`.zrdp-update-<32桁hex>`へ置く。同じClientのcopyを一時helper
として起動し、親process終了を待って、同一volume上のAgent、Clientの順にbackup付きで置換する。
二fileの再検証前に新版を起動しない。取得失敗では双方を適用せず、通常の部分置換失敗は変更済みfileだけを旧セットへ戻す。
helperへ任意path、URL、実行commandを渡さない。永続helper、service、installer、
追加の配布EXEは必要ない。同一配置への更新はmutexで直列化する。

差替え後の検証失敗や、新processの起動失敗では、元hash・署名を確認した旧二EXEへ復元して再起動する。
新版が生存したまま起動確認を返さない場合は、強制終了や稼働EXEのrollbackを行わず、
stageと旧版を保全する。旧版復元直後は同じ失敗版を自動再提示しない。

正常更新後は、旧版を同じdirectoryの`ZRdpSvnBridge.previous.exe`と`ZRdpSvnAgent.previous.exe`へ一世代だけ保全する。
既存backupが無関係なfileや別署名者の場合は上書きしない。回収は今回の固定stage
fileだけを対象とし、directoryの再帰削除は行わない。

権限、file lock、異常終了等でstageが残る場合がある。残存stageが2件以上あると
新しい自動更新を停止し、無制限に蓄積しない。手動復旧では、その配置を使うClientと
helperが終了したことを確認し、旧版backupの署名・製品名・versionを確認したうえで、
二EXEを同一版セットとして復元する。二file同時のOS-level atomic性は保証せず、電源断・helper強制終了・復元不能時はstage内の`previous.exe`と`agent-previous.exe`を確認する。stageや未知のbackupを一括削除せず、内容を確認する。
`client.ini`、`agent.ini`、履歴、logは変更しない。

## 配布担当者の手順

1. ClientとAgentのFileVersionとProductVersionを現在の配布版より大きい同一四要素へ更新する。
   UIのversion表示も合わせる。Bridge protocolと更新bootstrap形式は互換性を確認する。
2. Debug／Release test、実機受入、署名・timestampのRelease Gateを通過させる。
3. 完成した同一版・同一署名者の署名済み二EXEから、空の出力directoryへmanifestを作成する。

   ```powershell
   ./scripts/New-ClientUpdateManifest.ps1 -ClientExecutable ./out/ZRdpSvnBridge.exe -AgentExecutable ./out/ZRdpSvnAgent.exe -Tag v0.7.21.0 -OutputDirectory ./work/tmp/update-release
   ```

   この例のtagは説明用であり、Releaseの作成を意味しない。scriptは二EXEの同版・同一署名者、有効な署名と
   timestamp、製品情報、x64、bootstrapを確認し、sizeとSHA-256を計算する。
   既存manifestを上書きせず、EXEの署名やGitHub uploadを行わない。
4. 公開が明示承認された場合だけ、指定repositoryの対応tagのReleaseへ
   `ZRdpSvnBridge.exe`、`ZRdpSvnAgent.exe`、`zrdp-client-update.ini`を添付する。
5. manifestのtagとassetを一致させ、GitHubのlatest Releaseとして公開する。
   manifest作成後にEXEを再署名・変更した場合は、manifestも作り直す。

利用者向けZIPにはLICENSE／NOTICE／依存物inventoryも含める。
固定二EXEの同梱・hash・署名前検査は [local配布準備](portable-package.md) を参照する。
旧三Releaseは公開対象から外す方針で保全し、削除・差替えは別の明示承認を必要とする。

manifest format 2は次の十keyを持つASCII INIである。値を手入力せず、完成EXEから生成する。

```ini
[update]
format=2
version=<major.minor.patch.build>
tag=<Release tag>
asset=ZRdpSvnBridge.exe
size=<EXE byte数>
sha256=<64文字hex>
bridge_protocol=1
agent_asset=ZRdpSvnAgent.exe
agent_size=<Agent EXE byte数>
agent_sha256=<Agent EXEの64文字hex>
```

初回の署名済みbootstrap版を手動適用した後、同じ署名鍵を使用する次の新版から
自動更新を利用できる。公開後は、通知・拒否・同意・session終了待ち・再起動・
backup保全を確認済みのlocal配置で受入確認する。

## 検証状態

初期追加時は隔離buildのDebug／Releaseで各37 CTestを通過し、Agent設定追加後は
2026-10-08に各41 CTestで回帰確認した。同意状態、有限queue、
session生存中の適用禁止、manifest・URL検証、異常CLI拒否を単体testし、差替えと
復元はfake transactionおよび隔離したWindows file fixtureで確認した。
既存署名済みClientのWindows署名検証と、未署名候補の拒否も確認した。
初期実装ではmanifest生成scriptのserializationと拒否条件、旧更新menuと無効設定のdialog、
dialog中のWindow終了も確認した。画面検証ではRDP接続を開始していない。
2026-10-10の追加指示で旧menuと手動確認入口を除去し、起動時と24時間周期の自動確認へ統一した。
EXE取得・適用の同意、署名検証と全session終了待ちは変更しない。

公開Releaseから署名済み新版へ更新する一連の実機受入は、初回bootstrap版の
署名・手動配布と次の新版Releaseを用意した後のGateとして残る。
初回実装の検証時点では旧署名済み`out/`、製品version、実設定、repositoryのPrivate設定を
変更していない。その後の開発成果物は利用者指定の`D:\develop\zrdp\out`へ出力し、
旧署名済みEXEを事前保全する。出力先の更新だけでは署名済み配布版の更新とは扱わない。

セット更新は[ADR 0023](adr/0023-paired-client-agent-updates.md)、従来の同意・適用境界は[ADR 0013](adr/0013-consented-client-updates.md)を参照する。


2026-10-10のセット更新ではformat 1の拒否、Agent必須key・上限・固定asset、未署名・別署名者・混在版の拒否、
二fileの実置換・各file lock・第二file失敗時のAgent復元を隔離fixtureで確認した。
Debug／Release各47 CTest、format確認とformatter self-test 16件、Doxygenの新規警告なしを確認した。
署名済み公開Releaseからの二EXE取得、正常再起動と両backup保全は実機受入Gateとして残る。
