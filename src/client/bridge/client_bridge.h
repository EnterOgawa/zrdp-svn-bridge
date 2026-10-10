//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_bridge.h
 * @brief Client側handshake、Echo、stream routing、bounded SVC queueを定義する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details protocol stateはCOM STAのUI threadで変更する。送信byte列だけは
 *          future workerからもqueue可能なmutex境界を持ち、COM callbackや
 *          mutex保持中にActiveX methodを呼ばない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  限定設定controlとACK前の中継制限を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "protocol/frame.h"
#include "protocol/hello.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

namespace zrdp::client::bridge {

// production ClientがHELLOで提示する同時HTTP stream上限
inline constexpr std::uint32_t kMaximumClientBridgeStreams = 8U ;

/**
 * @brief Client UIへ公開するBridge状態。
 */
enum class ClientBridgeState {
	/// RDP未接続または切断済み。
	Stopped,
	/// HELLOを送信しAgentのHELLO_ACK待ち。
	WaitingForAgent,
	/// compatible Agentとhandshake済み。
	Ready,
	/// 段階的binary Echo検証中。
	EchoRunning,
	/// 全対象sizeのEchoが完全一致した。
	EchoPassed,
	/// protocolまたはtransport異常で停止した。
	Failed,
	/// protocol versionまたはcapabilityが非互換。
	Incompatible,
} ;

/**
 * @brief payloadを含めずUI／診断へ渡せるBridge失敗分類。
 */
enum class ClientBridgeError {
	/// 異常は検出されておらず、要求の処理を継続できる。
	None,
	/// 送信frameを確保またはencodeできず、部分dataを送信しない。
	EncodeFailed,
	/// 受信frameを検証・再構成できずBridgeを停止する。
	DecodeFailed,
	/// 有限queueの上限を超え、追加dataを保持できない。
	QueueLimitExceeded,
	/// HELLOの長さ・version・capabilityが交渉条件を満たさない。
	InvalidHello,
	/// 現在の状態または役割で許可していないmessageを受信した。
	UnexpectedMessage,
	/// 受信sequenceが期待値と一致せず、順序を保証できない。
	SequenceMismatch,
	/// Echoで受信したbinary dataが送信dataと一致しない。
	PayloadMismatch,
	/// SVCへframeを完全送信できなかった。
	TransportSendFailed,
	/// peerの応答期限を超え、旧streamを維持できない。
	ResponseTimeout,
	/// stream／Socket中継の異常を検出し、Bridgeを停止する。
	StreamRelayFailed,
} ;

/**
 * @brief Client UIが一時copyとして表示できる非機密Bridge状態。
 */
struct ClientBridgeSnapshot {
	/// 現在のBridge lifecycle。
	ClientBridgeState state{ClientBridgeState::Stopped} ;

	/// payloadを含まない最後の失敗分類。
	ClientBridgeError error{ClientBridgeError::None} ;

	/// Echo testで完全一致を確認済みの累積byte数。
	std::uint64_t	  echo_completed_bytes{0U} ;

	/// 一回のfull Echo testが確認する総byte数。
	std::uint64_t	  echo_total_bytes{0U} ;

	/// HELLOをbounded queueへ登録した回数。
	std::uint32_t	  hello_attempt_count{0U} ;

	/// Agent死活確認用PINGの応答待ち中ならtrue。
	bool			  health_check_outstanding{false} ;
	/// 設定応答。0は未要求／待機、1は利用可、2は再起動、3は拒否。
	std::uint8_t	  configuration_status{0U} ;
} ;

/**
 * @brief Client connection-control stateとtransport chunk queueを所有する。
 *
 * ActiveX lifecycleを直接参照せず、MainWindowがRDP eventとtransport結果を
 * methodへ変換する。これによりprotocol testをRDP接続なしで実行できる。
 */
class ClientBridge final {
public:
	/**
	 * @brief Phase 5 capabilityを提示する未接続Client Bridgeを作成する。
	 *
	 * HTTP/SVN clientの並行要求を扱いつつ資源を有限にするため、同時stream上限は
	 * production ProxyおよびAgentと同じ8とする。
	 */
	ClientBridge() noexcept ;

	/**
	 * @brief RDP login完了後の新しいBridge sessionを開始する。
	 * @return 初回HELLOをbounded queueへ登録できた場合はtrue。
	 *
	 * 旧decoder、sequence、Echo状態、送信queueを再利用しない。
	 */
	[[nodiscard]] bool StartSession() noexcept ;

	/**
	 * @brief RDP切断時に全protocol stateとqueued dataを破棄する。
	 */
	void			   StopSession() noexcept ;

	/**
	 * @brief Agent待機中にHELLOを再送予約する。
	 * @return HELLOをqueueできた場合はtrue。handshake成立後はfalse。
	 *
	 * HELLOはAgent側stream sessionをresetするため、activeになり得るhandshake
	 * 成立後には周期送信しない。
	 */
	[[nodiscard]] bool QueueHelloRetry() noexcept ;

	/**
	 * @brief 1 byteから10 MiBまでの段階的binary Echoを開始する。
	 * @return ready状態から最初のPINGをqueueできた場合はtrue。
	 */
	[[nodiscard]] bool BeginEchoTest() noexcept ;

	/**
	 * @brief 成立済みBridgeをresetしない死活確認PINGを一件予約する。
	 * @return 応答待ちのないReady／EchoPassed状態でqueueできた場合はtrue。
	 *
	 * payloadは固定binaryであり、endpointやHTTP情報を含めない。手動Echoとは
	 * 同時実行せず、一度に一件だけを未応答として保持する。
	 */
	[[nodiscard]] bool QueueHealthCheck() noexcept ;

	/** @brief 同じHELLO世代へ承認済みINIを一件だけqueueする。
	 * @param[in] ini 最大64 KiBの検証済みINI。内容を記録しない。
	 * @return Readyと共通lifecycle featureがあり、一回だけqueueできた場合true。
	 */
	[[nodiscard]] bool QueueAgentConfiguration( std::string_view ini) noexcept ;
	/** @brief 対象Agentの協調停止をqueueし、新規streamを拒否する。
	 * @return 停止frameを完全queueできた場合true。強制終了や再送はしない。
	 */
	[[nodiscard]] bool QueueAgentStop() noexcept ;

	/**
	 * @brief ESVNBRG callbackの所有copyをincremental decoderへ渡す。
	 * @param[in] data callback終了後も有効なbinary fragment。
	 * @return dataを受理し、sessionを継続できる場合はtrue。
	 *
	 * PONG処理で次のPINGがqueueされる場合がある。呼出し側は復帰後に
	 * Window Messageをpostし、COM callback外で送信queueをdrainする。
	 */
	[[nodiscard]] bool OnChannelData( std::span<const std::byte> data) noexcept ;

	/**
	 * @brief ActiveX送信失敗をBridge停止へ反映する。
	 *
	 * RDP session自体の切断は呼出し側へ要求しない。
	 */
	void			   OnTransportSendFailed() noexcept ;

	/**
	 * @brief 未応答PINGの待機期限超過をBridge停止へ反映する。
	 *
	 * Echo実行中以外に到着したstale timer通知は状態を変更しない。
	 * RDP session自体の切断は呼出し側へ要求しない。
	 */
	void			   OnEchoTimeout() noexcept ;

	/**
	 * @brief 未応答health PINGの期限超過後に新しいhandshakeを開始する。
	 * @return 期限対象を確認し、HELLO待機またはfail-closedへ遷移した場合はtrue。
	 *
	 * 旧stream、decoder、queueは破棄し、RDP画面接続自体は切断しない。
	 */
	[[nodiscard]] bool OnHealthCheckTimeout() noexcept ;

	/**
	 * @brief COM UI threadへ渡す先頭transport chunkをqueueから取り出す。
	 * @param[out] chunk 成功時に最大1600 byteを所有する送信data。
	 * @param[out] more_data 取出し後にもqueueが残る場合はtrue。
	 * @return 一件を取得できた場合はtrue。
	 *
	 * mutex解放後に呼出し側が`SendOnVirtualChannel`を実行する。
	 */
	[[nodiscard]] bool TakeOutboundChunk( std::vector<std::byte> &chunk, bool &more_data) noexcept ;

	/**
	 * @brief workerへ渡す先頭stream Frameを所有moveで取り出す。
	 * @param[out] frame Agentから受信したdecode済みFrame。
	 * @return 一件取得できた場合はtrue。
	 */
	[[nodiscard]] bool TakeInboundStreamFrame( protocol::Frame &frame) noexcept ;

	/**
	 * @brief workerが生成したstream FrameをSVC送信queueへ登録する。
	 * @param[in] frame Client multiplexerの検証済みFrame。
	 * @return UI thread上でencode／queueできた場合はtrue。
	 */
	[[nodiscard]] bool QueueStreamFrame( const protocol::Frame &frame) noexcept ;

	/**
	 * @brief worker queue／relay失敗をBridge停止へ反映する。
	 *
	 * RDP画面接続は呼出し側で維持する。
	 */
	void			   OnStreamRelayFailed() noexcept ;

	/**
	 * @brief HELLOで確定したcapability copyを返す。
	 * @return handshake前はEcho-only既定値、成立後は交渉値。
	 */
	[[nodiscard]] protocol::HelloCapabilities negotiated_capabilities() const noexcept ;

	/**
	 * @brief UI表示用の非機密snapshotを取得する。
	 * @return UI thread上の現在stateとcounterの値copy。
	 */
	[[nodiscard]] ClientBridgeSnapshot		  snapshot() const noexcept ;

private:
	/** @brief 一件の設定要求と固定一byteのACKを照合する。
	 * @param[in] frame 同じHELLO境界のACK。
	 * @return 未交渉・重複・不正応答を拒否して受理できた場合true。
	 */
	[[nodiscard]] bool			HandleConfigurationAck( const protocol::Frame &frame) noexcept ;
	/**
	 * @brief HELLOまたはHELLO_ACK capability frameを送信queueへ登録する。
	 * @param[in] type `Hello`または`HelloAck`。
	 * @return encodeとqueueの両方が成功した場合はtrue。
	 */
	[[nodiscard]] bool			QueueHelloFrame( protocol::MessageType type) noexcept ;

	/**
	 * @brief 検証済みframeをencodeし、SVC chunkへ分割してqueueする。
	 * @param[in] frame queueへ登録する所有frame。
	 * @return queue上限内ですべて登録できた場合はtrue。
	 */
	[[nodiscard]] bool			QueueFrame( const protocol::Frame &frame) noexcept ;

	/**
	 * @brief HELLO_ACKを検証してready状態へ遷移する。
	 * @param[in] frame decode済みHELLO_ACK。
	 * @return capabilityがcompatibleならtrue。
	 */
	[[nodiscard]] bool			HandleHelloAck( const protocol::Frame &frame) noexcept ;

	/**
	 * @brief Agentのconnection-level CLOSEを検証してHELLO待機へ戻る。
	 * @param[in] frame decode済みCLOSE。
	 * @return 新しいHELLOをqueueして再待機できた場合はtrue。
	 */
	[[nodiscard]] bool			HandleAgentClose( const protocol::Frame &frame) noexcept ;

	/**
	 * @brief Echo中のPONGをsequenceと全byte一致で検証する。
	 * @param[in] frame decode済みPONG。
	 * @return 正しい応答を受理し次状態へ進めた場合はtrue。
	 */
	[[nodiscard]] bool			HandlePong( const protocol::Frame &frame) noexcept ;

	/**
	 * @brief 現在test targetの次の有限PING payloadを生成してqueueする。
	 * @return queue成功または全test完了ならtrue。
	 */
	[[nodiscard]] bool			QueueNextEchoChunk() noexcept ;

	/**
	 * @brief Bridgeを指定分類でfail-closedにしてqueueを破棄する。
	 * @param[in] state `Failed`または`Incompatible`。
	 * @param[in] error payloadを含まない失敗分類。
	 */
	void						Fail( ClientBridgeState state, ClientBridgeError error) noexcept ;

	/**
	 * @brief transport送信queueをmutex境界内で空にする。
	 */
	void						ClearOutboundQueue() noexcept ;

	/**
	 * @brief Agent stream Frameをbounded worker queueへ移す。
	 * @param[in] frame decoder所有Frame。
	 * @return 件数・byte上限内でqueueできた場合はtrue。
	 */
	[[nodiscard]] bool			QueueInboundStreamFrame( protocol::Frame frame) noexcept ;

	/**
	 * @brief worker向け受信Frame queueを破棄する。
	 */
	void						ClearInboundStreamQueue() noexcept ;

	/// current RDP sessionのincremental frame decoder。
	protocol::FrameDecoder		decoder_ ;

	/// local processが提示する固定capability。
	protocol::HelloCapabilities local_capabilities_ ;

	/// HELLO_ACK後に両端の小さい値へ確定したcapability。
	protocol::HelloCapabilities negotiated_capabilities_ ;

	/// UI threadだけが変更するBridge lifecycle。
	ClientBridgeState			state_{ClientBridgeState::Stopped} ;

	/// UI／診断へ公開可能な固定失敗分類。
	ClientBridgeError			error_{ClientBridgeError::None} ;

	/// HELLO retryを含むqueue登録回数。
	std::uint32_t				hello_attempt_count_{0U} ;

	/// Echo target size配列の現在位置。
	std::size_t					echo_target_index_{0U} ;

	/// 現在target内でPONG確認済みのbyte数。
	std::uint64_t				echo_target_completed_bytes_{0U} ;

	/// 全targetを通じてPONG確認済みのbyte数。
	std::uint64_t				echo_completed_bytes_{0U} ;

	/// 次のPINGへ割り当てる非0 sequence。
	std::uint32_t				next_sequence_{1U} ;

	/// 現在一件だけ許容する未応答PING sequence。
	std::uint32_t				outstanding_sequence_{0U} ;

	/// PONGと完全一致させる未応答binary payload。
	std::vector<std::byte>		outstanding_payload_ ;

	/// outstanding PINGが周期的なAgent死活確認ならtrue。
	bool						health_check_outstanding_{false} ;
	/// HELLOごとに最大一件だけを保持する設定要求の受付状態。
	bool						configuration_requested_{false} ;
	/// 自身が協調停止を要求したAgentの遅延callbackだけを破棄する。
	bool						remote_stop_requested_{false} ;
	/// 固定一byteの設定結果。HTTP payloadを含めない。
	std::uint8_t				configuration_status_{0U} ;

	/// peer timeout直後の旧session FrameをHELLO_ACKまで破棄する境界。
	bool						discard_stale_frames_until_hello_ack_{false} ;

	/// future workerとUI message drainの間を保護するqueue mutex。
	mutable std::mutex			outbound_mutex_ ;

	/// SVC write単位へ分割済みの所有chunk列。
	std::deque<std::vector<std::byte>> outbound_chunks_ ;

	/// queue件数だけでなくmemory上限も検査する現在byte合計。
	std::size_t						   outbound_queued_bytes_{0U} ;

	/// UI callbackからProxy workerへ渡す所有stream Frame列。
	std::deque<protocol::Frame>		   inbound_stream_frames_ ;

	/// inbound queueのwire header相当を含むbyte合計。
	std::size_t						   inbound_stream_bytes_{0U} ;
} ;

} // namespace zrdp::client::bridge
