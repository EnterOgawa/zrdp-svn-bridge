# Phase 9 改善I-002 session title集約UI ローカル検証

更新日: 2026-07-31

## 1. 対象

- Client／Agent product version: `0.7.1.0`
- 対象改善: 複数RDP session Windowの表示簡素化
- wire protocol: Version `1`のまま
- Agent allowlist、HTTP Proxy境界、最大stream数: 変更なし

## 2. 実装結果

- 接続条件と表示modeは接続管理Windowで確定する。
- session Window内の接続欄、表示mode欄、Bridge欄、status barは表示しない。
- Remote Desktop ActiveX child windowはsession Windowのclient領域全体を使用する。
- session titleへ次の非payload情報を集約する。
  - 接続先PC名またはIPアドレス
  - RDP状態
  - Agent状態
  - Proxy状態とsession専用loopback port
  - active stream数
  - Client→Agent／Agent→Client転送byte数
- Proxy設定異常時はportを表示せず、固定の設定error分類を表示する。
- 全画面切替はMicrosoft ActiveXが通知する標準`Ctrl+Alt+Break`要求を使用する。
- session Windowの閉じる操作は確認後に対象RDPだけを切断し、他sessionと
  接続管理Windowを維持する。

## 3. ローカル検証

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug --parallel
ctest --preset debug
cmake --build --preset release --parallel
ctest --preset release
```

- Debug build: 成功、`/W4`・`/WX`
- Debug CTest: 26/26成功
- Release build: 成功、`/W4`・`/WX`
- Release CTest: 26/26成功
- resource契約: title format、接続管理Window titleを確認
- portable binary契約: Client／Agentともx64、Version `0.7.1.0`

## 4. portable成果物

| file | size | version | SHA-256 | Authenticode |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 663,552 byte | `0.7.1.0` | `3FCA2A1E977F77892BD24B3295ABC5A1CB23EB2466F3005D520B8A8E667A7983` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 423,424 byte | `0.7.1.0` | `CCA6440EC3E8812F6CB6E4DA346A37B262C27EEED8400A1C3E77F2CEDDE7B7ED` | NotSigned |

## 5. ユーザー実機確認待ち

- [ ] session Windowにtitle barとRDP画面だけが表示される。
- [ ] Window size変更時にRDP表示がclient領域全体へ追従する。
- [ ] titleのRDP／Agent／Proxy状態が実際のlifecycleに追従する。
- [ ] 通常利用の最初のsessionでProxy portがbase portとして表示される。
- [ ] `Ctrl+Alt+Break`で全画面とWindow表示を往復できる。
- [ ] 閉じる操作で対象sessionだけが切断・終了する。
- [ ] 二つ目のsessionと一つ目のsessionが独立して継続する。

実接続先、ドメイン、ユーザー名、実URL、repository名、credential、画面captureは
本記録へ含めない。
