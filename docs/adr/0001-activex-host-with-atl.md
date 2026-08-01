# ADR 0001: ATLによるRemote Desktop ActiveX host

- Status: Accepted
- Date: 2026-07-30

## Context

`ZRdpSvnBridge.exe`は、Windowsにインストール済みのRemote Desktop ActiveX ControlをWin32 window内へ安全にhostする必要がある。

独自OLE containerは多数のCOM interface、in-place activation、focus、resize、参照countを実装する必要があり、Phase 0の目的に対して実装riskが大きい。現行Visual Studio Build Tools 2022にはATL componentが未導入だが、ユーザーから必要な開発toolを`C:\develop\tools`へ追加する承認を得ている。

## Decision

- Visual Studio 2022 Build Toolsの`Microsoft.VisualStudio.Component.VC.ATL`を追加する。
- ActiveX control containmentにATLの`CAxWindow`を使用する。
- UI application frameworkは追加せず、main windowとmessage loopはWin32で実装する。
- COM smart pointerとBSTR ownershipにATL utilityを使用する。
- `mstscax.dll` Type Libraryの取込は単一internal headerへ限定する。
- 製品はCOM serverやActiveX controlを公開しないため、`ZRdpSvnBridge.exe`自身のCOM registrationを行わない。

## Consequences

### Positive

- 汎用OLE containerを独自実装せずに済む。
- ActiveX lifecycle、focus、resizeの既知実装を利用できる。
- ATLはheader-onlyであり、追加ATL runtimeを配布しない。
- Win32 UIとRDP／Bridgeの責務分離を維持できる。

### Negative

- Build環境にATL componentが必要になる。
- ATL headerの更新はVisual Studio toolset更新に連動する。
- ATL host境界についてもcallback例外、thread affinity、参照countを製品側で検証する必要がある。

## Alternatives

### 独自OLE container

追加toolは不要だが、実装量とlifecycle riskが高いため不採用とした。

### MFC ActiveX container

UI framework全体がMFCへ依存するため不採用とした。

### 独自RDP protocol／FreeRDP

プロジェクトの禁止事項および初期scopeに反するため不採用とした。

## Validation

- `CAxWindow`を使用したx64最小hostを`/W4 /WX`でbuildする。
- `dumpbin /dependents`でATL redistributable dependencyがないことを確認する。
- Phase 0でWindow／全画面／clipboard／disconnectを実機確認する。

## Rollback

ATL host sourceとcomponent requirementを取り除き、ActiveX host方式を再提案する。Windows system DLL、registry、RDP policyは変更しない。

## References

- https://learn.microsoft.com/en-us/cpp/atl/reference/caxwindow-class
- https://learn.microsoft.com/en-us/cpp/windows/redistributing-an-atl-application
- https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools
