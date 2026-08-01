# 複数RDPセッションの独立Window化とProxyポート割当

作成日: 2026-07-31

> 追記（2026-07-31）: 本提案の「全sessionを一つのCOM STAで処理する」判断は、
> 実機で一sessionの終了中に別sessionが応答不能となったため、
> ADR 0005のsession専用COM STA方式で置き換えた。Proxy port規則とsecurity
> boundaryは変更していない。

## 1. 変更理由

Version 0.6.0.0のClientは、`MainWindow`が一つのRemote Desktop ActiveX、
`ClientBridge`、`ClientProxyWorker`を所有する単一セッション構造である。
複数の支給PCへ同時接続するため、接続入力を行う管理Windowと、RDP接続ごとの
独立Windowへ分離する。

各RDPセッションは従来どおりSVN Bridgeを必須の構成要素として持つ。RDP画面だけを
独自実装したり、Bridgeを共有してセッション境界を曖昧にしたりしない。

## 2. 採用方式

一つの`ZRdpSvnBridge.exe`プロセスのCOM STA UI thread上に、次を生成する。

```text
ClientApplication
├─ ConnectionManagerWindow
│  └─ 接続先／domain／user／表示mode／接続履歴
├─ MainWindow (session 1)
│  ├─ RdpControlHost
│  ├─ ClientBridge
│  └─ ClientProxyWorker -> 127.0.0.1:18080
├─ MainWindow (session 2)
│  ├─ RdpControlHost
│  ├─ ClientBridge
│  └─ ClientProxyWorker -> 127.0.0.1:18081
└─ MainWindow (session 3)
   ├─ RdpControlHost
   ├─ ClientBridge
   └─ ClientProxyWorker -> 127.0.0.1:18082
```

- 接続管理Windowの「接続」は、新しい`MainWindow`を生成して非同期に接続を開始する。
- 各`MainWindow`はActiveX、Virtual Channel、Bridge、Proxy workerを独立所有する。
- 一つのsession Windowを閉じても、他sessionと接続管理Windowを終了しない。
- 全ActiveX／COM操作はsessionごとの専用COM STA UI threadへ限定する。
- 接続履歴workerは接続管理Windowだけが所有し、複数Windowから同一履歴fileを
  同時更新しない。
- 同時RDPセッション数は16件を上限とし、COM、Socket、queue、memoryの総量を
  有限にする。

## 3. Proxyポート規則

- `client.ini`の`[proxy] port`は、最初のsessionへ割り当てるbase portとする。
- `client.ini`がない場合のbase portは従来どおり`18080`とする。
- 接続時に未使用の最小slotを取得し、`base port + slot`を割り当てる。
- 通常利用するsessionを最初に開始すれば、従来どおり`127.0.0.1:18080`となる。
- session終了時にslotを解放し、次の新規sessionから再利用できるようにする。
- 65535を超えるportは生成しない。空きslotまたはportがない場合は新規sessionだけを
  拒否し、既存sessionを維持する。
- 実際の割当portは、接続管理Window、session Window title、従来のBridge状態欄へ
  表示する。
- 別processが割当portを使用していてbindできない場合、別経路へ自動fallbackせず、
  対象sessionのProxyだけを`BindFailed`として停止する。RDP画面は維持する。

## 4. 変更対象

### 新規

- `src/client/app/client_application.h/.cpp`
  - manager Window、有限session slot、session専用STA thread群を所有する。
- `src/client/app/connection_manager_window.h/.cpp`
  - 接続入力、履歴、session生成要求、割当port表示を所有する。
- `src/client/app/session_launch_settings.h`
  - managerからsessionへ渡す非機密RDP設定とProxy portを定義する。
- `src/client/app/session_proxy_port_allocator.h/.cpp`
  - base portと有限slotから重複しないportを割り当てる。
- `tests/unit/session_proxy_port_allocator_test.cpp`
  - 順次割当、解放、再利用、上限、overflowを検証する。

### 変更

- `src/client/app/main.cpp`
  - 単一`MainWindow::RunMessageLoop`から`ClientApplication`へ起動所有権を移す。
- `src/client/app/main_window.h/.cpp`
  - 接続済みsession Windowとしてstartup設定を受け取り、自動接続する。
  - 接続履歴所有をmanagerへ移す。
  - Window破棄時の`PostQuitMessage`を廃止し、他sessionを維持する。
  - session titleへ接続先と割当Proxy portを表示する。
- `src/client/app/resource.h`
- `src/client/app/ZRdpSvnBridge.rc`
  - manager、session port、session上限の日本語表示を追加する。
- `src/client/CMakeLists.txt`
- `tests/CMakeLists.txt`
  - 新しいapplication部品とtestを登録する。
- `AGENTS.md`
- `docs/architecture.md`
- `docs/configuration.md`
- `docs/test-plan.md`
- `docs/roadmap.md`
  - 複数sessionの所有境界、port運用、受入観点を記録する。

## 5. security boundaryへの影響

- Proxyのbind先は全sessionで`127.0.0.1`固定のままとする。
- Agent allowlist、HTTP限定、`CONNECT`拒否、hostname拒否は変更しない。
- 一つのProxy workerは一つのRDP／Agent sessionだけへ接続し、別sessionへ
  自動fallbackしない。
- RDP server、domain、user、URL、endpoint、credentialをlogへ追加しない。
- passwordは引き続きWindows標準認証UIだけで扱い、Window間で受け渡さない。
- session数とport数を有限化し、複数接続による無制限resource生成を防止する。

## 6. ActiveX／COM／Virtual Channelへの影響

- `RdpControlHost`をsessionごとに一instance生成する。
- 各instanceのevent sink、Connection Point、BSTR、ActiveX child Windowは
  対応する`MainWindow`がRAIIで所有する。
- `ESVNBRG`というchannel名は同じでも、RDP sessionが異なるためtransportは
  独立する。
- `SendOnVirtualChannel`とActiveX event処理は対応session専用COM STAで行う。
- session破棄時は、そのsessionのProxy、Bridge、event subscription、ActiveXだけを
  停止する。

## 7. protocol compatibility

- wire protocol version、frame、feature bit、message typeは変更しない。
- Agent EXEと`agent.ini`形式は変更しない。
- Version 0.6.0 Agentとの互換性を維持する。

## 8. 失敗時動作

- manager生成失敗: Clientを起動せず固定エラーを表示する。
- session ActiveX生成失敗: 対象sessionだけを破棄しmanagerへ失敗を返す。
- session上限／port枯渇: 新規接続だけを拒否する。
- Proxy bind失敗: session Windowへ割当portと固定errorを表示し、RDPは継続する。
- Agent未起動／非互換: 対象sessionだけをAgent待機／Bridge停止とする。
- session切断／終了: 対応するHTTP streamとSocketだけを終了する。

## 9. test観点

- base `18080`から`18080`、`18081`、`18082`の順に割り当てる。
- 中間session終了後に解放portを再利用する。
- 同じportを二つの生存sessionへ割り当てない。
- base port付近のoverflowと16 session上限を拒否する。
- 二つのsession Window生成、独立ActiveX host生成、片方の破棄後も他方が生存する。
- 各sessionのProxy snapshotが異なるloopback portを表示する。
- 一sessionのRDP切断／Agent停止／Proxy失敗が他sessionへ伝播しない。
- 既存25 testとDebug／Release buildを回帰実行する。

## 10. rollback

新規application／manager／allocatorを除去し、`main.cpp`を単一`MainWindow`の
message loopへ戻す。`MainWindow`の履歴所有、Proxyの`client.ini`読込み、
`WM_DESTROY`時の`PostQuitMessage`を復元すれば、wire protocol、Agent、
allowlist、既存repository設定を変更せずVersion 0.6.0の単一session構造へ戻せる。
