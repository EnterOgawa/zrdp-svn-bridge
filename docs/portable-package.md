# 配布物のlocal準備と検査

更新日: 2026-10-13。正式版 `0.7.21.0`。配布内容と確認範囲は [リリースノート](release-notes-0.7.21.md) を参照する。
この手順はarchiveをlocalに作るだけで、署名・Release作成・upload・tag pushをしない。
Git管理外の `work/tmp` 以下だけへcreate-newで出力し、既存archiveを上書きしない。

開発PCへのインストール用Setupは [.issによるインストーラー](installer.md) を参照する。
ZIPは従来のportable配布・Client／Agentセット更新向けとして維持する。

## 署名前の同梱検査

```powershell
./scripts/Test-PortablePackage.ps1 -ExecutableDirectory ./out
./scripts/New-PortablePackage.ps1 -ExecutableDirectory ./out -OutputDirectory ./work/tmp/package-review -Development
```

`-Development` は署名前の配布**準備**の検査専用であり、Client／Agentの起動経路を変えない。
生成名に `_development`、同梱receiptに `development-not-for-release` を付ける。
更新manifestは作らず、公式Release／自動更新assetとして扱わない。

## 署名済みセット

署名検証と実機受入を完了したセットから作成する。Releaseの作成・uploadとPublic化は別々の明示承認を必要とする。

```powershell
./scripts/New-PortablePackage.ps1 -ExecutableDirectory ./out -OutputDirectory ./work/tmp/signed-package-review -Tag v0.7.21.0
```

通常modeはWindowsで有効な署名・timestamp、同一公開鍵、Release版、
root CMakeと一致するFileVersion／ProductVersion、固定製品名・EXE名、x64・subsystemを確認する。
二入力はwrite／renameを共有しないreadonly HANDLEで保持し、Copy後のhashも確認する。
更新manifestは既存の [manifest生成script](../scripts/New-ClientUpdateManifest.ps1) を再利用し、
署名・固定metadata・setup／paired update対応の検査を省略しない。
生成後の署名・EXE変更ではarchiveとmanifestを作り直す。

## 同梱するfile

| file | 用途 |
|---|---|
| `ZRdpSvnBridge.exe`、`ZRdpSvnAgent.exe` | 同Versionで同じfolderに置く二EXE |
| `LICENSE`、`NOTICE` | license全文と権利・帰属表示 |
| `DEPENDENCIES.md` | build／runtime componentのinventory |
| `README.txt` | 最小操作とdevelopment／正式候補の区別 |
| `package-files.json` | 各fileのbasename・size・SHA-256、製品版、purpose |
| `zrdp-client-update.ini` | 署名済み通常modeだけ。format 2の二EXEmanifest |

固定fileだけをCopyし、入力folder全体をarchiveにしない。
実INI、履歴、鍵、ログ、dump、PDB、DLL、過去配布物を同梱しない。
出力にsourceの絶対pathや証明書subjectを記録しない。

検査scriptは未署名の正式候補拒否、同梱内容の完全一致、receiptのhash／size、
Agent偽装名・不一致tagの拒否、
既存archiveの非上書き、Agent欠落、出力のworkspace逸脱拒否、入力EXEの不変を確認する。
署名済み実更新の合格をこの検査から推定しない。

失敗時のstageは削除せず検査用に残す。全入力HANDLEは閉じ、元EXEを変更しない。
stage／archiveはGit管理外のlocal生成物である。確認後の整理は対象を明示して行う。
公開時の操作は [公開・配布Gate](publication-checklist.md) に従う。
