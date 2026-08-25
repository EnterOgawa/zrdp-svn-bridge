/**
 * @file diagnostic_log_test.cpp
 * @brief 診断ログの有限サイズ、世代管理、固定語彙、秘密情報排除を検証する。
 *
 * test実行ファイル隣接のlogsだけを作成・除去し、実環境ログを対象にしない。
 */

#include "common/diagnostic_log.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace {

/**
 * @brief test実行ファイルが置かれたディレクトリを取得する。
 * @return 完全なmodule pathを取得できた場合の親ディレクトリ。
 */
[[nodiscard]] std::filesystem::path TestExecutableDirectory() {
    std::array<wchar_t, MAX_PATH> module_path{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        module_path.data(),
        static_cast<DWORD>(module_path.size()));
    // 切り詰められたpathを削除・作成対象に使用しない。
    if (length == 0U || length >= module_path.size()) {
        return {};
    }

    return std::filesystem::path(
        std::wstring(module_path.data(), length)).parent_path();
}

/**
 * @brief このtestの現行ログと有限backup世代だけを除去する。
 * @param current_log test実行ファイル隣接のログ完全パス。
 */
void RemoveTestLogs(
    const std::filesystem::path& current_log) noexcept {
    std::error_code error;
    std::filesystem::remove(current_log, error);

    // test実行ファイルディレクトリ内のlogger世代だけを有限回数で除去する。
    for (unsigned int generation = 1U;
         generation <= zrdp::common::kDiagnosticLogBackupGenerations;
         ++generation) {
        error.clear();
        std::filesystem::remove(
            current_log.wstring() +
                L"." +
                std::to_wstring(generation),
            error);
    }
}

/**
 * @brief 完全な固定tokenの出現回数を数える。
 * @param text 検査するログ内容。
 * @param pattern 数える固定event token。
 * @return 重複を含む出現回数。
 */
[[nodiscard]] std::size_t CountOccurrences(
    const std::string_view text,
    const std::string_view pattern) noexcept {
    std::size_t count = 0U;
    std::size_t position = 0U;

    // 停止recordの重複を検出するため、完全な固定event tokenを最後まで数える。
    while ((position = text.find(pattern, position)) !=
           std::string_view::npos) {
        ++count;
        position += pattern.size();
    }

    return count;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief 診断logのredaction、rotation、process role別file名を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    // test対象を実行ファイル隣接の専用logsディレクトリへ限定する。
    const std::filesystem::path executable_directory =
        TestExecutableDirectory();
    // 安全な対象pathを確定できなければファイル操作を開始しない。
    if (executable_directory.empty()) {
        return 1;
    }

    const std::filesystem::path log_directory =
        executable_directory / L"logs";
    const std::filesystem::path client_log =
        log_directory / L"ZRdpSvnBridge.log";
    const std::filesystem::path agent_log =
        log_directory / L"ZRdpSvnAgent.log";
    const std::filesystem::path first_generation =
        client_log.wstring() + L".1";

    std::error_code error;
    std::filesystem::create_directories(log_directory, error);
    // directory生成失敗時は不明な代替pathでtestを続けない。
    if (error) {
        return 2;
    }

    RemoveTestLogs(client_log);
    RemoveTestLogs(agent_log);

    std::ofstream oversized_log(
        client_log,
        std::ios::binary |
        std::ios::out |
        std::ios::trunc);
    // seed fileを作成できなければrotation testを中止する。
    if (!oversized_log.is_open()) {
        return 3;
    }

    std::array<char, 4096> block{};
    block.fill('X');
    const std::uint64_t block_count =
        zrdp::common::kDiagnosticLogMaximumBytes /
        static_cast<std::uint64_t>(block.size());

    // 初期化時の追記前rotationを発生させるため、上限ちょうどのseed fileを作る。
    for (std::uint64_t index = 0U; index < block_count; ++index) {
        oversized_log.write(
            block.data(),
            static_cast<std::streamsize>(block.size()));
    }

    oversized_log.close();
    // logger初期化失敗時は公開APIの以後の前提が成立しない。
    if (!zrdp::common::InitializeDiagnosticLog(
            zrdp::common::DiagnosticRole::Client)) {
        RemoveTestLogs(client_log);
        RemoveTestLogs(agent_log);
        return 4;
    }

    // Client roleが従来のClient専用ファイル名を選んだことを確認する。
    if (std::filesystem::path(zrdp::common::DiagnosticLogPath()) !=
        client_log) {
        zrdp::common::ShutdownDiagnosticLog();
        RemoveTestLogs(client_log);
        RemoveTestLogs(agent_log);
        return 5;
    }

    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::ConnectButtonClicked,
        {
            zrdp::common::DiagnosticHResult(S_OK),
            zrdp::common::DiagnosticCode(-7L),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::SmartSizingCallReturned,
        {zrdp::common::DiagnosticHResult(S_OK)});
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::DynamicResolutionCallReturned,
        {zrdp::common::DiagnosticHResult(S_OK)});
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Warning,
        zrdp::common::DiagnosticEvent::
            PreLoginConnectionRetryScheduled,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::AttemptCount,
                2U),
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::MaximumAttemptCount,
                2U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            PreLoginConnectionRetryStarted,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::AttemptCount,
                2U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            PreLoginConnectionRetryCompleted,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::AttemptCount,
                2U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Error,
        zrdp::common::DiagnosticEvent::
            PreLoginConnectionRetryExhausted,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::AttemptCount,
                2U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Warning,
        zrdp::common::DiagnosticEvent::BridgePeerTimeout);
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::ConnectionSettingsApplyStarted,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::DisplayMode,
                2U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            ActiveXDefaultDialogParentSelected);
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            SecurityDialogConfigurationReturned,
        {
            zrdp::common::DiagnosticHResult(S_OK),
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::DialogVersion,
                1U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            AuthenticationLevelConfigurationReturned,
        {
            zrdp::common::DiagnosticHResult(S_OK),
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::AuthenticationLevel,
                2U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::EventDisconnected,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::
                    CredentialPromptCancelled,
                1U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::EventDisconnected,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::
                    SecurityWarningCancelled,
                1U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            EventRequestContainerMinimize);
    zrdp::common::ShutdownDiagnosticLog();

    // rotation後に新現行ログと第1世代の両方が存在することを確認する。
    if (!std::filesystem::exists(client_log) ||
        !std::filesystem::exists(first_generation)) {
        RemoveTestLogs(client_log);
        RemoveTestLogs(agent_log);
        return 6;
    }

    // 第1世代が設定上限以上のseed内容を保持していることを確認する。
    if (std::filesystem::file_size(first_generation, error) <
        zrdp::common::kDiagnosticLogMaximumBytes ||
        error) {
        RemoveTestLogs(client_log);
        RemoveTestLogs(agent_log);
        return 7;
    }

    std::ifstream produced_log(client_log, std::ios::binary);
    const std::string contents{
        std::istreambuf_iterator<char>(produced_log),
        std::istreambuf_iterator<char>(),
    };
    produced_log.close();

    const bool contains_expected_records =
        contents.find("event=process.started") != std::string::npos &&
        contents.find("event=ui.connect_button_clicked") != std::string::npos &&
        contents.find("event=rdp.smart_sizing_call_returned") !=
            std::string::npos &&
        contents.find("event=rdp.dynamic_resolution_call_returned") !=
            std::string::npos &&
        contents.find("event=bridge.peer_timeout") !=
            std::string::npos &&
        contents.find(
            "event=rdp.pre_login_retry_scheduled "
            "attempt=2 max_attempts=2") !=
            std::string::npos &&
        contents.find(
            "event=rdp.pre_login_retry_started attempt=2") !=
            std::string::npos &&
        contents.find(
            "event=rdp.pre_login_retry_completed attempt=2") !=
            std::string::npos &&
        contents.find(
            "event=rdp.pre_login_retry_exhausted attempt=2") !=
            std::string::npos &&
        contents.find(
            "event=rdp.connection_settings_apply_started display_mode=2") !=
            std::string::npos &&
        contents.find(
            "event=rdp.activex_default_dialog_parent_selected") !=
            std::string::npos &&
        contents.find(
            "event=rdp.security_dialog_configuration_returned "
            "hresult=0x00000000 dialog_version=1") !=
            std::string::npos &&
        contents.find(
            "event=rdp.authentication_level_configuration_returned "
            "hresult=0x00000000 authentication_level=2") !=
            std::string::npos &&
        contents.find(
            "event=rdp.event.disconnected "
            "credential_prompt_cancelled=1") !=
            std::string::npos &&
        contents.find(
            "event=rdp.event.disconnected "
            "security_warning_cancelled=1") !=
            std::string::npos &&
        contents.find(
            "event=rdp.event.request_container_minimize") !=
            std::string::npos &&
        contents.find("hresult=0x00000000") != std::string::npos &&
        contents.find("code=-7") != std::string::npos &&
        CountOccurrences(contents, "event=process.stopping") == 1U;
    // 固定event、数値field、停止event一件というrecord契約をまとめて確認する。
    if (!contains_expected_records) {
        RemoveTestLogs(client_log);
        RemoveTestLogs(agent_log);
        return 8;
    }

    const bool contains_forbidden_free_form_data =
        contents.find("192.0.2.1") != std::string::npos ||
        contents.find("Authorization:") != std::string::npos ||
        contents.find("password") != std::string::npos;
    // 任意の接続先・認証header・password文字列がログへ混入してはならない。
    if (contains_forbidden_free_form_data) {
        RemoveTestLogs(client_log);
        RemoveTestLogs(agent_log);
        return 9;
    }

    RemoveTestLogs(client_log);

    // Agent roleではClientログと世代管理を共有せず、Agent専用名を選択する。
    if (!zrdp::common::InitializeDiagnosticLog(
            zrdp::common::DiagnosticRole::Agent)) {
        RemoveTestLogs(agent_log);
        return 10;
    }

    if (std::filesystem::path(zrdp::common::DiagnosticLogPath()) !=
        agent_log) {
        zrdp::common::ShutdownDiagnosticLog();
        RemoveTestLogs(agent_log);
        return 11;
    }

    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::BridgeReady);
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            AgentAllowlistLoadReturned,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::
                    RecordCount,
                3U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            AgentWinsockInitializationReturned,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::
                    Reason,
                0U),
        });
    zrdp::common::WriteDiagnosticLog(
        zrdp::common::DiagnosticLevel::Information,
        zrdp::common::DiagnosticEvent::
            AgentStreamRelayEnabled,
        {
            zrdp::common::DiagnosticValue(
                zrdp::common::DiagnosticFieldName::
                    Enabled,
                1U),
        });
    zrdp::common::ShutdownDiagnosticLog();

    // Agent起動がClient名のログを再作成していないことも分離契約として確認する。
    if (!std::filesystem::exists(agent_log) ||
        std::filesystem::exists(client_log)) {
        RemoveTestLogs(client_log);
        RemoveTestLogs(agent_log);
        return 12;
    }

    std::ifstream produced_agent_log(agent_log, std::ios::binary);
    const std::string agent_contents{
        std::istreambuf_iterator<char>(produced_agent_log),
        std::istreambuf_iterator<char>(),
    };
    produced_agent_log.close();

    // Agent専用ログにも固定roleとeventだけが記録されることを確認する。
    if (agent_contents.find("role=agent") == std::string::npos ||
        agent_contents.find("event=bridge.ready") == std::string::npos ||
        agent_contents.find(
            "event=agent.allowlist_load_returned records=3") ==
            std::string::npos ||
        agent_contents.find(
            "event=agent.winsock_initialization_returned reason=0") ==
            std::string::npos ||
        agent_contents.find(
            "event=agent.stream_relay_enabled enabled=1") ==
            std::string::npos) {
        RemoveTestLogs(agent_log);
        return 13;
    }

    RemoveTestLogs(agent_log);
    return 0;
}
