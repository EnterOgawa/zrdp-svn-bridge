# ADR 0008: Session Window破棄前の明示RDP切断

- Status: Accepted
- Date: 2026-07-31

## Context

Version `0.7.13.0`を約2時間接続した後、Session Windowの閉じる操作で画面は消えたが、
`ZRdpSvnBridge.exe` processが残留する事象を確認した。固定診断logは
`ui.close_requested`、`rdp.event.confirm_close`、Proxy停止まで到達した一方、
`process.stopping`へ到達しなかった。

当該経路は`IMsRdpClient::RequestClose`、`OnConfirmClose`、`DestroyWindow`の順であり、
Microsoftの公開手順には適合していた。しかしATL hostの破棄は内部で
`IOleObject::Close(OLECLOSE_NOSAVE)`を同期実行するため、接続状態のControlを直ちに
破棄する場合にMicrosoft Control内部で長時間待機し得る。

ZRdpはSession Windowを閉じる前に独自の確認dialogを表示しており、`RequestClose`が
提供する追加確認を主要経路として使用する必要はない。通常の「切断」buttonでは
`Disconnect()`後の`OnDisconnected`を待ってからControlを破棄しており、実運用で
成立している。

## Decision

- 有効なRDP接続中にSession Windowを閉じる場合、確認後に`Disconnect()`を呼ぶ。
- `OnDisconnected`を受けるまでtop-level WindowとActiveX hostを破棄しない。
- 既に`Disconnecting`なら二重のCOM呼出しを行わず、進行中の`OnDisconnected`を待つ。
- 未接続または切断済みなら非同期eventを待たず、直ちにWindowを破棄する。
- `Disconnect()`が失敗した場合はWindowを維持し、従来の分類済みerrorを表示する。
- `S_FALSE`で既切断が示された場合は、新しいeventを待たずWindowを破棄する。
- `OnConfirmClose` eventの購読と安全な応答は、Microsoft Control互換境界として維持する。

## Consequences

### Positive

- ActiveXを接続状態のままOLE破棄する機会を通常終了経路から除外できる。
- 「切断」buttonとWindowの閉じる操作が同じ切断完了eventへ収束する。
- 一sessionの終了方針を純粋policyとして回帰testできる。
- 別session、Proxy、Agent、Application Protocol、security設定へ影響しない。

### Negative

- Microsoft Controlが`OnDisconnected`を通知するまでSession Windowの終了が完了しない。
- ActiveX内部が`Disconnect()`自体で停止する未知の条件は、この変更だけでは回復できない。
- 実RDPを使用した終了確認は自動CTestでは代替できず、署名前の実機Gateを必要とする。

## Security and protocol impact

RDP password、SVN／Trac credential、接続先、payloadを新たに取得または記録しない。
NLA、CredSSP、server certificate検証、認証警告を変更しない。Static Virtual Channel、
Application Protocol、Proxy、Agent allowlist、port、接続履歴形式に変更はない。

## Alternatives

### `OnConfirmClose`直後の`DestroyWindow`を継続する

Microsoft公開手順には適合するが、長時間接続後の残留processを実際に確認したため、
通常終了経路としては不採用とした。

### Session threadを強制終了する

COM、Socket、mutex、heapの所有状態を壊し、別sessionへprocess corruptionを波及させる
ため不採用とした。

### Process全体をtimeoutで強制終了する

別sessionや進行中のSVN／Trac処理まで終了するため、現時点では不採用とした。

## Validation

- 未接続、接続中、切断中の3状態で開始actionを純粋policy testする。
- Debug／Release buildと全CTestを成功させる。
- 実RDPへ接続し、Session Windowの閉じる操作後に対象WindowとClient processが
  残留しないことを確認する。
- 終了logが`rdp.disconnect_call_returned`、`rdp.event.disconnected`、
  `process.stopping`へ到達することを確認する。

### Validation result

2026-07-31、Debug／Release各30 CTestに成功したVersion `0.7.14.0`で実RDPへ接続し、
Session Windowと接続管理Windowを閉じた後にClient processが残留しないことを
ユーザーが確認した。固定診断logでも`rdp.event.disconnected`と`process.stopping`への
到達を確認したため、本ADRの署名前実機Gateは通過した。

## References

- Microsoft `IMsRdpClient::RequestClose`
- Microsoft `IMsTscAxEvents::OnConfirmClose`
- Microsoft `IMsTscAx::Disconnect`
- Microsoft `IMsTscAxEvents::OnDisconnected`
