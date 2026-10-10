//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file multiplexer.h
 * @brief ESVNBRG Phase 2の複数stream状態機械とflow controlを定義する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details Socket、COM、WTS APIを呼ばず、所有Frameと所有eventだけを扱う。
 *          一つのowner threadから呼び、Client workerとAgent I/O workerの
 *          どちらにも配置できるtransport非依存境界とする。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "protocol/frame.h"
#include "protocol/hello.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <vector>

namespace zrdp::protocol {

// connection全体で同時にpeerへ付与する受信credit上限
inline constexpr std::uint64_t kMaximumConnectionWindowBytes = 4ULL * 1024ULL * 1024ULL ;

// multiplexerが同時保持できる送信Frame数
inline constexpr std::size_t   kMaximumMultiplexerOutboundFrames = 4096U ;

// multiplexer送信queueが所有できるheader込みbyte数
inline constexpr std::size_t   kMaximumMultiplexerOutboundBytes = 4U * 1024U * 1024U ;

// callerが未取得の受信event数上限
inline constexpr std::size_t   kMaximumMultiplexerEvents = 4096U ;

// callerが未取得の受信DATA payload総量上限
inline constexpr std::size_t   kMaximumMultiplexerInboundBytes = 4U * 1024U * 1024U ;

/**
 * @brief stream開始権限と受信message方向を決めるendpoint role。
 */
enum class MultiplexerRole {
	/// 非0 stream IDを割り当ててOPENを開始する。
	Client,
	/// OPENを受信し、callerのallowlist判定結果を応答する。
	Agent,
} ;

/**
 * @brief 一つのBridge session内のmultiplexer lifecycle。
 */
enum class MultiplexerState {
	/// capability未設定またはRDP切断相当。
	Stopped,
	/// stream messageを処理できる。
	Ready,
	/// peer由来の違反後にResetを必要とする。
	Failed,
} ;

/**
 * @brief local APIの受付結果。
 *
 * `WouldBlock`はcreditまたはbounded queueが回復すれば再試行できる非fatal結果である。
 */
enum class MultiplexerResult {
	/// 要求した操作を完了し、出力状態を確定した。
	Ok,
	/// 直ちに進められず、creditまたはSocketの準備を待つ。
	WouldBlock,
	/// 操作引数が事前条件または値域を満たさない。
	InvalidArgument,
	/// Client／Agentの役割で許可していない操作を要求した。
	InvalidRole,
	/// 存在しないstreamIdの操作またはframeを受信した。
	UnknownStream,
	/// 現在のlifecycleでは要求操作を実行できない。
	InvalidState,
	/// 同時stream数の交渉済み上限へ到達している。
	StreamLimitExceeded,
	/// streamIdが上限へ到達し、旧IDを再利用しない。
	StreamIdExhausted,
	/// sequenceが上限へ到達し、wrapして転送を継続しない。
	SequenceExhausted,
	/// 継続不能な異常を検出し、追加の処理を拒否する。
	Failed,
} ;

/**
 * @brief payloadを含めず診断可能なfatal protocol分類。
 */
enum class MultiplexerError : std::uint32_t {
	/// 異常は検出されておらず、要求の処理を継続できる。
	None				   = 0U,
	/// stream数・payload・window等の交渉値が製品上限を満たさない。
	InvalidCapabilities	   = 1U,
	/// 現在の状態または役割で許可していないmessageを受信した。
	UnexpectedMessage	   = 2U,
	/// message typeに対応するpayload構文または長さが不正である。
	InvalidPayload		   = 3U,
	/// 既存streamIdと衝突するOPENを受信した。
	DuplicateStreamId	   = 4U,
	/// 存在しないstreamIdの操作またはframeを受信した。
	UnknownStream		   = 5U,
	/// 現在のstream状態で成立しない遷移を要求している。
	InvalidStateTransition = 6U,
	/// 受信sequenceが期待値と一致せず、順序を保証できない。
	SequenceMismatch	   = 7U,
	/// peerが許可された受信creditを超えてdataを送信した。
	FlowControlViolation   = 8U,
	/// 同時stream数の交渉済み上限へ到達している。
	StreamLimitExceeded	   = 9U,
	/// 有限queueの上限を超え、追加dataを保持できない。
	QueueLimitExceeded	   = 10U,
	/// streamIdが上限へ到達し、旧IDを再利用しない。
	StreamIdExhausted	   = 11U,
	/// peerがprotocol errorによる継続不能を通知した。
	PeerProtocolError	   = 12U,
	/// memory等の有限資源を確保できず、部分状態を公開しない。
	ResourceExhausted	   = 13U,
} ;

/**
 * @brief AgentがOPENへ返す固定wire error code。
 */
enum class OpenErrorCode : std::uint32_t {
	/// allowlist完全一致の許可条件を満たさず、Socketを作成しない。
	RejectedByPolicy  = 1U,
	/// 許可済みendpointへのTCP接続を完了できなかった。
	ConnectFailed	  = 2U,
	/// 所定の期限までに完了しなかった。
	Timeout			  = 3U,
	/// memory等の有限資源を確保できず、部分状態を公開しない。
	ResourceExhausted = 4U,
} ;

/**
 * @brief 確立済みstreamを終了する固定wire reason。
 */
enum class StreamCloseReason : std::uint32_t {
	/// 対象streamの正常な終了と資源回収を通知する。
	Normal			= 0U,
	/// 所定の期限までに完了しなかった。
	Timeout			= 1U,
	/// peerが接続を強制終了し、streamを回収する必要がある。
	PeerReset		= 2U,
	/// local I/Oの異常により対象streamを終了する。
	LocalError		= 3U,
	/// Bridge sessionの終了に伴い、対象streamを回収する。
	SessionStopping = 4U,
} ;

/**
 * @brief callerへ公開するstream lifecycle。
 */
enum class StreamState {
	/// ClientがOPEN_OK／OPEN_ERRORを待つ。
	Opening,
	/// Agent callerがOPENを検証中。
	AwaitingAccept,
	/// 双方向DATAを送受信できる。
	Open,
	/// localからのDATA送信だけを終了した。
	LocalHalfClosed,
	/// peerからのDATA受信だけが終了した。
	RemoteHalfClosed,
	/// 両方向のHALF_CLOSEを確認した。
	BothHalfClosed,
	/// local CLOSE requestのack待ち。
	CloseSent,
	/// 双方がCLOSE requestを送り、ack待ち。
	CloseCrossed,
} ;

/**
 * @brief `OPEN` payloadで運ぶIPv4 literalとTCP port。
 *
 * Phase 3のAgentはこの値を不可信入力として完全一致allowlistへ渡す。
 * host name、CIDR、port rangeは表現しない。
 */
struct OpenTarget {
	/// network byte orderと同じ並びのIPv4 octet。
	std::array<std::uint8_t, 4> ipv4{} ;

	/// host byte orderの非0 TCP port。
	std::uint16_t				port{0U} ;

	/**
	 * @brief IPv4 octetとportの完全一致を比較する。
	 * @param[in] other 比較対象endpoint。
	 * @return 全fieldが一致する場合はtrue。
	 */
	[[nodiscard]] bool			operator==( const OpenTarget &other) const noexcept = default ;
} ;

/**
 * @brief owner workerへ渡す所有event種別。
 */
enum class StreamEventType {
	/// Agentがallowlist／connect判定を開始する。
	OpenRequested,
	/// ClientがOPEN_OKを受信した。
	Opened,
	/// ClientがOPEN_ERRORを受信した。
	OpenFailed,
	/// callerがdownstreamへ書き出すbinary data。
	DataReceived,
	/// peerがDATA送信方向を終了した。
	RemoteHalfClosed,
	/// CLOSE requestまたはackでstreamが終了した。
	Closed,
} ;

/**
 * @brief multiplexerからowner workerへ移す非文字列event。
 */
struct StreamEvent {
	/// eventの意味。
	StreamEventType		   type{StreamEventType::Opened} ;

	/// event対象の非0 stream ID。
	std::uint32_t		   stream_id{0U} ;

	/// `OpenRequested`だけが使用する接続候補。
	OpenTarget			   target{} ;

	/// `OpenFailed`だけが使用する固定code。
	OpenErrorCode		   open_error{OpenErrorCode::RejectedByPolicy} ;

	/// `Closed`だけが使用する固定reason。
	StreamCloseReason	   close_reason{StreamCloseReason::Normal} ;

	/// `DataReceived`だけが所有する任意binary payload。
	std::vector<std::byte> data ;
} ;

/**
 * @brief UI／testへ公開できる有限counter snapshot。
 */
struct MultiplexerSnapshot {
	/// session lifecycle。
	MultiplexerState state{MultiplexerState::Stopped} ;
	/// 最後のfatal分類。
	MultiplexerError error{MultiplexerError::None} ;
	/// 現在の非terminal stream数。
	std::uint32_t	 active_streams{0U} ;
	/// transport待ちFrame件数。
	std::size_t		 outbound_frames{0U} ;
	/// transport待ちpayload byte数。
	std::size_t		 outbound_bytes{0U} ;
	/// owner未取得event件数。
	std::size_t		 pending_events{0U} ;
	/// owner未取得DATA byte数。
	std::size_t		 inbound_bytes{0U} ;
	/// 全stream共通送信credit。
	std::uint64_t	 connection_send_window{0U} ;
	/// 全stream共通受信credit。
	std::uint64_t	 connection_receive_window{0U} ;
} ;

/**
 * @brief 複数streamと双方向creditを一つのBridge sessionへ多重化する。
 *
 * @note 全public methodは一つのowner threadから呼ぶ。class内部ではblocking I/O、
 *       callback、log出力を行わない。
 */
class StreamMultiplexer final {
public:
	/**
	 * @brief endpoint roleを固定して空のmultiplexerを作成する。
	 * @param[in] role Client-only OPEN規則を決めるrole。
	 */
	explicit StreamMultiplexer( MultiplexerRole role) noexcept ;

	/**
	 * @brief 交渉済みHELLO上限で新しいmultiplexer sessionを開始する。
	 * @param[in] capabilities 両端で確定済みのPhase 2 capability。
	 * @return stream featureと有限上限が有効なら`Ok`。
	 *
	 * 旧stream ID、sequence、credit、queue、eventは必ず破棄する。
	 */
	[[nodiscard]] MultiplexerResult	  StartSession( const HelloCapabilities &capabilities) noexcept ;

	/**
	 * @brief RDP切断相当で全streamと所有bufferを破棄する。
	 */
	void							  StopSession() noexcept ;

	/**
	 * @brief Clientから新しいstreamのOPENをqueueする。
	 * @param[in] target AgentがPhase 3でallowlist判定するIPv4とport。
	 * @param[out] stream_id 成功時にsession内で再利用しない非0 ID。
	 * @return queue受付結果。Agent roleでは`InvalidRole`。
	 */
	[[nodiscard]] MultiplexerResult	  OpenStream( const OpenTarget &target,
												 std::uint32_t	  &stream_id) noexcept ;

	/**
	 * @brief Agentが保留中OPENを受理してOPEN_OKをqueueする。
	 * @param[in] stream_id `OpenRequested`で通知されたID。
	 * @return 状態とqueueの受付結果。
	 */
	[[nodiscard]] MultiplexerResult	  AcceptStream( std::uint32_t stream_id) noexcept ;

	/**
	 * @brief Agentが保留中OPENを固定codeで拒否する。
	 * @param[in] stream_id `OpenRequested`で通知されたID。
	 * @param[in] error endpointやOS文字列を含まない固定code。
	 * @return 状態とqueueの受付結果。
	 */
	[[nodiscard]] MultiplexerResult	  RejectStream( std::uint32_t stream_id,
												   OpenErrorCode error) noexcept ;

	/**
	 * @brief 一streamへ最大一frame分のDATAをqueueする。
	 * @param[in] stream_id 確立済みstream。
	 * @param[in] data 非空かつ交渉済みframe上限以下のbinary data。
	 * @return credit／queue不足は`WouldBlock`、不正状態は分類結果。
	 *
	 * 成功時だけstreamとconnectionの送信creditを減算する。
	 */
	[[nodiscard]] MultiplexerResult	  SendData( std::uint32_t			  stream_id,
											   std::span<const std::byte> data) noexcept ;

	/**
	 * @brief callerがdownstreamへ処理済みの受信byteへcreditを返す。
	 * @param[in] stream_id `DataReceived`の対象stream。
	 * @param[in] bytes 実際に消費済みの非0 byte数。
	 * @return stream／connection両方のWINDOW_UPDATEをqueueした結果。
	 */
	[[nodiscard]] MultiplexerResult	  ConsumeReceivedData( std::uint32_t stream_id,
														  std::uint32_t bytes) noexcept ;

	/**
	 * @brief localのDATA送信方向をHALF_CLOSEする。
	 * @param[in] stream_id 確立済みstream。
	 * @return 状態とqueueの受付結果。
	 */
	[[nodiscard]] MultiplexerResult	  HalfCloseStream( std::uint32_t stream_id) noexcept ;

	/**
	 * @brief stream全体のCLOSE requestを固定reasonでqueueする。
	 * @param[in] stream_id 終了対象stream。
	 * @param[in] reason normal、timeout、peer reset等の固定reason。
	 * @return 状態とqueueの受付結果。
	 */
	[[nodiscard]] MultiplexerResult	  CloseStream( std::uint32_t		stream_id,
												  StreamCloseReason reason) noexcept ;

	/**
	 * @brief peerから受信した一つのdecode済みFrameを状態機械へ適用する。
	 * @param[in] frame stream payloadを所有したFrame。DATA eventへmoveできる。
	 * @return session継続時は`Ok`、違反時は`Failed`。
	 */
	[[nodiscard]] MultiplexerResult	  OnFrame( Frame frame) noexcept ;

	/**
	 * @brief transportへ渡す先頭Frameを所有moveで取り出す。
	 * @param[out] frame 成功時の送信Frame。
	 * @return 一件取得できた場合はtrue。Failed状態でもPROTOCOL_ERRORを取得できる。
	 */
	[[nodiscard]] bool				  TakeOutboundFrame( Frame &frame) noexcept ;

	/**
	 * @brief owner workerへ渡す先頭eventを所有moveで取り出す。
	 * @param[out] event 成功時のevent。
	 * @return 一件取得できた場合はtrue。
	 *
	 * DATA event取得だけではcreditを返さず、実消費後に
	 * `ConsumeReceivedData`を呼ぶ。
	 */
	[[nodiscard]] bool				  TakeEvent( StreamEvent &event) noexcept ;

	/**
	 * @brief 指定streamの現在stateを取得する。
	 * @param[in] stream_id 検索対象。
	 * @param[out] state 存在時のstate。
	 * @return active streamが存在する場合はtrue。
	 */
	[[nodiscard]] bool				  TryGetStreamState( std::uint32_t stream_id,
														StreamState	 &state) const noexcept ;

	/**
	 * @brief payloadを含まない現在counterを取得する。
	 * @return owner thread上のsnapshot。
	 */
	[[nodiscard]] MultiplexerSnapshot snapshot() const noexcept ;

private:
	/**
	 * @brief 一streamの方向別sequence、credit、未確認byteを所有する。
	 */
	struct StreamRecord {
		/// stream lifecycle。
		StreamState	  state{StreamState::Opening} ;
		/// OPEN時に固定したallowlist判定対象。
		OpenTarget	  target{} ;
		/// 次に送るDATA sequence。
		std::uint32_t next_send_sequence{1U} ;
		/// 次に受理するDATA sequence。
		std::uint32_t next_receive_sequence{1U} ;
		/// peerから得たstream送信credit。
		std::uint32_t send_window{0U} ;
		/// localが残すstream受信credit。
		std::uint32_t receive_window{0U} ;
		/// peer未返却の送信byte数。
		std::uint32_t sent_unacknowledged{0U} ;
		/// owner未消費の受信byte数。
		std::uint32_t received_unconsumed{0U} ;
	} ;

	/**
	 * @brief Agent側でOPENを検証しOpenRequested eventへ変換する。
	 * @param[in] frame 所有OPEN frame。
	 * @return 状態遷移結果。
	 */
	[[nodiscard]] MultiplexerResult HandleOpen( Frame frame) noexcept ;

	/**
	 * @brief Client側でOPEN_OKを確立済みstateへ変換する。
	 * @param[in] frame 所有OPEN_OK frame。
	 * @return 状態遷移結果。
	 */
	[[nodiscard]] MultiplexerResult HandleOpenOk( Frame frame) noexcept ;

	/**
	 * @brief Client側でOPEN_ERRORを固定error eventへ変換する。
	 * @param[in] frame 所有OPEN_ERROR frame。
	 * @return 状態遷移結果。
	 */
	[[nodiscard]] MultiplexerResult HandleOpenError( Frame frame) noexcept ;

	/**
	 * @brief 方向別sequenceと受信creditを検証してDATA eventをqueueする。
	 * @param[in] frame 所有DATA frame。
	 * @return 状態遷移結果。
	 */
	[[nodiscard]] MultiplexerResult HandleData( Frame frame) noexcept ;

	/**
	 * @brief streamまたはconnectionの送信creditを検証回復する。
	 * @param[in] frame 所有WINDOW_UPDATE frame。
	 * @return credit更新結果。
	 */
	[[nodiscard]] MultiplexerResult HandleWindowUpdate( Frame frame) noexcept ;

	/**
	 * @brief peer送信方向を一度だけHALF_CLOSEへ遷移させる。
	 * @param[in] frame 所有HALF_CLOSE frame。
	 * @return 状態遷移結果。
	 */
	[[nodiscard]] MultiplexerResult HandleHalfClose( Frame frame) noexcept ;

	/**
	 * @brief CLOSE request／ackとcrossed closeをterminal stateへ遷移させる。
	 * @param[in] frame 所有CLOSE frame。
	 * @return 状態遷移結果。
	 */
	[[nodiscard]] MultiplexerResult HandleClose( Frame frame) noexcept ;

	/**
	 * @brief peerの固定PROTOCOL_ERRORを応答loopなしでfail-closedへ反映する。
	 * @param[in] frame 所有PROTOCOL_ERROR frame。
	 * @return 常に`Failed`。
	 */
	[[nodiscard]] MultiplexerResult HandleProtocolError( Frame frame) noexcept ;

	/**
	 * @brief 一Frameをbounded outbound queueへ登録する。
	 * @param[in] frame 所有Frame。
	 * @return queue受付結果。
	 */
	[[nodiscard]] MultiplexerResult QueueOutboundFrame( Frame frame) noexcept ;

	/**
	 * @brief 複数Frameをwire順の一transactionとしてqueueする。
	 * @param[in] frames 所有Frame列。
	 * @return 全件受付または非fatal backpressure。
	 */
	[[nodiscard]] MultiplexerResult QueueOutboundFrames( std::vector<Frame> frames) noexcept ;

	/**
	 * @brief owner worker向けeventを件数／byte上限内でqueueする。
	 * @param[in] event 所有event。
	 * @return queueできた場合はtrue。peer起因の超過はsessionを停止する。
	 */
	[[nodiscard]] bool				QueueEvent( StreamEvent event) noexcept ;

	/**
	 * @brief terminal close対象の未取得eventと受信creditを破棄する。
	 * @param[in] stream_id 対象stream。
	 * @param[in,out] stream 受信counterを初期化するrecord。
	 */
	void DiscardPendingEventsForStream( std::uint32_t stream_id, StreamRecord &stream) noexcept ;

	/**
	 * @brief 終了streamが占有するconnection creditを他streamへ戻す。
	 * @param[in,out] stream 未確認counterを初期化するrecord。
	 */
	void ReleaseConnectionCredits( StreamRecord &stream) noexcept ;

	/**
	 * @brief 全streamをfail-closedで破棄し、必要なら固定errorを一件queueする。
	 * @param[in] error payloadを含まないfatal分類。
	 * @param[in] notify_peer peerへPROTOCOL_ERRORを送る場合はtrue。
	 */
	void Fail( MultiplexerError error, bool notify_peer) noexcept ;

	/// constructorで固定し、AgentからのOPEN開始を禁止するrole。
	MultiplexerRole						  role_ ;

	/// current sessionのlifecycle。
	MultiplexerState					  state_{MultiplexerState::Stopped} ;

	/// UI／診断へ公開可能な最後のfatal分類。
	MultiplexerError					  error_{MultiplexerError::None} ;

	/// HELLOで確定済みの有限上限。
	HelloCapabilities					  capabilities_{} ;

	/// session内のactive stream。keyは非0で再利用しない。
	std::map<std::uint32_t, StreamRecord> streams_ ;

	/// Clientが次回OPENへ割り当てる単調増加ID。
	std::uint32_t						  next_local_stream_id_{1U} ;

	/// Agentが確認済みの最大Client stream ID。
	std::uint32_t						  last_remote_stream_id_{0U} ;

	/// 全streamで共有するconnection creditの初期値。
	std::uint64_t						  initial_connection_window_{0U} ;

	/// peer WINDOW_UPDATEで回復する送信可能byte数。
	std::uint64_t						  connection_send_window_{0U} ;

	/// local消費で回復する受信可能byte数。
	std::uint64_t						  connection_receive_window_{0U} ;

	/// peerからconnection creditが未返却の送信byte数。
	std::uint64_t						  connection_sent_unacknowledged_{0U} ;

	/// callerがdownstreamへ未消費の受信byte数。
	std::uint64_t						  connection_received_unconsumed_{0U} ;

	/// transportへwire順で渡すbounded所有Frame列。
	std::deque<Frame>					  outbound_frames_ ;

	/// `outbound_frames_`が保持するpayload byte合計。
	std::size_t							  outbound_queued_bytes_{0U} ;

	/// owner workerへ渡すbounded所有event列。
	std::deque<StreamEvent>				  events_ ;

	/// `events_`内のDATA payload byte合計。
	std::size_t							  inbound_queued_bytes_{0U} ;
} ;

} // namespace zrdp::protocol
