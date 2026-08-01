# Phase 9 複数session分離／動的解像度retry ローカル検証

実施日: 2026-07-31

## 1. 対象

- Client／Agent version: `0.7.10.0`
- 対象不具合1: 動的解像度を選択した新規接続で、RDP接続は成立するが
  表示mode変更errorとなり等倍表示へ縮退する
- 対象不具合2: 異なる二つのRDP接続のうちAgent接続済みsessionを閉じると、
  もう一方のsessionも応答不能になる

実接続先、domain、ユーザー名、URL、payload、process session IDは本記録へ
含めない。

## 2. 診断logの確認

動的解像度の再発logでは、`rdp.event.login_complete`から約0.24秒後の最初の
`rdp.dynamic_resolution_call_returned`が`0x8000FFFF (E_UNEXPECTED)`だった。
直後にSmart SizingをOFFにして等倍表示へ縮退しており、接続前設定、認証、
RDP接続そのものは成功していた。

複数sessionの実機logでは、base portと次のportの二sessionが同時に存在し、
次のport側だけでAgent／Proxy Readyを確認した。そのsessionのclose要求、
confirm close、Proxy停止後に別RDPの切断eventが続き、利用者から残存Windowが
応答不能になったとの報告を受けた。

従来は全RDP ActiveXとsession Windowを一つのCOM STA message loopで処理していた。
一sessionのMicrosoft ActiveX終了処理が待機すると、C++ objectの所有権が別でも
同じSTA上の他Windowをdispatchできないことを構造原因と判定した。

## 3. 修正

- `E_UNEXPECTED`だけを一時失敗として扱い、500 ms、1,000 ms、1,500 ms後に
  最大3回再試行する
- interface不在、不正引数、成功値、再試行上限後は再試行しない
- 各試行へ`attempt`とsession識別用Proxy `port`を記録する
- 接続管理Windowをmanager COM STA、各RDP session Window／ActiveXを
  session専用COM STAへ分離する
- session threadから接続管理Windowへ直接触れず、接続履歴をqueueとthread
  messageでmanager STAへmarshalする
- session終了済み通知をmanagerへ送り、終了済みthreadだけをjoinして対応Proxy
  slotだけを解放する
- `RequestClose`後にActiveXが`Disconnecting`なら、破棄時の重複`Disconnect`を
  行わない
- close、confirm close、disconnect eventへsession識別用Proxy `port`を記録する

wire protocol version、`ESVNBRG`、Agent、allowlist、loopback bind、HTTP制限、
履歴形式は変更していない。設計判断はADR 0005に記録した。

## 4. ローカル検証

```text
cmake --preset vs2022-x64
cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure
cmake --build --preset release --parallel
ctest --preset release --output-on-failure
```

- [x] Debug build: `/W4`／`/WX`でwarningなし
- [x] Debug CTest: 29／29成功
- [x] Release build: `/W4`／`/WX`でwarningなし
- [x] Release CTest: 29／29成功
- [x] 動的解像度retryのdelay、上限、対象HRESULTを単体testで確認
- [x] 異なる二つのSTAで非接続ActiveXを生成
- [x] 一方のActiveX破棄完了後、他方のSTAが5秒以内にping messageへ応答
- [x] Release Clientのmanager Windowを起動し、`WM_CLOSE`で正常終了code 0
- [x] Client／AgentにVersion `0.7.10.0`を埋め込み
- [x] Client／Agentがx64 portable EXEであることをbinary contract testで確認

## 5. Release成果物

| 成果物 | size | SHA-256 | 署名 |
|---|---:|---|---|
| `out/ZRdpSvnBridge.exe` | 673,792 byte | `D38D9589E372D0C49145260EDCBEBA10A43AC51D2554A40668DA541209C698D7` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 413,696 byte | `0E291F3833D491048974131A39B09D898B8EF7C3698A301555405B930AFD0F5D` | NotSigned |

## 6. ユーザー実機Gate

- [x] 動的解像度を選択して、再発した接続先へ新しいsessionを開始する
- [x] error dialogを表示せず、動的解像度で追従する
- [x] 異なる二台へ同時接続し、一方のRDP session内だけでAgentを起動する
- [x] Agent接続済みsessionを閉じても、残存RDP画面を操作できる
- [x] 同じ操作中も接続管理Windowと残存sessionの表示modeが応答する
- [x] 新しいsessionで解放済みProxy portを再利用できる

Gate結果: 2026-07-31、ユーザーから上記すべてが期待どおり動作したとの受入結果を
得た。これにより動的解像度の再発修正と複数session相互停止修正を実環境完了と
する。
