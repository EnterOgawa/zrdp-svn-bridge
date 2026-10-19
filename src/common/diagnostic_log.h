//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file diagnostic_log.h
 * @brief 機密情報を受け付けない構造化診断ログの公開契約を定義する。
 * @version 01.00.08
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 任意文字列のログAPIを公開せず、固定event名と数値fieldだけを
 *          bounded queueへ渡すことで、接続情報やpayloadの誤記録を防止する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  初期配置の停止段階とHRESULT診断を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  自動入力のfocusとWindowsキー転送を確認。
 * Ver.01.00.03   Shunji Ogawa  2026/10/09  配置から独立した開発用Win+R testを追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Win単独とR単独の固定結果eventを追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  RのWindows入力比較と元Win32 errorを数値記録。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  固定Win+RのSendInput比較と停止境界を追加。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include <Windows.h>

#include <cstdint>
#include <initializer_list>
#include <string>

namespace zrdp::common {

/**
 * @brief 診断recordへ固定値として記録するprocessの役割。
 */
enum class DiagnosticRole {
	/// 開発PC側のRDP Client。
	Client,
	/// 支給PC側のSVN Bridge Agent。
	Agent,
} ;

/**
 * @brief 構造化診断eventのseverity。
 */
enum class DiagnosticLevel {
	/// 正常な状態遷移または完了。
	Information,
	/// 接続継続可能な異常または縮退。
	Warning,
	/// 操作失敗または接続不能な異常。
	Error,
} ;

/**
 * @brief 診断loggerが受け付ける固定event名。
 *
 * 自由形式messageを意図的に提供しないことで、接続先、domain、ユーザー名、
 * credential、URL、header、payloadが誤ってloggerへ渡ることを防止する。
 */
enum class DiagnosticEvent {
	/// processが起動条件の確認を開始したことを記録する。
	ProcessStarted,
	/// processの資源解放と停止を記録する。
	ProcessStopping,
	/// COM STAの初期化失敗を記録する。
	ComInitializationFailed,
	/// top-level Window生成の開始を記録する。
	MainWindowCreateStarted,
	/// top-level Window生成の成功を記録する。
	MainWindowCreated,
	/// top-level Window生成の失敗を記録する。
	MainWindowCreateFailed,
	/// Microsoft Control生成の開始を記録する。
	ActiveXCreateStarted,
	/// Microsoft Control生成の成功を記録する。
	ActiveXCreated,
	/// Microsoft Control生成の失敗を記録する。
	ActiveXCreateFailed,
	/// 利用可能なMicrosoft Controlの選択結果を記録する。
	ActiveXControlSelected,
	/// 認証UIの親をControlの既定Windowへ委譲したことを記録する。
	ActiveXDefaultDialogParentSelected,
	/// 認証dialogの互換設定の結果を記録する。
	SecurityDialogConfigurationReturned,
	/// server認証level設定の結果を記録する。
	AuthenticationLevelConfigurationReturned,
	/// CredSSP設定の結果を記録する。
	CredSspConfigurationReturned,
	/// SVCの接続前登録の結果を記録する。
	VirtualChannelRegistrationReturned,
	/// DPAPI履歴の読込みの結果を記録する。
	ConnectionHistoryLoadReturned,
	/// 接続履歴の暗号化保存の結果を記録する。
	ConnectionHistorySaveReturned,
	/// 利用者から新しい接続要求を受けたことを記録する。
	ConnectButtonClicked,
	/// 接続入力が事前条件を満たさず拒否したことを記録する。
	ConnectInputRejected,
	/// RDP接続前設定の適用の開始を記録する。
	ConnectionSettingsApplyStarted,
	/// RDP接続前設定の適用の結果を記録する。
	ConnectionSettingsApplyReturned,
	/// ActiveXの接続要求の開始を記録する。
	ConnectCallStarted,
	/// ActiveXの接続要求の結果を記録する。
	ConnectCallReturned,
	/// 条件に一致するlogin前失敗の有限再試行予約を記録する。
	PreLoginConnectionRetryScheduled,
	/// 予約済みのlogin前再試行の開始を記録する。
	PreLoginConnectionRetryStarted,
	/// 有限再試行後のlogin成立を記録する。
	PreLoginConnectionRetryCompleted,
	/// login前再試行上限への到達を記録する。
	PreLoginConnectionRetryExhausted,
	/// local操作による接続終了要求を記録する。
	DisconnectButtonClicked,
	/// ActiveXの切断要求の結果を記録する。
	DisconnectCallReturned,
	/// containerとControlの全画面切替の結果を記録する。
	FullScreenCallReturned,
	/// Client側表示の拡縮設定の結果を記録する。
	SmartSizingCallReturned,
	/// remote desktop寸法の更新の結果を記録する。
	DynamicResolutionCallReturned,
	/// Windowの終了要求を受けたことを記録する。
	CloseRequested,
	/// 旧streamを破棄し新しいBridge交渉を開始したことを記録する。
	BridgeSessionStarted,
	/// HELLO交渉が成立したことを記録する。
	BridgeReady,
	/// stream受付を停止してbinary Echoを開始したことを記録する。
	BridgeEchoStarted,
	/// Echo内容と順序の検証成功を記録する。
	BridgeEchoPassed,
	/// peer応答期限を超えたことを記録する。
	BridgePeerTimeout,
	/// Bridgeの継続不能な異常を記録する。
	BridgeFailed,
	/// Proxy workerの状態遷移を記録する。
	ClientProxyStateChanged,
	/// Proxy listenerの開始失敗を記録する。
	ClientProxyStartFailed,
	/// current RDP sessionの検証の結果を記録する。
	AgentSessionValidationReturned,
	/// 同じRDP sessionのSVC openの結果を記録する。
	AgentChannelOpenReturned,
	/// allowlist全体の検証の結果を記録する。
	AgentAllowlistLoadReturned,
	/// AgentのWinsock初期化の結果を記録する。
	AgentWinsockInitializationReturned,
	/// allowlist限定中継を有効にしたことを記録する。
	AgentStreamRelayEnabled,
	/// 旧SVC handleの読込み失敗を記録する。
	AgentChannelReadFailed,
	/// SVCの完全書込みに失敗したことを記録する。
	AgentChannelWriteFailed,
	/// Microsoft Controlのeventを固定分類で記録する。RDP接続と認証処理が進行中である。
	EventConnecting,
	/// Microsoft Controlのeventを固定分類で記録する。RDP transportが成立したが、ログイン完了をまだ保証しない。
	EventConnected,
	/// Microsoft Controlのeventを固定分類で記録する。認証完了後のsession利用開始を通知する。
	EventLoginComplete,
	/// Microsoft Controlのeventを固定分類で記録する。RDP接続を保持せず、新しい接続操作を受け付ける。
	EventDisconnected,
	/// Microsoft Controlのeventを固定分類で記録する。Controlが全画面状態へ移ったことを通知する。
	EventEnterFullScreen,
	/// Microsoft Controlのeventを固定分類で記録する。Controlが通常Window状態へ戻ったことを通知する。
	EventLeaveFullScreen,
	/// Microsoft Controlのeventを固定分類で記録する。受信BSTRのbinary dataを所有copyとしてworkerへ渡す。
	EventChannelDataReceived,
	/// Microsoft Controlのeventを固定分類で記録する。Controlからcontainer全体の全画面化を要求している。
	EventRequestGoFullScreen,
	/// Microsoft Controlのeventを固定分類で記録する。Controlからcontainerの通常配置への復帰を要求している。
	EventRequestLeaveFullScreen,
	/// Microsoft Controlのeventを固定分類で記録する。接続バーからSession Window全体の標準最小化を要求している。
	EventRequestContainerMinimize,
	/// Microsoft Controlのeventを固定分類で記録する。Controlの致命的errorを分類済み数値で通知する。
	EventFatalError,
	/// Microsoft Controlのeventを固定分類で記録する。操作継続可能な警告を分類済み数値で通知する。
	EventWarning,
	/// Microsoft Controlのeventを固定分類で記録する。Control破棄に先立つ接続終了の確認を通知する。
	EventConfirmClose,
	/// Microsoft Controlのeventを固定分類で記録する。Microsoft Controlによる同じsessionへの自動再接続を通知する。
	EventAutoReconnecting,
	/// Microsoft Controlのeventを固定分類で記録する。RDPが自動回復し、新しいBridge交渉を開始できる。
	EventAutoReconnected,
	/// Microsoft Controlのeventを固定分類で記録する。認証警告UIの表示中であり、containerはfocusを奪わない。
	EventAuthenticationWarningDisplayed,
	/// Microsoft Controlのeventを固定分類で記録する。認証警告UIの終了を通知し、利用者の判断を維持する。
	EventAuthenticationWarningDismissed,
	/// ActiveX eventの検証またはUIへの配送失敗を記録する。
	EventDispatchFailed,
	/// 外部callbackへ漏らさず捕捉した予期しない例外を記録する。
	UnexpectedException,
	/// bounded log queueから記録を破棄した件数を記録する。
	LogRecordsDropped,
	/// 初回Agent配置の各入力段階のAPI結果を記録する。
	AutomaticDeploymentInputReturned,
	/// 初回Agent配置画面の最初の停止結果を記録する。
	AutomaticDeploymentStopped,
	/// 標準RDPのWindowsキー転送設定とreadbackを記録する。
	DeploymentKeyboardConfigurationReturned,
	/// 固定入力前のforeground／focus確認の結果を記録する。
	DeploymentInputFocusReturned,
	/// 配置とは独立した、固定Win+R開発用testの数値結果。
	RunInputTestReturned,
	/// 左Windowsキー単独testの固定数値結果。
	WinKeyInputTestReturned,
	/// Rキー単独testの固定数値結果。
	RKeyInputTestReturned,
	/// R単独SendInput比較のcontroller段階・分類・HRESULT。
	RKeyEmulationTestReturned,
	/// R単独SendInput境界の受付数・固定分類・Win32 error。
	RKeyEmulationInputReturned,
	/// 固定Win+R SendInput比較のcontroller段階・分類・HRESULT。
	WinRKeyEmulationTestReturned,
	/// 固定Win+R SendInput境界の受付数・固定分類・Win32 error。
	WinRKeyEmulationInputReturned,
	/// 初回配置Win+RのSendInput受付数と元error。
	DeploymentRunInputReturned,
	/// 確認済み配置Ctrl+VのSendInput受付数と元error。
	DeploymentPasteInputReturned,
	/// 固定Agent EnterのSendInput受付数と元error。
	AgentStartInputReturned,
	/// 配置なしのCtrl+V診断controllerの段階と元結果。
	PasteKeyEmulationTestReturned,
	/// 配置なしのCtrl+V診断の受付数と元error。clipboard内容は含めない。
	PasteKeyEmulationInputReturned,
	/// メモ帳向けEnter診断controllerの段階と元結果。
	EnterKeyEmulationTestReturned,
	/// メモ帳向けEnter診断の受付数と元error。入力先は含めない。
	EnterKeyEmulationInputReturned,
} ;

/**
 * @brief 診断recordへ追加できる固定数値field名。
 */
enum class DiagnosticFieldName {
	/// COM境界のHRESULTをbit patternで保持する。
	HResult,
	/// 呼出し境界が定義した固定errorまたは状態code。
	Code,
	/// 現在の回復期間で完了した試行回数。
	AttemptCount,
	/// 実行可能な再試行回数の上限。
	MaximumAttemptCount,
	/// network利用可能性をbool相当の数値で示す。
	NetworkAvailable,
	/// 転送または処理したbyte数。
	ByteCount,
	/// 読み込んだentryまたは破棄したlog recordの件数。
	RecordCount,
	/// 選択したMicrosoft Controlのversion。
	ControlVersion,
	/// Microsoft認証dialogの互換version。
	DialogVersion,
	/// 設定したserver認証のlevel。
	AuthenticationLevel,
	/// processまたは境界処理の終了code。
	ExitCode,
	/// Microsoft Control等が返した基本終了理由。
	Reason,
	/// Microsoft Controlが返した拡張切断理由。
	ExtendedReason,
	/// 対象機能の有効／無効をbool相当で示す。
	Enabled,
	/// 排他的RDP表示modeの固定値。
	DisplayMode,
	/// Bridgeのlifecycle状態。
	BridgeState,
	/// Bridge停止の分類済みerror。
	BridgeError,
	/// 利用者がlocal操作で切断を要求したかを示す。
	LocallyRequested,
	/// Windows資格情報UIの取消を識別する。
	CredentialPromptCancelled,
	/// server認証警告の明示拒否を識別する。
	SecurityWarningCancelled,
	/// local Proxy workerのlifecycle状態。
	ProxyState,
	/// Proxy停止の分類済みerror。
	ProxyError,
	/// sessionへ割り当てたloopback TCP port。
	Port,
	/// Windows APIの元error。0は追加error情報なしであり、成功を意味しない。
	Win32Error,
} ;

/**
 * @brief 一つの構造化診断eventへ付加する数値field。
 */
struct DiagnosticField {
	/// 数値の意味と出力keyを決める固定field名。
	DiagnosticFieldName name{DiagnosticFieldName::Code} ;

	/// signed値もbit patternを保持して格納する64-bit値。
	std::uint64_t		value{0U} ;
} ;

// rotation前のcurrent log最大size
inline constexpr std::uint64_t			kDiagnosticLogMaximumBytes = 1024U * 1024U ;

// current logの隣に保持するbackup世代数
inline constexpr unsigned int			kDiagnosticLogBackupGenerations = 3U ;

/**
 * @brief 自由形式textを使わずHRESULT fieldを生成する。
 * @param[in] result 記録するHRESULT。
 * @return 32-bitのHRESULT bit patternを保持した診断field。
 */
[[nodiscard]] constexpr DiagnosticField DiagnosticHResult( const HRESULT result) noexcept
{
	return DiagnosticField{
		DiagnosticFieldName::HResult,
		static_cast<std::uint64_t>( static_cast<std::uint32_t>( result)),
	} ;
}

/**
 * @brief 自由形式textを使わずsigned code fieldを生成する。
 * @param[in] code 記録するActiveXまたはWin32由来のcode。
 * @return 符号を保持した診断field。
 */
[[nodiscard]] constexpr DiagnosticField DiagnosticCode( const long code) noexcept
{
	return DiagnosticField{
		DiagnosticFieldName::Code,
		static_cast<std::uint64_t>( static_cast<std::int64_t>( code)),
	} ;
}

/**
 * @brief 固定名を持つunsigned数値fieldを生成する。
 * @param[in] name 診断recordへ出力するfield名。
 * @param[in] value 記録する数値。
 * @return 指定値を保持した診断field。
 */
[[nodiscard]] constexpr DiagnosticField DiagnosticValue( const DiagnosticFieldName name,
														const std::uint64_t		  value) noexcept
{
	return DiagnosticField{name, value} ;
}

/**
 * @brief ClientまたはAgent用のbounded診断logを開始する。
 *
 * @param[in] role 診断recordへ付与するprocessの役割。
 * @return workerとlog fileを利用可能にできた場合はtrue。
 *
 * EXE隣接のlogs directoryを第一候補とし、書込み不能な場合だけ
 * LOCALAPPDATA以下へfallbackする。
 */
[[nodiscard]] bool		   InitializeDiagnosticLog( DiagnosticRole role) noexcept ;

/**
 * @brief file I/Oを待たずに固定名の診断recordをqueueへ投入する。
 * @param[in] level eventのseverity。
 * @param[in] event 自由形式文字列を含まない固定event。
 * @param[in] fields eventへ付加する固定名の数値field群。
 *
 * queue満杯またはlogger停止中はrecordを破棄し、例外を呼出し元へ漏らさない。
 */
void					   WriteDiagnosticLog( DiagnosticLevel level, DiagnosticEvent event,
											  std::initializer_list<DiagnosticField> fields = {}) noexcept ;

/**
 * @brief 受理済みrecordをflushして診断logを停止する。
 *
 * 呼出しthreadでworkerの終了を待つため、UI終了処理以外からは呼び出さない。
 */
void					   ShutdownDiagnosticLog() noexcept ;

/**
 * @brief 選択されたlog pathのcopyを取得する。
 * @return 初期化成功時はlog path、失敗時は空文字列。
 */
[[nodiscard]] std::wstring DiagnosticLogPath() noexcept ;

} // namespace zrdp::common
