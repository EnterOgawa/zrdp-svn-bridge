/**
 * @file main.cpp
 * @brief ZRdpSvnAgentのsession検証、SVC／TCP I/O loop、診断log lifecycleを実装する。
 *
 * @details 通常ユーザーのcurrent RDP sessionだけで動作し、service、inbound port、
 *          allowlist一致時だけoutbound Socketを作成する。WTSとSocket I/Oは
 *          同一owner threadで直列化し、Ctrl+CまたはRDP切断時に全resourceを解放する。
 */

#include "agent/app/session_validation.h"
#include "agent/bridge/agent_bridge.h"
#include "agent/bridge/agent_stream_relay.h"
#include "agent/channel/wts_virtual_channel.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "common/diagnostic_log.h"
#include "common/resource_ids.h"
#include "protocol/frame.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

/// Ctrl+C／console close handlerとI/O loopが共有する停止flag。
std::atomic_bool g_stop_requested{false};

/// WTS readからconsole eventへ戻る最大待機時間。
constexpr ULONG kChannelReadTimeoutMilliseconds = 100U;

/// portable AgentがEXEと同じdirectoryから読む固定設定名。
constexpr wchar_t kAgentConfigurationFileName[] = L"agent.ini";

/// Windows long path上限を越えたmodule pathを設定探索へ使用しない。
constexpr std::size_t kMaximumModulePathCharacters = 32768U;

/**
 * @brief console control eventをallocationなしの停止要求へ変換する。
 * @param[in] control_type Windowsが通知したcontrol event。
 * @return 処理対象eventならTRUE。
 */
BOOL WINAPI ConsoleControlHandler(
    const DWORD control_type) noexcept {
    // process終了系eventだけを協調停止へ変換する。
    if (control_type == CTRL_C_EVENT ||
        control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT ||
        control_type == CTRL_LOGOFF_EVENT ||
        control_type == CTRL_SHUTDOWN_EVENT) {
        g_stop_requested.store(
            true,
            std::memory_order_release);
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief 固定の非機密status文字列をWindows consoleへ表示する。
 * @param[in] message compile-time固定の日本語message。
 */
void WriteConsoleStatus(const wchar_t* message) noexcept {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    // consoleなしで起動された場合は診断logだけを使用する。
    if (output == nullptr ||
        output == INVALID_HANDLE_VALUE ||
        message == nullptr) {
        return;
    }

    DWORD length = 0U;
    // 固定文字列だけを有限走査し、実環境値をconsoleへ含めない。
    while (message[length] != L'\0' &&
        length < 1024U) {
        ++length;
    }

    DWORD written = 0U;
    static_cast<void>(
        WriteConsoleW(
            output,
            message,
            length,
            &written,
            nullptr));
}

/**
 * @brief Agent console windowへEXE埋込みのZRdp iconを設定する。
 *
 * Windows Terminal等で独立console HWNDがない場合もAgent処理は継続する。
 * `LR_SHARED`で取得するため、呼出し側でHICONを破棄しない。
 */
void ApplyConsoleIcon() noexcept {
    const HWND console_window = GetConsoleWindow();
    // console windowを所有しないhostではExplorerのEXE iconだけを使用する。
    if (console_window == nullptr) {
        return;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const auto large_icon = static_cast<HICON>(
        LoadImageW(
            instance,
            MAKEINTRESOURCEW(IDI_ZRDP_APP),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR | LR_SHARED));
    const auto small_icon = static_cast<HICON>(
        LoadImageW(
            instance,
            MAKEINTRESOURCEW(IDI_ZRDP_APP),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR | LR_SHARED));
    // 片方でもresource取得に失敗した場合は部分的なwindow icon設定を行わない。
    if (large_icon == nullptr ||
        small_icon == nullptr) {
        return;
    }

    static_cast<void>(
        SendMessageW(
            console_window,
            WM_SETICON,
            ICON_BIG,
            reinterpret_cast<LPARAM>(large_icon)));
    static_cast<void>(
        SendMessageW(
            console_window,
            WM_SETICON,
            ICON_SMALL,
            reinterpret_cast<LPARAM>(small_icon)));
}

/**
 * @brief Agent EXEと同じdirectoryの固定allowlist pathを取得する。
 * @return 完全なmodule pathを取得できた場合の`agent.ini` path。
 *
 * @warning 戻り値pathは診断logへ出力しない。
 */
[[nodiscard]] std::filesystem::path
AgentConfigurationPath() noexcept {
    try {
        std::vector<wchar_t> module_path(
            kMaximumModulePathCharacters);
        const DWORD length = GetModuleFileNameW(
            nullptr,
            module_path.data(),
            static_cast<DWORD>(module_path.size()));
        // API失敗または切り詰めpathから別directoryを推測しない。
        if (length == 0U ||
            length >= module_path.size()) {
            return {};
        }

        return std::filesystem::path(
            std::wstring(
                module_path.data(),
                length)).parent_path() /
            kAgentConfigurationFileName;
    } catch (...) {
        // path allocation失敗は設定利用不能へ縮退し、例外をentry pointへ漏らさない。
        return {};
    }
}

/**
 * @brief Agent Bridge失敗を固定数値fieldだけで診断記録する。
 * @param[in] snapshot payloadを含まないAgent state。
 */
void LogBridgeFailure(
    const zrdp::agent::bridge::AgentBridgeSnapshot& snapshot) noexcept {
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Error,
        zrdp::common::DiagnosticEvent::BridgeFailed,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::BridgeState,
                static_cast<std::uint64_t>(
                    snapshot.state)),
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::BridgeError,
                static_cast<std::uint64_t>(
                    snapshot.error)),
        });
}

}  // namespace

/**
 * @brief AgentのUnicode console entry point。
 * @return 正常停止は0、session／channel／protocol失敗は固定非0 code。
 */
int wmain() {
    using zrdp::agent::app::SessionValidationCode;
    using zrdp::agent::bridge::AgentBridge;
    using zrdp::agent::bridge::AgentBridgeState;
    using zrdp::agent::channel::ChannelReadStatus;

    ApplyConsoleIcon();
    static_cast<void>(
        zrdp::common::InitializeDiagnosticLog(
            zrdp::common::DiagnosticRole::Agent));
    WriteConsoleStatus(
        L"ZRdp SVN Agent: RDPセッションを確認しています。\r\n");

    const zrdp::agent::app::SessionValidationResult
        session_result =
            zrdp::agent::app::ValidateCurrentSession();
    // current active RDP session以外ではSVC openへ進まない。
    if (session_result.code != SessionValidationCode::Valid) {
        zrdp::common::WriteDiagnosticLog(
            zrdp::common::DiagnosticLevel::Error,
            zrdp::common::DiagnosticEvent::
                AgentSessionValidationReturned,
            {
                zrdp::common::DiagnosticCode(
                    static_cast<long>(
                        session_result.code)),
                zrdp::common::DiagnosticValue(
                    zrdp::common::DiagnosticFieldName::Reason,
                    session_result.win32_error),
            });
        WriteConsoleStatus(
            L"ZRdp SVN Agent: activeなRDPユーザーセッション内で起動してください。\r\n");
        zrdp::common::ShutdownDiagnosticLog();
        return 2;
    }

    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            AgentSessionValidationReturned);

    zrdp::agent::channel::WtsVirtualChannel channel;
    const DWORD open_error = channel.Open();
    // ClientがESVNBRGを登録していないsessionでは待受や別経路へfallbackしない。
    if (open_error != ERROR_SUCCESS) {
        zrdp::common::WriteDiagnosticLog(
            zrdp::common::DiagnosticLevel::Error,
            zrdp::common::DiagnosticEvent::
                AgentChannelOpenReturned,
            {
                zrdp::common::DiagnosticValue(
                    zrdp::common::DiagnosticFieldName::Reason,
                    open_error),
            });
        WriteConsoleStatus(
            L"ZRdp SVN Agent: ESVNBRGを開けません。ZRdp Clientから接続した同じRDPセッションで起動してください。\r\n");
        zrdp::common::ShutdownDiagnosticLog();
        return 3;
    }

    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            AgentChannelOpenReturned);
    WriteConsoleStatus(
        L"ZRdp SVN Agent: allowlist設定を確認しています。\r\n");

    zrdp::agent::network::Allowlist allowlist;
    zrdp::agent::network::AllowlistResult
        allowlist_result{
            zrdp::agent::network::AllowlistError::
                FileUnavailable,
            0U,
        };
    const std::filesystem::path configuration_path =
        AgentConfigurationPath();
    // 不完全なmodule pathではcurrent directory等へ設定探索をfallbackしない。
    if (!configuration_path.empty()) {
        allowlist_result =
            zrdp::agent::network::LoadAllowlistFile(
                configuration_path,
                allowlist);
    }

    zrdp::agent::network::WinsockRuntime
        winsock_runtime;
    zrdp::agent::network::WinsockUpstreamConnector
        upstream_connector;
    zrdp::agent::bridge::AgentStreamRelay
        stream_relay(
            allowlist,
            upstream_connector);
    bool stream_relay_enabled = false;
    if (allowlist_result.succeeded()) {
        zrdp::common::WriteDiagnosticLog(
            zrdp::common::DiagnosticLevel::
                Information,
            zrdp::common::DiagnosticEvent::
                AgentAllowlistLoadReturned,
            {
                zrdp::common::DiagnosticValue(
                    zrdp::common::
                        DiagnosticFieldName::
                            RecordCount,
                    allowlist.size()),
            });
        const std::uint32_t winsock_error =
            winsock_runtime.Start();
        zrdp::common::WriteDiagnosticLog(
            winsock_error == 0U
                ? zrdp::common::DiagnosticLevel::
                      Information
                : zrdp::common::DiagnosticLevel::
                      Warning,
            zrdp::common::DiagnosticEvent::
                AgentWinsockInitializationReturned,
            {
                zrdp::common::DiagnosticValue(
                    zrdp::common::
                        DiagnosticFieldName::Reason,
                    winsock_error),
            });
        // valid allowlistとWinsockの両方が揃った場合だけstream capabilityを広告する。
        if (winsock_error == 0U) {
            stream_relay_enabled = true;
            zrdp::common::WriteDiagnosticLog(
                zrdp::common::DiagnosticLevel::
                    Information,
                zrdp::common::DiagnosticEvent::
                    AgentStreamRelayEnabled,
                {
                    zrdp::common::DiagnosticValue(
                        zrdp::common::
                            DiagnosticFieldName::
                                Enabled,
                        1U),
                });
            WriteConsoleStatus(
                L"ZRdp SVN Agent: allowlist限定SVN streamを有効にしました。ClientのHELLOを待機しています。\r\n");
        } else {
            WriteConsoleStatus(
                L"ZRdp SVN Agent: Winsockを初期化できないため、RDP Echoのみで継続します。\r\n");
        }
    } else {
        zrdp::common::WriteDiagnosticLog(
            zrdp::common::DiagnosticLevel::Warning,
            zrdp::common::DiagnosticEvent::
                AgentAllowlistLoadReturned,
            {
                zrdp::common::DiagnosticValue(
                    zrdp::common::
                        DiagnosticFieldName::Reason,
                    static_cast<std::uint64_t>(
                        allowlist_result.error)),
                zrdp::common::DiagnosticValue(
                    zrdp::common::
                        DiagnosticFieldName::
                            ExtendedReason,
                    allowlist_result.line),
            });
        WriteConsoleStatus(
            L"ZRdp SVN Agent: agent.iniが未設定または不正なため、RDP Echoのみで継続します。\r\n");
    }

    WriteConsoleStatus(
        L"ZRdp SVN Agent: 終了はCtrl+Cです。\r\n");
    static_cast<void>(
        SetConsoleCtrlHandler(
            &ConsoleControlHandler,
            TRUE));

    AgentBridge agent_bridge;
    if (stream_relay_enabled) {
        agent_bridge.ConfigureStreamRelay(
            &stream_relay);
    } else {
        agent_bridge.Reset();
    }
    std::array<
        std::byte,
        zrdp::protocol::kStaticVirtualChannelChunkBytes>
        read_buffer{};
    AgentBridgeState last_state =
        agent_bridge.snapshot().state;
    int exit_code = 0;

    // 一つのI/O ownerだけがWTS readとserialized writer queueを順番に処理する。
    while (!g_stop_requested.load(
        std::memory_order_acquire)) {
        const zrdp::agent::channel::ChannelReadResult
            read_result = channel.Read(
                read_buffer,
                kChannelReadTimeoutMilliseconds);
        // RDP切断やchannel failureでは残dataを再利用せずloopを終了する。
        if (read_result.status ==
            ChannelReadStatus::Error) {
            zrdp::common::WriteDiagnosticLog(
                zrdp::common::DiagnosticLevel::Error,
                zrdp::common::DiagnosticEvent::
                    AgentChannelReadFailed,
                {
                    zrdp::common::DiagnosticValue(
                        zrdp::common::DiagnosticFieldName::Reason,
                        read_result.win32_error),
                });
            exit_code = 4;
            break;
        }

        // timeout以外で受信したfragmentだけをincremental decoderへ渡す。
        if (read_result.status !=
            ChannelReadStatus::Timeout) {
            const std::span<const std::byte> received(
                read_buffer.data(),
                read_result.bytes_read);
            // malformed protocolではPONGや別frameを追加送信しない。
            if (!agent_bridge.OnChannelData(received)) {
                LogBridgeFailure(
                    agent_bridge.snapshot());
                WriteConsoleStatus(
                    L"ZRdp SVN Agent: protocol errorでBridgeを停止しました。\r\n");
                exit_code = 5;
                break;
            }
        }

        // WTS timeout中もnon-blocking connect／Socket I/Oを有限回進める。
        if (!agent_bridge.PumpStreams(
                GetTickCount64())) {
            LogBridgeFailure(
                agent_bridge.snapshot());
            WriteConsoleStatus(
                L"ZRdp SVN Agent: SVN stream処理を安全に停止しました。\r\n");
            exit_code = 5;
            break;
        }

        const auto current_snapshot =
            agent_bridge.snapshot();
        // HELLO成立時だけ一度、payloadを含まないready状態を表示する。
        if (last_state != current_snapshot.state &&
            current_snapshot.state ==
                AgentBridgeState::Ready) {
            zrdp::common::WriteDiagnosticLog(
                zrdp::common::DiagnosticLevel::Information,
                zrdp::common::DiagnosticEvent::BridgeReady);
            WriteConsoleStatus(
                stream_relay_enabled
                    ? L"ZRdp SVN Agent: Clientと接続しました。Echo／SVN streamを待機しています。\r\n"
                    : L"ZRdp SVN Agent: Clientと接続しました。Echo要求を待機しています。\r\n");
        }
        last_state = current_snapshot.state;

        bool more_data = false;
        // protocol handlerが生成した応答を一つのserialized writer経路で順に送る。
        do {
            std::vector<std::byte> chunk;
            if (!agent_bridge.TakeOutboundChunk(
                    chunk,
                    more_data)) {
                break;
            }

            const zrdp::agent::channel::ChannelWriteResult
                write_result = channel.Write(chunk);
            // partial writeもframe欠落としてchannel sessionを終了する。
            if (!write_result.succeeded) {
                zrdp::common::WriteDiagnosticLog(
                    zrdp::common::DiagnosticLevel::Error,
                    zrdp::common::DiagnosticEvent::
                        AgentChannelWriteFailed,
                    {
                        zrdp::common::DiagnosticValue(
                            zrdp::common::DiagnosticFieldName::Reason,
                            write_result.win32_error),
                    });
                exit_code = 6;
                break;
            }
        } while (more_data);

        // write failure後はread loopへ戻らずchannelを閉じる。
        if (exit_code != 0) {
            break;
        }
    }

    // 正常終了時はClientをHELLO待機へ戻し、同じRDP内でAgentを再起動可能にする。
    if (exit_code == 0 &&
        agent_bridge.QueueSessionClose()) {
        bool more_data = false;
        // CLOSEを含む残queueをchannel close前にwire順で排出する。
        do {
            std::vector<std::byte> chunk;
            if (!agent_bridge.TakeOutboundChunk(
                    chunk,
                    more_data)) {
                break;
            }

            const zrdp::agent::channel::ChannelWriteResult
                write_result = channel.Write(chunk);
            // 終了通知のwrite失敗もpayloadなしの固定errorとして記録する。
            if (!write_result.succeeded) {
                zrdp::common::WriteDiagnosticLog(
                    zrdp::common::DiagnosticLevel::Error,
                    zrdp::common::DiagnosticEvent::
                        AgentChannelWriteFailed,
                    {
                        zrdp::common::DiagnosticValue(
                            zrdp::common::DiagnosticFieldName::Reason,
                            write_result.win32_error),
                    });
                exit_code = 6;
                break;
            }
        } while (more_data);
    }

    // RDP切断やwrite failureを含む全経路でupstream Socketをchannel close前に閉じる。
    agent_bridge.Reset();
    static_cast<void>(
        SetConsoleCtrlHandler(
            &ConsoleControlHandler,
            FALSE));
    channel.Close();
    WriteConsoleStatus(
        L"ZRdp SVN Agent: 停止しました。\r\n");
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
