//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_bridge.h
 * @brief Agent側HELLO応答、PING Echo、正常CLOSE、bounded writer queueを定義する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details WTS channel APIを直接呼ばず、受信fragmentと送信chunkの所有bufferだけを
 *          扱う。これによりAgent protocolをRDP sessionなしで検証できる。
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
#include <span>
#include <optional>
#include <vector>

namespace zrdp::agent::bridge {

class AgentStreamRelay ;

/**
 * @brief Agent Bridgeのconnection-level lifecycle。
 */
enum class AgentBridgeState {
	/// channel open後、Client HELLO待ち。
	WaitingForHello,
	/// compatible HELLOへACK済み。
	Ready,
	/// malformed frameまたはstate違反で停止。
	Failed,
	/// protocol versionまたはcapability不一致。
	Incompatible,
} ;

/**
 * @brief Agent console／診断へpayloadなしで公開できる失敗分類。
 */
enum class AgentBridgeError {
	/// 異常は検出されておらず、要求の処理を継続できる。
	None,
	/// 受信frameを検証・再構成できずBridgeを停止する。
	DecodeFailed,
	/// HELLOの長さ・version・capabilityが交渉条件を満たさない。
	InvalidHello,
	/// 現在の状態または役割で許可していないmessageを受信した。
	UnexpectedMessage,
	/// 受信sequenceが期待値と一致せず、順序を保証できない。
	SequenceMismatch,
	/// 有限queueの上限を超え、追加dataを保持できない。
	QueueLimitExceeded,
	/// stream／Socket中継の異常を検出し、Bridgeを停止する。
	StreamRelayFailed,
} ;

/**
 * @brief Agent runtimeが状態遷移とcounterを確認する非機密snapshot。
 */
struct AgentBridgeSnapshot {
	/// 現在のprotocol state。
	AgentBridgeState state{AgentBridgeState::WaitingForHello} ;

	/// 最後の固定失敗分類。
	AgentBridgeError error{AgentBridgeError::None} ;

	/// 正しいPINGとして受理しPONGへqueueした累積byte数。
	std::uint64_t	 echoed_bytes{0U} ;

	/// 順序検証済みの最後のPING sequence。
	std::uint32_t	 last_ping_sequence{0U} ;
} ;

/**
 * @brief Agent側のconnection-control protocolとserialized writer queueを所有する。
 *
 * 全methodはWTS channelを所有する同一I/O threadから呼ぶ。writer queueを
 * 一つに集約し、WTSVirtualChannelWriteの呼出し順序をwire順へ固定する。
 */
class AgentBridge final {
public:
	/** @brief 実Agentだけで限定設定controlと設定確認前のstream拒否を有効にする。
	 * @param[in] enabled 新Clientによる設定確認を必須とする場合true。
	 */
	void			   EnableConfigurationControl( bool enabled) noexcept ;
	/** @brief 一件だけの設定要求をI/O ownerへ移す。
	 * @param[out] request 最大64 KiBのINIと固定sequenceを所有するframe。
	 * @return 未処理の設定要求がある場合true。
	 */
	[[nodiscard]] bool TakeConfiguration( protocol::Frame &request) noexcept ;
	/** @brief 検証・永続化後の設定結果だけを固定ACKへqueueする。
	 * @param[in] status 1は利用可、2は再起動、3は拒否。
	 * @return 完全queueできた場合true。1以外はstreamを許可しない。
	 */
	[[nodiscard]] bool CompleteConfiguration( std::uint8_t status) noexcept ;
	/** @brief 限定停止要求の受理状態を返す。 @return I/O ownerが協調停止すべき場合true。 */
	[[nodiscard]] bool StopRequested() const noexcept ;
	/** @brief 現在のINIを確認して中継可能か返す。 @return lifecycle無効か設定ACK済みならtrue。 */
	[[nodiscard]] bool ConfigurationReady() const noexcept ;
	/**
	 * @brief optional Phase 3 relayを設定し、新しいHELLO待機状態へ戻す。
	 * @param[in] relay 有効設定時の非owning pointer。Echo-onlyではnullptr。
	 *
	 * @note relayはこのAgentBridgeより長く生存し、同じI/O threadから呼ばれる。
	 */
	void			   ConfigureStreamRelay( AgentStreamRelay *relay) noexcept ;

	/**
	 * @brief 新しいchannel sessionのdecoder、sequence、queueを初期化する。
	 */
	void			   Reset() noexcept ;

	/**
	 * @brief Clientからの任意fragmentをdecodeしてHELLOまたはPINGを処理する。
	 * @param[in] data WTS readから所有copyしたbinary fragment。
	 * @return sessionを継続できる場合はtrue。
	 */
	[[nodiscard]] bool OnChannelData( std::span<const std::byte> data) noexcept ;

	/**
	 * @brief 正常終了前のconnection-level CLOSEをwriter queueへ登録する。
	 * @return 空payloadのCLOSE全体をqueueできた場合はtrue。
	 *
	 * ClientへAgent終了を通知し、RDPを維持したままHELLO待機へ戻せるようにする。
	 */
	[[nodiscard]] bool QueueSessionClose() noexcept ;

	/**
	 * @brief optional non-blocking Socket relayを有限回進める。
	 * @param[in] now_milliseconds `GetTickCount64`相当の単調時刻。
	 * @return Bridge sessionを継続できる場合はtrue。
	 */
	[[nodiscard]] bool PumpStreams( std::uint64_t now_milliseconds) noexcept ;

	/**
	 * @brief serialized writer queueの先頭SVC chunkを取り出す。
	 * @param[out] chunk 成功時に最大1600 byteを所有するdata。
	 * @param[out] more_data 取出し後にもqueueが残る場合はtrue。
	 * @return 一件取得できた場合はtrue。
	 */
	[[nodiscard]] bool TakeOutboundChunk( std::vector<std::byte> &chunk, bool &more_data) noexcept ;

	/**
	 * @brief runtimeとtestへ非機密state copyを返す。
	 * @return 現在のAgent Bridge snapshot。
	 */
	[[nodiscard]] AgentBridgeSnapshot snapshot() const noexcept ;

private:
	/** @brief 限定controlの方向・世代・サイズ・重複を検証する。
	 * @param[in] frame 設定または協調停止だけの所有frame。
	 * @return 一件を受理できた場合true。
	 */
	[[nodiscard]] bool			HandleConfigurationControl( protocol::Frame frame) noexcept ;
	/**
	 * @brief HELLOを検証しHELLO_ACKをwriter queueへ登録する。
	 * @param[in] frame decode済みHELLO。
	 * @return compatibleでACK全体をqueueできた場合はtrue。
	 */
	[[nodiscard]] bool			HandleHello( const protocol::Frame &frame) noexcept ;

	/**
	 * @brief PINGの順序を検証し同じbinary payloadのPONGをqueueする。
	 * @param[in] frame decode済みPING。
	 * @return PONG全体をqueueできた場合はtrue。
	 */
	[[nodiscard]] bool			HandlePing( const protocol::Frame &frame) noexcept ;

	/**
	 * @brief Phase 2 stream frameをrelayへ渡して生成frameをwriter queueへ戻す。
	 * @param[in] frame decode済み所有stream frame。
	 * @return relayとwriter queueが継続可能ならtrue。
	 */
	[[nodiscard]] bool			HandleStreamFrame( protocol::Frame frame) noexcept ;

	/**
	 * @brief relay outbound frameを既存serialized transport queueへ移す。
	 * @return 全Frameをqueueできた場合はtrue。
	 */
	[[nodiscard]] bool			DrainStreamFrames() noexcept ;

	/**
	 * @brief 一frameをencodeしてSVC transport chunkへ分割する。
	 * @param[in] frame encode対象。
	 * @return 件数・総byte上限内で全chunkをqueueできた場合はtrue。
	 */
	[[nodiscard]] bool			QueueFrame( const protocol::Frame &frame) noexcept ;

	/**
	 * @brief Agent Bridgeをfail-closed状態へ移し、残queueを破棄する。
	 * @param[in] state `Failed`または`Incompatible`。
	 * @param[in] error 固定失敗分類。
	 */
	void						Fail( AgentBridgeState state, AgentBridgeError error) noexcept ;

	/// current channel sessionのincremental frame decoder。
	protocol::FrameDecoder		decoder_ ;

	/// Agentが提示するPhase 1 capability。
	protocol::HelloCapabilities local_capabilities_ ;

	/// Client HELLOと交渉済みの有限上限。
	protocol::HelloCapabilities negotiated_capabilities_ ;

	/// I/O threadだけが変更するprotocol lifecycle。
	AgentBridgeState			state_{AgentBridgeState::WaitingForHello} ;

	/// payloadを含まない最後の失敗分類。
	AgentBridgeError			error_{AgentBridgeError::None} ;

	/// Client開始sequenceの単調増加を確認する直前値。
	std::uint32_t				last_ping_sequence_{0U} ;

	/// 正常Echoとしてwriter queueへ登録した累積byte数。
	std::uint64_t				echoed_bytes_{0U} ;

	/// 一つのWTS writer経路だけがdrainするtransport chunk列。
	std::deque<std::vector<std::byte>> outbound_chunks_ ;

	/// 件数と独立して検証するqueue memory合計。
	std::size_t						   outbound_queued_bytes_{0U} ;

	/// mainが所有し、設定有効時だけ接続するPhase 3 relay。
	AgentStreamRelay				  *stream_relay_{nullptr} ;
	/// 実runtimeだけが有効化する限定設定feature。
	bool							   configuration_control_{false} ;
	/// 現HELLO世代でINIの確認を完了した状態。
	bool							   configuration_ready_{true} ;
	/// 同じHELLOで二重設定・二重ACKを拒否する受付履歴。
	bool							   configuration_received_{false} ;
	/// 一HELLOにつき設定結果を一度だけ応答する。
	bool							   configuration_completed_{false} ;
	/// ACK前に保持する最大一件だけの所有INI frame。
	std::optional<protocol::Frame>	   configuration_request_ ;
	/// 利用者の設定撤回に対応する協調停止状態。
	bool							   stop_requested_{false} ;
} ;

} // namespace zrdp::agent::bridge
