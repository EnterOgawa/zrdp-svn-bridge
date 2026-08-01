/**
 * @file wts_virtual_channel.h
 * @brief Agent側ESVNBRG SVC handleのRAII read／write境界を定義する。
 *
 * @details 同一instanceのReadとWriteは一つのI/O loopだけから順番に呼ぶ。
 *          WTS APIがthread-safeでないため、複数threadから同一handleへ
 *          同時アクセスするinterfaceを提供しない。
 */
#pragma once

#include <Windows.h>

#include <cstddef>
#include <span>

namespace zrdp::agent::channel {

/**
 * @brief WTS read一回の結果分類。
 */
enum class ChannelReadStatus {
    Data,
    Timeout,
    Error,
};

/**
 * @brief WTS readのbyte数と秘密情報を含まないerror。
 */
struct ChannelReadResult {
    /// data、poll timeout、channel errorの分類。
    ChannelReadStatus status{ChannelReadStatus::Error};

    /// Data時の有効byte数。
    std::size_t bytes_read{0U};

    /// Error時のGetLastError。Timeoutでは0。
    DWORD win32_error{ERROR_SUCCESS};
};

/**
 * @brief WTS write一回の完全書込み結果。
 */
struct ChannelWriteResult {
    /// 指定chunk全体を書けた場合はtrue。
    bool succeeded{false};

    /// 失敗またはpartial write時の分類済みWin32 error。
    DWORD win32_error{ERROR_SUCCESS};
};

/**
 * @brief current RDP sessionのESVNBRG SVC handleを一意所有する。
 */
class WtsVirtualChannel final {
public:
    WtsVirtualChannel() = default;

    /// 所有channel handleをWTS APIで閉じる。
    ~WtsVirtualChannel();

    WtsVirtualChannel(const WtsVirtualChannel&) = delete;
    WtsVirtualChannel& operator=(const WtsVirtualChannel&) = delete;

    /**
     * @brief current sessionのStatic Virtual Channelをflags 0で開く。
     * @return 成功時はERROR_SUCCESS、失敗時はGetLastError。
     */
    [[nodiscard]] DWORD Open() noexcept;

    /**
     * @brief Clientから一つのSVC chunkを有限timeoutで読む。
     * @param[out] buffer `CHANNEL_CHUNK_LENGTH`以上の受信buffer。
     * @param[in] timeout_milliseconds Ctrl+C確認へ戻る有限timeout。
     * @return data、timeout、errorの分類。
     */
    [[nodiscard]] ChannelReadResult Read(
        std::span<std::byte> buffer,
        ULONG timeout_milliseconds) noexcept;

    /**
     * @brief AgentBridge queueの一chunkをClientへ完全書込みする。
     * @param[in] data 1～`CHANNEL_CHUNK_LENGTH` byte。
     * @return API結果とpartial writeを分類した値。
     */
    [[nodiscard]] ChannelWriteResult Write(
        std::span<const std::byte> data) noexcept;

    /**
     * @brief 所有channelを反復可能に閉じる。
     */
    void Close() noexcept;

    /**
     * @brief channel handleを所有しているか確認する。
     * @return Open成功後かつClose前ならtrue。
     */
    [[nodiscard]] bool is_open() const noexcept;

private:
    /// WTSVirtualChannelCloseで解放する所有handle。
    HANDLE handle_{nullptr};
};

}  // namespace zrdp::agent::channel
