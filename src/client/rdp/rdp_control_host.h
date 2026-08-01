/**
 * @file rdp_control_host.h
 * @brief Microsoft Remote Desktop ActiveX Controlの安全なhost境界を定義する。
 *
 * @details COM interface、event購読、ActiveX child windowを一つの所有classへ
 *          閉じ込め、全操作を生成元COM STAへ限定する。
 */
#pragma once

#include "client/rdp/rdp_audio_settings.h"

#include <Windows.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zrdp::client::rdp {

/**
 * @brief Microsoft RDP ActiveX hostが公開する接続lifecycle。
 */
enum class RdpConnectionState {
    NotInitialized,  ///< ActiveX child windowをまだ生成していない。
    Disconnected,    ///< 接続可能だがRDP sessionは存在しない。
    Connecting,      ///< Connect後の非同期接続処理中。
    Connected,       ///< transport接続が成立した。
    LoggedIn,        ///< login完了eventを受信した。
    Reconnecting,    ///< Microsoft controlによる自動再接続中。
    Disconnecting,   ///< 切断または終了要求の完了待ち。
    Failed,          ///< fatal errorを受信し通常操作できない。
};

/**
 * @brief session display設定を安全に更新できる接続状態か判定する。
 * @param[in] state 現在のRDP接続状態。
 * @return ログイン完了済みsessionならtrue。
 *
 * `OnConnected`はtransport成立を示すだけで、remote desktopの初期化完了を
 * 保証しない。`UpdateSessionDisplaySettings`は`OnLoginComplete`後に限定し、
 * 接続直後の`E_UNEXPECTED`と意図しない等倍表示への縮退を防ぐ。
 */
[[nodiscard]] constexpr bool CanUpdateSessionDisplaySettings(
    const RdpConnectionState state) noexcept {
    return state == RdpConnectionState::LoggedIn;
}

/**
 * @brief remote desktop寸法とClient側描画の関係を表す排他的な表示mode。
 */
enum class RdpDisplayMode : unsigned int {
    NativeScroll = 0U,       ///< remote寸法を維持し、等倍表示とscrollを使用する。
    SmartSizing = 1U,        ///< remote寸法を維持し、Client側だけを縮小表示する。
    DynamicResolution = 2U,  ///< remote寸法をZRdpのRDP表示領域へ追従させる。
};

/**
 * @brief IMsTscAxEvents COM callbackから所有copy化するevent種別。
 */
enum class RdpEventKind {
    Connecting,
    Connected,
    LoginComplete,
    Disconnected,
    EnterFullScreen,
    LeaveFullScreen,
    ChannelDataReceived,
    RequestGoFullScreen,
    RequestLeaveFullScreen,
    FatalError,
    Warning,
    ConfirmClose,
    AutoReconnecting,
    AutoReconnected,
    AuthenticationWarningDisplayed,
    AuthenticationWarningDismissed,
};

/**
 * @brief RDP ActiveX eventの所有表現。
 *
 * Virtual Channel payloadはBSTR byte長を使ってcopyする。文字列変換、
 * 終端検索、payload log出力は行わない。
 */
struct RdpEvent {
    /// dispatchするevent種別。
    RdpEventKind kind{RdpEventKind::Connecting};

    /// event固有のActiveX codeまたはdisconnect reason。
    long code{0};

    /// Windowsが返した拡張disconnect reason。
    long extended_disconnect_reason{0};

    /// ZRdp自身の切断要求に対応するeventならtrue。
    bool locally_requested_disconnect{false};

    /// Microsoft標準credential UIを利用者が閉じた接続前キャンセルならtrue。
    bool credential_prompt_cancelled{false};

    /// 自動再接続eventが報告したnetwork利用可否。
    bool network_available{false};

    /// 現在の自動再接続試行回数。
    long attempt_count{0};

    /// Microsoft controlが報告した最大試行回数。
    long max_attempt_count{0};

    /// Microsoft由来のbounded説明。UI表示だけに使用しlogへ出さない。
    std::wstring error_description;

    /// callback中にcopyしたVirtual Channel名。通常logへ出さない。
    std::wstring channel_name;

    /// callback中にbyte長でcopyした任意binary payload。
    std::vector<std::byte> channel_payload;
};

/**
 * @brief ActiveX切断値をログイン前credential promptキャンセルとして分類する。
 * @param[in] state_before_disconnect 切断callbackを受ける直前の接続state。
 * @param[in] disconnect_reason `OnDisconnected`が通知したreason。
 * @param[in] extended_disconnect_reason ActiveXから取得した拡張reason。
 * @param[in] locally_requested_disconnect ZRdp自身が切断を要求した場合はtrue。
 * @return 利用者がMicrosoft標準credential UIを閉じた再現条件ならtrue。
 *
 * 公開一覧にないreasonを広く正常扱いしないため、ログイン完了前のstate、
 * 固定reason、拡張情報なし、非local切断の完全一致だけを受け付ける。
 */
[[nodiscard]] bool IsCredentialPromptCancellation(
    RdpConnectionState state_before_disconnect,
    long disconnect_reason,
    long extended_disconnect_reason,
    bool locally_requested_disconnect) noexcept;

/**
 * @brief RDP ActiveX境界が受け付ける接続前設定。
 *
 * passwordは意図的に含めず、認証をMicrosoft標準RDP credential UIへ委譲する。
 */
struct RdpConnectionSettings {
    /// 接続先PC名またはIP。logへ出さず、全画面connection barへ表示する。
    std::wstring server;

    /// 任意のRDPドメイン。user_nameとは別propertyとしてActiveXへ設定する。
    std::wstring domain;

    /// 任意のユーザー名。passwordは別memberとして追加しない。
    std::wstring user_name;

    /// 接続開始時にClient Core Dataへ設定するremote desktop幅。
    long desktop_width{1280};

    /// 接続開始時にClient Core Dataへ設定するremote desktop高さ。
    long desktop_height{720};

    /// 接続開始時に適用する排他的なremote表示mode。
    RdpDisplayMode display_mode{RdpDisplayMode::NativeScroll};

    /// 接続時に確定し、確立済みsessionへ途中反映しない音声設定。
    RdpAudioRedirectionSettings audio;
};

/**
 * @brief COM STAのUI threadで所有済みRDP eventを受け取るobserver。
 */
class IRdpEventObserver {
public:
    /**
     * @brief observer interface経由の安全な破棄を許可する。
     */
    virtual ~IRdpEventObserver() = default;

    /**
     * @brief 一件のActiveX event所有copyを処理する。
     * @param[in] event callback中に必要dataをcopy済みのevent。
     *
     * 実装はeventを別のbounded queueへ移動できる。例外はCOM callback境界で
     * 捕捉されるが、通常はこのmethodから例外を送出しない。
     */
    virtual void OnRdpEvent(RdpEvent event) = 0;
};

/**
 * @brief ATL ActiveX window、Microsoft RDP control、event購読を所有する。
 *
 * 破棄を含む全methodはCreateを呼んだthreadで実行する。applicationは
 * そのthreadをCOM STAとして初期化する。
 */
class RdpControlHost final {
public:
    /**
     * @brief event observerへ紐付いた未初期化hostを構築する。
     * @param[in] observer ActiveX eventの通知先。hostより長く生存すること。
     */
    explicit RdpControlHost(IRdpEventObserver& observer);

    /**
     * @brief 生成元STAでActiveX controlと購読を解放する。
     */
    ~RdpControlHost();

    RdpControlHost(const RdpControlHost&) = delete;
    RdpControlHost& operator=(const RdpControlHost&) = delete;
    RdpControlHost(RdpControlHost&&) = delete;
    RdpControlHost& operator=(RdpControlHost&&) = delete;

    /**
     * @brief ATL child windowと利用可能な最新RDP controlを生成する。
     * @param[in] parent ActiveXをcontainする有効な親window。
     * @param[in] bounds 親client座標での正の表示矩形。
     * @return 生成、親設定、event購読を含むHRESULT。
     */
    [[nodiscard]] HRESULT Create(HWND parent, const RECT& bounds) noexcept;

    /**
     * @brief Connect前に非機密RDP設定を一括適用する。
     * @param[in] settings 検証対象の接続設定。passwordは含まない。
     * @return 全設定を適用できた場合はS_OK。
     *
     * 一部だけ適用して失敗した場合もConnectは許可せず、次の完全な設定要求を待つ。
     */
    [[nodiscard]] HRESULT ApplyConnectionSettings(
        const RdpConnectionSettings& settings) noexcept;

    /**
     * @brief Microsoft標準RDPの非同期接続を開始する。
     * @return 接続開始要求のHRESULT。完了はeventで通知する。
     */
    [[nodiscard]] HRESULT Connect() noexcept;

    /**
     * @brief ESVNBRGへ一つのbinary transport chunkを送信する。
     * @param[in] data 1～1600 byteの所有元binary data。
     * @return UI thread、接続state、長さ、ActiveX送信の検証結果。
     *
     * BSTRを文字列として扱わず、`SysAllocStringByteLen`で明示byte長を維持する。
     * 呼出し側はbounded queueから取り出した後、COM callback外のUI threadで
     * このmethodを呼ぶ。
     */
    [[nodiscard]] HRESULT SendStaticVirtualChannelData(
        std::span<const std::byte> data) noexcept;

    /**
     * @brief 現在のRDP connectionへ切断を要求する。
     * @return 切断要求のHRESULT。完了はOnDisconnectedで通知する。
     */
    [[nodiscard]] HRESULT Disconnect() noexcept;

    /**
     * @brief application終了前にcontrolのclean shutdownを要求する。
     * @param[out] can_close_now 即時破棄可能な場合はtrue。
     * @return RequestCloseのHRESULT。
     */
    [[nodiscard]] HRESULT RequestClose(bool& can_close_now) noexcept;

    /**
     * @brief Microsoft controlのWindow／全画面modeを切り替える。
     * @param[in] full_screen 全画面へ移行する場合はtrue。
     * @return ActiveX設定結果を表すHRESULT。
     */
    [[nodiscard]] HRESULT SetFullScreen(bool full_screen) noexcept;

    /**
     * @brief 接続先の領域を変えず、ローカル表示の拡縮だけを切り替える。
     *
     * 接続中にも変更可能な Microsoft Smart Sizing プロパティだけを操作し、
     * セッションの DesktopWidth／DesktopHeight は更新しない。
     *
     * @param[in] enabled Client側拡縮を有効にする場合はtrue。
     * @return ActiveX設定結果を表すHRESULT。
     */
    [[nodiscard]] HRESULT SetSmartSizing(bool enabled) noexcept;

    /**
     * @brief 成立済みsessionのremote desktop寸法を明示的に変更する。
     * @param[in] desktop_width 新しいremote desktop幅。
     * @param[in] desktop_height 新しいremote desktop高さ。
     * @return `IMsRdpClient9::UpdateSessionDisplaySettings`のHRESULT。
     *
     * 動的解像度modeだけがUI側から呼び出す。物理寸法と回転は未指定、
     * desktop／device scaleは100%とし、Reconnectは行わない。
     */
    [[nodiscard]] HRESULT UpdateSessionDisplaySettings(
        long desktop_width,
        long desktop_height) noexcept;

    /**
     * @brief Window modeのActiveX child windowだけをresizeする。
     * @param[in] bounds 親client座標での正の表示矩形。
     * @return MoveWindowの結果を表すHRESULT。
     *
     * このmethod自身はremote desktop寸法を変更しない。動的解像度modeでは、
     * UI側のdebounce完了後にUpdateSessionDisplaySettingsを別途呼び出す。
     */
    [[nodiscard]] HRESULT Resize(const RECT& bounds) noexcept;

    /**
     * @brief hosted RDP controlへkeyboard focusを移す。
     */
    void Focus() noexcept;

    /**
     * @brief event購読、COM interface、ActiveX child windowを順序付きで解放する。
     *
     * 複数回呼び出しても安全で、生成元STA以外からの呼出しは何も行わない。
     */
    void Destroy() noexcept;

    /**
     * @brief hosted ActiveX child HWNDを取得する。
     * @return Create前またはDestroy後はnullptr。
     */
    [[nodiscard]] HWND window() const noexcept;

    /**
     * @brief 最後に観測したRDP接続状態を取得する。
     * @return UI thread上のcurrent state。
     */
    [[nodiscard]] RdpConnectionState state() const noexcept;

    /**
     * @brief Microsoft controlが全画面modeか取得する。
     * @return 全画面中の場合はtrue。
     */
    [[nodiscard]] bool full_screen() const noexcept;

    /**
     * @brief 実際に選択したMicrosoft RDP Control class名を取得する。
     * @return hostが所有する文字列へのread-only view。
     */
    [[nodiscard]] std::wstring_view selected_control_name() const noexcept;

private:
    /// COM型を公開headerへ漏らさないActiveX実装本体。
    class Impl;

    /// ActiveX lifecycle全体を一意所有するPImpl。
    std::unique_ptr<Impl> impl_;
};

}  // namespace zrdp::client::rdp
