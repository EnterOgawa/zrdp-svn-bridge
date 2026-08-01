# ADR 0002: Portable EXEのCRT静的link

- Status: Accepted
- Date: 2026-07-30

## Context

`ZRdpSvnBridge.exe`と`ZRdpSvnAgent.exe`はinstallerなしで配布し、通常ユーザーがportable EXEとして起動できる必要がある。

`/MD`はMicrosoft Visual C++ Redistributableの存在または追加配布を必要とする可能性がある。`/MT`はEXE sizeとservicing責任が増える一方、実行環境へVC++ Redistributableをinstallせずに済む。

## Decision

- Releaseは`/MT`、Debugは`/MTd`を使用する。
- Debug binaryは配布しない。
- ClientとAgentを含む同一target内の全moduleでruntime library設定を統一する。
- ReleaseごとにVisual Studio toolset versionを記録する。
- MSVC／CRTのsecurity updateが公開された場合は、影響を確認して再build／再配布する。

## Consequences

### Positive

- installerとVC++ Redistributableの事前installを不要にできる。
- ClientとAgentをそれぞれ単体EXEに近づけられる。
- app-local CRT DLLの配置漏れを防止できる。

### Negative

- EXE sizeが増える。
- CRT修正はWindows Updateだけでは製品へ反映されず、再buildと再配布が必要になる。
- 将来追加するlibraryが`/MD`を要求する場合は互換性を再評価する必要がある。

## Security

- `/GS`、ASLR、DEP、Control Flow Guard等のRelease mitigationを無効化しない。
- toolsetとWindows SDKを既知versionへ固定し、Release前に更新状況を確認する。
- Debug CRTを配布しない。

## Alternatives

### `/MD`とVC++ Redistributable

central servicingには優れるが、installerなしという配布要件と追加prerequisiteが競合するため初期版では不採用とした。

### app-local CRT DLL

installerは不要だが配布ファイル数と更新管理が増え、Microsoftもservicing上推奨していないため不採用とした。

## Validation

- clean Windows test環境で追加VC++ Redistributableなしに起動できることを確認する。
- `dumpbin /dependents`で`VCRUNTIME*.dll`、`MSVCP*.dll`へのdependencyがないことを確認する。
- Debug／Releaseのruntime library設定をCMake testで確認する。

## Rollback

配布方式を再評価し、ADRをSupersededへ変更した上で全targetを`/MD`へ統一する。configurationごとに`/MT`と`/MD`を混在させない。

## References

- https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library
- https://learn.microsoft.com/en-us/cpp/windows/choosing-a-deployment-method
- https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files
