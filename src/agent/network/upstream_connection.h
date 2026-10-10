//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file upstream_connection.h
 * @brief Agentのallowlist検証後に使用するnon-blocking outbound TCP境界を定義する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details production実装はDNS、bind、listen、accept、OS Proxyを使用しない。
 *          testでは同じinterfaceをfake接続へ差し替え、policy拒否時に接続APIが
 *          呼ばれないことを検証できる。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "protocol/multiplexer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace zrdp::agent::network {

/**
 * @brief non-blocking Socket一操作の固定結果。
 */
enum class UpstreamIoStatus {
	/// connect／shutdown完了、または正のbyte数を処理した。
	Completed,
	/// connect完了を後続Pumpで確認する。
	Pending,
	/// send／receiveが現在は進まない。
	WouldBlock,
	/// receiveで順序正しいFINを受信した。
	PeerClosed,
	/// peer reset／abortを受信した。
	PeerReset,
	/// OSがtimeoutを明示した。
	TimedOut,
	/// その他のWinsockまたはlocal resource失敗。
	Failed,
} ;

/**
 * @brief endpoint文字列を含まないSocket操作結果。
 */
struct UpstreamIoResult {
	/// operationの状態。
	UpstreamIoStatus status{UpstreamIoStatus::Failed} ;

	/// send／receiveが今回処理したbyte数。
	std::size_t		 bytes{0U} ;

	/// `Failed`または`PeerReset`時のWinsock error code。
	std::uint32_t	 winsock_error{0U} ;
} ;

/**
 * @brief 一つのoutbound TCP Socketを所有する抽象接続。
 *
 * @note 全methodはAgentの同一I/O owner threadから呼ぶ。
 */
class IUpstreamConnection {
public:
	/**
	 * @brief 派生instanceの破棄をvirtual境界で完了する。
	 */
	virtual ~IUpstreamConnection() = default ;

	/**
	 * @brief pending connectの完了または失敗をpollする。
	 * @return `Completed`、`Pending`、`PeerReset`、`Failed`のいずれか。
	 */
	[[nodiscard]] virtual UpstreamIoResult PollConnect() noexcept = 0 ;

	/**
	 * @brief caller所有bufferの未送信部分をnon-blockingで送る。
	 * @param[in] data 空でないbinary data。
	 * @return 正のbyte数を持つ`Completed`、`WouldBlock`、または失敗分類。
	 */
	[[nodiscard]] virtual UpstreamIoResult Send( std::span<const std::byte> data) noexcept = 0 ;

	/**
	 * @brief caller所有bufferへnon-blockingで受信する。
	 * @param[out] buffer 空でない書込み先。
	 * @return 正のbyte数を持つ`Completed`、`WouldBlock`、FIN、または失敗分類。
	 */
	[[nodiscard]] virtual UpstreamIoResult Receive( std::span<std::byte> buffer) noexcept = 0 ;

	/**
	 * @brief Socketの送信方向だけをhalf-closeする。
	 * @return 完了または分類済み失敗。
	 */
	[[nodiscard]] virtual UpstreamIoResult ShutdownSend() noexcept = 0 ;

	/**
	 * @brief 所有Socketをidempotentに閉じる。
	 */
	virtual void						   Close() noexcept = 0 ;
} ;

/**
 * @brief allowlist一致後の接続開始だけを公開するfactory境界。
 */
class IUpstreamConnector {
public:
	/**
	 * @brief 派生instanceの破棄をvirtual境界で完了する。
	 */
	virtual ~IUpstreamConnector() = default ;

	/**
	 * @brief IPv4 literalへnon-blocking connectを開始する。
	 * @param[in] target allowlist完全一致済みの接続先。
	 * @param[out] connection 成功またはpending時の所有接続。
	 * @return `Completed`、`Pending`、`PeerReset`、`Failed`のいずれか。
	 *
	 * @warning このmethodをallowlist判定前に呼び出してはならない。
	 */
	[[nodiscard]] virtual UpstreamIoResult
	BeginConnect( const protocol::OpenTarget			  &target,
				 std::unique_ptr<IUpstreamConnection> &connection) noexcept = 0 ;
} ;

/**
 * @brief process内Winsock初期化をRAIIで管理する。
 */
class WinsockRuntime final {
public:
	/**
	 * @brief 初期状態の管理instanceを構築し、所有資源の解放責任を固定する。
	 */
	WinsockRuntime() noexcept = default ;
	/**
	 * @brief 所有資源を終了経路で解放し、後続の参照を残さない。
	 */
	~WinsockRuntime() ;

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	WinsockRuntime( const WinsockRuntime &)					 = delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	WinsockRuntime &operator=( const WinsockRuntime &)		 = delete ;
	/**
	 * @brief 稼働中の所有資源とlifecycleの移動を禁止する。
	 */
	WinsockRuntime( WinsockRuntime &&)						 = delete ;
	/**
	 * @brief 稼働中の所有資源とlifecycleの移動を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	WinsockRuntime			   &operator=( WinsockRuntime &&) = delete ;

	/**
	 * @brief Winsock 2.2を一度だけ初期化する。
	 * @return 成功時は0、失敗時はWinsock error code。
	 */
	[[nodiscard]] std::uint32_t Start() noexcept ;

	/**
	 * @brief 対応する`WSACleanup`を一度だけ実行する。
	 */
	void						Stop() noexcept ;

	/**
	 * @brief connectorを使用可能な初期化状態か返す。
	 * @return `Start`成功後ならtrue。
	 */
	[[nodiscard]] bool			started() const noexcept ;

private:
	/// `WSAStartup`成功後だけtrueとなるcleanup ownership。
	bool started_{false} ;
} ;

/**
 * @brief WinsockによるIPv4 literal専用outbound connector。
 *
 * @pre 同じprocessで`WinsockRuntime::Start`が成功していること。
 */
class WinsockUpstreamConnector final : public IUpstreamConnector {
public:
	/**
	 * @brief 許可済みIPv4 literalへ一つのnon-blocking Socket接続を開始する。
	 * @param[in] target allowlist完全一致を確認済みのIPv4とport。
	 * @param[out] connection 成功・pending時だけ移転するSocket所有権。失敗時はnull。
	 * @return 即時完了、pending、または分類済みWinsock/確保失敗。
	 * @note Agent I/O owner threadでWinsock初期化後に呼ぶ。allowlist判定や別経路fallbackは行わない。
	 */
	[[nodiscard]] UpstreamIoResult
	BeginConnect( const protocol::OpenTarget			  &target,
				 std::unique_ptr<IUpstreamConnection> &connection) noexcept override ;
} ;

} // namespace zrdp::agent::network
