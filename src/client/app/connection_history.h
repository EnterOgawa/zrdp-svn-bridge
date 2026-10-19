//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file connection_history.h
 * @brief RDP接続履歴の重複排除、DPAPI永続化、非同期worker契約を定義する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details passwordを型から排除し、current-user DPAPIと有限queueによって
 *          接続履歴の機密性とUI threadの応答性を維持する。
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

#include "client/app/rdp_user_identity.h"
#include "client/rdp/rdp_audio_settings.h"

#include <Windows.h>

#include <cstddef>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zrdp::client::app {

/**
 * @brief 認証が完了した一組のRDP接続profile履歴。
 *
 * passwordや他のcredentialは意図的にmemberへ含めない。
 */
struct ConnectionHistoryEntry {
	/// 利用者が任意指定する接続プロファイル名。空文字列は名前なし履歴を表す。
	std::wstring					 connection_name ;

	/// 接続先PC名またはIPアドレス。永続化時はDPAPIで保護する。
	std::wstring					 server ;

	/// RDPドメイン。空文字列はWindows標準UIでの選択を許容する。
	std::wstring					 domain ;

	/// RDPユーザー名。空文字列はWindows標準UIでの補完を許容する。
	std::wstring					 user_name ;

	/// profileから次のsessionへ復元する接続時音声設定。
	rdp::RdpAudioRedirectionSettings audio ;
} ;

// Windowsユーザーごとに保持する直近接続組の上限
inline constexpr std::size_t kMaximumConnectionHistoryEntries = 10U ;

// 接続名のUTF-16文字数上限
inline constexpr std::size_t kMaximumConnectionNameCharacters = 64U ;

/**
 * @brief current-user DPAPIで有限件数のRDP接続履歴を永続化する。
 *
 * 既定fileは現在のWindowsユーザーのLocalAppData以下へ置く。
 * 平文内容をlogまたはportable EXEの隣へ保存しない。
 */
class ConnectionHistoryStore final {
public:
	/**
	 * @brief production用またはtest用の履歴storeを生成する。
	 * @param[in] storage_path 空ならLocalAppData、指定時はtest用保存先。
	 */
	explicit ConnectionHistoryStore( std::filesystem::path storage_path = {}) ;

	/**
	 * @brief current-user履歴を読込み、復号、検証する。
	 *
	 * @retval S_OK 有効な履歴を読込んだ。
	 * @retval S_FALSE 履歴fileがまだ存在しない。
	 * @return 拒否、破損、読込み不能の場合は原因を表すHRESULT。
	 *
	 * 失敗時は途中まで復号したentryを公開せず、既存のin-memory履歴を空にする。
	 */
	[[nodiscard]] HRESULT Load() noexcept ;

	/**
	 * @brief 認証成功した接続profileを最新位置へ移動して保存する。
	 * @param[in] connection_name 任意の接続名。空文字列は名前なしを表す。
	 * @param[in] server 接続先を表すbounded文字列。
	 * @param[in] domain 任意のRDPドメイン。passwordは受け付けない。
	 * @param[in] user_name ユーザー名。passwordは受け付けない。
	 * @param[in] audio 接続時に確定する音声リダイレクト設定。
	 * @return 永続化結果を表すHRESULT。
	 *
	 * 重複接続組と同名の非空プロファイルは大小文字を区別せず比較し、
	 * それぞれ一件だけ保持する。
	 */
	[[nodiscard]] HRESULT Remember( std::wstring connection_name, std::wstring server,
								   std::wstring domain, std::wstring user_name,
								   rdp::RdpAudioRedirectionSettings audio = {}) noexcept ;

	/**
	 * @brief process内に保持する復号済みentryを新しい順で参照する。
	 * @return 次の変更操作まで有効なread-only参照。
	 */
	[[nodiscard]] const std::vector<ConnectionHistoryEntry> &entries() const noexcept ;

private:
	/**
	 * @brief 現在のentry全体をserialize、暗号化し、atomicに保存する。
	 * @return 保存結果を表すHRESULT。
	 */
	[[nodiscard]] HRESULT Save() ;

	/**
	 * @brief 明示pathまたはLocalAppDataから実保存先を解決する。
	 * @param[out] resolved_path 検証済み保存先path。
	 * @return pathを安全に構築できた場合はS_OK。
	 */
	[[nodiscard]] HRESULT ResolveStoragePath( std::filesystem::path &resolved_path) const ;

	/// 空の場合はproduction既定pathを遅延解決する。
	std::filesystem::path storage_path_ ;

	/// 新しい順に並べた、process内だけの復号済み履歴。
	std::vector<ConnectionHistoryEntry> entries_ ;
} ;

/**
 * @brief background workerが扱う履歴操作の種類。
 */
enum class ConnectionHistoryOperation {
	/// 起動時の読込み。
	Load,
	/// 認証成功組の追加と保存。
	Save,
} ;

/**
 * @brief 履歴workerからUI threadへ所有権付きで公開する完了結果。
 */
struct ConnectionHistoryResult {
	/// 完了した要求の種類。
	ConnectionHistoryOperation			operation{ConnectionHistoryOperation::Load} ;

	/// 操作結果。例外はこのHRESULTへ変換する。
	HRESULT								result{E_UNEXPECTED} ;

	/// UIへ渡す新しい順の所有copy。機密logへは出力しない。
	std::vector<ConnectionHistoryEntry> entries ;
} ;

/**
 * @brief DPAPIとlocal file処理を有限queueでUI thread外へ分離する。
 *
 * 完了時は呼出し元指定のWM_APP messageだけを通知する。UIは所有copyを
 * drainし、storeのmutable stateをworkerと共有しない。
 */
class ConnectionHistoryWorker final {
public:
	/**
	 * @brief production用またはtest用の履歴workerを生成する。
	 * @param[in] storage_path 空ならproduction既定path、指定時はtest用保存先。
	 */
	explicit ConnectionHistoryWorker( std::filesystem::path storage_path = {}) ;

	/**
	 * @brief 受理済み要求をdrainしてworkerをjoinする。
	 */
	~ConnectionHistoryWorker() ;

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	ConnectionHistoryWorker( const ConnectionHistoryWorker &)			= delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	ConnectionHistoryWorker &operator=( const ConnectionHistoryWorker &) = delete ;

	/**
	 * @brief workerを開始して初回Load要求をqueueへ登録する。
	 * @param[in] notification_window 完了messageを受け取るUI window。
	 * @param[in] notification_message WM_APP範囲の通知message。
	 * @return 開始結果を表すHRESULT。
	 */
	[[nodiscard]] HRESULT	 Start( HWND notification_window, UINT notification_message) noexcept ;

	/**
	 * @brief 認証成功した接続組を有限queueへ登録する。
	 * @param[in] connection_name 任意の接続名の所有文字列。
	 * @param[in] server 接続先の所有文字列。
	 * @param[in] domain 任意のRDPドメインの所有文字列。
	 * @param[in] user_name ユーザー名の所有文字列。
	 * @param[in] audio 接続時に確定する音声リダイレクト設定。
	 * @return 受理結果。queue満杯時はERROR_NOT_ENOUGH_QUOTA由来のHRESULT。
	 */
	[[nodiscard]] HRESULT	 QueueRemember( std::wstring connection_name, std::wstring server,
										   std::wstring domain, std::wstring user_name,
										   rdp::RdpAudioRedirectionSettings audio = {}) noexcept ;

	/**
	 * @brief 完了した全結果を呼出しthreadへ移動する。
	 * @return workerと共有しない結果の所有vector。
	 */
	[[nodiscard]] std::vector<ConnectionHistoryResult> TakeResults() noexcept ;

	/**
	 * @brief 受理済み処理をdrainし、background file処理を停止する。
	 *
	 * 複数回呼び出しても安全であり、戻り時にworkerはjoin済みとなる。
	 */
	void											   Stop() noexcept ;

private:
	/**
	 * @brief workerへ所有権を渡す一件の要求。
	 */
	struct Request {
		/// 実行するstore操作。
		ConnectionHistoryOperation		 operation{ConnectionHistoryOperation::Load} ;

		/// Save要求だけが使用する任意の接続名。
		std::wstring					 connection_name ;

		/// Save要求だけが使用する接続先。
		std::wstring					 server ;

		/// Save要求だけが使用するRDPドメイン。
		std::wstring					 domain ;

		/// Save要求だけが使用するユーザー名。
		std::wstring					 user_name ;

		/// Save要求だけが使用する接続時音声設定。
		rdp::RdpAudioRedirectionSettings audio ;
	} ;

	/**
	 * @brief 要求を直列実行し、所有結果をbounded queueへ公開するthread entry。
	 *
	 * 例外はHRESULTへ変換し、thread境界から漏らさない。
	 */
	void								WorkerLoop() noexcept ;

	/// UI連打によるmemory増加を防ぐ未処理要求数の上限。
	static constexpr std::size_t		kMaximumPendingRequests = 16U ;

	/// UI停止中でも結果memoryを有限に保つ完了結果数の上限。
	static constexpr std::size_t		kMaximumPendingResults = 16U ;

	/// worker threadだけがmutable操作する永続化store。
	ConnectionHistoryStore				store_ ;

	/// request、result、lifecycle stateを保護するmutex。
	std::mutex							mutex_ ;

	/// 新規requestまたは停止要求をworkerへ通知するcondition variable。
	std::condition_variable				request_ready_ ;

	/// UIからworkerへ所有権を移した未処理要求。
	std::deque<Request>					requests_ ;

	/// workerからUIへ所有権を移す完了結果。
	std::deque<ConnectionHistoryResult> results_ ;

	/// StartからStopまで一つだけ存在するbackground thread。
	std::thread							worker_ ;

	/// 完了通知先。workerはHWNDを所有しない。
	HWND								notification_window_{nullptr} ;

	/// 完了通知に使うWM_APP message。
	UINT								notification_message_{0U} ;

	/// Startの重複実行を拒否するlifecycle flag。
	bool								started_{false} ;

	/// 新規要求を拒否し、queue drain後に終了するためのflag。
	bool								stopping_{false} ;
} ;

} // namespace zrdp::client::app
