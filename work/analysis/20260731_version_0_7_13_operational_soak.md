# Version 0.7.13 運用観察baseline

記録日: 2026-07-31

## 1. 目的

Version `0.7.13.0`を通常のRDP、Mylyn、SVN／Trac利用へ継続投入し、短い機能確認では
再現しにくい接続、長時間通信、複数session、表示操作の残件を自然条件で確認する。
新しい通信機能やsecurity例外は追加せず、問題が判明した項目だけを独立した改善として
再オープンする。

## 2. 開始時baseline

- Client／Agent: Version `0.7.13.0`
- Debug: build成功、全30 CTest成功
- Release: build成功、全30 CTest成功
- 標準成果物のSHA-256:
  - Client: `9DA3ED367BFAD9FEBEB153C016FA40B2085E0AACE9290CEF3EF0B6A1A3B2C9F3`
  - Agent: `4A4258932A64749E2905476B3D989028B05635AF4BA5C2690F30D79D2465954D`
- Authenticode: Client／Agentとも未署名
- Agent配置: `%LOCALAPPDATA%\ZRdp`相当の通常ユーザー書込み可能directory
- Agent設定: `ZRdpSvnAgent.exe`と`agent.ini`を同じdirectoryへ配置
- Agent診断log: 同directoryの`logs`生成を確認
- Clientは観察開始時に応答中であり、標準`out`成果物を置換していない。

## 3. 現時点の判断

- titleの`C->A`はlocalhost HTTP clientからAgent方向へ受け付けた累積byte、
  `A->C`はAgentからlocalhost HTTP clientへ返した累積byteである。
- Mylynの定期同期中に両値が増えることは正常であり、RDP画面転送量ではない。
- 2026-07-31の観察ではMylyn同期中もClientは継続し、転送量増加だけでは異常を
  再現していない。
- 先に観測した終了は診断log上でRDPの外部切断後に`exit_code=0`で終了しており、
  Client crashまたはBridge転送量起因とは確定していない。
- 書込み禁止directoryへの配置はEXE実行自体を妨げる根拠にはならないが、診断logを
  見つけにくくするため、観察期間は書込み可能directoryを使用する。

## 4. 通常利用で確認する残件

### 複数session

- [ ] 二つの実RDP sessionでそれぞれAgentとProxyをReadyにする。
- [ ] 一方を終了しても、他方のRDP、Agent、Proxy、SVN／Trac通信が継続する。
- [ ] 解放されたProxy portを後続sessionが安全に再利用する。

### 音声とWindow表示

- [ ] マイク転送、リモート音声、名前付きprofileからの設定復元を確認する。
- [ ] 辺／角drag、最大化、Windowsスナップで意図したアスペクト比方針を確認する。
- [ ] 全画面buttonで移行／復帰し、動的解像度がmonitorとWindowへ再追従する。
- [ ] 等倍表示とSmart Sizingではremote desktop寸法が維持される。
- [ ] Explorer、taskbar、各Window、Agent consoleで最終iconを確認する。

### 切断と長時間動作

- [ ] 成立済み接続先の再起動または通信断でerror dialogを出さず対象Windowだけ閉じる。
- [ ] Mylynの定期同期を含む長時間利用でRDP、Agent、Proxyが継続する。
- [ ] 観測済み`263/0`ログイン前失敗が自然再発した場合、一度だけ自動復旧する。
- [ ] Microsoft認証UIの取消と、成立済みsessionの外部切断へ回帰がない。

## 5. 再発時に保存する情報

payload、URL、credential、実endpointは記録せず、次だけを保存する。

- 発生時刻と行っていた操作の分類（Mylyn同期、SVN参照、更新、Commit等）
- 対象session titleのProxy port、`stream`、`C->A`、`A->C`
- Clientの`ZRdpSvnBridge.log`
- 対応するRDP session内Agentの`ZRdpSvnAgent.log`
- error dialogが出た場合はreason／HRESULTの数値
- Client processが残っているか、接続管理Windowと別sessionが応答するか

診断logは固定fieldだけを使用し、外部共有前に実IP、URL、ユーザー名、payloadが
含まれていないことを再確認する。
