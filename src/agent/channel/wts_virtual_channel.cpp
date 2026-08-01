/**
 * @file wts_virtual_channel.cpp
 * @brief WTSVirtualChannelOpenEx／Read／Write／Closeを一つの所有境界へ閉じ込める。
 */

#include "agent/channel/wts_virtual_channel.h"

#include "protocol/frame.h"

#include <PChannel.h>
#include <WtsApi32.h>

#include <limits>

namespace zrdp::agent::channel {
namespace {

static_assert(
    protocol::kStaticVirtualChannelChunkBytes ==
        CHANNEL_CHUNK_LENGTH,
    "Protocol transport chunk must match the Microsoft SVC read limit.");

}  // namespace

WtsVirtualChannel::~WtsVirtualChannel() {
    Close();
}

DWORD WtsVirtualChannel::Open() noexcept {
    // 同一instanceで複数handleを開いてmessage routingを曖昧にしない。
    if (handle_ != nullptr) {
        return ERROR_ALREADY_INITIALIZED;
    }

    handle_ = WTSVirtualChannelOpenEx(
        WTS_CURRENT_SESSION,
        const_cast<LPSTR>(
            protocol::kStaticVirtualChannelName),
        0U);
    // WTSは失敗時にnullptrとGetLastErrorを返す。
    if (handle_ == nullptr) {
        const DWORD error = GetLastError();
        return error == ERROR_SUCCESS
            ? ERROR_OPEN_FAILED
            : error;
    }

    return ERROR_SUCCESS;
}

ChannelReadResult WtsVirtualChannel::Read(
    const std::span<std::byte> buffer,
    const ULONG timeout_milliseconds) noexcept {
    // 未open handleやdocumented最小buffer未満ではWTS APIを呼ばない。
    if (handle_ == nullptr ||
        buffer.size() <
            protocol::kStaticVirtualChannelChunkBytes ||
        buffer.size() >
            static_cast<std::size_t>(
                std::numeric_limits<ULONG>::max())) {
        return {
            ChannelReadStatus::Error,
            0U,
            ERROR_INVALID_PARAMETER,
        };
    }

    ULONG bytes_read = 0U;
    SetLastError(ERROR_SUCCESS);
    const BOOL result = WTSVirtualChannelRead(
        handle_,
        timeout_milliseconds,
        reinterpret_cast<PCHAR>(buffer.data()),
        static_cast<ULONG>(buffer.size()),
        &bytes_read);
    // 成功でも0 byteなら次loopへ戻るpoll timeoutとして扱う。
    if (result != FALSE && bytes_read == 0U) {
        return {
            ChannelReadStatus::Timeout,
            0U,
            ERROR_SUCCESS,
        };
    }

    if (result != FALSE) {
        // WTSが指定bufferを越えるbyte数を返した場合は破損として停止する。
        if (bytes_read > buffer.size()) {
            return {
                ChannelReadStatus::Error,
                0U,
                ERROR_INVALID_DATA,
            };
        }

        return {
            ChannelReadStatus::Data,
            static_cast<std::size_t>(bytes_read),
            ERROR_SUCCESS,
        };
    }

    const DWORD error = GetLastError();
    // Windows版差で無data時に返る既知値だけをpoll timeoutとして受け入れる。
    if (bytes_read == 0U &&
        (error == ERROR_SUCCESS ||
         error == ERROR_TIMEOUT ||
         error == ERROR_IO_INCOMPLETE)) {
        return {
            ChannelReadStatus::Timeout,
            0U,
            ERROR_SUCCESS,
        };
    }

    return {
        ChannelReadStatus::Error,
        0U,
        error == ERROR_SUCCESS ? ERROR_READ_FAULT : error,
    };
}

ChannelWriteResult WtsVirtualChannel::Write(
    const std::span<const std::byte> data) noexcept {
    // 空またはSVC上限超過dataをWTS APIへ渡さない。
    if (handle_ == nullptr ||
        data.empty() ||
        data.size() >
            protocol::kStaticVirtualChannelChunkBytes) {
        return {
            false,
            ERROR_INVALID_PARAMETER,
        };
    }

    ULONG bytes_written = 0U;
    SetLastError(ERROR_SUCCESS);
    const BOOL result = WTSVirtualChannelWrite(
        handle_,
        reinterpret_cast<PCHAR>(
            const_cast<std::byte*>(data.data())),
        static_cast<ULONG>(data.size()),
        &bytes_written);
    if (result == FALSE) {
        const DWORD error = GetLastError();
        return {
            false,
            error == ERROR_SUCCESS
                ? ERROR_WRITE_FAULT
                : error,
        };
    }

    // partial writeを成功扱いしてframe欠落を隠さない。
    if (bytes_written != data.size()) {
        return {
            false,
            ERROR_WRITE_FAULT,
        };
    }

    return {
        true,
        ERROR_SUCCESS,
    };
}

void WtsVirtualChannel::Close() noexcept {
    // 有効handleを一度だけWTSへ返す。
    if (handle_ != nullptr) {
        static_cast<void>(
            WTSVirtualChannelClose(handle_));
        handle_ = nullptr;
    }
}

bool WtsVirtualChannel::is_open() const noexcept {
    return handle_ != nullptr;
}

}  // namespace zrdp::agent::channel
