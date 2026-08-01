# ADR 0005: RDP sessionごとの専用COM STA

- Status: Accepted
- Date: 2026-07-31
- Supersedes: 複数RDP session提案の単一COM STA方式

## Context

Version `0.7.6.0`の実機確認で、二つの異なるRDP接続を一つのClient processから
開始し、一方だけでAgent／Proxyを成立させた。Agent接続中のsession Windowを
閉じたところ、もう一方のsession Windowも応答しなくなり、processの強制終了が
必要になった。

従来は接続管理Windowと全`MainWindow`／Remote Desktop ActiveXを一つのCOM STA
message loopで処理していた。各C++ objectやBridge workerの所有権は分離されて
いても、Microsoft ActiveXの切断、event購読解除、COM解放のいずれかが待機すると、
同じSTAにある他sessionのWindow messageも処理できない。

実RDP接続を伴うActiveX終了処理の所要時間をZRdp側から確実に制限する公開APIは
ない。単一STAのまま終了順やtimeoutだけを調整しても、別sessionの応答性を保証
できない。

## Decision

- 接続管理Windowはprocessのmain threadにあるmanager COM STAで所有する。
- RDP sessionごとに一つの専用threadを開始し、そのthreadを
  `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`で初期化する。
- 一つのsession専用STAが、対応する`MainWindow`、Remote Desktop ActiveX、
  event subscription、`ClientBridge`、`ClientProxyWorker`を生成から破棄まで
  所有する。
- session Windowのmessageは、そのsession専用message loopだけでdispatchする。
- session終了時の`Disconnect`、event購読解除、ActiveX破棄、COM解放は、
  すべて生成元STA上で行う。
- `RequestClose`がすでにActiveXを`Disconnecting`へ移した場合、`Destroy`から
  重複した`Disconnect`を呼ばない。
- 接続履歴の更新だけは、非機密profileをboundedなapplication queueへ移し、
  thread messageでmanager STAへ通知する。session threadからmanager objectへ
  直接触れない。
- Proxy port slotの割当・解放と最大16 sessionの上限はmanager STAで直列化する。

```text
ZRdpSvnBridge.exe
  manager STA
    `- ConnectionManagerWindow
         |- Proxy port allocator
         `- connection history worker

  session STA 1
    `- MainWindow 1
         |- Remote Desktop ActiveX 1 + ESVNBRG
         |- ClientBridge 1
         `- ClientProxyWorker 1

  session STA 2
    `- MainWindow 2
         |- Remote Desktop ActiveX 2 + ESVNBRG
         |- ClientBridge 2
         `- ClientProxyWorker 2
```

## Consequences

- 一sessionのActiveX終了処理が待機しても、managerと他sessionは別message loopで
  応答できる。
- 最大16 sessionに対して最大16本の専用STA threadが増える。既存の有限session
  上限を維持し、無制限なthread生成は許可しない。
- ATL ActiveX containmentはprocess単位で初期化したまま、各ActiveXのCOM interface
  は生成元STA外へ渡さない。
- wire protocol、Static Virtual Channel名、Agent、allowlist、loopback Proxy、
  HTTP制限は変更しない。
- 非接続ActiveX二組を異なるSTAで生成し、一方を破棄した後も他方のSTAがmessageへ
  応答することをローカルtestで確認する。
- 実接続時のMicrosoft ActiveX内部待機まではMockできないため、二つの実RDP接続で
  一方を閉じても他方とmanagerが応答し続けることをユーザー実機Gateとする。

## Rejected alternatives

### 単一STAの終了順だけを変更する

ActiveX内部の同期待機を別sessionから隔離できず、再発防止条件を満たさない。

### sessionごとにClient processを分ける

COM隔離は強いが、接続管理、共通履歴、Proxy port割当を一つのアプリへ統合する
要件と合わない。process crash isolationが必要と実測された場合だけ別ADRで
再検討する。

### ActiveXをworker MTAで破棄する

生成元STAというCOM／ActiveX所有境界に反し、未定義のthread間操作となるため
採用しない。
