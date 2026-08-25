# R-013 Microsoft connection bar Z-order ローカル検証

日時: 2026-08-25

## 実機事実

- Version `0.7.16.0`: Microsoft connection barは表示されたが、ZRdp containerは
  Window modeのままだった。
- Version `0.7.17.0`: borderless container全画面は成立したが、connection barが
  表示されなくなった。
- Version `0.7.18.0`: connection barの表示、raised、最小化／復帰button設定は
  setter／readbackに成功したが、実RDPではbarが表示されなかった。ZRdp fallbackと
  `Ctrl+Alt+Break`による復帰eventは成功した。

## 原因

ActiveXの`FullScreen`を先に有効化した後、ZRdp containerを`HWND_TOP`へ移動していた。
Microsoft connection barはActiveXとは別のtop-level Windowとして描画され得るため、
後段のcontainerがbarを覆うZ-order競合を作っていた。

## 修正

- 全画面開始: container borderless配置、ActiveX全画面化の順に変更
- Window復帰: ActiveX全画面解除、container style／配置復元の順を維持
- monitor配置から`HWND_TOP`を除去し、`SWP_NOZORDER`を使用
- ActiveX／USER32片側失敗時は逆順rollback
- Version `0.7.18.0`のZRdp fallbackは実機Gate完了まで維持

Virtual Channel、Protocol、Proxy、Agent runtime、allowlistは変更していない。

## 自動検証

- 隔離Debug build／CTest: 成功、30／30
- 隔離Release build／CTest: 成功、30／30
- 標準Debug build／CTest: 成功、30／30
- 標準Release build／CTest: 成功、30／30
- build warning: 0
- source comment／resource／portable binary contract: 成功
- container monitor配置の`SWP_NOZORDER` policy: 成功
- RDP ActiveX接続前smoke test: 成功

## 未署名標準成果物

| File | size | Version | Authenticode | SHA-256 |
|---|---:|---|---|---|
| `ZRdpSvnBridge.exe` | 683,008 byte | `0.7.19.0` | `NotSigned` | `A46DFE847A95CCF77B55D8A261D8081429086AC108DB72B7A075A80D68A2C43E` |
| `ZRdpSvnAgent.exe` | 417,280 byte | `0.7.19.0` | `NotSigned` | `6A6748ECADFC665A048D1C2273383EAAD06EE2FEB03C184FDCD84B84FB5E0341` |

標準`out`へ配置済みである。更新前の未署名Version `0.7.18.0`はGit管理外の
`work/tmp/unsigned-0.7.18.0-before-r013`へ退避した。

## 残る実機Gate

- borderless全画面より前面にMicrosoft connection barが表示されること
- connection barの最小化／Window復帰buttonが機能すること
- fallbackと`Ctrl+Alt+Break`も引き続き機能すること
- 動的解像度、RDP、Agent、Proxy、HTTP stream、別sessionへ回帰がないこと
