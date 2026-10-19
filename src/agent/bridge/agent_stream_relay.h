//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_stream_relay.h
 * @brief Agentのstream protocolをallowlist限定outbound TCPへ中継する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 一つのI/O owner thread上で`StreamMultiplexer`と有限Socket tableを所有する。
 *          allowlist不一致ではconnectorを呼ばず、RDP切断相当の`StopSession`で
 *          Socketと全bufferを同期的に破棄する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "protocol/multiplexer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace zrdp::agent::bridge {

// OPEN完了を待つ最大時間
inline constexpr std::uint64_t kAgentConnectTimeoutMilliseconds = 5000U ;

// 接続済みupstream Socketで双方向とも進捗がない保持上限
inline constexpr std::uint64_t kAgentStreamIdleTimeoutMilliseconds = 30U * 60U * 1000U ;

// production Agentが同時所有できる最大upstream stream数
inline constexpr std::uint32_t kMaximumAgentRelayStreams = 8U ;

// Clientから一Socketへ未送信のまま保持できる最大byte数
inline constexpr std::size_t   kMaximumRelaySocketWriteBytes = 256U * 1024U ;

// 全Socketの未送信dataを合計した上限
inline constexpr std::size_t   kMaximumRelayTotalSocketWriteBytes =
	kMaximumRelaySocketWriteBytes * kMaximumAgentRelayStreams ;

// 一Pumpで全Socketを処理する最大operation数
inline constexpr std::size_t kMaximumRelayOperationsPerPump = 32U ;

/**
 * @brief Agent TCP relay全体のlifecycle。
 */
enum class AgentStreamRelayState {
	/// HELLO未成立またはRDP切断相当。
	Stopped,
	/// OPENまたはactive streamを処理できる。
	Ready,
	/// internal／protocol不変条件違反。session resetが必要。
	Failed,
} ;

/**
 * @brief payloadや実endpointを含まないrelay失敗分類。
 */
enum class AgentStreamRelayError : std::uint32_t {
	/// 異常は検出されておらず、要求の処理を継続できる。
	None				= 0U,
	/// stream数・payload・window等の交渉値が製品上限を満たさない。
	InvalidCapabilities = 1U,
	/// 多重化の状態・frame処理に失敗し、転送を継続できない。
	MultiplexerFailed	= 2U,
	/// 現在のrelay状態で処理できないstream eventを受信した。
	UnexpectedEvent		= 3U,
	/// 有限queueの上限を超え、追加dataを保持できない。
	QueueLimitExceeded	= 4U,
	/// Socketが返したbyte数またはI/O結果が契約を満たさない。
	InvalidSocketResult = 5U,
	/// memory等の有限資源を確保できず、部分状態を公開しない。
	ResourceExhausted	= 6U,
} ;

/**
 * @brief runtime／testへ公開できる非機密relay counter。
 */
struct AgentStreamRelaySnapshot {
	/// relay全体のlifecycle。
	AgentStreamRelayState state{AgentStreamRelayState::Stopped} ;
	/// 最後のfatal分類。
	AgentStreamRelayError error{AgentStreamRelayError::None} ;
	/// 最小active stream ID。
	std::uint32_t		  active_stream_id{0U} ;
	/// 最小streamの匿名allowlist ID。
	std::uint32_t		  endpoint_id{0U} ;
	/// 全Socket送信待ちbyte数。
	std::size_t			  socket_write_bytes{0U} ;
	/// 全Socket受信保留byte数。
	std::size_t			  pending_socket_read_bytes{0U} ;
	/// 一件以上がconnect完了待ち。
	bool				  connecting{false} ;

	/// active／closingを含む所有stream数。
	std::uint32_t		  active_streams{0U} ;

	/// connect完了待ちstream数。
	std::uint32_t		  connecting_streams{0U} ;

	/// current sessionでidle timeoutにより終了したstream数。
	std::uint64_t		  timed_out_streams{0U} ;
} ;

/**
 * @brief 複数streamのallowlist判定、Socket接続、双方向flow controlを所有する。
 *
 * @note 全public methodはAgentのWTS I/O owner threadからだけ呼ぶ。
 * @note `allowlist`と`connector`はこのinstanceより長く生存する必要がある。
 */
class AgentStreamRelay final {
public:
	/**
	 * @brief security boundaryとSocket factoryを非owning参照で固定する。
	 * @param[in] allowlist 検証済み完全一致allowlist。
	 * @param[in] connector allowlist一致後だけ呼ぶoutbound factory。
	 */
	AgentStreamRelay( const network::Allowlist	 &allowlist,
					 network::IUpstreamConnector &connector) noexcept ;

	/**
	 * @brief 交渉済みcapabilityで新しいBridge sessionを開始する。
	 * @param[in] capabilities HELLOで確定したPhase 2上限。
	 * @return relayをReadyへ移せた場合はtrue。
	 *
	 * 旧Socket、stream、credit、bufferは開始前に必ず破棄する。
	 */
	[[nodiscard]] bool StartSession( const protocol::HelloCapabilities &capabilities) noexcept ;

	/**
	 * @brief RDP切断／Agent終了相当で全Socketとbufferを破棄する。
	 */
	void			   StopSession() noexcept ;

	/**
	 * @brief 一つのstream frameをmultiplexerとSocket stateへ適用する。
	 * @param[in] frame decode済み所有frame。
	 * @return sessionを継続できる場合はtrue。
	 */
	[[nodiscard]] bool OnFrame( protocol::Frame frame) noexcept ;

	/**
	 * @brief non-blocking connectとSocket I/Oを有限回だけ進める。
	 * @param[in] now_milliseconds 単調増加時刻。wrapしない64-bit tick。
	 * @return sessionを継続できる場合はtrue。
	 */
	[[nodiscard]] bool Pump( std::uint64_t now_milliseconds) noexcept ;

	/**
	 * @brief WTS writerへ渡す先頭stream frameを取り出す。
	 * @param[out] frame 成功時の所有frame。
	 * @return 一件取得できた場合はtrue。
	 */
	[[nodiscard]] bool TakeOutboundFrame( protocol::Frame &frame) noexcept ;

	/**
	 * @brief payloadなしのrelay状態を返す。
	 * @return 現在snapshot。
	 */
	[[nodiscard]] AgentStreamRelaySnapshot snapshot() const noexcept ;

private:
	/**
	 * @brief Socketへ送信完了するまで所有する一DATA event。
	 */
	struct PendingWrite {
		/// Client由来の所有binary data。
		std::vector<std::byte> data ;
		/// Socketへ送信済みの先頭offset。
		std::size_t			   offset{0U} ;
	} ;

	/**
	 * @brief 一つのupstream stream state。
	 */
	struct ActiveStream {
		/// multiplexerの非0 route key。
		std::uint32_t								  stream_id{0U} ;
		/// logへ出せる匿名allowlist ID。
		std::uint32_t								  endpoint_id{0U} ;
		/// allowlist一致後に生成した所有Socket境界。
		std::unique_ptr<network::IUpstreamConnection> connection ;
		/// Socket送信待ちqueue。
		std::deque<PendingWrite>					  socket_writes ;
		/// 上記queueのbyte合計。
		std::size_t									  socket_write_bytes{0U} ;
		/// credit待ち受信data。
		std::vector<std::byte>						  pending_socket_read ;
		/// non-blocking connect完了待ち。
		bool										  connecting{false} ;
		/// connect期限の基準取得済み。
		bool										  connect_clock_started{false} ;
		/// connect開始tick。
		std::uint64_t								  connect_started_ms{0U} ;
		/// idle期限の基準取得済み。
		bool										  activity_clock_started{false} ;
		/// 最後のI/O進捗tick。
		std::uint64_t								  last_activity_ms{0U} ;
		/// ClientのHALF_CLOSE受理済み。
		bool										  remote_half_closed{false} ;
		/// upstreamへSD_SEND発行済み。
		bool										  send_shutdown{false} ;
		/// upstreamのorderly FIN受理済み。
		bool										  upstream_half_closed{false} ;
		/// 双方向FIN後の正常CLOSE待ち。
		bool										  normal_close_pending{false} ;
		/// CLOSE requestまたはack待ち。
		bool										  closing{false} ;
	} ;

	/**
	 * @brief multiplexerが公開した全eventをSocket処理へ反映する。
	 * @return event列を正常に消費できた場合はtrue。
	 */
	[[nodiscard]] bool DrainEvents() noexcept ;

	/**
	 * @brief Client OPENをpolicy判定後に接続開始または固定拒否へ変換する。
	 * @param[in] event `OpenRequested` event。
	 * @return stream単位の拒否を含め正常処理できた場合はtrue。
	 */
	[[nodiscard]] bool HandleOpenRequested( const protocol::StreamEvent &event) noexcept ;

	/**
	 * @brief Client DATAを有限Socket write queueへ移す。
	 * @param[in] event 所有payloadを持つ`DataReceived` event。
	 * @return queueへ移せた場合はtrue。
	 */
	[[nodiscard]] bool HandleDataReceived( protocol::StreamEvent event) noexcept ;

	/**
	 * @brief pending connectを完了、timeout、拒否へ遷移させる。
	 * @param[in,out] stream 接続待ちの所有state。
	 * @param[in] now_milliseconds 現在の単調時刻。
	 * @return relay sessionを継続できる場合はtrue。
	 */
	[[nodiscard]] bool PumpConnect( ActiveStream &stream, std::uint64_t now_milliseconds) noexcept ;

	/**
	 * @brief Client由来DATAをSocketへ最大有限回書き出す。
	 * @param[in,out] stream 対象Socketと送信queue。
	 * @param[in,out] operations 現Pumpで消費済みのoperation数。
	 * @param[in] now_milliseconds 進捗時刻として記録する単調時刻。
	 * @return relay sessionを継続できる場合はtrue。
	 */
	[[nodiscard]] bool PumpSocketWrites( ActiveStream &stream, std::size_t &operations,
										std::uint64_t now_milliseconds) noexcept ;

	/**
	 * @brief upstream dataを読み、Client向けDATAへ変換する。
	 * @param[in,out] stream 対象Socketと受信待ちbuffer。
	 * @param[in,out] operations 現Pumpで消費済みのoperation数。
	 * @param[in] now_milliseconds 進捗時刻として記録する単調時刻。
	 * @return relay sessionを継続できる場合はtrue。
	 */
	[[nodiscard]] bool PumpSocketReads( ActiveStream &stream, std::size_t &operations,
									   std::uint64_t now_milliseconds) noexcept ;

	/**
	 * @brief Client HALF_CLOSEをqueue消費後の`shutdown(SD_SEND)`へ適用する。
	 * @param[in,out] stream 対象Socketとhalf-close状態。
	 * @param[in,out] operations 現Pumpで消費済みのoperation数。
	 * @return relay sessionを継続できる場合はtrue。
	 */
	[[nodiscard]] bool ApplyPendingHalfClose( ActiveStream &stream,
											 std::size_t  &operations) noexcept ;

	/**
	 * @brief 接続失敗をOPEN_ERRORへ変換してactive候補を破棄する。
	 * @param[in] stream_id 拒否する接続待ちstream。
	 * @param[in] error peerへ返す固定code。
	 * @return responseをqueueできた場合はtrue。
	 */
	[[nodiscard]] bool RejectActiveOpen( std::uint32_t			stream_id,
										protocol::OpenErrorCode error) noexcept ;

	/**
	 * @brief 確立streamをCLOSEへ移しSocketとpayload bufferを破棄する。
	 * @param[in] stream_id 終了対象stream。
	 * @param[in] reason peerへ返す固定reason。
	 * @return CLOSEをqueueできた場合はtrue。
	 */
	[[nodiscard]] bool CloseActiveStream( std::uint32_t				 stream_id,
										 protocol::StreamCloseReason reason) noexcept ;

	/**
	 * @brief internal不変条件違反でSocketを閉じrelayだけをfail-closedにする。
	 * @param[in] error payloadを含まない固定分類。
	 */
	void			   Fail( AgentStreamRelayError error) noexcept ;

	/// 設定全体の検証後にだけ構築される完全一致security boundary。
	const network::Allowlist	&allowlist_ ;

	/// allowlist一致後だけ呼ぶ非owning Socket factory。
	network::IUpstreamConnector &connector_ ;

	/// Agent roleのPhase 2状態機械。
	protocol::StreamMultiplexer	 multiplexer_{protocol::MultiplexerRole::Agent} ;

	/// current negotiated frame上限。
	protocol::HelloCapabilities	 capabilities_{} ;

	/// stream IDごとに所有する有限upstream Socket table。
	std::map<std::uint32_t, std::unique_ptr<ActiveStream>> active_streams_ ;

	/// 全streamのSocket送信待ちbyte合計。
	std::size_t											   total_socket_write_bytes_{0U} ;

	/// 公平な次Pump開始位置として直前に処理したstream ID。
	std::uint32_t										   last_pump_stream_id_{0U} ;

	/// current relay lifecycle。
	AgentStreamRelayState								   state_{AgentStreamRelayState::Stopped} ;

	/// current sessionでidle timeoutにより閉じたstream数。
	std::uint64_t										   timed_out_streams_{0U} ;

	/// payloadなしの最後のfatal分類。
	AgentStreamRelayError								   error_{AgentStreamRelayError::None} ;
} ;

} // namespace zrdp::agent::bridge
