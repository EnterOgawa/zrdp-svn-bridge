# Phase 1 Release検証記録

最終更新: 2026-07-30 20:49:26（Asia/Tokyo）

## 1. 対象

| File | Size | File／Product version | SHA-256 |
|---|---:|---|---|
| `out/ZRdpSvnBridge.exe` | 537,600 byte | `0.2.1.0` | `4A3E08A7D0D34C58EBEF4B081596A124A3127B1D51723396D4759C08B92750F4` |
| `out/ZRdpSvnAgent.exe` | 370,688 byte | `0.2.1.0` | `84CE85755F21C05F01D0A722A1908ECFAC1C1BBEAFB030E9A222FA442A9BB77C` |

両EXEは未署名の実機検証用portable成果物である。installer、Release asset、
remote uploadは作成していない。

## 2. Build／CTest

Visual Studio 2022、Windows SDK 10.0.26100.0、x64、C++20、`/W4`、`/WX`、
static CRT設定で実行した。

| Configuration | Build | CTest |
|---|---|---|
| Debug | 成功、warning 0 | 12/12成功 |
| Release | 成功、warning 0 | 12/12成功 |

統合testはfake SVCでClientとAgentを接続し、1／31／257／509／997-byteへ
再fragmentしながら、1 byte、1 KiB、64 KiB、1 MiB、10 MiBの合計
11,600,897 byteを完全一致で往復した。さらにAgentのconnection-level CLOSE後と、
CLOSE消失相当の両方で、同じfake RDP session内の新Agentへ再handshakeした。

## 3. Binary contract

自動testと`dumpbin`で次を確認した。

- 両EXEがx64 PE
- FileVersionとProductVersionがともに`0.2.1.0`
- Dynamic Base、NX Compatible、Control Flow Guardが有効
- Client依存:
  `COMCTL32.dll`、`GDI32.dll`、`ole32.dll`、`SHELL32.dll`、`USER32.dll`、
  `CRYPT32.dll`、`OLEAUT32.dll`、`KERNEL32.dll`
- Agent依存: `WTSAPI32.dll`、`ole32.dll`、`KERNEL32.dll`、`USER32.dll`
- MSVC runtime等の外部配布DLL依存なし
- Client／Agentの両方に共通ZRdp icon groupを埋込み
- Windowsのassociated icon取得で両EXEから32×32の製品iconを取得
- Clientは`ZRdpSvnBridge.log`、Agentは`ZRdpSvnAgent.log`を選択し、
  current logとbackup世代を共有しない

## 4. 残Gate

2026-07-30の実機logで、最初のAgentがReady後にCtrl+Cで終了し、Clientが
同じRDP sessionを維持したまま新しいAgent processと再度Readyになったことを
確認した。再起動後のEchoは11,600,897 byteで成功した。Clientを利用者が切断する
までRDP disconnect、protocol error、payload mismatchは記録されていない。

これによりPhase 1 Gateは通過とする。Agent異常終了timeoutやRDP切断時の
Agent自動終了等、未確認の障害系はPhase 6までの回帰項目として維持する。

Phase 1にはlocalhost HTTP Proxy、upstream Socket、allowlist、HTTP／SVN中継を
まだ実装していないため、実SVNへは接続しない。
