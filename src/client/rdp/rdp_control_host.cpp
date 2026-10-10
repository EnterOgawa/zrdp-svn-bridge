//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file rdp_control_host.cpp
 * @brief Microsoft Remote Desktop ActiveXの生成、設定、イベント購読を実装する。
 * @version 01.00.14
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * 全COM操作を生成元STAへ限定し、Microsoftが提供するNLA、CredSSP、認証UI、
 * 画面転送、クリップボード転送を利用する。イベント引数は型・個数・長さを
 * 検証して所有バッファへコピーし、COM所有メモリやC++例外を境界外へ出さない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  session画面のAgent配置・確認付き起動を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/09  自動入力のfocusとWindowsキー転送を確認。
 * Ver.01.00.05   Shunji Ogawa  2026/10/09  遅延送信前の変更しないfocus再確認を追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  三種類に限定した診断入力を追加。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  実Control focusとキー解放を確認してRを一回生成。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  固定Win+RのSendInput比較と停止境界を追加。
 * Ver.01.00.09   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.10   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * Ver.01.00.11   Shunji Ogawa  2026/10/10  通常focusを認証完了後かつ警告なしへ限定。
 * Ver.01.00.12   Shunji Ogawa  2026/10/10  開発入力診断APIを廃止し固定入力を維持。
 * Ver.01.00.13   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * Ver.01.00.14   Shunji Ogawa  2026/10/11  公開CLSIDで旧Windows type libraryへのbuild依存を除去。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/rdp/rdp_control_host.h"

#include "client/rdp/rdp_import.h"
#include "client/rdp/rdp_input_focus.h"
#include "common/diagnostic_log.h"
#include "protocol/frame.h"

#include <atlbase.h>
#include <atlhost.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace zrdp::client::rdp {
namespace {

// ActiveXへ渡す入力値と、COM callbackから所有コピーする値の有限上限
constexpr long		  kMinimumDesktopDimension			 = 200 ;
// ActiveXへ要求できるdesktop幅・高さの各pixel上限
constexpr long		  kMaximumDesktopDimension			 = 8192 ;
// COMへ渡す接続先のUTF-16 code unit上限。表示省略と接続条件を混同しない
constexpr std::size_t kMaximumServerLength				 = 255 ;
// COMへ渡すDomainのUTF-16 code unit上限
constexpr std::size_t kMaximumDomainLength				 = 255 ;
// COMへ渡す分離済みUserNameのUTF-16 code unit上限
constexpr std::size_t kMaximumUserNameLength			 = 512 ;
// Microsoft error説明をUIへcopyするUTF-16 code unit上限
constexpr UINT		  kMaximumErrorDescriptionCharacters = 2048U ;
// 受信channel名を所有copyするUTF-16 code unit上限
constexpr UINT		  kMaximumChannelNameCharacters		 = 64 ;
// 一callbackからcopyできるbinary payloadのbyte上限
constexpr UINT		  kMaximumChannelPayloadBytes		 = 1024U * 1024U ;
// FullScreenTitleの終端NULを除くUTF-16 code unit上限
constexpr std::size_t kMaximumFullScreenTitleCharacters	 = static_cast<std::size_t>( MAX_PATH) - 1U ;
// 全画面titleとMicrosoft barでsessionを識別する正式製品名prefix
constexpr std::wstring_view kFullScreenConnectionLabelPrefix = L"ZRdp SVN Bridge - " ;
// 表示上限超過時だけlabel末尾へ付ける省略記号。接続先設定は変えない
constexpr std::wstring_view kTruncatedConnectionLabelSuffix	 = L"..." ;
// 標準認証dialog互換versionを照会・設定するActiveX拡張property名
constexpr wchar_t		kSecurityDialogVersionPropertyName[] = L"RedirectionWarningDialogVersion" ;
// 2026年4月更新前の、ActiveX container互換security dialog版
constexpr unsigned long kCompatibleSecurityDialogVersion	 = 1U ;
// Windows credential promptを閉じた際に現在のActiveXが通知する切断reason
constexpr long			kCredentialPromptCancellationDisconnectReason = 0x1F07L ;
// credential promptキャンセル時に取得される追加情報なしの拡張reason
constexpr long			kNoExtendedDisconnectReason					  = 0L ;

/**
 * @brief 接続設定modelへ定義された表示modeだけを受け入れる。
 * @param[in] mode 検証する表示mode。
 * @return 既知の排他的な3 modeのいずれかならtrue。
 */
[[nodiscard]] constexpr bool IsValidDisplayMode( const RdpDisplayMode mode) noexcept
{
	return mode == RdpDisplayMode::NativeScroll || mode == RdpDisplayMode::SmartSizing ||
		   mode == RdpDisplayMode::DynamicResolution ;
}

/**
 * @brief 互換propertyを実装していない旧ActiveXのHRESULTか判定する。
 * @param[in] result `IMsRdpExtendedSettings::put_Property`の結果。
 * @return property未対応を表す既知結果ならtrue。
 *
 * 旧Windowsは更新前dialogを既定で使用するため、この分類だけは接続継続を
 * 許可する。ほかの失敗を未対応と推測して隠さない。
 */
[[nodiscard]] constexpr bool IsUnsupportedSecurityDialogProperty( const HRESULT result) noexcept
{
	return result == E_INVALIDARG || result == E_NOTIMPL || result == DISP_E_MEMBERNOTFOUND ||
		   result == DISP_E_UNKNOWNNAME || result == HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED) ;
}

// Microsoft IMsTscAxEvents dispinterfaceで定義されたイベントDISPID
constexpr DISPID kDispIdOnConnecting					 = 1 ;
// transport接続成立をlogin完了と分けて受け取る固定DISPID
constexpr DISPID kDispIdOnConnected						 = 2 ;
// Windows標準認証後のlogin完了を受け取る固定DISPID
constexpr DISPID kDispIdOnLoginComplete					 = 3 ;
// 理由code付きRDP切断を受け取る固定DISPID
constexpr DISPID kDispIdOnDisconnected					 = 4 ;
// ActiveXの全画面状態への遷移を受け取る固定DISPID
constexpr DISPID kDispIdOnEnterFullScreen				 = 5 ;
// ActiveXのWindow表示復帰を受け取る固定DISPID
constexpr DISPID kDispIdOnLeaveFullScreen				 = 6 ;
// BSTRのbinary channel payloadを受け取る固定DISPID
constexpr DISPID kDispIdOnChannelReceivedData			 = 7 ;
// Controlのcontainer全画面化要求を受け取る固定DISPID
constexpr DISPID kDispIdOnRequestGoFullScreen			 = 8 ;
// Controlのcontainer Window復帰要求を受け取る固定DISPID
constexpr DISPID kDispIdOnRequestLeaveFullScreen		 = 9 ;
// 継続不能なRDP errorを受け取る固定DISPID
constexpr DISPID kDispIdOnFatalError					 = 10 ;
// 接続継続可能なControl警告を受け取る固定DISPID
constexpr DISPID kDispIdOnWarning						 = 11 ;
// Microsoft barのSession Window最小化要求を受け取る固定DISPID
constexpr DISPID kDispIdOnRequestContainerMinimize		 = 14 ;
// Control破棄前の終了確認へ応答する固定DISPID
constexpr DISPID kDispIdOnConfirmClose					 = 15 ;
// 旧形式の再接続理由・試行回数・継続値を受け取る固定DISPID
constexpr DISPID kDispIdOnAutoReconnecting				 = 17 ;
// 標準認証警告UIの表示中をfocus制御へ伝える固定DISPID
constexpr DISPID kDispIdOnAuthenticationWarningDisplayed = 18 ;
// 標準認証警告UIの解除をfocus制御へ伝える固定DISPID
constexpr DISPID kDispIdOnAuthenticationWarningDismissed = 19 ;
// 既存RDP user sessionへの自動復帰完了を受け取る固定DISPID
constexpr DISPID kDispIdOnAutoReconnected				 = 33 ;
// 拡張形式の再接続理由・network状態・試行上限を受け取る固定DISPID
constexpr DISPID kDispIdOnAutoReconnecting2				 = 34 ;

// 公開CLSIDだけを保持し、旧system type libraryに最新coclass宣言を要求しない。
// 値の出典はMicrosoft Learnの各MsRdpClientNNotSafeForScripting class文書。
constexpr CLSID	 kRdpClient12ClassId{
	 0x3f859aa3, 0xc2d4, 0x4faa, {0xb0, 0xe4, 0xfd, 0x0c, 0x9c, 0x4e, 0x5e, 0x3a}} ;
// MsRdpClient11の公開CLSID。type libraryと実行時の登録有無を分離する。
constexpr CLSID kRdpClient11ClassId{
	0x1df7c823, 0xb2d4, 0x4b54, {0x97, 0x5a, 0xf2, 0xac, 0x5d, 0x7c, 0xf8, 0xb8}} ;
// MsRdpClient10の公開CLSID。新しいControl未登録時だけ選択候補になる。
constexpr CLSID kRdpClient10ClassId{
	0xa0c63c30, 0xf08d, 0x4ab4, {0x90, 0x7c, 0x34, 0x90, 0x5d, 0x77, 0x0c, 0x7d}} ;
// MsRdpClient9の公開CLSID。生成失敗のsecurity回避には使わない。
constexpr CLSID kRdpClient9ClassId{
	0x8b918b82, 0x7985, 0x4c24, {0x89, 0xdf, 0xc3, 0x3a, 0xd2, 0xbb, 0xfb, 0xcd}} ;
// MsRdpClient8の公開CLSID。OS登録済みControlだけを生成する。
constexpr CLSID kRdpClient8ClassId{
	0xa3bc03a0, 0x041d, 0x42e3, {0xad, 0x22, 0x88, 0x2b, 0x78, 0x65, 0xc9, 0xc5}} ;
// MsRdpClient7の公開CLSID。候補下限は従来どおり維持する。
constexpr CLSID kRdpClient7ClassId{
	0x54d38bf7, 0xb1ef, 0x4479, {0x96, 0x74, 0x1b, 0xd6, 0xea, 0x46, 0x52, 0x58}} ;

/**
 * @brief 利用可能性を新しい版から確認するRDP ActiveX候補。
 */
struct RdpControlCandidate {
	/// Windows登録済みクラスを指定するCLSID。
	const CLSID		 *class_id ;
	/// UI表示用の固定Control名。
	std::wstring_view name ;
	/// 秘密情報を含まない診断用の版番号。
	unsigned int	  version ;
} ;

/**
 * @brief 対応するMicrosoft RDP ActiveX候補を新しい順で返す。
 * @return プロセス全体で共有する固定候補一覧。
 */
[[nodiscard]] const std::array<RdpControlCandidate, 6> &ControlCandidates() noexcept
{
	static const std::array<RdpControlCandidate, 6> candidates{{
		{&kRdpClient12ClassId, L"MsRdpClient12", 12U},
		{&kRdpClient11ClassId, L"MsRdpClient11", 11U},
		{&kRdpClient10ClassId, L"MsRdpClient10", 10U},
		{&kRdpClient9ClassId, L"MsRdpClient9", 9U},
		{&kRdpClient8ClassId, L"MsRdpClient8", 8U},
		{&kRdpClient7ClassId, L"MsRdpClient7", 7U},
	}} ;
	return candidates ;
}

// ---------------------------------------------------------------------------
// Automationイベント引数の検証
// ---------------------------------------------------------------------------

/**
 * @brief 宣言順の位置から逆順格納されたAutomation引数を取得する。
 * @param[in] parameters COMから渡された引数列。
 * @param[in] declared_index イベント宣言上の0始まり位置。
 * @return 対応VARIANTへの非所有ポインター。不正位置ではnullptr。
 */
[[nodiscard]] const VARIANTARG *GetArgument( const DISPPARAMS *parameters,
											const UINT		  declared_index) noexcept
{
	// COMはIDispatch引数を宣言順と逆向きに格納する
	if( parameters == nullptr || declared_index >= parameters->cArgs) {
		return nullptr ;
	}

	return &parameters->rgvarg[parameters->cArgs - declared_index - 1U] ;
}

/**
 * @brief callbackの引数個数がイベント定義と一致するか検証する。
 * @param[in] parameters COMから渡された引数列。
 * @param[in] expected_count 期待する引数個数。
 * @return 完全一致する場合はtrue。
 */
[[nodiscard]] bool HasArgumentCount( const DISPPARAMS *parameters,
									const UINT		  expected_count) noexcept
{
	// 破損callbackを範囲外VARIANTとして解釈しない
	if( parameters == nullptr) {
		return false ;
	}

	return parameters->cArgs == expected_count ;
}

/**
 * @brief 許可した整数VARIANTを符号付きlongへ読み取る。
 * @param[in] argument 読み取る非所有VARIANT。
 * @param[out] value 検証成功時の整数値。
 * @return 型と範囲が有効な場合はtrue。
 */
[[nodiscard]] bool ReadLong( const VARIANTARG *argument, long &value) noexcept
{
	// Microsoftイベント源が返す整数VARIANT形式だけを受け入れる
	if( argument == nullptr) {
		return false ;
	}

	// COM eventが32-bit整数値を直接持つ場合は変換せずcodeを取得する
	if( argument->vt == VT_I4) {
		value = argument->lVal ;
		return true ;
	}

	// AutomationのINT値も同じevent codeとして受理する
	if( argument->vt == VT_INT) {
		value = argument->intVal ;
		return true ;
	}

	// 符号なし値はlongへ狭めてもoverflowしない範囲だけを受け入れる
	if( argument->vt == VT_UI4 &&
	   argument->ulVal <= static_cast<unsigned long>( std::numeric_limits<long>::max())) {
		value = static_cast<long>( argument->ulVal) ;
		return true ;
	}

	return false ;
}

/**
 * @brief AutomationのVARIANT_BOOLをboolへ読み取る。
 * @param[in] argument 読み取る非所有VARIANT。
 * @param[out] value 検証成功時の真偽値。
 * @return 型がVT_BOOLの場合はtrue。
 */
[[nodiscard]] bool ReadBoolean( const VARIANTARG *argument, bool &value) noexcept
{
	// 真偽値callbackはAutomationのVARIANT_BOOL契約を厳密に維持する
	if( argument == nullptr || argument->vt != VT_BOOL) {
		return false ;
	}

	value = argument->boolVal != VARIANT_FALSE ;
	return true ;
}

/**
 * @brief 値渡しまたは参照渡しのBSTRを非所有ポインターとして読み取る。
 * @param[in] argument 読み取る非所有VARIANT。
 * @param[out] value callback中だけ有効なCOM所有BSTR。
 * @return 対応するBSTR形式の場合はtrue。
 */
[[nodiscard]] bool ReadBstr( const VARIANTARG *argument, BSTR &value) noexcept
{
	// BSTRの所有権はCOMに残るため、呼び出し側はInvoke復帰前に必ずコピーする
	if( argument == nullptr) {
		return false ;
	}

	// 直接BSTRの場合はcallback中だけ有効な非所有値を取得する
	if( argument->vt == VT_BSTR) {
		value = argument->bstrVal ;
		return true ;
	}

	// by-reference BSTRは有効な参照先がある場合だけ読み出す
	if( argument->vt == (VT_BSTR | VT_BYREF) && argument->pbstrVal != nullptr) {
		value = *argument->pbstrVal ;
		return true ;
	}

	return false ;
}

/**
 * @brief 参照渡しのAutomation真偽値へ結果を書き戻す。
 * @param[in,out] argument 書込先VARIANT。
 * @param[in] value 書き戻すVARIANT_BOOL。
 * @return 型と出力先が有効な場合はtrue。
 */
[[nodiscard]] bool SetBooleanByReference( const VARIANTARG  *argument,
										 const VARIANT_BOOL value) noexcept
{
	// OnConfirmCloseは参照渡しAutomation真偽値への明示応答を要求する
	if( argument == nullptr || argument->vt != (VT_BOOL | VT_BYREF) ||
	   argument->pboolVal == nullptr) {
		return false ;
	}

	*argument->pboolVal = value ;
	return true ;
}

/**
 * @brief 参照渡しのlongへ再接続継続方針を書き戻す。
 * @param[in,out] argument 書込先VARIANT。
 * @param[in] value 書き戻すlong値。
 * @return 型と出力先が有効な場合はtrue。
 */
[[nodiscard]] bool SetLongByReference( const VARIANTARG *argument, const long value) noexcept
{
	// 旧再接続イベントは継続状態を参照渡しLONGとして公開する
	if( argument == nullptr || argument->vt != (VT_I4 | VT_BYREF) || argument->plVal == nullptr) {
		return false ;
	}

	*argument->plVal = value ;
	return true ;
}

/**
 * @brief Virtual Channelイベントを検証し、所有RdpEventへ変換する。
 * @param[in] parameters COMから渡されたイベント引数。
 * @param[out] event 検証済みチャネル名とbinary payloadを所有するイベント。
 * @return 成功時はS_OK、個数・型・長さの不正時は対応するDISP_E_*。
 */
[[nodiscard]] HRESULT BuildChannelEvent( const DISPPARAMS *parameters, RdpEvent &event)
{
	// チャネル名とデータは、このイベントで両方必須とする
	if( !HasArgumentCount( parameters, 2U)) {
		return DISP_E_BADPARAMCOUNT ;
	}

	BSTR channel_name = nullptr ;
	BSTR channel_data = nullptr ;
	// 型が一致しないcallbackをBSTRとして参照しない
	if( !ReadBstr( GetArgument( parameters, 0U), channel_name) ||
	   !ReadBstr( GetArgument( parameters, 1U), channel_data)) {
		return DISP_E_TYPEMISMATCH ;
	}

	const UINT channel_name_length = channel_name == nullptr ? 0U : SysStringLen( channel_name) ;
	const UINT channel_data_length = channel_data == nullptr ? 0U : SysStringByteLen( channel_data) ;

	// callback一回でUIスレッドのheapを無制限に増加させない
	if( channel_name_length > kMaximumChannelNameCharacters ||
	   channel_data_length > kMaximumChannelPayloadBytes) {
		return DISP_E_OVERFLOW ;
	}

	event.kind = RdpEventKind::ChannelDataReceived ;
	// 空名は空文字列のまま保持し、nullptrに対するポインター演算を避ける
	if( channel_name_length > 0U) {
		event.channel_name.assign( channel_name, channel_name + channel_name_length) ;
	}

	event.channel_payload.resize( channel_data_length) ;
	// BSTRを文字列扱いせず、埋め込みNULを含むbyte列として所有コピーする
	if( channel_data_length > 0U) {
		std::memcpy( event.channel_payload.data(), static_cast<const void *>( channel_data),
					channel_data_length) ;
	}

	return S_OK ;
}

// ---------------------------------------------------------------------------
// ActiveXイベントのCOM境界
// ---------------------------------------------------------------------------

/**
 * @brief 例外とCOM所有BSTRをActiveX callback境界から越境させないevent sink。
 *
 * COM参照カウントで自己所有し、検証済みRdpEventだけをobserverへ渡す。
 */
class RdpEventSink final : public IDispatch {
public:
	/**
	 * @brief 初期参照を一つ持つCOM所有event sinkを生成する。
	 * @param[in] observer 所有コピー化したイベントの通知先。
	 * @param[out] event_sink 呼び出し側がReleaseするIDispatch参照。
	 * @return 生成結果を表すHRESULT。
	 */
	[[nodiscard]] static HRESULT Create( IRdpEventObserver &observer,
										IDispatch		 **event_sink) noexcept
	{
		// 呼び出し側へは所有参照一つ、またはnullptrのどちらかだけを返す
		if( event_sink == nullptr) {
			return E_POINTER ;
		}

		*event_sink = nullptr ;
		auto *sink	= new( std::nothrow) RdpEventSink( observer) ;
		// COM生成境界ではallocation例外を使わずHRESULTへ変換する
		if( sink == nullptr) {
			return E_OUTOFMEMORY ;
		}

		*event_sink = sink ;
		return S_OK ;
	}

	/**
	 * @brief IUnknown、IDispatch、IMsTscAxEventsの同一性を提供する。
	 * @param[in] interface_id 要求されたinterface ID。
	 * @param[out] object 成功時にAddRef済みinterfaceを受け取る。
	 * @return 対応結果を表すHRESULT。
	 */
	HRESULT STDMETHODCALLTYPE QueryInterface( REFIID interface_id, void **object) noexcept override
	{
		// QueryInterfaceから未初期化出力ポインターを返さない
		if( object == nullptr) {
			return E_POINTER ;
		}

		*object = nullptr ;
		// event sinkの公開interfaceだけを許可し、同じCOM所有者の参照を追加する
		if( IsEqualIID( interface_id, IID_IUnknown) || IsEqualIID( interface_id, IID_IDispatch) ||
		   IsEqualIID( interface_id, DIID_IMsTscAxEvents)) {
			*object = static_cast<IDispatch *>( this) ;
			AddRef() ;
			return S_OK ;
		}

		return E_NOINTERFACE ;
	}

	/**
	 * @brief COM所有者を一つ追加し、追加後の参照数を返す。
	 * @return 追加後のCOM参照数。
	 */
	ULONG STDMETHODCALLTYPE AddRef() noexcept override
	{
		return reference_count_.fetch_add( 1U, std::memory_order_relaxed) + 1U ;
	}

	/**
	 * @brief COM所有者を一つ解放し、最後の参照で自身を破棄する。
	 * @return 解放後のCOM参照数。0の場合は自身を破棄済み。
	 */
	ULONG STDMETHODCALLTYPE Release() noexcept override
	{
		const ULONG remaining = reference_count_.fetch_sub( 1U, std::memory_order_acq_rel) - 1U ;

		// すべての所有者が参照を解放した時点だけでCOM objectの寿命を終える
		if( remaining == 0U) {
			delete this ;
		}

		return remaining ;
	}

	/**
	 * @brief type informationを公開しないことをAutomationへ通知する。
	 * @param[out] type_info_count 常に0を受け取る。
	 * @return 出力先が有効ならS_OK。
	 */
	HRESULT STDMETHODCALLTYPE GetTypeInfoCount( UINT *type_info_count) noexcept override
	{
		// このsinkはDISPIDだけを使用し、dispatch可能なtype informationを公開しない
		if( type_info_count == nullptr) {
			return E_POINTER ;
		}

		*type_info_count = 0U ;
		return S_OK ;
	}

	/**
	 * @brief type information非対応をE_NOTIMPLで返す。
	 * @return 常にE_NOTIMPL。
	 * @note index、locale、出力先はCOM署名維持のため無名とし、参照・書換しない。
	 */
	HRESULT STDMETHODCALLTYPE GetTypeInfo( UINT, LCID, ITypeInfo **) noexcept override
	{
		return E_NOTIMPL ;
	}

	/**
	 * @brief 名前解決非対応をE_NOTIMPLで返し、DISPID定義だけを使用する。
	 * @return 常にE_NOTIMPL。
	 * @note interface、名前列、件数、locale、DISPID出力先は使用・書換しない。
	 */
	HRESULT STDMETHODCALLTYPE GetIDsOfNames( REFIID, LPOLESTR *, UINT, LCID,
											DISPID *) noexcept override
	{
		return E_NOTIMPL ;
	}

	/**
	 * @brief Microsoft ActiveXイベントを検証してRdpEventへdispatchする。
	 * @param[in] dispatch_id 発生したイベントのDISPID。
	 * @param[in] interface_id dispinterfaceではIID_NULLでなければならない。
	 * @param[in,out] parameters COM所有のイベント引数。
	 * @return dispatch結果。C++例外はE_UNEXPECTEDへ変換する。
	 * @note 無名のlocaleとflagsは使用せず、戻り値・例外・引数errorの出力先は書換しない。
	 */
	HRESULT STDMETHODCALLTYPE Invoke( DISPID dispatch_id, REFIID interface_id, LCID, WORD,
									 DISPPARAMS *parameters, VARIANT *, EXCEPINFO *,
									 UINT *) noexcept override
	{
		// このdispinterfaceのIDispatch::InvokeはIID_NULLだけを受け入れる
		if( !IsEqualIID( interface_id, IID_NULL)) {
			return DISP_E_UNKNOWNINTERFACE ;
		}

		try {
			const HRESULT result = DispatchEvent( dispatch_id, parameters) ;
			// 不正callbackは内容を残さず、固定イベントと数値codeだけを診断する
			if( FAILED( result)) {
				common::WriteDiagnosticLog( common::DiagnosticLevel::Warning,
										   common::DiagnosticEvent::EventDispatchFailed,
										   {
											   common::DiagnosticHResult( result),
											   common::DiagnosticCode( dispatch_id),
										   }) ;
			}

			return result ;
		} catch( ...) {
			common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
									   common::DiagnosticEvent::EventDispatchFailed,
									   {
										   common::DiagnosticHResult( E_UNEXPECTED),
										   common::DiagnosticCode( dispatch_id),
									   }) ;
			// C++例外をCOM callback vtableから決して漏らさない
			return E_UNEXPECTED ;
		}
	}

private:
	/**
	 * @brief observerは外部所有で、sinkより長く生存する。
	 * @param[in] observer callbackの所有copyを受け取る非所有通知先。sinkより長く生存する。
	 */
	explicit RdpEventSink( IRdpEventObserver &observer) noexcept : observer_( observer)
	{
	}

	/**
	 * @brief Release()から最後の参照時にだけ破棄される。
	 */
	~RdpEventSink() = default ;

	/**
	 * @brief 引数なしイベントを検証して通知する。
	 * @param[in] parameters COM引数列。
	 * @param[in] kind 生成する内部イベント種別。
	 * @return 検証・通知結果。
	 */
	[[nodiscard]] HRESULT DispatchSimpleEvent( const DISPPARAMS *parameters, const RdpEventKind kind)
	{
		// 引数なしDISPIDへ予期しないデータを付加したcallbackを受け入れない
		if( !HasArgumentCount( parameters, 0U)) {
			return DISP_E_BADPARAMCOUNT ;
		}

		RdpEvent event{} ;
		event.kind = kind ;
		observer_.OnRdpEvent( std::move( event)) ;
		return S_OK ;
	}

	/**
	 * @brief 符号付きcode一つを持つイベントを検証して通知する。
	 * @param[in] parameters COM引数列。
	 * @param[in] kind 生成する内部イベント種別。
	 * @return 検証・通知結果。
	 */
	[[nodiscard]] HRESULT DispatchCodeEvent( const DISPPARAMS *parameters, const RdpEventKind kind)
	{
		// errorとdisconnectイベントは符号付きcodeを正確に一つだけ持つ
		if( !HasArgumentCount( parameters, 1U)) {
			return DISP_E_BADPARAMCOUNT ;
		}

		RdpEvent event{} ;
		event.kind = kind ;
		// 異なるVARIANT型をerror codeとして解釈しない
		if( !ReadLong( GetArgument( parameters, 0U), event.code)) {
			return DISP_E_TYPEMISMATCH ;
		}

		observer_.OnRdpEvent( std::move( event)) ;
		return S_OK ;
	}

	/**
	 * @brief 旧形式の自動再接続イベントを検証して通知する。
	 * @param[in,out] parameters reason、試行回数、継続状態を持つCOM引数列。
	 * @return 検証・通知結果。
	 */
	[[nodiscard]] HRESULT DispatchAutoReconnecting( const DISPPARAMS *parameters)
	{
		// 旧イベントはreason、試行回数、入出力の継続状態を持つ
		if( !HasArgumentCount( parameters, 3U)) {
			return DISP_E_BADPARAMCOUNT ;
		}

		RdpEvent event{} ;
		event.kind = RdpEventKind::AutoReconnecting ;
		// 全引数を検証できない場合は部分的な再接続状態を通知しない
		if( !ReadLong( GetArgument( parameters, 0U), event.code) ||
		   !ReadLong( GetArgument( parameters, 1U), event.attempt_count) ||
		   !SetLongByReference( GetArgument( parameters, 2U), 0L)) {
			return DISP_E_TYPEMISMATCH ;
		}

		observer_.OnRdpEvent( std::move( event)) ;
		return S_OK ;
	}

	/**
	 * @brief 拡張形式の自動再接続イベントを検証して通知する。
	 * @param[in] parameters reason、network、試行回数、最大回数を持つCOM引数列。
	 * @return 検証・通知結果。
	 */
	[[nodiscard]] HRESULT DispatchAutoReconnecting2( const DISPPARAMS *parameters)
	{
		// 拡張イベントはreason、network状態、試行回数、最大回数を持つ
		if( !HasArgumentCount( parameters, 4U)) {
			return DISP_E_BADPARAMCOUNT ;
		}

		RdpEvent event{} ;
		event.kind = RdpEventKind::AutoReconnecting ;
		// 全引数を検証できない場合は部分的な再接続状態を通知しない
		if( !ReadLong( GetArgument( parameters, 0U), event.code) ||
		   !ReadBoolean( GetArgument( parameters, 1U), event.network_available) ||
		   !ReadLong( GetArgument( parameters, 2U), event.attempt_count) ||
		   !ReadLong( GetArgument( parameters, 3U), event.max_attempt_count)) {
			return DISP_E_TYPEMISMATCH ;
		}

		observer_.OnRdpEvent( std::move( event)) ;
		return S_OK ;
	}

	/**
	 * @brief UIで確認済みの終了要求をOnConfirmCloseへ応答する。
	 * @param[in,out] parameters 許可結果の参照渡しVARIANTを持つCOM引数列。
	 * @return 検証・通知結果。
	 */
	[[nodiscard]] HRESULT DispatchConfirmClose( const DISPPARAMS *parameters)
	{
		// MainWindowがRequestClose前に確認済みなので、このcallbackでは許可を返す
		if( !HasArgumentCount( parameters, 1U) ||
		   !SetBooleanByReference( GetArgument( parameters, 0U), VARIANT_TRUE)) {
			return DISP_E_TYPEMISMATCH ;
		}

		RdpEvent event{} ;
		event.kind = RdpEventKind::ConfirmClose ;
		observer_.OnRdpEvent( std::move( event)) ;
		return S_OK ;
	}

	/**
	 * @brief Virtual Channelデータを所有イベントへ変換して通知する。
	 * @param[in] parameters チャネル名とbinary payloadを持つCOM引数列。
	 * @return 検証・通知結果。
	 */
	[[nodiscard]] HRESULT DispatchChannelData( const DISPPARAMS *parameters)
	{
		RdpEvent	  event{} ;
		const HRESULT result = BuildChannelEvent( parameters, event) ;
		// 不正な長さ・型のchannel callbackをobserverへ渡さない
		if( FAILED( result)) {
			return result ;
		}

		observer_.OnRdpEvent( std::move( event)) ;
		return S_OK ;
	}

	/**
	 * @brief DISPIDを対応する検証処理へ振り分ける。
	 * @param[in] dispatch_id 発生したMicrosoftイベントのDISPID。
	 * @param[in] parameters COM引数列。
	 * @return 既知イベントの処理結果。未知の任意イベントはS_OKで無視する。
	 */
	[[nodiscard]] HRESULT DispatchEvent( const DISPID dispatch_id, const DISPPARAMS *parameters)
	{
		// Microsoft eventのDISPIDを固定のRdpEvent契約へ振り分ける
		switch( dispatch_id) {
		// 接続開始eventは引数なしの状態通知へ変換する
		case kDispIdOnConnecting:
			return DispatchSimpleEvent( parameters, RdpEventKind::Connecting) ;
		// transport接続成立eventはlogin完了と分けて通知する
		case kDispIdOnConnected:
			return DispatchSimpleEvent( parameters, RdpEventKind::Connected) ;
		// login完了eventを標準認証成立の通知へ変換する
		case kDispIdOnLoginComplete:
			return DispatchSimpleEvent( parameters, RdpEventKind::LoginComplete) ;
		// 切断eventは理由codeを検証して通知する
		case kDispIdOnDisconnected:
			return DispatchCodeEvent( parameters, RdpEventKind::Disconnected) ;
		// 全画面開始eventをcontainer状態通知へ変換する
		case kDispIdOnEnterFullScreen:
			return DispatchSimpleEvent( parameters, RdpEventKind::EnterFullScreen) ;
		// Window復帰eventをcontainer状態通知へ変換する
		case kDispIdOnLeaveFullScreen:
			return DispatchSimpleEvent( parameters, RdpEventKind::LeaveFullScreen) ;
		// channel受信eventはBSTRの明示byte長で所有copyを作る
		case kDispIdOnChannelReceivedData:
			return DispatchChannelData( parameters) ;
		// 全画面開始要求をcontainer側の操作要求へ変換する
		case kDispIdOnRequestGoFullScreen:
			return DispatchSimpleEvent( parameters, RdpEventKind::RequestGoFullScreen) ;
		// 全画面解除要求をcontainer側のWindow復帰要求へ変換する
		case kDispIdOnRequestLeaveFullScreen:
			return DispatchSimpleEvent( parameters, RdpEventKind::RequestLeaveFullScreen) ;
		// connection bar最小化要求をSession Windowへの通知へ変換する
		case kDispIdOnRequestContainerMinimize:
			return DispatchSimpleEvent( parameters, RdpEventKind::RequestContainerMinimize) ;
		// 致命的error eventは固定code付き失敗通知へ変換する
		case kDispIdOnFatalError:
			return DispatchCodeEvent( parameters, RdpEventKind::FatalError) ;
		// 警告eventは接続継続可否と区別するためcode付きで通知する
		case kDispIdOnWarning:
			return DispatchCodeEvent( parameters, RdpEventKind::Warning) ;
		// 終了確認eventではControlへ返す同意値を専用契約で扱う
		case kDispIdOnConfirmClose:
			return DispatchConfirmClose( parameters) ;
		// 旧再接続eventでは再試行情報を検証して通知する
		case kDispIdOnAutoReconnecting:
			return DispatchAutoReconnecting( parameters) ;
		// 標準認証警告の表示開始をfocus制御へ通知する
		case kDispIdOnAuthenticationWarningDisplayed:
			return DispatchSimpleEvent( parameters, RdpEventKind::AuthenticationWarningDisplayed) ;
		// 標準認証警告の解除をfocus制御へ通知する
		case kDispIdOnAuthenticationWarningDismissed:
			return DispatchSimpleEvent( parameters, RdpEventKind::AuthenticationWarningDismissed) ;
		// 自動再接続完了を既存user sessionへの復帰として通知する
		case kDispIdOnAutoReconnected:
			return DispatchSimpleEvent( parameters, RdpEventKind::AutoReconnected) ;
		// 拡張再接続eventは最新の引数契約に従って通知する
		case kDispIdOnAutoReconnecting2:
			return DispatchAutoReconnecting2( parameters) ;
		// 未処理のMicrosoft eventは状態を推測せず正常に無視する
		default:
			return S_OK ;
		}
	}

	/// COM規約に従うthread-safeな自己参照数。
	std::atomic<ULONG> reference_count_{1U} ;
	/// sinkより長く生存する内部イベント通知先。
	IRdpEventObserver &observer_ ;
} ;

/**
 * @brief 一つのIMsTscAxEvents connection-point cookieを所有するRAII型。
 */
class ConnectionPointSubscription final {
public:
	/**
	 * @brief 未購読状態を構築する。
	 */
	ConnectionPointSubscription() = default ;

	/**
	 * @brief 残っている購読を必ず解除する。
	 */
	~ConnectionPointSubscription()
	{
		Reset() ;
	}

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	ConnectionPointSubscription( const ConnectionPointSubscription &)			= delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	ConnectionPointSubscription &operator=( const ConnectionPointSubscription &) = delete ;

	/**
	 * @brief event sourceとsinkを接続し、解除cookieを所有する。
	 * @param[in] source IMsTscAxEventsを公開するActiveX。
	 * @param[in] sink callbackを受け取るCOM sink。
	 * @return 購読結果を表すHRESULT。
	 */
	[[nodiscard]] HRESULT		 Advise( IUnknown *source, IUnknown *sink) noexcept
	{
		// cookieを先に解放せず、既存購読を別購読で置き換えない
		if( advised_) {
			return HRESULT_FROM_WIN32( ERROR_ALREADY_INITIALIZED) ;
		}

		// connection pointを検索する前に、両COM endpointの有効性を確認する
		if( source == nullptr || sink == nullptr) {
			return E_POINTER ;
		}

		ATL::CComQIPtr<IConnectionPointContainer> container( source) ;
		// event sourceがconnection pointを提供しない場合は購読不能とする
		if( container == nullptr) {
			return E_NOINTERFACE ;
		}

		HRESULT result = container->FindConnectionPoint( DIID_IMsTscAxEvents, &connection_point_) ;
		// 対象dispinterfaceが見つからない場合はAdviseへ進まない
		if( FAILED( result)) {
			return result ;
		}

		result = connection_point_->Advise( sink, &cookie_) ;
		// Advise失敗時は部分的に取得したinterfaceとcookieを破棄する
		if( FAILED( result)) {
			connection_point_.Release() ;
			cookie_ = 0U ;
			return result ;
		}

		advised_ = true ;
		return S_OK ;
	}

	/**
	 * @brief 購読中ならUnadviseし、保持interfaceとcookieを初期化する。
	 * @return Unadviseの結果。未購読時はS_OK。
	 */
	HRESULT Reset() noexcept
	{
		HRESULT result = S_OK ;
		// 有効なcookieとconnection pointの組がある場合だけ解除を要求する
		if( advised_ && connection_point_ != nullptr) {
			result = connection_point_->Unadvise( cookie_) ;
		}

		advised_ = false ;
		cookie_	 = 0U ;
		connection_point_.Release() ;
		return result ;
	}

private:
	/// 購読解除まで保持するconnection point。
	ATL::CComPtr<IConnectionPoint> connection_point_ ;
	/// Adviseが返した解除用cookie。
	DWORD						   cookie_{0U} ;
	/// cookieが有効であることを示す。
	bool						   advised_{false} ;
} ;

/**
 * @brief UI文字列に埋め込みNULが含まれるか検査する。
 * @param[in] value 検査するUTF-16文字列。
 * @return NULを含む場合はtrue。
 */
[[nodiscard]] bool HasEmbeddedNull( const std::wstring &value) noexcept
{
	return value.find( L'\0') != std::wstring::npos ;
}

/**
 * @brief std::wstringを長さ明示の所有BSTRへ変換する。
 * @param[in] value 変換元のUTF-16文字列。
 * @param[out] output 成功時に所有権を受け取るCComBSTR。
 * @return 変換結果を表すHRESULT。
 */
[[nodiscard]] HRESULT MakeBstr( const std::wstring &value, ATL::CComBSTR &output) noexcept
{
	// SysAllocStringLenのUINT長へ安全に変換できない入力を拒否する
	if( value.size() > static_cast<std::size_t>( std::numeric_limits<UINT>::max())) {
		return HRESULT_FROM_WIN32( ERROR_ARITHMETIC_OVERFLOW) ;
	}

	// 空の任意項目はnull BSTRのまま許可し、不要なallocationを行わない
	if( value.empty()) {
		output.Empty() ;
		return S_OK ;
	}

	BSTR candidate = SysAllocStringLen( value.data(), static_cast<UINT>( value.size())) ;
	// BSTRを確保できなければ不完全な文字列をActiveXへ渡さない
	if( candidate == nullptr) {
		return E_OUTOFMEMORY ;
	}

	output.Attach( candidate) ;
	return S_OK ;
}

/**
 * @brief 製品名と検証済み接続先を持つ全画面表示用BSTRを構築する。
 * @param[in] server 接続先PC名またはIP。事前に有限長とNUL不在を検証すること。
 * @param[in] maximum_character_count ActiveX propertyが許可する最大文字数。
 * @param[out] output connection barと全画面titleへ渡す所有BSTR。
 * @return 接続先を先頭から表示する有限長文字列を構築できた場合はS_OK。
 *
 * 接続先は利用者の画面へだけ表示し、診断logへ渡さない。ActiveX propertyの
 * 上限を超える場合は接続先設定を変更せず、表示文字列だけを末尾省略する。
 */
[[nodiscard]] HRESULT MakeFullScreenConnectionLabel( const std::wstring &server,
													const std::size_t	maximum_character_count,
													ATL::CComBSTR	   &output) noexcept
{
	// prefixと省略記号すら収まらないproperty上限を暗黙に受け入れない
	if( maximum_character_count <
	   kFullScreenConnectionLabelPrefix.size() + kTruncatedConnectionLabelSuffix.size()) {
		return E_INVALIDARG ;
	}

	// SysAllocStringLenのUINT長へ安全に変換できないproperty上限を拒否する
	if( maximum_character_count > static_cast<std::size_t>( std::numeric_limits<UINT>::max())) {
		return HRESULT_FROM_WIN32( ERROR_ARITHMETIC_OVERFLOW) ;
	}

	const std::size_t maximum_server_characters =
		maximum_character_count - kFullScreenConnectionLabelPrefix.size() ;
	const bool		  is_truncated = server.size() > maximum_server_characters ;
	const std::size_t server_character_count =
		is_truncated ? maximum_server_characters - kTruncatedConnectionLabelSuffix.size()
					 : server.size() ;
	const std::size_t suffix_character_count =
		is_truncated ? kTruncatedConnectionLabelSuffix.size() : 0U ;
	const std::size_t character_count =
		kFullScreenConnectionLabelPrefix.size() + server_character_count + suffix_character_count ;

	BSTR label = SysAllocStringLen( nullptr, static_cast<UINT>( character_count)) ;
	// 全画面識別labelの確保失敗では接続先表示を省いた接続を開始しない
	if( label == nullptr) {
		return E_OUTOFMEMORY ;
	}

	// BSTRの明示長を維持したまま、固定prefixと接続先を順にコピーする
	std::memcpy( label, kFullScreenConnectionLabelPrefix.data(),
				kFullScreenConnectionLabelPrefix.size() * sizeof( wchar_t)) ;
	std::memcpy( label + kFullScreenConnectionLabelPrefix.size(), server.data(),
				server_character_count * sizeof( wchar_t)) ;
	// 省略時だけmarkerを付け、画面表示と実際の接続先を混同させない
	if( is_truncated) {
		std::memcpy( label + kFullScreenConnectionLabelPrefix.size() + server_character_count,
					kTruncatedConnectionLabelSuffix.data(),
					suffix_character_count * sizeof( wchar_t)) ;
	}

	label[character_count] = L'\0' ;

	output.Attach( label) ;
	return S_OK ;
}

} // namespace

bool IsCredentialPromptCancellation( const RdpConnectionState state_before_disconnect,
									const long				 disconnect_reason,
									const long				 extended_disconnect_reason,
									const bool				 locally_requested_disconnect) noexcept
{
	const bool before_login = state_before_disconnect == RdpConnectionState::Connecting ||
							  state_before_disconnect == RdpConnectionState::Connected ;
	return !locally_requested_disconnect && before_login &&
		   disconnect_reason == kCredentialPromptCancellationDisconnectReason &&
		   extended_disconnect_reason == kNoExtendedDisconnectReason ;
}

// ---------------------------------------------------------------------------
// ActiveX hostの実装と接続状態機械
// ---------------------------------------------------------------------------

/**
 * @brief ActiveX子ウィンドウ、COM interface、購読、接続状態を一括所有する。
 *
 * 構築スレッドIDを記録し、全変更操作を同じCOM STAへ制限する。外側observer
 * へ渡す前に内部状態を更新し、UIが常にcallback後の状態を参照できるようにする。
 */
class RdpControlHost::Impl final : public IRdpEventObserver {
public:
	/**
	 * @brief 未生成のActiveX hostを構築する。
	 * @param[in] observer 所有コピー化したRDPイベントの最終通知先。
	 */
	explicit Impl( IRdpEventObserver &observer) noexcept
		: observer_( observer), ui_thread_id_( GetCurrentThreadId())
	{
	}

	/**
	 * @brief 生成元STA上で購読、COM参照、子ウィンドウを順に解放する。
	 */
	~Impl() override
	{
		Destroy() ;
	}

	/**
	 * @brief 利用可能な最新ActiveXを子ウィンドウへ生成してイベント購読する。
	 * @param[in] parent 所有元のトップレベルウィンドウ。
	 * @param[in] bounds 初期表示領域。
	 * @return 完全に利用可能なhostを生成した場合はS_OK。
	 */
	[[nodiscard]] HRESULT Create( HWND parent, const RECT &bounds) noexcept
	{
		const HRESULT thread_result = EnsureUiThread() ;
		// ActiveX生成をCOM初期化済みの所有STA以外で実行しない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		// 初期化済みControlの再利用は古いevent購読と接続状態を残すため禁止する
		if( state_ != RdpConnectionState::NotInitialized) {
			return HRESULT_FROM_WIN32( ERROR_ALREADY_INITIALIZED) ;
		}

		// ActiveX containerには有効な親HWNDと正の矩形が必要である
		if( !IsWindow( parent) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
			return E_INVALIDARG ;
		}

		HRESULT last_result = REGDB_E_CLASSNOTREG ;

		// 既知Controlを新しい版から調べ、クラス未登録の場合だけ旧版へ下げる
		for( const auto &candidate : ControlCandidates()) {
			HRESULT result = CreateHostWindow( parent, bounds) ;
			// 子ウィンドウ生成失敗はControl版に依存しないため直ちに返す
			if( FAILED( result)) {
				return result ;
			}

			result = CreateControl( candidate) ;
			// 最初に生成成功した最新Controlを選択し、以降の候補探索を終える
			if( SUCCEEDED( result)) {
				selected_control_name_.assign( candidate.name) ;
				common::WriteDiagnosticLog(
					common::DiagnosticLevel::Information,
					common::DiagnosticEvent::ActiveXControlSelected,
					{
						common::DiagnosticValue( common::DiagnosticFieldName::ControlVersion,
												candidate.version),
					}) ;
				last_result = S_OK ;
				break ;
			}

			last_result = result ;
			DestroyHostWindow() ;

			// 存在するControlの失敗を旧版への暗黙切替で迂回しない
			if( result != REGDB_E_CLASSNOTREG && result != CLASS_E_CLASSNOTAVAILABLE) {
				return result ;
			}
		}

		// 対応Microsoft RDP ActiveXが一つも完全生成できなければfail-closedとする
		if( FAILED( last_result) || control_ == nullptr || client_ == nullptr) {
			DestroyHostWindow() ;
			return FAILED( last_result) ? last_result : E_NOINTERFACE ;
		}

		// credential UIと後続certificate UIのmodal ownershipを、同じ
		// ActiveX Control Windowへ一貫して委譲する
		common::WriteDiagnosticLog( common::DiagnosticLevel::Information,
								   common::DiagnosticEvent::ActiveXDefaultDialogParentSelected) ;

		ATL::CComPtr<IDispatch> event_sink ;
		HRESULT					result = RdpEventSink::Create( this_as_observer(), &event_sink) ;
		// event sinkを生成できなければ状態追跡不能なのでhost全体を破棄する
		if( FAILED( result)) {
			Destroy() ;
			return result ;
		}

		result = event_subscription_.Advise( control_, event_sink) ;
		// 購読失敗時はeventなしで接続可能な状態を作らない
		if( FAILED( result)) {
			Destroy() ;
			return result ;
		}

		event_sink_ = event_sink ;
		state_		= RdpConnectionState::Disconnected ;
		return S_OK ;
	}

	/**
	 * @brief 接続前に非機密設定とWindows標準セキュリティ方針を適用する。
	 * @param[in] settings 接続先、logon、desktop、表示、音声の接続前設定。
	 * @return 全設定と必須readbackに成功した場合はS_OK。
	 *
	 * passwordは受け取らず、Microsoftの標準認証UIへ委ねる。設定途中で失敗した
	 * 場合はsettings_applied_を立てず、Connect()をfail-closedにする。
	 */
	[[nodiscard]] HRESULT ApplyConnectionSettings( const RdpConnectionSettings &settings) noexcept
	{
		const HRESULT thread_result = EnsureUiThread() ;
		// 接続前設定をActiveX所有STA以外から変更しない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		// このmethodのRDP propertyは未接続状態でだけ変更できる接続前設定である
		if( state_ != RdpConnectionState::Disconnected || client_ == nullptr) {
			return HRESULT_FROM_WIN32( ERROR_INVALID_STATE) ;
		}

		// 再設定途中の失敗で以前の成功flagを再利用しないよう、検証前に無効化する
		settings_applied_ = false ;

		// 接続先は有限長かつ必須とし、NUL後へ意図しない文字列を隠せないようにする
		if( settings.server.empty() || settings.server.size() > kMaximumServerLength ||
		   HasEmbeddedNull( settings.server)) {
			return E_INVALIDARG ;
		}

		// logon識別子は省略可能だが有限長とし、埋込みNULを拒否する
		if( settings.domain.size() > kMaximumDomainLength ||
		   settings.user_name.size() > kMaximumUserNameLength || HasEmbeddedNull( settings.domain) ||
		   HasEmbeddedNull( settings.user_name)) {
			return E_INVALIDARG ;
		}

		// 専用modelではDomainとUserNameを分離し、曖昧な結合済み値を許可しない
		if( settings.domain.find( L'\\') != std::wstring::npos ||
		   settings.user_name.find( L'\\') != std::wstring::npos ||
		   (!settings.domain.empty() && settings.user_name.empty())) {
			return E_INVALIDARG ;
		}

		const std::size_t qualified_user_name_length =
			settings.domain.empty() ? settings.user_name.size()
									: settings.domain.size() + 1U + settings.user_name.size() ;
		// 履歴互換の有限logon識別子上限をActiveX境界でも同じく強制する
		if( qualified_user_name_length > kMaximumUserNameLength) {
			return E_INVALIDARG ;
		}

		// ActiveXへ渡すデスクトップ寸法を有限かつ実用的な範囲へ制限する
		if( settings.desktop_width < kMinimumDesktopDimension ||
		   settings.desktop_width > kMaximumDesktopDimension ||
		   settings.desktop_height < kMinimumDesktopDimension ||
		   settings.desktop_height > kMaximumDesktopDimension) {
			return E_INVALIDARG ;
		}

		// 未知のenum値をSmart Sizing無効として暗黙に受け入れない
		if( !IsValidDisplayMode( settings.display_mode)) {
			return E_INVALIDARG ;
		}

		// 未知の音声出力値をActiveX固有の数値として渡さない
		if( !IsValidAudioPlaybackMode( settings.audio.playback_mode)) {
			return E_INVALIDARG ;
		}

		// 検証済み文字列を、長さを保持するBSTRへ変換する
		ATL::CComBSTR server ;
		HRESULT		  result = MakeBstr( settings.server, server) ;
		// 接続先のBSTR変換失敗では接続前設定の適用を中止する
		if( FAILED( result)) {
			return result ;
		}

		ATL::CComBSTR user_name ;
		result = MakeBstr( settings.user_name, user_name) ;
		// UserNameの変換失敗では識別子を空欄へ読み替えて接続しない
		if( FAILED( result)) {
			return result ;
		}

		ATL::CComBSTR domain ;
		result = MakeBstr( settings.domain, domain) ;
		// Domainの変換失敗では前回のdomainを再利用しない
		if( FAILED( result)) {
			return result ;
		}

		ATL::CComBSTR full_screen_title ;
		result = MakeFullScreenConnectionLabel( settings.server, kMaximumFullScreenTitleCharacters,
											   full_screen_title) ;
		// 全画面titleの構築失敗では識別表示なしで接続を開始しない
		if( FAILED( result)) {
			return result ;
		}

		ATL::CComBSTR connection_bar_label ;
		result = MakeFullScreenConnectionLabel( settings.server, kMaximumFullScreenTitleCharacters,
											   connection_bar_label) ;
		// connection bar labelを構築できなければ設定の適用を中止する
		if( FAILED( result)) {
			return result ;
		}

		// Windows更新後も利用者が操作できる以前の標準security dialogを選択する
		result = ConfigureConnectionSecurityDialog() ;
		// S_FALSEはpropertyを持たない旧ActiveXであり、既定の旧dialogを維持する
		if( FAILED( result)) {
			return result ;
		}

		// 接続前の基本表示値をMicrosoft Controlへ順番に適用する
		result = client_->put_Server( server) ;
		// Server propertyの設定失敗では別の接続先へ進めない
		if( FAILED( result)) {
			return result ;
		}

		// 空値も毎回設定し、同じControlで前回domainを次接続へ持ち越さない
		result = client_->put_Domain( domain) ;
		// Domain propertyの設定失敗では既存domainを持ち越さない
		if( FAILED( result)) {
			return result ;
		}

		// DomainとUserNameを標準propertyへ分離してcredential UIへ渡す
		result = client_->put_UserName( user_name) ;
		// UserName propertyの設定失敗では標準認証UIの入力条件を偽装しない
		if( FAILED( result)) {
			return result ;
		}

		result = client_->put_DesktopWidth( settings.desktop_width) ;
		// 初期desktop幅を設定できなければ不明なbaselineで接続しない
		if( FAILED( result)) {
			return result ;
		}

		result = client_->put_DesktopHeight( settings.desktop_height) ;
		// 初期desktop高さを設定できなければ不明なアスペクト比で接続しない
		if( FAILED( result)) {
			return result ;
		}

		result = client_->put_ColorDepth( 32L) ;
		// 色深度設定の失敗では接続前設定が完了したと扱わない
		if( FAILED( result)) {
			return result ;
		}

		// 全画面windowにも接続先を表示し、複数sessionを識別可能にする
		result = client_->put_FullScreenTitle( full_screen_title) ;
		// 全画面titleを設定できなければsession識別を欠く接続を開始しない
		if( FAILED( result)) {
			return result ;
		}

		// Smart Sizingを含む追加設定interfaceがなければ対応Controlとして扱わない
		ATL::CComPtr<IMsRdpClientAdvancedSettings> advanced_settings ;
		result = client_->get_AdvancedSettings2( &advanced_settings) ;
		// 必要な追加設定interfaceがないControlは互換扱いで使用しない
		if( FAILED( result) || advanced_settings == nullptr) {
			return FAILED( result) ? result : E_NOINTERFACE ;
		}

		// 固定Win+Rと通常Windows shortcutの転送を接続前に設定・readbackする
		deployment_keyboard_configuration_result_ = DeploymentKeyboardSettings( true) ;
		LogDeploymentKeyboard( deployment_keyboard_configuration_result_) ;
		// keyboard設定の失敗は自動配置だけを停止し、標準RDP画面接続を維持する
		// 全画面shortcutとconnection bar操作をcontainer eventへ統一する
		constexpr long container_handled_full_screen_enabled = 1L ;
		result = advanced_settings->put_ContainerHandledFullScreen(
			container_handled_full_screen_enabled) ;
		// S_FALSEはevent通知非対応を意味するため、白いcontainerを残す接続を開始しない
		if( result != S_OK) {
			return FAILED( result) ? result : HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED) ;
		}

		long container_handled_full_screen = 0L ;
		result = advanced_settings->get_ContainerHandledFullScreen( &container_handled_full_screen) ;
		// container全画面処理のreadbackに失敗したら操作経路の一致を保証しない
		if( FAILED( result)) {
			return result ;
		}

		// COM実装は真値を1または-1で返し得るため、非0を有効として検証する
		if( container_handled_full_screen == 0L) {
			return E_UNEXPECTED ;
		}

		// container全画面でもMicrosoft connection barを明示的に利用する
		result = advanced_settings->put_DisplayConnectionBar( VARIANT_TRUE) ;
		// connection bar表示を有効化できなければ復帰操作なしで接続しない
		if( FAILED( result)) {
			return result ;
		}

		VARIANT_BOOL display_connection_bar = VARIANT_FALSE ;
		result = advanced_settings->get_DisplayConnectionBar( &display_connection_bar) ;
		// bar表示のreadbackが失敗または無効なら接続を中止する
		if( FAILED( result) || display_connection_bar == VARIANT_FALSE) {
			return FAILED( result) ? result : E_FAIL ;
		}

		// falseはbarをraised状態にし、復帰操作を画面上で利用可能にする
		result = advanced_settings->put_PinConnectionBar( VARIANT_FALSE) ;
		// raised状態への設定失敗ではbarを操作可能と扱わない
		if( FAILED( result)) {
			return result ;
		}

		VARIANT_BOOL connection_bar_lowered = VARIANT_TRUE ;
		result = advanced_settings->get_PinConnectionBar( &connection_bar_lowered) ;
		// barのraised状態をreadbackできなければ接続を中止する
		if( FAILED( result) || connection_bar_lowered != VARIANT_FALSE) {
			return FAILED( result) ? result : E_FAIL ;
		}

		ATL::CComQIPtr<IMsRdpClientAdvancedSettings3> connection_bar_settings( advanced_settings) ;
		// barの最小化・Window復帰buttonを制御できないControlは使用しない
		if( connection_bar_settings == nullptr) {
			return E_NOINTERFACE ;
		}

		// Windows標準同様にlocal desktopとWindow modeへの両復帰操作を公開する
		result = connection_bar_settings->put_ConnectionBarShowMinimizeButton( VARIANT_TRUE) ;
		// 最小化button設定の失敗ではlocal desktopへの復帰操作を保証しない
		if( FAILED( result)) {
			return result ;
		}

		result = connection_bar_settings->put_ConnectionBarShowRestoreButton( VARIANT_TRUE) ;
		// Window復帰button設定の失敗では全画面を開始しない
		if( FAILED( result)) {
			return result ;
		}

		VARIANT_BOOL show_minimize_button = VARIANT_FALSE ;
		result =
			connection_bar_settings->get_ConnectionBarShowMinimizeButton( &show_minimize_button) ;
		// 最小化buttonの有効化をreadbackできなければ接続を中止する
		if( FAILED( result) || show_minimize_button == VARIANT_FALSE) {
			return FAILED( result) ? result : E_FAIL ;
		}

		VARIANT_BOOL show_restore_button = VARIANT_FALSE ;
		result = connection_bar_settings->get_ConnectionBarShowRestoreButton( &show_restore_button) ;
		// Window復帰buttonの有効化をreadbackできなければ接続を中止する
		if( FAILED( result) || show_restore_button == VARIANT_FALSE) {
			return FAILED( result) ? result : E_FAIL ;
		}

		result = advanced_settings->put_SmartSizing(
			settings.display_mode == RdpDisplayMode::SmartSizing ? VARIANT_TRUE : VARIANT_FALSE) ;
		// Smart Sizing設定失敗では選択した表示modeが適用済みと扱わない
		if( FAILED( result)) {
			return result ;
		}

		// 動的解像度を選んだ接続は、接続後の更新APIを利用できるControlだけで開始する
		if( settings.display_mode == RdpDisplayMode::DynamicResolution) {
			ATL::CComQIPtr<IMsRdpClient9> display_client( client_) ;
			// 動的表示更新APIを持たないControlではWindow追従modeを開始しない
			if( display_client == nullptr) {
				return E_NOINTERFACE ;
			}
		}

		ATL::CComQIPtr<IMsRdpClientAdvancedSettings4> authentication_settings( advanced_settings) ;
		// サーバー認証警告の標準処理を設定できないControlは使用しない
		if( authentication_settings == nullptr) {
			return E_NOINTERFACE ;
		}

		// 認証は実施し、失敗時はMicrosoft警告を表示して利用者判断を求める
		constexpr unsigned int authentication_level_warn_on_failure = 2U ;
		result =
			authentication_settings->put_AuthenticationLevel( authentication_level_warn_on_failure) ;
		// サーバー認証設定の失敗は固定診断へ残し、認証を弱めず接続を中止する
		if( FAILED( result)) {
			common::WriteDiagnosticLog(
				common::DiagnosticLevel::Error,
				common::DiagnosticEvent::AuthenticationLevelConfigurationReturned,
				{
					common::DiagnosticHResult( result),
					common::DiagnosticValue( common::DiagnosticFieldName::AuthenticationLevel,
											authentication_level_warn_on_failure),
				}) ;
			return result ;
		}

		unsigned int authentication_level_readback = 0U ;
		result = authentication_settings->get_AuthenticationLevel( &authentication_level_readback) ;
		common::WriteDiagnosticLog(
			FAILED( result) || authentication_level_readback != authentication_level_warn_on_failure
				? common::DiagnosticLevel::Error
				: common::DiagnosticLevel::Information,
			common::DiagnosticEvent::AuthenticationLevelConfigurationReturned,
			{
				common::DiagnosticHResult( result),
				common::DiagnosticValue( common::DiagnosticFieldName::AuthenticationLevel,
										authentication_level_readback),
			}) ;
		// setterが受理されても警告なし／hard-failへ変化したControlは使用しない
		if( FAILED( result) ||
		   authentication_level_readback != authentication_level_warn_on_failure) {
			return FAILED( result) ? result : E_FAIL ;
		}

		ATL::CComQIPtr<IMsRdpClientAdvancedSettings6> credential_security_settings(
			advanced_settings) ;
		// 対応経路ではNLA対応のMicrosoft Controlを必須とする
		if( credential_security_settings == nullptr) {
			common::WriteDiagnosticLog(
				common::DiagnosticLevel::Error,
				common::DiagnosticEvent::CredSspConfigurationReturned,
				{
					common::DiagnosticHResult( E_NOINTERFACE),
					common::DiagnosticValue( common::DiagnosticFieldName::Enabled, 0U),
				}) ;
			return E_NOINTERFACE ;
		}

		// NLA必須サーバーがWindows標準経路を使うようCredSSPを明示的に有効化する
		result = credential_security_settings->put_EnableCredSspSupport( VARIANT_TRUE) ;
		// CredSSPを有効化できない場合は別認証経路へfallbackせず中止する
		if( FAILED( result)) {
			common::WriteDiagnosticLog(
				common::DiagnosticLevel::Error,
				common::DiagnosticEvent::CredSspConfigurationReturned,
				{
					common::DiagnosticHResult( result),
					common::DiagnosticValue( common::DiagnosticFieldName::Enabled, 0U),
				}) ;
			return result ;
		}

		// setter成功だけを信用せず、実際に有効化された値をreadbackする
		VARIANT_BOOL credssp_enabled = VARIANT_FALSE ;
		result = credential_security_settings->get_EnableCredSspSupport( &credssp_enabled) ;
		common::WriteDiagnosticLog(
			FAILED( result) || credssp_enabled == VARIANT_FALSE
				? common::DiagnosticLevel::Error
				: common::DiagnosticLevel::Information,
			common::DiagnosticEvent::CredSspConfigurationReturned,
			{
				common::DiagnosticHResult( result),
				common::DiagnosticValue( common::DiagnosticFieldName::Enabled,
										credssp_enabled != VARIANT_FALSE ? 1U : 0U),
			}) ;
		// setter成功でもreadbackが無効なら、NLAを弱めずfail-closedとする
		if( FAILED( result) || credssp_enabled == VARIANT_FALSE) {
			return FAILED( result) ? result : E_FAIL ;
		}

		ATL::CComQIPtr<IMsRdpClientAdvancedSettings5> clipboard_settings( advanced_settings) ;
		// Phase 0必須のclipboard propertyを持たないControlは使用しない
		if( clipboard_settings == nullptr) {
			return E_NOINTERFACE ;
		}

		// クリップボード転送はPhase 0の必須要件として明示的に有効化する
		result = clipboard_settings->put_RedirectClipboard( VARIANT_TRUE) ;
		// clipboard設定失敗では接続前の必須条件が完了したと扱わない
		if( FAILED( result)) {
			return result ;
		}

		VARIANT_BOOL clipboard_enabled = VARIANT_FALSE ;
		result = clipboard_settings->get_RedirectClipboard( &clipboard_enabled) ;
		// クリップボード有効化をreadbackできなければ接続を開始しない
		if( FAILED( result) || clipboard_enabled == VARIANT_FALSE) {
			return FAILED( result) ? result : E_FAIL ;
		}

		// リモート音声の出力先は接続時に確定するため、Connect前に明示する
		const unsigned int audio_playback_mode =
			static_cast<unsigned int>( settings.audio.playback_mode) ;
		result = clipboard_settings->put_AudioRedirectionMode( audio_playback_mode) ;
		// リモート音声の出力先設定失敗では別再生先へfallbackしない
		if( FAILED( result)) {
			return result ;
		}

		unsigned int audio_playback_readback = 0U ;
		result = clipboard_settings->get_AudioRedirectionMode( &audio_playback_readback) ;
		// 別の出力先へ接続する誤動作を避け、完全一致できないControlを拒否する
		if( FAILED( result) || audio_playback_readback != audio_playback_mode) {
			return FAILED( result) ? result : E_FAIL ;
		}

		ATL::CComQIPtr<IMsRdpClientAdvancedSettings7> audio_capture_settings( advanced_settings) ;
		// マイク転送を接続前に明示制御できない旧Controlは使用しない
		if( audio_capture_settings == nullptr) {
			return E_NOINTERFACE ;
		}

		const VARIANT_BOOL redirect_microphone =
			settings.audio.redirect_microphone ? VARIANT_TRUE : VARIANT_FALSE ;
		result = audio_capture_settings->put_AudioCaptureRedirectionMode( redirect_microphone) ;
		// マイク転送設定の失敗ではprivacy選択を反転・無視して接続しない
		if( FAILED( result)) {
			return result ;
		}

		VARIANT_BOOL microphone_readback = VARIANT_FALSE ;
		result = audio_capture_settings->get_AudioCaptureRedirectionMode( &microphone_readback) ;
		// privacy設定の反転を防ぎ、ON／OFFともsetter後の実値を確認する
		if( FAILED( result) ||
		   (microphone_readback != VARIANT_FALSE) != settings.audio.redirect_microphone) {
			return FAILED( result) ? result : E_FAIL ;
		}

		ATL::CComQIPtr<IMsRdpClientNonScriptable3> prompt_settings( control_) ;
		// Microsoft標準credential promptを設定できないControlは使用しない
		if( prompt_settings == nullptr) {
			return E_NOINTERFACE ;
		}

		// passwordをこのprocessへ取り込まず、Microsoft Controlのpromptを表示する
		result = prompt_settings->put_PromptForCredentials( VARIANT_TRUE) ;
		// 標準credential prompt設定失敗では独自password入力へ切り替えない
		if( FAILED( result)) {
			return result ;
		}

		// Microsoft connection barへ同じ接続先表示を設定し、全画面中も識別可能にする
		result = prompt_settings->put_ConnectionBarText( connection_bar_label) ;
		// connection bar識別表示を設定できなければ接続を開始しない
		if( FAILED( result)) {
			return result ;
		}

		ATL::CComBSTR connection_bar_readback ;
		result = prompt_settings->get_ConnectionBarText( &connection_bar_readback) ;
		// connection bar識別表示をreadbackできなければ設定成功だけで進めない
		if( FAILED( result)) {
			return result ;
		}

		const UINT expected_connection_bar_length = SysStringLen( connection_bar_label) ;
		const UINT actual_connection_bar_length	  = SysStringLen( connection_bar_readback) ;
		// setterが成功しても接続先表示が欠落したControlでは接続を開始しない
		if( actual_connection_bar_length != expected_connection_bar_length ||
		   std::memcmp( connection_bar_readback.m_str, connection_bar_label.m_str,
					   expected_connection_bar_length * sizeof( wchar_t)) != 0) {
			return E_FAIL ;
		}

		ATL::CComQIPtr<IMsRdpClientNonScriptable4> credential_policy( control_) ;
		// credential保存禁止を設定できないControlは使用しない
		if( credential_policy == nullptr) {
			return E_NOINTERFACE ;
		}

		// このアプリはRDP credentialの保存機能を提供・実装しない
		result = credential_policy->put_AllowCredentialSaving( VARIANT_FALSE) ;
		// credential保存禁止を設定できなければ認証情報保存を許可して接続しない
		if( FAILED( result)) {
			return result ;
		}

		// NLA credentialはMicrosoft client側ダイアログで収集する
		result = credential_policy->put_PromptForCredsOnClient( VARIANT_TRUE) ;
		// Client側credential promptの設定失敗では別入力経路を使用しない
		if( FAILED( result)) {
			return result ;
		}

		ATL::CComQIPtr<IMsRdpClientNonScriptable5> prompting_policy( control_) ;
		// 標準prompt有効化を設定できないControlは使用しない
		if( prompting_policy == nullptr) {
			return E_NOINTERFACE ;
		}

		// NLAやCredSSPを弱めず、標準credential promptを有効なまま維持する
		result = prompting_policy->put_AllowPromptingForCredentials( VARIANT_TRUE) ;
		// 標準promptを有効なまま設定できない場合は接続を中止する
		if( FAILED( result)) {
			return result ;
		}

		// scriptable SVCはMicrosoft仕様どおり最初のConnectより前に登録する
		if( !virtual_channel_registered_) {
			ATL::CComBSTR channel_name ;
			channel_name.Attach( SysAllocString( protocol::kStaticVirtualChannelNameWide)) ;
			// SVC名のBSTR確保失敗ではchannel登録を行わない
			if( channel_name.m_str == nullptr) {
				return E_OUTOFMEMORY ;
			}

			result = client_->CreateVirtualChannels( channel_name) ;
			common::WriteDiagnosticLog( FAILED( result) ? common::DiagnosticLevel::Error
													  : common::DiagnosticLevel::Information,
									   common::DiagnosticEvent::VirtualChannelRegistrationReturned,
									   {common::DiagnosticHResult( result)}) ;
			// SVCなしのRDP専用modeへ暗黙に縮退せず製品接続を開始しない
			if( FAILED( result)) {
				return result ;
			}

			virtual_channel_registered_ = true ;
		}

		// 通常driveとdynamic driveを禁止し、配布用の一driveだけを選択する
		result = ConfigureDeploymentDrive( settings.deployment_drive) ;
		// 公開範囲をreadbackできなければ安全な接続設定と扱わない
		if( FAILED( result)) {
			return result ;
		}

		settings_applied_ = true ;
		return S_OK ;
	}

	/** @brief drive単位の明示選択を適用する。
	 * @param[in] selected 専用drive letter。0は全て無効。
	 * @return 他driveの無効化もreadbackできた場合だけS_OK。
	 */
	[[nodiscard]] HRESULT ConfigureDeploymentDrive( wchar_t selected) noexcept
	{
		ATL::CComQIPtr<IMsRdpClientNonScriptable3>	 settings( control_) ;
		ATL::CComQIPtr<IMsRdpClientAdvancedSettings> advanced ;
		// driveを個別選択できないControlで全driveを公開しない
		if( settings == nullptr || FAILED( client_->get_AdvancedSettings2( &advanced)) ||
		   advanced == nullptr) {
			return E_NOINTERFACE ;
		}

		HRESULT result = settings->put_RedirectDynamicDrives( VARIANT_FALSE) ;
		// 接続後のUSB等を自動追加しない
		if( FAILED( result)) {
			return result ;
		}

		VARIANT_BOOL dynamic = VARIANT_TRUE ;
		// dynamic drive禁止を必ずreadbackする
		if( FAILED( settings->get_RedirectDynamicDrives( &dynamic)) || dynamic != VARIANT_FALSE) {
			return E_FAIL ;
		}

		result = advanced->put_RedirectDrives( selected != 0 ? VARIANT_TRUE : VARIANT_FALSE) ;
		// 全体gate設定に失敗した状態で個別転送しない
		if( FAILED( result)) {
			return result ;
		}

		ATL::CComPtr<IMsRdpDriveCollection> drives ;
		// 新規検出driveは無効を既定にする
		if( FAILED( settings->get_DriveCollection( &drives)) || drives == nullptr ||
		   FAILED( drives->RescanDrives( VARIANT_FALSE))) {
			return E_FAIL ;
		}

		ULONG count = 0U ;
		// drive letterの範囲を超える異常collectionを採用しない
		if( FAILED( drives->get_DriveCount( &count)) || count > 26U) {
			return E_FAIL ;
		}

		bool found = selected == 0 ;
		// 全driveを検査し、専用drive以外を無効化する
		for( ULONG index = 0U; index < count; ++index) {
			ATL::CComPtr<IMsRdpDrive> drive ;
			ATL::CComBSTR			  name ;
			// 名前取得不能のdriveは公開しない
			if( FAILED( drives->get_DriveByIndex( index, &drive)) || drive == nullptr ||
			   FAILED( drive->get_Name( &name))) {
				return E_FAIL ;
			}

			std::wstring_view value( name.m_str == nullptr ? L"" : name.m_str, SysStringLen( name)) ;
			// Windowsのdrive名BSTRが長さに含める末尾NUL一つだけを除く
			if( !value.empty() && value.back() == L'\0') {
				value.remove_suffix( 1U) ;
			}

			const bool chosen = selected != 0 && value.size() >= 2U && value.size() <= 5U &&
								value[0] == selected && value[1] == L':' &&
								std::all_of( value.begin() + 2, value.end(),
											/**
											 * @cond LOCAL_CALLBACK_CONTRACT
											 * @brief 配布用単一drive指定の末尾がroot区切りだけか検証する。
											 * @param[in] character 候補末尾の一文字。decodeやpath探索は行わない。
											 * @return rootのbackslashならtrue。それ以外のpath文字は拒否する。
											 * @endcond
											 */
											[]( wchar_t character) { return character == L'\\' ; }) ;
			const VARIANT_BOOL enabled	= chosen ? VARIANT_TRUE : VARIANT_FALSE ;
			VARIANT_BOOL	   readback = chosen ? VARIANT_FALSE : VARIANT_TRUE ;
			// 非選択driveの禁止ができない場合も接続前に停止する
			if( FAILED( drive->put_RedirectionState( enabled)) ||
			   FAILED( drive->get_RedirectionState( &readback)) ||
			   (readback != VARIANT_FALSE) != chosen) {
				return E_FAIL ;
			}

			found = found || chosen ;
		}

		// Windowsの全体gate設定で復元されたdynamic選択を最後に明示解除する
		result = settings->put_RedirectDynamicDrives( VARIANT_FALSE) ;
		// dynamic解除失敗を接続成功にしない
		if( FAILED( result)) {
			return result ;
		}

		dynamic = VARIANT_TRUE ;
		// 全体gateとcollection選択の後もdynamic drive禁止が維持されていることを確認する
		if( FAILED( settings->get_RedirectDynamicDrives( &dynamic)) || dynamic != VARIANT_FALSE) {
			return E_FAIL ;
		}

		// dynamic設定の後も、専用driveが有効のままであることを確認する
		if( selected != 0) {
			bool selected_enabled = false ;
			// 個別選択を再設定せず、最後の実状態だけを確認する
			for( ULONG index = 0U; index < count; ++index) {
				ATL::CComPtr<IMsRdpDrive> drive ;
				ATL::CComBSTR			  name ;
				VARIANT_BOOL			  enabled = VARIANT_FALSE ;
				// 名前と実状態の取得失敗は公開範囲不明として停止する
				if( FAILED( drives->get_DriveByIndex( index, &drive)) || drive == nullptr ||
				   FAILED( drive->get_Name( &name)) ||
				   FAILED( drive->get_RedirectionState( &enabled))) {
					return E_FAIL ;
				}

				const bool chosen =
					SysStringLen( name) >= 2U && name[0] == selected && name[1] == L':' ;
				// 他driveの有効化も専用driveの消失も接続前に拒否する
				if( (enabled != VARIANT_FALSE) != chosen) {
					return E_FAIL ;
				}

				selected_enabled = selected_enabled || chosen ;
			}

			found = selected_enabled ;
		}

		return found ? S_OK : E_FAIL ;
	}

	/** @brief 接続前だけキー転送設定を変更し、その後はreadbackのみを行う。
	 * @param[in] apply 接続前設定の場合だけtrue。
	 * @return Windowsキー有効かつhook mode 1を確認できた場合だけS_OK。
	 */
	[[nodiscard]] HRESULT DeploymentKeyboardSettings( bool apply) noexcept
	{
		deployment_windows_key_		   = 0L ;
		deployment_keyboard_hook_mode_ = -1L ;
		ATL::CComPtr<IMsRdpClientAdvancedSettings> advanced ;
		ATL::CComPtr<IMsRdpClientSecuredSettings>  secured ;
		HRESULT result = client_->get_AdvancedSettings2( &advanced) ;
		// 非対応Controlへ設定を推測して入力しない
		if( FAILED( result) || advanced == nullptr) {
			return FAILED( result) ? result : E_NOINTERFACE ;
		}

		result = client_->get_SecuredSettings2( &secured) ;
		// SecuredSettingsが取得できない場合はWindowsキー転送を保証しない
		if( FAILED( result) || secured == nullptr) {
			return FAILED( result) ? result : E_NOINTERFACE ;
		}

		// 接続後のpolicy変更やキー転送modeの途中変更を行わない
		if( apply) {
			result = advanced->put_EnableWindowsKey( 1L) ;
			// Windowsキー有効化に失敗したら自動配置を拒否する
			if( FAILED( result)) {
				return result ;
			}

			result = secured->put_KeyboardHookMode( 1L) ;
			// 既定の全画面限定modeを成功した設定と混同しない
			if( FAILED( result)) {
				return result ;
			}
		}

		result = advanced->get_EnableWindowsKey( &deployment_windows_key_) ;
		// setterの結果だけで転送設定を確定しない
		if( FAILED( result)) {
			return result ;
		}

		result = secured->get_KeyboardHookMode( &deployment_keyboard_hook_mode_) ;
		// readback不能のControlへ固定Win+Rを送らない
		if( FAILED( result)) {
			return result ;
		}

		return deployment_windows_key_ != 0L && deployment_keyboard_hook_mode_ == 1L ? S_OK
																					 : E_FAIL ;
	}

	/** @brief キー転送設定の結果を接続情報なしで記録する。
	 * @param[in] result 設定またはreadbackの元HRESULT。
	 */
	void LogDeploymentKeyboard( HRESULT result) const noexcept
	{
		common::WriteDiagnosticLog(
			result == S_OK ? common::DiagnosticLevel::Information
						   : common::DiagnosticLevel::Warning,
			common::DiagnosticEvent::DeploymentKeyboardConfigurationReturned,
			{common::DiagnosticCode( deployment_keyboard_hook_mode_),
			 common::DiagnosticValue( common::DiagnosticFieldName::Enabled,
									 deployment_windows_key_ != 0L ? 1U : 0U),
			 common::DiagnosticHResult( result)}) ;
	}

	/** @brief 所有STAからだけ同じsessionの固定入力focusを準備する。
	 * @return 前景、active Window、focusを確認できた場合だけS_OK。
	 */
	[[nodiscard]] HRESULT PrepareDeploymentInput() noexcept
	{
		// 別STAではCOMやWindowの操作を開始しない
		if( GetCurrentThreadId() != ui_thread_id_) {
			return RPC_E_WRONG_THREAD ;
		}

		// 未認証・切断中のControlへfocusや入力を渡さない
		if( state_ != RdpConnectionState::LoggedIn || control_ == nullptr || client_ == nullptr) {
			return E_UNEXPECTED ;
		}

		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief 数値のfocus分類だけを記録し、元結果を返す。
		 * @param[in] result 入力準備の結果。
		 * @param[in] reason 固定失敗分類。
		 * @return resultを変更せず返す。
		 * @endcond
		 */
		const auto report = []( HRESULT result, std::uint64_t reason) {
			common::WriteDiagnosticLog(
				result == S_OK ? common::DiagnosticLevel::Information
							   : common::DiagnosticLevel::Warning,
				common::DiagnosticEvent::DeploymentInputFocusReturned,
				{common::DiagnosticValue( common::DiagnosticFieldName::Reason, reason),
				 common::DiagnosticValue( common::DiagnosticFieldName::Enabled,
										 result == S_OK ? 1U : 0U),
				 common::DiagnosticHResult( result)}) ;
			return result ;
		} ;
		// 接続前設定が成立しない場合は焦点を奪わず自動配置だけを停止する
		if( deployment_keyboard_configuration_result_ != S_OK) {
			return report( FAILED( deployment_keyboard_configuration_result_)
							  ? deployment_keyboard_configuration_result_
							  : E_FAIL,
						  5U) ;
		}

		const HRESULT keyboard = DeploymentKeyboardSettings( false) ;
		LogDeploymentKeyboard( keyboard) ;
		// 接続後は設定をreadbackするだけで、変更・迂回しない
		if( keyboard != S_OK) {
			return report( FAILED( keyboard) ? keyboard : E_FAIL, 5U) ;
		}

		// COM readback中に認証警告または切断へ進んだ場合もfocusを奪わない
		if( state_ != RdpConnectionState::LoggedIn || authentication_warning_active_) {
			return report( HRESULT_FROM_WIN32( ERROR_INVALID_STATE), 2U) ;
		}

		const HWND host				  = host_window_.m_hWnd ;
		const HWND target			  = GetAncestor( host, GA_ROOT) ;
		const HWND foreground		  = GetForegroundWindow() ;
		DWORD	   foreground_process = 0U, target_process = 0U ;
		static_cast<void>( GetWindowThreadProcessId( foreground, &foreground_process)) ;
		const DWORD target_thread = GetWindowThreadProcessId( target, &target_process) ;
		const bool	own_foreground =
			foreground != nullptr && foreground_process == GetCurrentProcessId() ;
		const bool ready = IsWindow( host) && IsWindow( target) && IsWindowVisible( target) &&
						   IsWindowVisible( host) && IsWindowEnabled( target) &&
						   IsWindowEnabled( host) && target_thread == ui_thread_id_ &&
						   target_process == GetCurrentProcessId() ;
		// 他アプリ前景、非表示、最小化、無効な対象では何もactivateしない
		if( !CanAcquireDeploymentInputFocus( own_foreground, ready, IsIconic( target) != FALSE)) {
			return report( E_ACCESSDENIED, own_foreground ? 2U : 1U) ;
		}

		// Windowsの前景制限を解除せず、同じprocessの対象へだけ移す
		if( GetForegroundWindow() != foreground) {
			return report( E_ACCESSDENIED, 1U) ;
		}

		// 同じprocessの対象を通常のWindows APIでactivateする
		if( GetForegroundWindow() != target && !SetForegroundWindow( target)) {
			return report( E_ACCESSDENIED, 3U) ;
		}

		// activation後に他アプリへ切り替わった場合はfocusを要求しない
		if( GetForegroundWindow() != target) {
			return report( E_ACCESSDENIED, 3U) ;
		}

		static_cast<void>( SetFocus( host)) ;
		const HWND focused = GetFocus() ;
		const bool verified =
			HasDeploymentInputFocus( GetForegroundWindow() == target, GetActiveWindow() == target,
									focused == host || IsChild( host, focused) != FALSE) ;
		// APIの戻り値だけでfocus取得成功と扱わない
		if( !verified) {
			return report( HRESULT_FROM_WIN32( ERROR_INVALID_STATE), 4U) ;
		}

		return report( S_OK, 0U) ;
	}

	/** @brief 遅延入力直前の標準キー設定とfocusを変更せず検証する。
	 * @return 所有STAと認証・対象・focus条件が全て成立した場合だけS_OK。
	 */
	[[nodiscard]] HRESULT VerifyDeploymentInput() noexcept
	{
		// 別STAではCOM readbackも行わない
		if( GetCurrentThreadId() != ui_thread_id_) {
			return RPC_E_WRONG_THREAD ;
		}

		// 認証警告または切断中に入力を続けない
		if( state_ != RdpConnectionState::LoggedIn || control_ == nullptr || client_ == nullptr ||
		   authentication_warning_active_) {
			return HRESULT_FROM_WIN32( ERROR_INVALID_STATE) ;
		}

		const HRESULT keyboard = DeploymentKeyboardSettings( false) ;
		LogDeploymentKeyboard( keyboard) ;
		// 接続後の設定は読み取りだけで確認し、成立しなければ停止する
		if( keyboard != S_OK) {
			return keyboard ;
		}

		const HWND	host		   = host_window_.m_hWnd ;
		const HWND	target		   = GetAncestor( host, GA_ROOT) ;
		DWORD		target_process = 0U ;
		const DWORD target_thread  = GetWindowThreadProcessId( target, &target_process) ;
		const HWND	focused		   = GetFocus() ;
		const bool	ready		   = state_ == RdpConnectionState::LoggedIn &&
						   !authentication_warning_active_ && IsWindow( host) && IsWindow( target) &&
						   IsWindowVisible( host) && IsWindowVisible( target) &&
						   IsWindowEnabled( host) && IsWindowEnabled( target) && !IsIconic( target) &&
						   target_thread == ui_thread_id_ &&
						   target_process == GetCurrentProcessId() ;
		const bool verified =
			ready &&
			HasDeploymentInputFocus( GetForegroundWindow() == target, GetActiveWindow() == target,
									focused == host || IsChild( host, focused) != FALSE) ;
		const HRESULT result = verified ? S_OK : HRESULT_FROM_WIN32( ERROR_INVALID_STATE) ;
		common::WriteDiagnosticLog(
			verified ? common::DiagnosticLevel::Information : common::DiagnosticLevel::Warning,
			common::DiagnosticEvent::DeploymentInputFocusReturned,
			{common::DiagnosticValue( common::DiagnosticFieldName::Reason, verified ? 0U : 4U),
			 common::DiagnosticValue( common::DiagnosticFieldName::Enabled, verified ? 1U : 0U),
			 common::DiagnosticHResult( result)}) ;
		return result ;
	}

	/** @brief 確認済み配置へ固定Ctrl+VだけをSendInputで要求する。
	 * @param[in] authorized COM照会後にも世代とclipboardが有効かを確認する同期境界。
	 * @return 全件受付だけS_OK。未認証・別STA・承認不在では入力しない。
	 */
	[[nodiscard]] RKeyEmulationResult
	SendDeploymentPaste( const std::function<bool()> &authorized) noexcept
	{
		return SendKeyboardInput( FixedKeyboardKind::Paste, true, authorized) ;
	}

	/** @brief 確認済み固定AgentへEnterだけをSendInputで要求する。
	 * @param[in] authorized COM照会後にも起動確認と世代が有効かを確認する同期境界。
	 * @return 全件受付だけS_OK。部分受付でも再送しない。
	 */
	[[nodiscard]] RKeyEmulationResult
	SendAgentStart( const std::function<bool()> &authorized) noexcept
	{
		return SendKeyboardInput( FixedKeyboardKind::Enter, true, authorized) ;
	}

	/** @brief 初期配置の固定Win+RをSendInputで要求する。
	 * @param[in] authorized COM照会後にも世代・clipboard・取消が有効かを確認する同期境界。
	 * @return API受付数と元Win32 error。配置成功とは別である。
	 */
	[[nodiscard]] RKeyEmulationResult
	SendDeploymentRun( const std::function<bool()> &authorized) noexcept
	{
		return SendKeyboardInput( FixedKeyboardKind::WinR, true, authorized) ;
	}

	/** @brief 固定入力だけを所有STAと対象focusの直前確認に限定する。
	 * @param[in] kind 内部の固定R／Win+R／Ctrl+V／Enterだけの種別。
	 * @param[in] deployment trueは配置・起動として同期のowner再承認を必須にする。
	 * @param[in] authorized COM照会後の世代・clipboard・取消確認。保持しない。
	 * @return 入力streamへの受付数と元Win32 errorを含む結果。
	 * @note 貼付け・Enterは同じsessionのowner承認を必須にし、開発用入力入口は公開しない。
	 */
	[[nodiscard]] RKeyEmulationResult
	SendKeyboardInput( FixedKeyboardKind kind, bool deployment,
					  const std::function<bool()> &authorized = {}) noexcept
	{
		RKeyEmulationObservation observed{} ;
		observed.owner_thread = GetCurrentThreadId() == ui_thread_id_ ;
		// 他STAではCOM照会、focus取得、key状態の読取りさえ行わない
		if( observed.owner_thread) {
			observed.authenticated = state_ == RdpConnectionState::LoggedIn &&
									 control_ != nullptr && !authentication_warning_active_ ;
		}

		// 未認証の比較試験ではWindows入力経路へ到達しない
		if( observed.authenticated) {
			ATL::CComQIPtr<IOleWindow> ole_window( control_) ;
			HWND					   control_window = nullptr ;
			observed.focus_result =
				ole_window != nullptr ? ole_window->GetWindow( &control_window) : E_NOINTERFACE ;
			const HWND	host			= host_window_.m_hWnd ;
			const HWND	target			= GetAncestor( host, GA_ROOT) ;
			const HWND	focused			= GetFocus() ;
			DWORD		control_process = 0U, target_process = 0U ;
			const DWORD control_thread = GetWindowThreadProcessId( control_window, &control_process) ;
			const DWORD target_thread  = GetWindowThreadProcessId( target, &target_process) ;
			const std::array<int, 6U> keys{kind == FixedKeyboardKind::Paste	  ? 'V'
										   : kind == FixedKeyboardKind::Enter ? VK_RETURN
																			  : 'R',
										   VK_SHIFT,
										   VK_CONTROL,
										   VK_MENU,
										   VK_LWIN,
										   VK_RWIN} ;
			observed.keys_released = true ;
			// 固定操作に関連するキーと修飾キーだけを読み、状態・内容を記録しない
			for( const auto key : keys) {
				// 既に押されたキーを勝手に解除せず、比較操作だけを拒否する
				if( (GetAsyncKeyState( key) & 0x8000) != 0) {
					observed.keys_released = false ;
				}
			}

			// COM再入後も同じ認証済みControlだけを扱う
			observed.authenticated = state_ == RdpConnectionState::LoggedIn &&
									 !authentication_warning_active_ && control_ != nullptr ;
			// host全体ではなく実ControlのWindowとその子へのfocusを最後に照合する
			observed.focused =
				observed.focus_result == S_OK && IsWindow( host) && IsWindow( control_window) &&
				IsChild( host, control_window) && IsWindowVisible( control_window) &&
				IsWindowEnabled( control_window) && IsWindowVisible( target) &&
				IsWindowEnabled( target) && !IsIconic( target) && control_thread == ui_thread_id_ &&
				target_thread == ui_thread_id_ && control_process == GetCurrentProcessId() &&
				target_process == GetCurrentProcessId() &&
				(focused == control_window || IsChild( control_window, focused)) &&
				GetForegroundWindow() == target && GetActiveWindow() == target ;
		}

		// 配置のPaste・EnterはCOM再入後の同一owner承認を必須にする
		if( observed.owner_thread && observed.authenticated &&
		   (deployment || kind == FixedKeyboardKind::Paste || kind == FixedKeyboardKind::Enter)) {
			try {
				observed.authorized = authorized && authorized() ;
			} catch( ...) {
				// owner確認の例外も入力せず失効分類で数値記録する
				observed.authorized = false ;
			}
		}

		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief 固定factoryの二／四要素だけを一回挿入し、不完全受付の元errorを保持する。
		 * @param[in,out] inputs 内部の固定R／Win+R／Ctrl+V／Enter factoryが生成した所有列。
		 * @return 受付数と不完全受付のWin32 error。remote入力結果ではない。
		 * @endcond
		 */
		const auto insert = []( auto &inputs) noexcept {
			SetLastError( ERROR_SUCCESS) ;
			const UINT	inserted = SendInput( static_cast<UINT>( inputs.size()), inputs.data(),
											 static_cast<int>( sizeof( INPUT))) ;
			const DWORD error	 = inserted == inputs.size() ? ERROR_SUCCESS : GetLastError() ;
			return RKeyEmulationInsertion{inserted, error} ;
		} ;
		const auto result = RunFixedKeyboardInput( kind, observed, insert) ;
		const auto event =
			kind == FixedKeyboardKind::LetterR ? common::DiagnosticEvent::RKeyEmulationInputReturned
			: kind == FixedKeyboardKind::WinR
				? (deployment ? common::DiagnosticEvent::DeploymentRunInputReturned
							  : common::DiagnosticEvent::WinRKeyEmulationInputReturned)
			: kind == FixedKeyboardKind::Paste
				? (deployment ? common::DiagnosticEvent::DeploymentPasteInputReturned
							  : common::DiagnosticEvent::PasteKeyEmulationInputReturned)
				: (deployment ? common::DiagnosticEvent::AgentStartInputReturned
							  : common::DiagnosticEvent::EnterKeyEmulationInputReturned) ;
		common::WriteDiagnosticLog(
			result.result == S_OK ? common::DiagnosticLevel::Information
								  : common::DiagnosticLevel::Warning,
			event,
			{common::DiagnosticValue( common::DiagnosticFieldName::Code, result.inserted),
			 common::DiagnosticValue( common::DiagnosticFieldName::Reason,
									 static_cast<std::uint64_t>( result.reason)),
			 common::DiagnosticValue( common::DiagnosticFieldName::Win32Error, result.error),
			 common::DiagnosticHResult( result.result)}) ;
		return result ;
	}

	/**
	 * @brief 完全な接続前設定を持つActiveXへ非同期接続を要求する。
	 * @return Microsoft ControlのConnect結果。
	 */
	[[nodiscard]] HRESULT Connect() noexcept
	{
		const HRESULT thread_result = EnsureUiThread() ;
		// ConnectをActiveX所有STA以外から呼ばない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		// 全接続前設定が成功した未接続ControlにだけConnectを許可する
		if( state_ != RdpConnectionState::Disconnected || client_ == nullptr || !settings_applied_) {
			return HRESULT_FROM_WIN32( ERROR_INVALID_STATE) ;
		}

		state_				 = RdpConnectionState::Connecting ;
		const HRESULT result = client_->Connect() ;
		// 同期的な失敗時はcallbackが来ないため、未接続状態へ戻す
		if( FAILED( result)) {
			state_ = RdpConnectionState::Disconnected ;
		}

		return result ;
	}

	/**
	 * @brief 接続済みActiveXのESVNBRGへbinary transport chunkを送る。
	 * @param[in] data 1～1600 byteの送信fragment。
	 * @return Microsoft ControlのSendOnVirtualChannel結果。
	 */
	[[nodiscard]] HRESULT
	SendStaticVirtualChannelData( const std::span<const std::byte> data) noexcept
	{
		const HRESULT thread_result = EnsureUiThread() ;
		// ActiveX送信を所有COM STA以外から呼ばない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		// channelが成立し得る接続stateと事前登録を送信直前にも確認する
		if( client_ == nullptr || !virtual_channel_registered_ ||
		   (state_ != RdpConnectionState::Connected && state_ != RdpConnectionState::LoggedIn)) {
			return HRESULT_FROM_WIN32( ERROR_INVALID_STATE) ;
		}

		// 空messageとMicrosoft SVC chunk上限を超える一回送信を拒否する
		if( data.empty() || data.size() > protocol::kStaticVirtualChannelChunkBytes) {
			return E_INVALIDARG ;
		}

		ATL::CComBSTR channel_name ;
		channel_name.Attach( SysAllocString( protocol::kStaticVirtualChannelNameWide)) ;
		// 送信channel名を確保できなければActiveX送信を要求しない
		if( channel_name.m_str == nullptr) {
			return E_OUTOFMEMORY ;
		}

		BSTR payload = SysAllocStringByteLen( nullptr, static_cast<UINT>( data.size())) ;
		// binary BSTRの確保失敗では空payloadに読み替えて送信しない
		if( payload == nullptr) {
			return E_OUTOFMEMORY ;
		}

		ATL::CComBSTR payload_owner ;
		payload_owner.Attach( payload) ;
		// BSTRをUTF-16へ変換せず、埋込みNULを含むbyte列を明示長でcopyする
		std::memcpy( static_cast<void *>( payload_owner.m_str), data.data(), data.size()) ;
		return client_->SendOnVirtualChannel( channel_name, payload_owner) ;
	}

	/**
	 * @brief 成立中または接続途中のRDPセッションへ切断を要求する。
	 * @return Microsoft ControlのDisconnect結果。既切断時はS_FALSE。
	 */
	[[nodiscard]] HRESULT Disconnect() noexcept
	{
		const HRESULT thread_result = EnsureUiThread() ;
		// DisconnectをActiveX所有STA以外から呼ばない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		// 未生成Controlが有効なRDP接続を所有しているとは扱わない
		if( client_ == nullptr || state_ == RdpConnectionState::NotInitialized) {
			return CO_E_NOTINITIALIZED ;
		}

		// 既切断状態ではMicrosoft Controlへ重複切断を要求しない
		if( state_ == RdpConnectionState::Disconnected) {
			return S_FALSE ;
		}

		const RdpConnectionState previous_state = state_ ;
		state_									= RdpConnectionState::Disconnecting ;
		const HRESULT result					= client_->Disconnect() ;
		// 同期的な失敗時は以前の状態へ戻し、UIの接続状態と一致させる
		if( FAILED( result)) {
			state_ = previous_state ;
		}

		return result ;
	}

	/**
	 * @brief ActiveXへ安全な終了可否を問い合わせる。
	 * @param[out] can_close_now 即時破棄可能な場合はtrue。
	 * @return 問合せ結果を表すHRESULT。
	 */
	[[nodiscard]] HRESULT RequestClose( bool &can_close_now) noexcept
	{
		can_close_now				= false ;
		const HRESULT thread_result = EnsureUiThread() ;
		// RequestCloseをActiveX所有STA以外から呼ばない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		// 未生成または未接続Controlでは非同期確認を待つ必要がない
		if( client_ == nullptr || state_ == RdpConnectionState::NotInitialized ||
		   state_ == RdpConnectionState::Disconnected) {
			can_close_now = true ;
			return S_OK ;
		}

		ControlCloseStatus close_status = controlCloseCanProceed ;
		const HRESULT	   result		= client_->RequestClose( &close_status) ;
		// Microsoft Controlの問合せ失敗時は即時破棄可能とみなさない
		if( FAILED( result)) {
			return result ;
		}

		// Controlが許可した場合だけ、呼び出し側へ即時破棄可能と通知する
		if( close_status == controlCloseCanProceed) {
			can_close_now = true ;
			return S_OK ;
		}

		// UI確認済みなので、event sinkはOnConfirmCloseで許可を返して完了を待つ
		if( close_status == controlCloseWaitForEvents) {
			state_ = RdpConnectionState::Disconnecting ;
			return S_OK ;
		}

		return E_UNEXPECTED ;
	}

	/**
	 * @brief 成立済みセッションのMicrosoft全画面modeを切り替える。
	 * @param[in] full_screen 全画面へ入る場合はtrue。
	 * @return ActiveX設定結果。
	 */
	[[nodiscard]] HRESULT SetFullScreen( const bool full_screen) noexcept
	{
		const HRESULT thread_result = EnsureUiThread() ;
		// 全画面設定をActiveX所有STA以外から変更しない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		// 全画面modeは成立済みまたは再接続中のセッションだけで有効である
		if( client_ == nullptr ||
		   (state_ != RdpConnectionState::Connected && state_ != RdpConnectionState::LoggedIn &&
			state_ != RdpConnectionState::Reconnecting)) {
			return HRESULT_FROM_WIN32( ERROR_INVALID_STATE) ;
		}

		const HRESULT result = client_->put_FullScreen( full_screen ? VARIANT_TRUE : VARIANT_FALSE) ;
		// ActiveXが受理した場合だけ内部表示状態を更新する
		if( SUCCEEDED( result)) {
			full_screen_ = full_screen ;
		}

		return result ;
	}

	/**
	 * @brief 接続先寸法を変えず、ローカルSmart Sizingだけを切り替える。
	 * @param[in] enabled ウィンドウ内へ縮尺表示する場合はtrue。
	 * @return ActiveX設定結果。
	 */
	[[nodiscard]] HRESULT SetSmartSizing( const bool enabled) noexcept
	{
		const HRESULT thread_result = EnsureUiThread() ;
		// Smart Sizing設定をActiveX所有STA以外から変更しない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		// Smart Sizing の実行時変更は確立済みセッションだけに限定する
		if( client_ == nullptr ||
		   (state_ != RdpConnectionState::Connected && state_ != RdpConnectionState::LoggedIn)) {
			return HRESULT_FROM_WIN32( ERROR_INVALID_STATE) ;
		}

		ATL::CComPtr<IMsRdpClientAdvancedSettings> advanced_settings ;
		const HRESULT result = client_->get_AdvancedSettings2( &advanced_settings) ;
		// 実行時設定interfaceを取得できない場合は部分適用しない
		if( FAILED( result) || advanced_settings == nullptr) {
			return FAILED( result) ? result : E_NOINTERFACE ;
		}

		// Smart Sizing は描画倍率だけを変更し、接続先の解像度を更新しない
		return advanced_settings->put_SmartSizing( enabled ? VARIANT_TRUE : VARIANT_FALSE) ;
	}

	/**
	 * @brief 成立済みsessionのremote desktop寸法を動的に更新する。
	 * @param[in] desktop_width 新しいremote desktop幅。
	 * @param[in] desktop_height 新しいremote desktop高さ。
	 * @return Microsoft display更新APIのHRESULT。
	 */
	[[nodiscard]] HRESULT UpdateSessionDisplaySettings( const long desktop_width,
													   const long desktop_height) noexcept
	{
		const HRESULT thread_result = EnsureUiThread() ;
		// session表示設定をActiveX所有STA以外から変更しない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		// transport成立だけではAPIがE_UNEXPECTEDとなるため、ログイン完了後に限定する
		if( client_ == nullptr || !CanUpdateSessionDisplaySettings( state_)) {
			return HRESULT_FROM_WIN32( ERROR_INVALID_STATE) ;
		}

		// serverへ送る寸法を有限範囲へ制限し、符号なし型への不正変換を防ぐ
		if( desktop_width < kMinimumDesktopDimension || desktop_width > kMaximumDesktopDimension ||
		   desktop_height < kMinimumDesktopDimension || desktop_height > kMaximumDesktopDimension) {
			return E_INVALIDARG ;
		}

		ATL::CComQIPtr<IMsRdpClient9> display_client( client_) ;
		// 接続前確認後もinterface取得に失敗した場合は別経路へfallbackしない
		if( display_client == nullptr) {
			return E_NOINTERFACE ;
		}

		constexpr unsigned long kUnspecifiedPhysicalDimension = 0U ;
		constexpr unsigned long kLandscapeOrientation		  = 0U ;
		constexpr unsigned long kDefaultScaleFactor			  = 100U ;
		// 物理寸法を未指定とし、pixel寸法だけを100% scaleで更新する
		return display_client->UpdateSessionDisplaySettings(
			static_cast<unsigned long>( desktop_width), static_cast<unsigned long>( desktop_height),
			kUnspecifiedPhysicalDimension, kUnspecifiedPhysicalDimension, kLandscapeOrientation,
			kDefaultScaleFactor, kDefaultScaleFactor) ;
	}

	/**
	 * @brief ActiveXを収容する子ウィンドウだけを指定矩形へ移動する。
	 * @param[in] bounds 親クライアント座標の表示矩形。
	 * @return MoveWindow結果を表すHRESULT。
	 *
	 * RDPデスクトップ寸法を更新するAPIは呼ばず、等倍表示時のスクロール位置と
	 * Smart Sizing時のローカル描画倍率だけをActiveXへ委ねる。
	 */
	[[nodiscard]] HRESULT Resize( const RECT &bounds) noexcept
	{
		const HRESULT thread_result = EnsureUiThread() ;
		// 子ウィンドウ操作をActiveX所有STA以外から行わない
		if( FAILED( thread_result)) {
			return thread_result ;
		}

		const long width  = bounds.right - bounds.left ;
		const long height = bounds.bottom - bounds.top ;

		// MoveWindowへ負または空のActiveX表示領域を渡さない
		if( !IsWindow( host_window_.m_hWnd) || width <= 0L || height <= 0L) {
			return E_INVALIDARG ;
		}

		// ActiveX host再配置の失敗はWin32 errorを保持してownerへ返す
		if( !MoveWindow( host_window_.m_hWnd, bounds.left, bounds.top, width, height, TRUE)) {
			// Win32失敗を分類可能なHRESULTへ変換する
			return HRESULT_FROM_WIN32( GetLastError()) ;
		}

		return S_OK ;
	}

	/**
	 * @brief 認証済みのactiveなsessionだけでActiveX子ウィンドウへfocusを移す。
	 * @note COM所有STA上でだけ実行し、認証UIと別Windowからfocusを奪わない。
	 */
	void Focus() noexcept
	{
		// event通知前やdialog間も、認証前のfocus操作を行わない
		if( GetCurrentThreadId() != ui_thread_id_ ||
		   !CanFocusRemoteDesktop( state_, authentication_warning_active_) ||
		   !IsWindow( host_window_.m_hWnd)) {
			return ;
		}

		const HWND root = GetAncestor( host_window_.m_hWnd, GA_ROOT) ;
		// modal ownerの無効化と他session／別アプリへの切替を尊重する
		if( !IsWindow( root) || !IsWindowEnabled( root) || !IsWindowEnabled( host_window_.m_hWnd) ||
		   GetActiveWindow() != root || GetForegroundWindow() != root) {
			return ;
		}

		SetFocus( host_window_.m_hWnd) ;
	}

	/**
	 * @brief 生成元STA上で切断、購読解除、COM解放、子ウィンドウ破棄を行う。
	 */
	void Destroy() noexcept
	{
		// COMとActiveX windowは生成元STA以外で破棄しない
		if( GetCurrentThreadId() != ui_thread_id_) {
			return ;
		}

		// 未要求の成立中接続だけを切断し、RequestClose後の二重COM呼出しを避ける
		if( client_ != nullptr && state_ != RdpConnectionState::NotInitialized &&
		   state_ != RdpConnectionState::Disconnected &&
		   state_ != RdpConnectionState::Disconnecting) {
			state_ = RdpConnectionState::Disconnecting ;
			static_cast<void>( client_->Disconnect()) ;
		}

		static_cast<void>( event_subscription_.Reset()) ;
		event_sink_.Release() ;
		client_.Release() ;
		control_.Release() ;
		DestroyHostWindow() ;

		selected_control_name_.clear() ;
		deployment_keyboard_configuration_result_ = E_UNEXPECTED ;
		deployment_windows_key_					  = 0L ;
		deployment_keyboard_hook_mode_			  = -1L ;
		authentication_warning_active_			  = false ;
		settings_applied_						  = false ;
		virtual_channel_registered_				  = false ;
		full_screen_							  = false ;
		state_									  = RdpConnectionState::NotInitialized ;
	}

	/**
	 * @brief ActiveXを収容する非所有HWNDを返す。
	 * @return 生成済みhost Window。未生成または破棄後はnullptr。
	 */
	[[nodiscard]] HWND window() const noexcept
	{
		return host_window_.m_hWnd ;
	}

	/**
	 * @brief callback反映済みの接続状態を返す。
	 * @return 現在のRDP lifecycle。
	 */
	[[nodiscard]] RdpConnectionState state() const noexcept
	{
		return state_ ;
	}

	/**
	 * @brief callbackまたはsetterで確認した全画面状態を返す。
	 * @return ActiveXが全画面中ならtrue。
	 */
	[[nodiscard]] bool full_screen() const noexcept
	{
		return full_screen_ ;
	}

	/**
	 * @brief 実際に生成したMicrosoft Controlの固定表示名を返す。
	 * @return instance lifetime中だけ有効な非所有view。
	 */
	[[nodiscard]] std::wstring_view selected_control_name() const noexcept
	{
		return selected_control_name_ ;
	}

	/**
	 * @brief event sinkから受けた所有イベントを状態へ反映して外部へ通知する。
	 * @param[in] event callback中に所有コピー化されたイベント。
	 */
	void OnRdpEvent( RdpEvent event) override
	{
		// 外部observerが参照する前に内部状態をcallback結果へ同期する
		switch( event.kind) {
		// 接続開始ではlogin前のConnecting状態へ遷移する
		case RdpEventKind::Connecting:
			state_ = RdpConnectionState::Connecting ;
			break ;

		// transport接続成立は認証完了とは分けてConnected状態へ遷移する
		case RdpEventKind::Connected:
			state_ = RdpConnectionState::Connected ;
			break ;

		// login成立後だけLoggedIn状態として表示更新を許可する
		case RdpEventKind::LoginComplete:
			state_ = RdpConnectionState::LoggedIn ;
			break ;

		// 切断時は診断情報を補い、local取消の条件を保持してDisconnectedへ移す
		case RdpEventKind::Disconnected:
			PopulateDisconnectDiagnostics( event) ;
			// ログイン前の完全一致条件だけを利用者のcredential取消として保持する
			event.credential_prompt_cancelled =
				IsCredentialPromptCancellation( state_, event.code, event.extended_disconnect_reason,
											   event.locally_requested_disconnect) ;
			state_			  = RdpConnectionState::Disconnected ;
			settings_applied_ = false ;
			full_screen_	  = false ;
			break ;

		// 全画面開始通知ではControlの全画面状態を保持する
		case RdpEventKind::EnterFullScreen:
			full_screen_ = true ;
			break ;

		// 全画面解除通知ではControlのWindow状態を保持する
		case RdpEventKind::LeaveFullScreen:
			full_screen_ = false ;
			break ;

		// 致命的errorはRDP lifecycleをFailedへ遷移させる
		case RdpEventKind::FatalError:
			state_ = RdpConnectionState::Failed ;
			break ;

		// 再接続開始では新しい接続と区別してReconnecting状態を保持する
		case RdpEventKind::AutoReconnecting:
			state_ = RdpConnectionState::Reconnecting ;
			break ;

		// 自動再接続完了では既存のlogin済みsessionへ戻った状態を保持する
		case RdpEventKind::AutoReconnected:
			// 既存のログイン済みsessionへ復帰するため、表示更新可能状態を復元する
			state_ = RdpConnectionState::LoggedIn ;
			break ;

		// 認証警告中は固定入力によるfocus取得を禁止する
		case RdpEventKind::AuthenticationWarningDisplayed:
			authentication_warning_active_ = true ;
			break ;

		// 警告解除だけではlogin完了へ遷移させない
		case RdpEventKind::AuthenticationWarningDismissed:
			authentication_warning_active_ = false ;
			break ;

		// channel受信だけではRDP lifecycleを変更せずobserverへ通知する
		case RdpEventKind::ChannelDataReceived:
		// 全画面開始要求は実際の状態変化eventまでlifecycleを変更しない
		case RdpEventKind::RequestGoFullScreen:
		// Window復帰要求はcontainer処理へ通知し、ここでは状態を推測しない
		case RdpEventKind::RequestLeaveFullScreen:
		// 最小化要求ではRDP接続状態を変えずownerへ通知する
		case RdpEventKind::RequestContainerMinimize:
		// 警告は致命的errorと区別して現在のRDP状態を維持する
		case RdpEventKind::Warning:
		// 終了確認要求だけでは切断済みへ遷移しない
		case RdpEventKind::ConfirmClose:
			break ;
		}

		observer_.OnRdpEvent( std::move( event)) ;
	}

private:
	/**
	 * @brief RdpEventSinkへ渡す内部observer参照を明示する。
	 * @return 本ImplをIRdpEventObserverとして参照する非所有reference。
	 */
	[[nodiscard]] IRdpEventObserver &this_as_observer() noexcept
	{
		return *this ;
	}

	/**
	 * @brief 現在スレッドがActiveX生成元COM STAか確認する。
	 * @return 所有threadではS_OK、それ以外ではRPC_E_WRONG_THREAD。
	 */
	[[nodiscard]] HRESULT EnsureUiThread() const noexcept
	{
		// 全ActiveX操作と購読操作を、それらを生成したCOM STAへ限定する
		if( GetCurrentThreadId() != ui_thread_id_) {
			return RPC_E_WRONG_THREAD ;
		}

		return S_OK ;
	}

	/**
	 * @brief 切断callbackへMicrosoft提供の追加診断を有限長で補う。
	 * @param[in,out] event 数値理由と所有エラー説明を受け取る切断イベント。
	 */
	void PopulateDisconnectDiagnostics( RdpEvent &event)
	{
		event.locally_requested_disconnect = state_ == RdpConnectionState::Disconnecting ;

		// 有効なclient interfaceなしで診断COM propertyを参照しない
		if( client_ == nullptr) {
			return ;
		}

		ExtendedDisconnectReasonCode extended_reason = exDiscReasonNoInfo ;
		const HRESULT extended_result = client_->get_ExtendedDisconnectReason( &extended_reason) ;
		// 取得成功時だけ既定値をMicrosoftの拡張理由で置き換える
		if( SUCCEEDED( extended_result)) {
			event.extended_disconnect_reason = static_cast<long>( extended_reason) ;
		}

		ATL::CComQIPtr<IMsRdpClient5> diagnostic_client( client_) ;
		// GetErrorDescription非対応の旧Controlでは数値診断だけを保持する
		if( diagnostic_client == nullptr) {
			return ;
		}

		ATL::CComBSTR description ;
		const HRESULT description_result = diagnostic_client->GetErrorDescription(
			static_cast<unsigned int>( event.code),
			static_cast<unsigned int>( event.extended_disconnect_reason), &description) ;
		// 説明取得失敗またはnull BSTRでは、数値理由だけを通知する
		if( FAILED( description_result) || description.m_str == nullptr) {
			return ;
		}

		const UINT description_length = SysStringLen( description) ;
		// Microsoft提供文字列も有限長だけをUI所有バッファへコピーする
		if( description_length > kMaximumErrorDescriptionCharacters) {
			return ;
		}

		event.error_description.assign( description.m_str, description.m_str + description_length) ;
	}

	/**
	 * @brief ActiveXを収容するATL子ウィンドウを生成する。
	 * @param[in] parent 所有元トップレベルウィンドウ。
	 * @param[in] bounds 親クライアント座標の初期矩形。
	 * @return 生成結果を表すHRESULT。
	 */
	[[nodiscard]] HRESULT CreateHostWindow( HWND parent, const RECT &bounds) noexcept
	{
		constexpr DWORD host_style =
			WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP ;

		RECT	   mutable_bounds = bounds ;
		const HWND created = host_window_.Create( parent, &mutable_bounds, nullptr, host_style) ;
		// ATLがLastErrorを設定しない失敗もE_FAILとして明示する
		if( created == nullptr) {
			const DWORD error = GetLastError() ;
			return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32( error) ;
		}

		return S_OK ;
	}

	/**
	 * @brief 一つの候補CLSIDからActiveXとIMsRdpClientを生成する。
	 * @param[in] candidate 試行するMicrosoft Control候補。
	 * @return 両COM interfaceを所有できた場合はS_OK。
	 */
	[[nodiscard]] HRESULT CreateControl( const RdpControlCandidate &candidate) noexcept
	{
		std::array<wchar_t, 64> class_id_text{} ;
		const int text_length = StringFromGUID2( *candidate.class_id, class_id_text.data(),
												static_cast<int>( class_id_text.size())) ;
		// CLSIDをATL host用文字列へ変換できない場合は生成へ進まない
		if( text_length == 0) {
			return E_UNEXPECTED ;
		}

		ATL::CComPtr<IUnknown> control ;
		HRESULT				   result =
			host_window_.CreateControlEx( class_id_text.data(), nullptr, nullptr, &control) ;
		// クラス未登録を含む生成結果を候補選択側へそのまま返す
		if( FAILED( result)) {
			return result ;
		}

		ATL::CComQIPtr<IMsRdpClient> client( control) ;
		// 必須client interfaceがないCOM objectはRDP Controlとして採用しない
		if( client == nullptr) {
			return E_NOINTERFACE ;
		}

		control_ = control ;
		client_	 = client ;
		return S_OK ;
	}

	/**
	 * @brief Windows更新前と同じMicrosoft connection security dialogを選択する。
	 * @return 設定成功時はS_OK、旧ActiveXの未対応時はS_FALSE、それ以外は失敗HRESULT。
	 *
	 * warningの抑止や自動承認は行わず、利用者が継続可否を明示選択できる
	 * dialog実装だけを切り替える。write-only propertyへ未文書化readbackを行わない。
	 */
	[[nodiscard]] HRESULT ConfigureConnectionSecurityDialog() noexcept
	{
		ATL::CComQIPtr<IMsRdpExtendedSettings> extended_settings( control_) ;
		// interface自体がない旧Controlは更新前の既定dialogをそのまま使用する
		if( extended_settings == nullptr) {
			common::WriteDiagnosticLog(
				common::DiagnosticLevel::Warning,
				common::DiagnosticEvent::SecurityDialogConfigurationReturned,
				{
					common::DiagnosticHResult( E_NOINTERFACE),
					common::DiagnosticValue( common::DiagnosticFieldName::DialogVersion,
											kCompatibleSecurityDialogVersion),
					common::DiagnosticValue( common::DiagnosticFieldName::Enabled, 0U),
				}) ;
			return S_FALSE ;
		}

		ATL::CComBSTR property_name( kSecurityDialogVersionPropertyName) ;
		// 固定property名のBSTR割当失敗を未対応ActiveXとして扱わない
		if( property_name.m_str == nullptr) {
			return E_OUTOFMEMORY ;
		}

		ATL::CComVariant property_value ;
		property_value.vt	 = VT_UI4 ;
		property_value.ulVal = kCompatibleSecurityDialogVersion ;
		const HRESULT result = extended_settings->put_Property( property_name, &property_value) ;
		// property追加前のsystemは既定で旧dialogなので、既知未対応結果だけ継続する
		if( IsUnsupportedSecurityDialogProperty( result)) {
			common::WriteDiagnosticLog(
				common::DiagnosticLevel::Warning,
				common::DiagnosticEvent::SecurityDialogConfigurationReturned,
				{
					common::DiagnosticHResult( result),
					common::DiagnosticValue( common::DiagnosticFieldName::DialogVersion,
											kCompatibleSecurityDialogVersion),
					common::DiagnosticValue( common::DiagnosticFieldName::Enabled, 0U),
				}) ;
			return S_FALSE ;
		}

		common::WriteDiagnosticLog(
			FAILED( result) ? common::DiagnosticLevel::Error : common::DiagnosticLevel::Information,
			common::DiagnosticEvent::SecurityDialogConfigurationReturned,
			{
				common::DiagnosticHResult( result),
				common::DiagnosticValue( common::DiagnosticFieldName::DialogVersion,
										kCompatibleSecurityDialogVersion),
				common::DiagnosticValue( common::DiagnosticFieldName::Enabled,
										SUCCEEDED( result) ? 1U : 0U),
			}) ;
		return result ;
	}

	/**
	 * @brief 存在するATL host子ウィンドウを破棄してHWNDを初期化する。
	 */
	void DestroyHostWindow() noexcept
	{
		// 有効なHWNDを所有している場合だけATLへ破棄を依頼する
		if( IsWindow( host_window_.m_hWnd)) {
			static_cast<void>( host_window_.DestroyWindow()) ;
		}

		host_window_.m_hWnd = nullptr ;
	}

	/// 外部所有でImplより長く生存する最終イベント通知先。
	IRdpEventObserver		   &observer_ ;
	/// COM STA制約を検査する生成元thread ID。
	const DWORD					ui_thread_id_ ;
	/// ActiveXを収容するATL子ウィンドウの所有wrapper。
	ATL::CAxWindow				host_window_ ;
	/// 選択済みActiveX objectの所有参照。
	ATL::CComPtr<IUnknown>		control_ ;
	/// 接続・表示propertyを操作する所有client参照。
	ATL::CComPtr<IMsRdpClient>	client_ ;
	/// connection pointへ登録中の所有event sink参照。
	ATL::CComPtr<IDispatch>		event_sink_ ;
	/// event sink購読cookieのRAII所有者。
	ConnectionPointSubscription event_subscription_ ;
	/// callbackと同期してUIへ公開する接続状態。
	RdpConnectionState			state_{RdpConnectionState::NotInitialized} ;
	/// 全接続前設定が成功しConnect可能であることを示す。
	bool						settings_applied_{false} ;
	/// 接続前のWindowsキー転送設定の結果。失敗時もRDP画面は維持する。
	HRESULT						deployment_keyboard_configuration_result_{E_UNEXPECTED} ;
	/// readbackしたWindowsキー有効値。診断へは有効／無効だけを記録する。
	long						deployment_windows_key_{0L} ;
	/// readbackした標準RDP hook mode。未取得は-1。
	long						deployment_keyboard_hook_mode_{-1L} ;
	/// Microsoft認証警告のfocusを固定入力で奪わないための表示状態。
	bool						authentication_warning_active_{false} ;
	/// 現ActiveX instanceでESVNBRGをConnect前登録済みであることを示す。
	bool						virtual_channel_registered_{false} ;
	/// ActiveXが全画面中であることを示す。
	bool						full_screen_{false} ;
	/// 選択したControl候補の固定表示名。
	std::wstring				selected_control_name_ ;
} ;

// ---------------------------------------------------------------------------
// 公開RdpControlHostから所有実装への委譲
// ---------------------------------------------------------------------------

RdpControlHost::RdpControlHost( IRdpEventObserver &observer)
	: impl_( std::make_unique<Impl>( observer))
{
}

RdpControlHost::~RdpControlHost()
{
	// impl_破棄前に生成元STAで明示的なActiveX終了順序を適用する
	Destroy() ;
}

HRESULT RdpControlHost::Create( HWND parent, const RECT &bounds) noexcept
{
	return impl_->Create( parent, bounds) ;
}

HRESULT RdpControlHost::ApplyConnectionSettings( const RdpConnectionSettings &settings) noexcept
{
	return impl_->ApplyConnectionSettings( settings) ;
}

HRESULT RdpControlHost::Connect() noexcept
{
	return impl_->Connect() ;
}

RKeyEmulationResult
RdpControlHost::SendDeploymentPaste( const std::function<bool()> &authorized) noexcept
{
	return impl_->SendDeploymentPaste( authorized) ;
}

RKeyEmulationResult RdpControlHost::SendAgentStart( const std::function<bool()> &authorized) noexcept
{
	return impl_->SendAgentStart( authorized) ;
}

RKeyEmulationResult
RdpControlHost::SendDeploymentRun( const std::function<bool()> &authorized) noexcept
{
	return impl_->SendDeploymentRun( authorized) ;
}

HRESULT RdpControlHost::PrepareDeploymentInput() noexcept
{
	return impl_->PrepareDeploymentInput() ;
}

HRESULT RdpControlHost::VerifyDeploymentInput() noexcept
{
	return impl_->VerifyDeploymentInput() ;
}

HRESULT RdpControlHost::SendStaticVirtualChannelData( const std::span<const std::byte> data) noexcept
{
	return impl_->SendStaticVirtualChannelData( data) ;
}

HRESULT RdpControlHost::Disconnect() noexcept
{
	return impl_->Disconnect() ;
}

HRESULT RdpControlHost::RequestClose( bool &can_close_now) noexcept
{
	return impl_->RequestClose( can_close_now) ;
}

HRESULT RdpControlHost::SetFullScreen( const bool full_screen) noexcept
{
	return impl_->SetFullScreen( full_screen) ;
}

HRESULT RdpControlHost::SetSmartSizing( const bool enabled) noexcept
{
	return impl_->SetSmartSizing( enabled) ;
}

HRESULT RdpControlHost::UpdateSessionDisplaySettings( const long desktop_width,
													 const long desktop_height) noexcept
{
	return impl_->UpdateSessionDisplaySettings( desktop_width, desktop_height) ;
}

HRESULT RdpControlHost::Resize( const RECT &bounds) noexcept
{
	return impl_->Resize( bounds) ;
}

void RdpControlHost::Focus() noexcept
{
	impl_->Focus() ;
}

void RdpControlHost::Destroy() noexcept
{
	// move等で実装所有が無い場合にも終了処理を安全に反復可能にする
	if( impl_ != nullptr) {
		impl_->Destroy() ;
	}
}

HWND RdpControlHost::window() const noexcept
{
	return impl_->window() ;
}

RdpConnectionState RdpControlHost::state() const noexcept
{
	return impl_->state() ;
}

bool RdpControlHost::full_screen() const noexcept
{
	return impl_->full_screen() ;
}

std::wstring_view RdpControlHost::selected_control_name() const noexcept
{
	return impl_->selected_control_name() ;
}

} // namespace zrdp::client::rdp
