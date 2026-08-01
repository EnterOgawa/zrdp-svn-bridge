/**
 * @file main.cpp
 * @brief ZRdp Client processの起動、manager COM STA、ATL host、診断log lifecycleを管理する。
 *
 * @details 接続管理Window用COM STAとATLを初期化する。各RDP ActiveXは
 *          ClientApplicationが開始するsession専用STAで生成し、全session破棄後に
 *          ATLとmanager COMを逆順で終了する。startup errorにも接続情報を含めない。
 */
#include "client/app/client_application.h"
#include "client/app/resource.h"
#include "common/diagnostic_log.h"

#include <Windows.h>
#include <CommCtrl.h>

#include <atlbase.h>
#include <atlhost.h>

#include <array>

namespace {

/**
 * @brief ATL ActiveX containmentに必要なprocess module型。
 */
class ZRdpAtlModule final : public ATL::CAtlExeModuleT<ZRdpAtlModule> {};

/// process全体で一つだけ存在するATL module instance。
ZRdpAtlModule g_atl_module;

/**
 * @brief resource messageとHRESULTだけをstartup errorとして表示する。
 * @param[in] instance resourceを所有するmodule instance。
 * @param[in] message_id 利用者向けmessageのresource ID。
 * @param[in] error 起動失敗を表すHRESULT。
 * @return WinMainへ返す固定の失敗exit code。
 */
[[nodiscard]] int ShowStartupError(
    HINSTANCE instance,
    const UINT message_id,
    const HRESULT error) noexcept {
    std::array<wchar_t, 512> message{};
    LoadStringW(
        instance,
        message_id,
        message.data(),
        static_cast<int>(message.size()));

    std::array<wchar_t, 768> detail{};
    const int written = swprintf_s(
        detail.data(),
        detail.size(),
        L"%s\n\nHRESULT: 0x%08lX",
        message.data(),
        static_cast<unsigned long>(error));
    MessageBoxW(
        nullptr,
        written < 0 ? message.data() : detail.data(),
        L"ZRdp SVN Bridge",
        MB_OK | MB_ICONERROR);
    return 1;
}

/**
 * @brief 継続可能なstartup warningをresource文字列だけで表示する。
 * @param[in] instance resourceを所有するmodule instance。
 * @param[in] message_id warning messageのresource ID。
 */
void ShowStartupWarning(
    HINSTANCE instance,
    const UINT message_id) noexcept {
    std::array<wchar_t, 512> message{};
    LoadStringW(
        instance,
        message_id,
        message.data(),
        static_cast<int>(message.size()));
    MessageBoxW(
        nullptr,
        message.data(),
        L"ZRdp SVN Bridge",
        MB_OK | MB_ICONWARNING);
}

}  // namespace

/**
 * @brief ZRdp Client processのUnicode Windows entry point。
 * @param[in] instance 現processのmodule instance。
 * @param[in] previous_instance Win32互換引数。常に未使用。
 * @param[in] command_line password等を受け取らない未使用command line。
 * @param[in] show_command main windowの初期表示command。
 * @return プロセス終了code。
 */
int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE previous_instance,
    PWSTR command_line,
    const int show_command) {
    static_cast<void>(previous_instance);
    static_cast<void>(command_line);

    const bool diagnostic_log_available =
        zrdp::common::InitializeDiagnosticLog(
            zrdp::common::DiagnosticRole::Client);
    // logなしでもRDP操作は継続できるが、原因調査不能になることを利用者へ示す。
    if (!diagnostic_log_available) {
        ShowStartupWarning(instance, IDS_LOG_CREATE_FAILED);
    }

    // 接続管理Windowとsession完了通知を一つのmanager UI threadへ固定する。
    const HRESULT com_result =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        zrdp::common::WriteDiagnosticLog(
            zrdp::common::DiagnosticLevel::Error,
            zrdp::common::DiagnosticEvent::ComInitializationFailed,
            {zrdp::common::DiagnosticHResult(com_result)});
        const int exit_code = ShowStartupError(
            instance,
            IDS_ACTIVE_X_CREATE_FAILED,
            com_result);
        zrdp::common::ShutdownDiagnosticLog();
        return exit_code;
    }

    int exit_code = 1;
    try {
        // Win32 controlとATL ActiveX containmentをMainWindow生成前に初期化する。
        INITCOMMONCONTROLSEX common_controls{};
        common_controls.dwSize = sizeof(common_controls);
        common_controls.dwICC = ICC_STANDARD_CLASSES;
        if (!InitCommonControlsEx(&common_controls)) {
            exit_code = ShowStartupError(
                instance,
                IDS_ACTIVE_X_CREATE_FAILED,
                HRESULT_FROM_WIN32(GetLastError()));
        } else if (!ATL::AtlAxWinInit()) {
            exit_code = ShowStartupError(
                instance,
                IDS_ACTIVE_X_CREATE_FAILED,
                E_FAIL);
        } else {
            {
                // 全session STAをscope内で停止し、ATL終了より先にCOM参照を解放する。
                zrdp::common::WriteDiagnosticLog(
                    zrdp::common::DiagnosticLevel::Information,
                    zrdp::common::DiagnosticEvent::MainWindowCreateStarted);
                zrdp::client::app::ClientApplication
                    application(instance);
                if (application.Create(show_command)) {
                    zrdp::common::WriteDiagnosticLog(
                        zrdp::common::DiagnosticLevel::Information,
                        zrdp::common::DiagnosticEvent::MainWindowCreated);
                    exit_code =
                        application.RunMessageLoop();
                } else {
                    zrdp::common::WriteDiagnosticLog(
                        zrdp::common::DiagnosticLevel::Error,
                        zrdp::common::DiagnosticEvent::MainWindowCreateFailed);
                }
            }

            ATL::AtlAxWinTerm();
        }
    } catch (...) {
        // WinMain境界からC++例外をOSへ漏らさず、固定診断へ変換する。
        zrdp::common::WriteDiagnosticLog(
            zrdp::common::DiagnosticLevel::Error,
            zrdp::common::DiagnosticEvent::UnexpectedException);
        exit_code = ShowStartupError(
            instance,
            IDS_UNEXPECTED_ERROR,
            E_UNEXPECTED);
    }

    // COM、診断logの順で、起動時と逆順にprocess resourceを終了する。
    CoUninitialize();
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::ProcessStopping,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::ExitCode,
                static_cast<std::uint64_t>(exit_code)),
        });
    zrdp::common::ShutdownDiagnosticLog();
    return exit_code;
}
