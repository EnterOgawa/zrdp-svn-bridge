//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file resource_contract_test.cpp
 * @brief 配布EXEがbranding iconと主要UI文字列を持つresource契約を検証する。
 * @version 01.00.18
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * EXEを実行せずresource dataとして開き、Client／Agent共通icon、domain欄、
 * 表示mode、全画面切替、複数session表示、接続時音声設定の欠落を検出する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  同意付き更新の文字列とmenuを検証。
 * Ver.01.00.02   Shunji Ogawa  2026/10/08  Agent設定の主要resource契約を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/09  session画面のAgent配置・確認付き起動を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/09  配置から独立した開発用Win+R testを追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  三種類の入力test menuと表示を検証。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  四番目の比較項目と数値表示の組込みを検証。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  固定Win+RのSendInput比較と停止境界を追加。
 * Ver.01.00.09   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.10   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * Ver.01.00.11   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.12   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.13   Shunji Ogawa  2026/10/10  接続別設定・自動Agent管理の回帰検証を追加。
 * Ver.01.00.14   Shunji Ogawa  2026/10/10  managerメニューを撤廃し自動更新確認へ統一。
 * Ver.01.00.15   Shunji Ogawa  2026/10/10  認証後の非modal要設定resourceを検証。
 * Ver.01.00.16   Shunji Ogawa  2026/10/10  登録済み接続別復元と切替時の保存保全を検証。
 * Ver.01.00.17   Shunji Ogawa  2026/10/10  更新通知と復旧案内がClient／Agentセットを明示するか検証。
 * Ver.01.00.18   Shunji Ogawa  2026/10/11  公開準備版のtitleと製品版を同期。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/app/resource.h"

#include <Windows.h>

#include <array>
#include <string_view>
#include <memory>
#include <type_traits>
#include <utility>

namespace {

/**
 * @brief resource dataとして開いたmoduleを一意所有するRAII型。
 */
class ScopedResourceModule final {
public:
	/**
	 * @brief LoadLibraryExWが返した所有moduleを受け取る。
	 * @param[in] module 解放対象のmodule。
	 */
	explicit ScopedResourceModule( const HMODULE module) noexcept : module_( module)
	{
	}

	/**
	 * @brief 所有moduleをFreeLibraryで解放する。
	 */
	~ScopedResourceModule()
	{
		// 有効なmoduleを所有している場合だけ参照数を解放する
		if( module_ != nullptr) {
			FreeLibrary( module_) ;
		}
	}

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	ScopedResourceModule( const ScopedResourceModule &)			  = delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	ScopedResourceModule &operator=( const ScopedResourceModule &) = delete ;

private:
	/// LoadLibraryExWから受け取った所有module。
	HMODULE module_{nullptr} ;
} ;

/**
 * @brief moduleに共通ZRdp icon groupが埋め込まれているか検証する。
 * @param[in] module resource dataとして開いたClientまたはAgent。
 * @return icon groupと実dataが存在する場合はtrue。
 */
[[nodiscard]] bool HasApplicationIcon( const HMODULE module) noexcept
{
	const HRSRC resource = FindResourceW( module, MAKEINTRESOURCEW( IDI_ZRDP_APP), RT_GROUP_ICON) ;
	// group entryだけでなく有限のresource dataを持つことを必須とする
	if( resource == nullptr) {
		return false ;
	}

	return SizeofResource( module, resource) > 0U ;
}

/**
 * @brief 一つのstring resource IDと期待する利用者向け表示を対応付ける。
 */
struct ExpectedResourceString {
	/// 検証対象のstring resource ID。
	UINT			  identifier ;
	/// 完全一致させる固定表示。
	std::wstring_view expected ;
} ;

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Client UI文字列とClient／Agent共通iconを検証する。
 * @param[in] argument_count EXE pathを含む引数個数。
 * @param[in] arguments 第1引数Client、第2引数AgentのEXE path。
 * @return 契約を満たす場合は0。それ以外は失敗段階を表す固定code。
 */
int wmain( const int argument_count, wchar_t *arguments[])
{
	// CTestからClient／Agentの二つが指定された場合に限定する
	if( argument_count != 3 || arguments[1] == nullptr || arguments[2] == nullptr) {
		return 1 ;
	}

	const HMODULE client_module = LoadLibraryExW(
		arguments[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE) ;
	// Clientを実行せずresourceとして開けない場合は検証対象として不成立とする
	if( client_module == nullptr) {
		return 2 ;
	}

	const ScopedResourceModule client_module_owner( client_module) ;
	const HMODULE			   agent_module = LoadLibraryExW(
		 arguments[2], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE) ;
	// Agent側のicon resourceも独立して検査する
	if( agent_module == nullptr) {
		return 3 ;
	}

	const ScopedResourceModule agent_module_owner( agent_module) ;
	// Explorer表示とwindow表示の共通iconが両配布物へ存在することを必須とする
	if( !HasApplicationIcon( client_module) || !HasApplicationIcon( agent_module)) {
		return 4 ;
	}

	constexpr std::array<ExpectedResourceString, 38> expectations{{
		{IDS_AGENT_SETTINGS_EMPTY,
		 L"この接続には登録済みの接続先がありません。IP・ポート・用途を入力して登録してください。"},
		{IDS_AGENT_SETTINGS_SELECT_CONNECTION,
		 L"接続マネージャーで接続先・ドメイン・ユーザーを選択してください。"},
		{IDS_TITLE_AGENT_NEEDS_SETTINGS, L"要設定"},
		{IDS_START_PENDING,
		 L"入力準備が完了しました。Enterはまだ送信していません。結果まで操作・ウィンドウ切替を控えてください。"},
		{IDS_SESSION_AGENT_DEPLOY, L"Agent配置・起動"},
		{IDS_SESSION_AGENT_START, L"Agent起動"},
		{IDS_AGENT_SETTINGS_TITLE, L"ZRdp SVN Bridge - Agent設定"},
		{IDS_AGENT_SETTINGS_COL_IP, L"リポジトリのIPv4"},
		{IDS_AGENT_SETTINGS_COL_PORT, L"HTTPポート"},
		{IDS_AGENT_SETTINGS_SVN, L"HTTP SVN"},
		{IDS_AGENT_SETTINGS_TRAC, L"HTTP Trac"},
		{IDS_AGENT_SETTINGS_UNAPPROVED, L"未承認"},
		{IDS_UPDATE_CHECKING, L"更新: 確認中"},
		{IDS_UPDATE_DOWNLOADING, L"更新: 取得・検証中"},
		{IDS_UPDATE_WAITING, L"更新: RDP／設定ウィンドウ終了待ち"},
		{IDS_UPDATE_APPLYING, L"更新: 適用中"},
		{IDS_UPDATE_LATEST, L"このClientより新しい互換バージョンはありません。"},
		{IDS_UPDATE_CONNECT_BLOCKED,
		 L"Clientの更新適用中は新しいRDP接続を開始できません。更新後の再起動をお待ちください。"},
		{IDS_CONNECTION_NAME_LABEL, L"接続名:"},
		{IDS_DOMAIN_LABEL, L"ドメイン:"},
		{IDS_DISPLAY_MODE_LABEL, L"表示モード:"},
		{IDS_DISPLAY_MODE_NATIVE, L"等倍表示（スクロール）"},
		{
			IDS_DISPLAY_MODE_SMART_SIZING,
			L"Smart Sizing（縮小表示）",
		},
		{
			IDS_DISPLAY_MODE_DYNAMIC_RESOLUTION,
			L"動的解像度（Window追従）",
		},
		{
			IDS_FULL_SCREEN,
			L"全画面",
		},
		{
			IDS_WINDOW_MODE,
			L"ウィンドウ",
		},
		{
			IDS_PROXY_STOPPED,
			L"Proxy: 停止",
		},
		{
			IDS_CONNECTION_MANAGER_TITLE,
			L"ZRdp 接続マネージャー (0.7.21)",
		},
		{
			IDS_CONNECT_NEW_SESSION,
			L"新しい接続",
		},
		{
			IDS_MANAGER_STATUS_FORMAT,
			L"接続中セッション: %u / 次のProxy: 127.0.0.1:%u",
		},
		{
			IDS_SESSION_TITLE_FORMAT,
			L"ZRdp SVN Bridge 0.7.21 - %s | RDP:%s | Agent:%s | Proxy:%s 127.0.0.1:%u | stream=%u | C→A=%llu | A→C=%llu",
		},
		{
			IDS_LOCAL_RESOURCES_LABEL,
			L"ローカル リソース:",
		},
		{
			IDS_REDIRECT_MICROPHONE,
			L"マイクを接続先で使用する",
		},
		{
			IDS_REMOTE_AUDIO_LABEL,
			L"リモート音声:",
		},
		{
			IDS_AUDIO_PLAY_ON_CLIENT,
			L"このPCで再生する",
		},
		{
			IDS_AUDIO_PLAY_ON_REMOTE,
			L"接続先PCで再生する",
		},
		{
			IDS_AUDIO_DO_NOT_PLAY,
			L"再生しない",
		},
		{
			IDS_STATUS_PRE_LOGIN_RETRY,
			L"RDP: 一時的な認証接続失敗を検出したため、しばらく待って再試行します | SVN Bridge: 停止",
		},
	}} ;

	// 接続管理とsession生成を含む主要表示を同じ完全一致契約で検証する
	for( const ExpectedResourceString &expectation : expectations) {
		std::array<wchar_t, 256> text{} ;
		const int length = LoadStringW( client_module, expectation.identifier, text.data(),
									   static_cast<int>( text.size())) ;
		// 設定文字列が欠落またはbuffer上限を超える状態を許可しない
		if( length <= 0 || static_cast<std::size_t>( length) >= text.size()) {
			return 5 ;
		}

		const std::wstring_view actual( text.data(), static_cast<std::size_t>( length)) ;
		// 表示modeの意味が曖昧な旧文字列へ戻らないよう完全一致させる
		if( actual != expectation.expected) {
			return 6 ;
		}
	}

	std::array<wchar_t, 2048U> update_offer{}, update_recovered{} ;
	// 二EXEの更新と接続先Agentへの次回反映を同意前に明示する
	if( LoadStringW( client_module, IDS_UPDATE_OFFER_FORMAT, update_offer.data(), 2048) <= 0 ||
	   LoadStringW( client_module, IDS_UPDATE_RECOVERED, update_recovered.data(), 2048) <= 0 ||
	   std::wstring_view( update_offer.data()).find( L"ClientとAgentをセットで更新") ==
		   std::wstring_view::npos ||
	   std::wstring_view( update_offer.data()).find( L"次回RDP接続時に更新") ==
		   std::wstring_view::npos ||
	   std::wstring_view( update_recovered.data()).find( L"旧版ClientとAgent") ==
		   std::wstring_view::npos) {
		return 14 ;
	}

	// 自動更新確認へ統一し、旧manager menuを配布EXEへ再公開しない
	if( FindResourceW( client_module, MAKEINTRESOURCEW( IDR_CLIENT_MENU), RT_MENU) != nullptr) {
		return 7 ;
	}

	// 設定画面を実行せずresourceの組込み欠落を検出する
	if( FindResourceW( client_module, MAKEINTRESOURCEW( IDD_AGENT_SETTINGS), RT_DIALOG) == nullptr) {
		return 8 ;
	}

	// 起動と配置の確認画面を配布EXEへ必ず組み込む
	if( FindResourceW( client_module, MAKEINTRESOURCEW( IDD_AGENT_START), RT_DIALOG) == nullptr ||
	   FindResourceW( client_module, MAKEINTRESOURCEW( IDD_AGENT_DEPLOYMENT), RT_DIALOG) == nullptr) {
		return 9 ;
	}

	// 廃止した未署名テストresourceを製品へ再公開しない
	for( const auto identifier : {353U, 354U, 355U, 356U}) {
		std::array<wchar_t, 2048U> text{} ;
		// 未署名だけの別警告・失敗経路は残さない
		if( LoadStringW( client_module, identifier, text.data(), static_cast<int>( text.size())) !=
		   0) {
			return 13 ;
		}
	}

	// 削除した開発入力menuを製品resourceへ再公開しない
	if( FindResourceW( client_module, MAKEINTRESOURCEW( IDR_INPUT_TEST_MENU), RT_MENU) != nullptr) {
		return 10 ;
	}

	std::array<wchar_t, 128U> agent_label{} ;
	// 設定操作は接続managerからだけ公開する
	if( LoadStringW( client_module, IDS_MANAGER_AGENT_SETTINGS, agent_label.data(), 128) <= 0 ||
	   std::wstring_view( agent_label.data()) != L"Agent設定") {
		return 11 ;
	}

	return 0 ;
}
