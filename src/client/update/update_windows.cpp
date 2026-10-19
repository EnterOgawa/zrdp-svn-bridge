//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_windows.cpp
 * @brief Windowsの公開Release確認と固定Clientの同意後差替えを実装する。
 * @version 01.00.01
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  同一EXEの限定helperとrollbackを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Client／Agentの二候補取得・セット置換・部分復元を実装。
 * </pre>
 *
 * @details networkとfile I/Oはworker／helperへ限定する。command lineは継承HANDLEと
 *          固定形式stage tokenだけを受け、任意の差替えpathやcommandを受けない。
 */
//------------------------------------------------------------------------------
#include "client/update/update_windows.h"

#include "client/update/github_release_source.h"
#include "client/update/update_apply.h"
#include "client/update/update_image_verifier.h"
#include "client/update/update_windows_handles.h"
#include "protocol/frame.h"

#include <Windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace zrdp::client::update {
namespace {

// 専用stage名のprefix。後続はランダムな32桁小文字hexだけ
constexpr std::wstring_view kStagePrefix				   = L".zrdp-update-" ;
// 一配置に放置stageを無制限に増やさない上限
constexpr std::size_t		kMaximumPendingStages		   = 2U ;
// 同じClientの一時copyだけを実行する固定helper名
constexpr wchar_t			kHelperFilename[]			   = L"ZRdpUpdateHelper.exe" ;
// 適用前の候補file名
constexpr wchar_t			kCandidateFilename[]		   = L"candidate.exe" ;
// 書込み後に閉じて検証する同意済みmanifest
constexpr wchar_t			kStageManifestFilename[]	   = L"manifest.ini" ;
// 旧版の不変を確認する固定digest file
constexpr wchar_t			kOriginalHashFilename[]		   = L"installed.sha256" ;
// transaction中の旧版backup名
constexpr wchar_t			kStageBackupFilename[]		   = L"previous.exe" ;
// 正常更新後に一世代だけ保全する旧版名
constexpr wchar_t			kRetainedBackupFilename[]	   = L"ZRdpSvnBridge.previous.exe" ;
// セットのClient側固定file名
constexpr wchar_t			kClientFilename[]			   = L"ZRdpSvnBridge.exe" ;
// Agent候補・元digest・backupはClientとは別の固定fileにする
constexpr wchar_t			kAgentFilename[]			   = L"ZRdpSvnAgent.exe" ;
constexpr wchar_t			kAgentCandidateFilename[]	   = L"agent-candidate.exe" ;
constexpr wchar_t			kAgentOriginalHashFilename[]   = L"installed-agent.sha256" ;
constexpr wchar_t			kAgentBackupFilename[]		   = L"agent-previous.exe" ;
constexpr wchar_t			kAgentRetainedFilename[]	   = L"ZRdpSvnAgent.previous.exe" ;
// helper受付と新版初期化の有限待機millisecond
constexpr ULONGLONG			kHandshakeTimeoutMilliseconds  = 30000U ;
// 親終了待機を永久常駐にしない上限millisecond
constexpr ULONGLONG			kParentExitTimeoutMilliseconds = 120000U ;
// stop要求を観測しながらHANDLEを待つ単位millisecond
constexpr DWORD				kWaitSliceMilliseconds		   = 100U ;

/**
 * @brief stage tokenを一つの安全なdirectory名に制限する。
 * @param[in] name prefixと32桁小文字hex。
 * @return 完全一致した場合だけtrue。
 */
[[nodiscard]] bool			IsStageName( const std::wstring_view name) noexcept
{
	// 空、相対path、slash、suffixを含む名前を受け付けない
	if( !name.starts_with( kStagePrefix) || name.size() != kStagePrefix.size() + 32U) {
		return false ;
	}

	// tokenは固定長ASCII hexだけに限定する
	for( const wchar_t value : name.substr( kStagePrefix.size())) {
		// Unicodeやpercent escapeをdirectory名へ混入させない
		if( !(value >= L'0' && value <= L'9') && !(value >= L'a' && value <= L'f')) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief reparseでない既存directoryか確認する。
 * @param[in] path 確認する絶対path。
 * @return 正常なdirectoryだけtrue。
 */
[[nodiscard]] bool IsPlainDirectory( const std::filesystem::path &path) noexcept
{
	const DWORD attributes = GetFileAttributesW( path.c_str()) ;
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
		   (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U ;
}

/**
 * @brief 固定Clientと専用隣接stageの関係を確認する。
 * @param[in] target 固定Client。
 * @param[in] stage 隣接stage。
 * @return 同じ親のplain directoryであればtrue。
 */
[[nodiscard]] bool HasSafeStageLayout( const std::filesystem::path &target,
									  const std::filesystem::path &stage) noexcept
{
	try {
		return target.is_absolute() && target.filename() == kClientFilename &&
			   stage.is_absolute() && stage.parent_path() == target.parent_path() &&
			   IsStageName( stage.filename().native()) && IsPlainDirectory( stage) &&
			   IsPlainDirectory( target.parent_path()) ;
	} catch( ...) {
		return false ;
	}
}

/**
 * @brief 実行中moduleの絶対pathを有限長で取得する。
 * @return path取得失敗／切詰め時は空。
 */
[[nodiscard]] std::filesystem::path ModulePath()
{
	std::vector<wchar_t> buffer( 32768U, L'\0') ;
	const DWORD			 length =
		GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>( buffer.size())) ;
	// 切詰めたpathで差替え先を推測しない
	if( length == 0U || length >= buffer.size()) {
		return {} ;
	}

	return std::filesystem::path( std::wstring( buffer.data(), length)).lexically_normal() ;
}

/**
 * @brief Windows ordinal規則で絶対image pathを比較する。
 * @param[in] left 比較元。
 * @param[in] right 比較先。
 * @return 大小文字差だけであればtrue。
 */
[[nodiscard]] bool SameImagePath( const std::filesystem::path &left,
								 const std::filesystem::path &right) noexcept
{
	const auto &first  = left.native() ;
	const auto &second = right.native() ;
	return !first.empty() && first.size() <= 32767U && second.size() <= 32767U &&
		   CompareStringOrdinal( first.data(), static_cast<int>( first.size()), second.data(),
								static_cast<int>( second.size()), TRUE) == CSTR_EQUAL ;
}

/**
 * @brief 継承process HANDLEが期待する固定imageを指すか確認する。
 * @param[in] process query権限を持つ非所有HANDLE。
 * @param[in] expected 期待するClient／helper絶対path。
 * @return 生存／終了済みprocessのimageを確認できた場合だけtrue。
 */
[[nodiscard]] bool ProcessHasImage( const HANDLE process, const std::filesystem::path &expected)
{
	std::vector<wchar_t> buffer( 32768U, L'\0') ;
	DWORD				 length = static_cast<DWORD>( buffer.size()) ;
	// 自分自身、無効HANDLE、別processを親／helperとして使わない
	if( GetProcessId( process) == 0U || GetProcessId( process) == GetCurrentProcessId() ||
	   !QueryFullProcessImageNameW( process, 0U, buffer.data(), &length) || length == 0U ||
	   length >= buffer.size()) {
		return false ;
	}

	return SameImagePath(
		std::filesystem::path( std::wstring( buffer.data(), length)).lexically_normal(), expected) ;
}

/**
 * @brief bounded file全体を読取り、欠落を区別する。
 * @param[in] path 固定local file。
 * @param[in] maximum byte上限。
 * @param[out] text 完全読取り値。
 * @param[in] allow_missing 不在を空入力として許可するか。
 * @return 完全読取り／許可した不在ならNone。
 */
[[nodiscard]] UpdateError ReadSmallFile( const std::filesystem::path &path,
										const std::size_t maximum, std::string &text,
										const bool allow_missing = false)
{
	text.clear() ;
	detail::KernelHandle file(
		CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) ;
	// 設定fileがない場合だけ、指定済みの既定sourceを使用する
	if( !file.valid()) {
		const DWORD error = GetLastError() ;
		return allow_missing && error == ERROR_FILE_NOT_FOUND ? UpdateError::None
															  : UpdateError::FileFailure ;
	}

	BY_HANDLE_FILE_INFORMATION information{} ;
	// 不変のplain fileだけを解析し、linkやdirectoryへ追従しない
	if( !GetFileInformationByHandle( file.get(), &information) ||
	   (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
		   0U ||
	   information.nNumberOfLinks != 1U || information.nFileSizeHigh != 0U ||
	   information.nFileSizeLow > maximum) {
		return UpdateError::DataTooLarge ;
	}

	text.resize( information.nFileSizeLow) ;
	// 空設定fileはparserへ空入力として渡す
	if( text.empty()) {
		return UpdateError::None ;
	}

	DWORD received = 0U ;
	// 全byteを読めなければ部分設定を採用しない
	if( !ReadFile( file.get(), text.data(), static_cast<DWORD>( text.size()), &received, nullptr) ||
	   received != text.size()) {
		text.clear() ;
		return UpdateError::FileFailure ;
	}

	return UpdateError::None ;
}

/**
 * @brief 既存fileを上書きせずbounded stage textを保存する。
 * @param[in] path stage内の固定file。
 * @param[in] text 検証済みASCII。
 * @return 全byteをflushできた場合だけNone。
 */
[[nodiscard]] UpdateError WriteNewText( const std::filesystem::path &path,
									   const std::string_view		text) noexcept
{
	// stage metadataを巨大値や空の不完全値で作成しない
	if( text.empty() || text.size() > kMaximumUpdateManifestBytes) {
		return UpdateError::InvalidManifest ;
	}

	detail::KernelHandle file( CreateFileW( path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
										  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) ;
	// 衝突したfileを他のupdateのmetadataとして上書きしない
	if( !file.valid()) {
		return UpdateError::FileFailure ;
	}

	DWORD written = 0U ;
	return WriteFile( file.get(), text.data(), static_cast<DWORD>( text.size()), &written, nullptr) &&
				   written == text.size() && FlushFileBuffers( file.get())
			   ? UpdateError::None
			   : UpdateError::FileFailure ;
}

/**
 * @brief 完全manifestを固定ASCII形式へ正規化する。
 * @param[in] release 検証済み／同意済みRelease。
 * @return 十の必須値だけを持つINI。
 */
[[nodiscard]] std::string SerializeManifest( const ReleaseManifest &release)
{
	return "[update]\nformat=2\nversion=" + FormatUpdateVersion( release.version) +
		   "\ntag=" + release.tag +
		   "\nasset=ZRdpSvnBridge.exe\nsha256=" + FormatSha256( release.sha256) +
		   "\nsize=" + std::to_string( release.size_bytes) +
		   "\nbridge_protocol=" + std::to_string( release.bridge_protocol) +
		   "\nagent_asset=ZRdpSvnAgent.exe\nagent_size=" +
		   std::to_string( release.agent_size_bytes) +
		   "\nagent_sha256=" + FormatSha256( release.agent_sha256) + "\n" ;
}

/**
 * @brief 専用stageの明示された固定fileだけを回収する。
 * @param[in] target 固定Client配置。
 * @param[in] stage 自身が作成または検証したstage。
 * @note recursive deleteや親directoryの削除は行わない。
 */
void RemoveOwnedStage( const std::filesystem::path &target,
					  const std::filesystem::path &stage) noexcept
{
	try {
		// 検証済みの専用stage以外は削除対象にしない
		if( !HasSafeStageLayout( target, stage)) {
			return ;
		}

		const std::array<const wchar_t *, 6U> names{
			kCandidateFilename, kStageManifestFilename,	 kOriginalHashFilename,
			kHelperFilename,	kAgentCandidateFilename, kAgentOriginalHashFilename} ;
		// 既知の一時fileだけを削除し、旧版backupや未知fileは保持する
		for( const wchar_t *name : names) {
			static_cast<void>( DeleteFileW( (stage / name).c_str())) ;
		}

		static_cast<void>( RemoveDirectoryW( stage.c_str())) ;
	} catch( ...) {
		// 回収失敗では未知fileへ対象を広げず、残ったstageを利用者確認用に保持する
	}
}

/** @brief FindFirstFileの列挙HANDLE専用解放責任。 */
struct FindCloser {
	/**
	 * @brief 有効な列挙HANDLEを閉じる。
	 * @param[in] value FindFirstFileで得た所有値。
	 */
	void operator()( void *value) const noexcept
	{
		static_cast<void>( FindClose( value)) ;
	}
} ;

/**
 * @brief 同一配置の放置stage数を有限にする。
 * @param[in] directory 固定Clientの親directory。
 * @return 既知stageが上限未満であればtrue。
 */
[[nodiscard]] bool CanCreateStage( const std::filesystem::path &directory)
{
	WIN32_FIND_DATAW data{} ;
	const HANDLE	 raw = FindFirstFileW( (directory / L".zrdp-update-*").c_str(), &data) ;
	// 該当fileがない場合だけ空のstage集合として扱う
	if( raw == INVALID_HANDLE_VALUE) {
		return GetLastError() == ERROR_FILE_NOT_FOUND ;
	}

	std::unique_ptr<void, FindCloser> search( raw) ;
	std::size_t						  count = 0U ;
	// Client隣接の既知prefixだけを数え、内容の探索や削除を行わない
	do {
		// 正式stage名を持つdirectoryだけを上限へ数える
		if( (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
		   IsStageName( data.cFileName)) {
			++count ;
			// 放置stageが重なった場合は新たなdisk消費を停止する
			if( count >= kMaximumPendingStages) {
				return false ;
			}
		}
	} while( FindNextFileW( search.get(), &data)) ;
	return GetLastError() == ERROR_NO_MORE_FILES ;
}

/**
 * @brief 同じvolumeの新規専用stageを一件作る。
 * @param[in] target 固定Client配置。
 * @param[out] stage 作成に成功した絶対path。
 * @return bounded stageを作成できた場合だけNone。
 */
[[nodiscard]] UpdateError CreateStage( const std::filesystem::path &target,
									  std::filesystem::path		  &stage)
{
	// plain親directoryと放置stage上限を確認してから新しい場所を作る
	if( !IsPlainDirectory( target.parent_path()) || !CanCreateStage( target.parent_path())) {
		return UpdateError::Busy ;
	}

	// 衝突時だけ最大四回の新しいrandom tokenを試す
	for( std::size_t attempt = 0U; attempt < 4U; ++attempt) {
		std::array<UCHAR, 16U> random{} ;
		// path tokenにはWindowsのCSPRNGを使用する
		if( BCryptGenRandom( nullptr, random.data(), static_cast<ULONG>( random.size()),
						   BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
			return UpdateError::ResourceExhausted ;
		}

		constexpr std::wstring_view alphabet = L"0123456789abcdef" ;
		std::wstring				name( kStagePrefix) ;
		// 16 random byteをslashのない32桁ASCII tokenへ変換する
		for( const UCHAR value : random) {
			name.push_back( alphabet[value >> 4U]) ;
			name.push_back( alphabet[value & 15U]) ;
		}

		const auto candidate = target.parent_path() / name ;
		// 既存stageを再利用せず、新規directoryだけを所有する
		if( CreateDirectoryW( candidate.c_str(), nullptr)) {
			stage = candidate ;
			return UpdateError::None ;
		}

		// 衝突以外の権限／disk失敗を別配置へのfallbackで回避しない
		if( GetLastError() != ERROR_ALREADY_EXISTS) {
			return UpdateError::PermissionDenied ;
		}
	}

	return UpdateError::Busy ;
}

/** @brief 同じClient親directoryを識別するprocess間update mutex。 */
class PlacementLock final {
public:
	/** @brief 未取得mutexを構築する。 */
	PlacementLock() noexcept = default ;
	/** @brief 同じthreadで取得したmutexだけを解放する。 */
	~PlacementLock()
	{
		// 取得前／失敗状態のmutexをReleaseMutexへ渡さない
		if( owned_) {
			static_cast<void>( ReleaseMutex( mutex_.get())) ;
		}
	}

	/** @brief lockの複製を禁止する。 */
	PlacementLock( const PlacementLock &)					   = delete ;
	/**
	 * @brief lockの複製を禁止する。
	 * @return 呼出し禁止のため値を生成しない。
	 */
	PlacementLock			 &operator=( const PlacementLock &) = delete ;
	/**
	 * @brief ローカルvolumeの親directory identityを使って排他的に取得する。
	 * @param[in] target 固定Client配置。
	 * @return 取得できた場合だけNone。競合はBusy。
	 */
	[[nodiscard]] UpdateError Acquire( const std::filesystem::path &target)
	{
		std::array<wchar_t, 32768U> volume{} ;
		// network shareや不明なvolumeをClient差替え先にしない
		if( !GetVolumePathNameW( target.c_str(), volume.data(), static_cast<DWORD>( volume.size())) ||
		   (GetDriveTypeW( volume.data()) != DRIVE_FIXED &&
			GetDriveTypeW( volume.data()) != DRIVE_REMOVABLE)) {
			return UpdateError::PermissionDenied ;
		}

		detail::KernelHandle	   directory( CreateFileW(
			  target.parent_path().c_str(), FILE_READ_ATTRIBUTES,
			  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) ;
		BY_HANDLE_FILE_INFORMATION information{} ;
		// 親directoryの同一identityを使い、pathの大小文字差でlockを分離しない
		if( !directory.valid() || !GetFileInformationByHandle( directory.get(), &information) ||
		   (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
		   (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
			return UpdateError::FileFailure ;
		}

		const auto index = (static_cast<std::uint64_t>( information.nFileIndexHigh) << 32U) |
						   information.nFileIndexLow ;
		const std::wstring name = L"Local\\ZRdpClientUpdate-" +
								  std::to_wstring( information.dwVolumeSerialNumber) + L"-" +
								  std::to_wstring( index) ;
		mutex_.reset( CreateMutexW( nullptr, FALSE, name.c_str())) ;
		// lock作成失敗では差替えを開始しない
		if( !mutex_.valid()) {
			return UpdateError::ResourceExhausted ;
		}

		const DWORD waited = WaitForSingleObject( mutex_.get(), 0U) ;
		// 他processのupdate中は待機や強制終了を行わず、対象updateだけを停止する
		if( waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
			return waited == WAIT_TIMEOUT ? UpdateError::Busy : UpdateError::FileFailure ;
		}

		owned_ = true ;
		return UpdateError::None ;
	}

private:
	/// このthreadが所有するnamed mutex HANDLE。
	detail::KernelHandle mutex_ ;
	/// ReleaseMutexが必要な取得済み状態だけtrue。
	bool				 owned_{false} ;
} ;

/**
 * @brief HANDLEを限定権限で子へ継承するために複製する。
 * @param[in] source 現在のprocessが保持するHANDLEまたはpseudo HANDLE。
 * @param[in] access 必要最小限の権限。
 * @param[out] duplicate 子へ継承する所有HANDLE。
 * @return 複製できた場合だけtrue。
 */
[[nodiscard]] bool InheritableHandle( const HANDLE source, const DWORD access,
									 detail::KernelHandle &duplicate) noexcept
{
	HANDLE value = nullptr ;
	// 任意の他processへHANDLEを複製せず、現在のprocess内だけで準備する
	if( !DuplicateHandle( GetCurrentProcess(), source, GetCurrentProcess(), &value, access, TRUE,
						0U)) {
		return false ;
	}

	duplicate.reset( value) ;
	return true ;
}

/** @brief STARTUPINFOEXの属性listをscopeで解放する。 */
class AttributeList final {
public:
	/** @brief 未初期化の属性listを構築する。 */
	AttributeList() = default ;
	/** @brief 初期化済み属性listだけをWindowsへ解放する。 */
	~AttributeList()
	{
		// 初期化に失敗したmemoryをWindowsのlist解放へ渡さない
		if( initialized_) {
			DeleteProcThreadAttributeList( get()) ;
		}
	}

	/** @brief 属性listの複製を禁止する。 */
	AttributeList( const AttributeList &)				= delete ;
	/**
	 * @brief 属性listの複製を禁止する。
	 * @return 呼出し禁止のため値を生成しない。
	 */
	AttributeList	  &operator=( const AttributeList &) = delete ;
	/**
	 * @brief 継承する二HANDLEだけを明示する。
	 * @param[in] handles 有効なinheritable HANDLE一覧。
	 * @return 初期化と一覧設定に成功した場合だけtrue。
	 */
	[[nodiscard]] bool Initialize( std::array<HANDLE, 2U> &handles)
	{
		SIZE_T bytes = 0U ;
		static_cast<void>( InitializeProcThreadAttributeList( nullptr, 1U, 0U, &bytes)) ;
		// Windowsの属性memoryも有限にし、異常な必要sizeを採用しない
		if( bytes == 0U || bytes > 65536U) {
			return false ;
		}

		storage_.resize( bytes) ;
		// 初期化されていないlistをCreateProcessへ渡さない
		if( !InitializeProcThreadAttributeList( get(), 1U, 0U, &bytes)) {
			return false ;
		}

		initialized_ = true ;
		return UpdateProcThreadAttribute( get(), 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
										 handles.data(), sizeof( handles), nullptr,
										 nullptr) != FALSE ;
	}

	/**
	 * @brief Windows ABIで所有memoryの非所有listを返す。
	 * @return 初期化先または初期化済みlist。
	 */
	[[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept
	{
		// Windowsが指定するopaque list memoryとしてだけ型付けする
		return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>( storage_.data()) ;
	}

private:
	/// Windowsが初期化するopaque属性memory。listより長く保持する。
	std::vector<std::byte> storage_ ;
	/// 初期化成功後だけtrue。
	bool				   initialized_{false} ;
} ;

/**
 * @brief shellを介さず固定EXEへ二HANDLEだけを継承して起動する。
 * @param[in] image 検証済みの絶対EXE path。
 * @param[in] arguments 限定内部構文だけを含む引数。
 * @param[in] first 親／helperのprocess待機HANDLE。
 * @param[in] second 受付／初期化完了event HANDLE。
 * @param[out] process 起動した子processの単独所有HANDLE。
 * @return 起動と所有移転が成功した場合だけtrue。
 */
[[nodiscard]] bool StartInternalProcess( const std::filesystem::path &image,
										const std::wstring &arguments, const HANDLE first,
										const HANDLE second, detail::KernelHandle &process)
{
	std::array<HANDLE, 2U> handles{first, second} ;
	AttributeList		   attributes ;
	// 継承先を限定できない状態でbInheritHandlesを有効にしない
	if( !attributes.Initialize( handles)) {
		return false ;
	}

	std::wstring   command = L"\"" + image.native() + L"\" " + arguments ;
	STARTUPINFOEXW startup{} ;
	startup.StartupInfo.cb	= sizeof( startup) ;
	startup.lpAttributeList = attributes.get() ;
	PROCESS_INFORMATION information{} ;
	// applicationNameを明示し、検索path、shell、管理者昇格を使用しない
	if( !CreateProcessW( image.c_str(), command.data(), nullptr, nullptr, TRUE,
					   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
					   image.parent_path().c_str(), &startup.StartupInfo, &information)) {
		return false ;
	}

	detail::KernelHandle thread( information.hThread) ;
	process.reset( information.hProcess) ;
	return true ;
}

/**
 * @brief 内部HANDLEの十進値をcommand line用に表す。
 * @param[in] handle 継承する有効なHANDLE。
 * @return pointer幅の十進表記。
 */
[[nodiscard]] std::wstring HandleArgument( const HANDLE handle)
{
	// Win32 HANDLEの数値表現は限定内部protocolにだけ使用し、logへ出さない
	return std::to_wstring( reinterpret_cast<std::uintptr_t>( handle)) ;
}

/**
 * @brief 継承HANDLEだけをpointer幅の十進値から確認する。
 * @param[in] argument 限定内部token。
 * @param[out] handle 確認済みの所有HANDLE。
 * @return 有効な継承HANDLEだけtrue。
 */
[[nodiscard]] bool ReadHandleArgument( const std::wstring_view argument,
									  detail::KernelHandle	 &handle) noexcept
{
	// pointer値の上限を越えるtextや空の値を解析しない
	if( argument.empty() || argument.size() > 20U) {
		return false ;
	}

	std::array<char, 20U> ascii{} ;
	// 十進数字以外を変換せず、符号やescape表現を拒否する
	for( std::size_t index = 0U; index < argument.size(); ++index) {
		// Unicodeを下位byteへ切詰めてHANDLE値にしない
		if( argument[index] < L'0' || argument[index] > L'9') {
			return false ;
		}

		ascii[index] = static_cast<char>( argument[index]) ;
	}

	std::uintptr_t parsed = 0U ;
	const auto	   result = std::from_chars( ascii.data(), ascii.data() + argument.size(), parsed) ;
	// null、overflow、部分解析をHANDLEへ変換しない
	if( result.ec != std::errc{} || result.ptr != ascii.data() + argument.size() || parsed == 0U) {
		return false ;
	}

	// Win32内部protocolの整数をHANDLEとしてだけ型付けする
	const HANDLE value = reinterpret_cast<HANDLE>( parsed) ;
	DWORD		 flags = 0U ;
	// 実際に継承された有効HANDLEだけを所有し、将来の子へ再継承しない
	if( value == INVALID_HANDLE_VALUE || !GetHandleInformation( value, &flags) ||
	   (flags & HANDLE_FLAG_INHERIT) == 0U ||
	   !SetHandleInformation( value, HANDLE_FLAG_INHERIT, 0U)) {
		return false ;
	}

	handle.reset( value) ;
	return true ;
}

/**
 * @brief 二HANDLEの有限handshakeを終了要求とともに待つ。
 * @param[in] ready 子が通知するevent。
 * @param[in] process 子process終了の確認HANDLE。
 * @param[in] stop worker終了要求。
 * @return readyはWAIT_OBJECT_0、子終了は+1、停止／期限はWAIT_TIMEOUT。
 */
[[nodiscard]] DWORD WaitHandshake( const HANDLE ready, const HANDLE process,
								  const std::stop_token stop) noexcept
{
	const std::array<HANDLE, 2U> handles{ready, process} ;
	const ULONGLONG				 started = GetTickCount64() ;
	// 無限待機をせず、各sliceで終了要求と全体期限を観測する
	while( !stop.stop_requested() && GetTickCount64() - started < kHandshakeTimeoutMilliseconds) {
		const DWORD waited = WaitForMultipleObjects( static_cast<DWORD>( handles.size()),
													handles.data(), FALSE, kWaitSliceMilliseconds) ;
		// 受付／終了／wait失敗だけをcallerへ返し、timeout sliceでは継続する
		if( waited != WAIT_TIMEOUT) {
			return waited ;
		}
	}

	return WAIT_TIMEOUT ;
}

/**
 * @brief 同意済みstageと旧版digestを同じ有限parserで再読込みする。
 * @param[in] stage 固定専用stage。
 * @param[out] release 完全な同意済みmanifest。
 * @param[out] original_hash 初回Client digest。
 * @param[out] agent_hash 初回Agent digest。
 * @return 全値を読めた場合だけNone。
 */
[[nodiscard]] UpdateError ReadStageMetadata( const std::filesystem::path &stage,
											ReleaseManifest &release, Sha256 &original_hash,
											Sha256 &agent_hash)
{
	std::string		  text ;
	const UpdateError read =
		ReadSmallFile( stage / kStageManifestFilename, kMaximumUpdateManifestBytes, text) ;
	// stage metadataの欠落を既定manifestへ置き換えない
	if( read != UpdateError::None) {
		return read ;
	}

	const UpdateError parsed = ParseReleaseManifest( text, release) ;
	// 不完全な同意情報でhelperを開始しない
	if( parsed != UpdateError::None) {
		return parsed ;
	}

	// 元のdigestを完全に読めなければ旧版の不変を保証できない
	if( ReadSmallFile( stage / kOriginalHashFilename, 64U, text) != UpdateError::None ||
	   !ParseSha256( text, original_hash) ||
	   ReadSmallFile( stage / kAgentOriginalHashFilename, 64U, text) != UpdateError::None ||
	   !ParseSha256( text, agent_hash)) {
		return UpdateError::InvalidManifest ;
	}

	return UpdateError::None ;
}

/**
 * @brief 保全済み旧版copyのdigestとWindows署名を確認する。
 * @param[in] path 確認対象の旧版copy。
 * @param[in] original_hash 初回Client digest。
 * @return 旧版が完全で信頼される場合だけtrue。
 */
[[nodiscard]] bool IsOriginalImage( const std::filesystem::path &path,
								   const Sha256				   &original_hash) noexcept
{
	Sha256				hash{} ;
	std::uint64_t		bytes = 0U ;
	ClientImageMetadata metadata ;
	return ComputeUpdateFileHash( path, hash, bytes) == UpdateError::None && hash == original_hash &&
		   VerifyClientSignaturePair( path, path) == UpdateError::None &&
		   ReadClientImageMetadata( path, metadata) == UpdateError::None &&
		   metadata.update_bootstrap && metadata.bridge_protocol == protocol::kProtocolVersion ;
}

/**
 * @brief 元Agentのhash・署名・同一製品版を固定Client copyへ照合する。
 * @param[in] path 元Agentまたは保全したbackup。
 * @param[in] anchor 署名済み元Clientまたはhelper copy。
 * @param[in] original_hash 元Agentの完全digest。
 * @return 元の署名済みAgentが完全な場合だけtrue。
 */
[[nodiscard]] bool IsOriginalAgent( const std::filesystem::path &path,
								   const std::filesystem::path &anchor,
								   const Sha256				   &original_hash) noexcept
{
	Sha256				hash{} ;
	std::uint64_t		bytes = 0U ;
	ClientImageMetadata client, agent ;
	return ComputeUpdateFileHash( path, hash, bytes) == UpdateError::None && hash == original_hash &&
		   VerifyClientSignaturePair( anchor, path) == UpdateError::None &&
		   ReadClientImageMetadata( anchor, client) == UpdateError::None &&
		   ReadAgentImageMetadata( path, agent) == UpdateError::None &&
		   agent.version == client.version && agent.agent_setup &&
		   agent.bridge_protocol == protocol::kProtocolVersion ;
}

/**
 * @brief 元セットの各fileの型・hash・署名を確認する。
 * @param[in] index 0はClient、1はAgent。その他は拒否する。
 * @param[in] path 元fileまたはbackupの確認対象。
 * @param[in] anchor 元Clientの保全copy。
 * @param[in] client_hash 元Clientの完全digest。
 * @param[in] agent_hash 元Agentの完全digest。
 * @return 対応する元fileが完全な場合だけtrue。
 */
[[nodiscard]] bool IsOriginalBundleFile( const std::size_t index, const std::filesystem::path &path,
										const std::filesystem::path &anchor,
										const Sha256				&client_hash,
										const Sha256				&agent_hash) noexcept
{
	return index == 0U ? IsOriginalImage( path, client_hash)
					   : index == 1U && IsOriginalAgent( path, anchor, agent_hash) ;
}

/**
 * @brief 固定二EXEと各候補・backupの三pathだけを許可する。
 * @param[in] target 固定ClientまたはAgent。
 * @param[in] candidate 対応するstage候補。
 * @param[in] backup 対応するstage backup。
 * @return 隣接専用stage内の完全一致pathだけtrue。
 */
[[nodiscard]] bool HasBundleFileLayout( const std::filesystem::path &target,
									   const std::filesystem::path &candidate,
									   const std::filesystem::path &backup)
{
	const bool agent  = target.filename() == kAgentFilename ;
	const auto anchor = target.parent_path() / kClientFilename ;
	return (target.filename() == kClientFilename || agent) &&
		   HasSafeStageLayout( anchor, candidate.parent_path()) &&
		   backup.parent_path() == candidate.parent_path() &&
		   candidate.filename() == (agent ? kAgentCandidateFilename : kCandidateFilename) &&
		   backup.filename() == (agent ? kAgentBackupFilename : kStageBackupFilename) ;
}

} // namespace

/** @brief 再起動した通常Clientがworkerへ渡す継承資源。 */
class ClientUpdateStartupState final {
public:
	/**
	 * @brief 内部protocolで確認した資源を単独所有する。
	 * @param[in] helper 終了を待つmaintenance process。
	 * @param[in] ready UI生成完了を通知するevent。
	 * @param[in] stage_name 隣接stageの限定token。
	 * @param[in] rollback 旧版へ復元した再起動の場合だけtrue。
	 */
	ClientUpdateStartupState( detail::KernelHandle helper, detail::KernelHandle ready,
							 std::wstring stage_name, const bool rollback)
		: helper_process( std::move( helper)), ready_event( std::move( ready)),
		  stage_token( std::move( stage_name)), recovered( rollback)
	{
	}

	/// helperが終了するまでstage fileを回収しないための所有HANDLE。
	detail::KernelHandle helper_process ;
	/// manager UI完成後だけSetEventする所有HANDLE。
	detail::KernelHandle ready_event ;
	/// 任意pathではなく、固定prefixと32桁hexだけのtoken。
	std::wstring		 stage_token ;
	/// 完全復元済み旧版の再起動だけtrue。
	bool				 recovered{false} ;
	/// UI threadが一度だけ通知する初期化状態。
	std::atomic<bool>	 ready_signalled{false} ;
} ;

UpdateError ReplaceClientUpdateFile( const std::filesystem::path &target,
									const std::filesystem::path &candidate,
									const std::filesystem::path &backup) noexcept
{
	try {
		// 任意pathを低位file APIへ渡さず、専用stageと固定三fileの関係を再確認する
		if( !HasBundleFileLayout( target, candidate, backup) ||
		   GetFileAttributesW( backup.c_str()) != INVALID_FILE_ATTRIBUTES ||
		   GetLastError() != ERROR_FILE_NOT_FOUND) {
			return UpdateError::ReplacementFailure ;
		}

		// ReplaceFileが旧版ACL等を保ち、同一volume内でbackupを作る
		return ReplaceFileW( target.c_str(), candidate.c_str(), backup.c_str(), 0U, nullptr, nullptr)
				   ? UpdateError::None
				   : UpdateError::ReplacementFailure ;
	} catch( ...) {
		return UpdateError::ReplacementFailure ;
	}
}

UpdateError RestoreClientUpdateFile( const std::filesystem::path &target,
									const std::filesystem::path &backup) noexcept
{
	try {
		// callerが検証した固定旧版backup以外へ復元範囲を広げない
		if( !HasBundleFileLayout( target,
								backup.parent_path() / (target.filename() == kAgentFilename
															? kAgentCandidateFilename
															: kCandidateFilename),
								backup)) {
			return UpdateError::RollbackFailure ;
		}

		return MoveFileExW( backup.c_str(), target.c_str(),
						   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
				   ? UpdateError::None
				   : UpdateError::RollbackFailure ;
	} catch( ...) {
		return UpdateError::RollbackFailure ;
	}
}

UpdateError ReplaceClientUpdatePair( const std::filesystem::path &target,
									const std::filesystem::path &stage) noexcept
{
	try {
		// ClientからだけAgentを導出し、任意の二fileをセットにしない
		if( !HasSafeStageLayout( target, stage)) {
			return UpdateError::ReplacementFailure ;
		}

		const auto agent =
			ReplaceClientUpdateFile( target.parent_path() / kAgentFilename,
									stage / kAgentCandidateFilename, stage / kAgentBackupFilename) ;
		// 第一fileの部分失敗も上位transactionの元hash照合・復元へ渡す
		if( agent != UpdateError::None) {
			return agent ;
		}

		return ReplaceClientUpdateFile( target, stage / kCandidateFilename,
									   stage / kStageBackupFilename) ;
	} catch( ...) {
		return UpdateError::ReplacementFailure ;
	}
}

namespace {

/** @brief helperが単独所有する実file transactionと新版process。 */
class WindowsApplyOperations final : public IClientUpdateApplyOperations {
public:
	/**
	 * @brief 固定配置、同意manifest、旧版digestを保持する。
	 * @param[in] target 固定Client絶対path。
	 * @param[in] stage 検証した隣接stage。
	 * @param[in] release 完全な同意manifest。
	 * @param[in] original_hash 初回Client digest。
	 * @param[in] agent_hash 初回Agent digest。
	 */
	WindowsApplyOperations( std::filesystem::path target, std::filesystem::path stage,
						   ReleaseManifest release, const Sha256 original_hash,
						   const Sha256 agent_hash)
		: target_( std::move( target)), stage_( std::move( stage)), release_( std::move( release)),
		  original_hash_( original_hash), agent_hash_( agent_hash)
	{
	}

	/**
	 * @brief 親終了後にhelper copy、旧版、候補を完全再検証する。
	 * @return 全要件が有効な場合だけNone。
	 */
	[[nodiscard]] UpdateError ValidateBeforeReplacement() noexcept override
	{
		try {
			// own stageと保全済み旧版copyの不変を確認してから候補を検証する
			if( !HasSafeStageLayout( target_, stage_) ||
			   !IsOriginalImage( stage_ / kHelperFilename, original_hash_) ||
			   !IsOriginalAgent( target_.parent_path() / kAgentFilename, stage_ / kHelperFilename,
								agent_hash_)) {
				return UpdateError::InstalledImageChanged ;
			}

			const auto client = ValidateDownloadedClient( target_, stage_ / kCandidateFilename,
														 release_, original_hash_) ;
			// Client候補が不完全ならAgentだけの適用にも進まない
			if( client != UpdateError::None) {
				return client ;
			}

			return ValidateDownloadedAgent( target_, stage_ / kAgentCandidateFilename, release_) ;
		} catch( ...) {
			return UpdateError::ResourceExhausted ;
		}
	}

	/**
	 * @brief 親Client終了後だけ固定配置をbackup付きで差替える。
	 * @return 差替え結果。
	 */
	[[nodiscard]] UpdateError Replace() noexcept override
	{
		try {
			return ReplaceClientUpdatePair( target_, stage_) ;
		} catch( ...) {
			return UpdateError::ReplacementFailure ;
		}
	}

	/**
	 * @brief 差替えた実fileを旧版copyと同意manifestへ照合する。
	 * @return 完全検証できた場合だけNone。
	 */
	[[nodiscard]] UpdateError ValidateInstalledCandidate() noexcept override
	{
		try {
			const auto client = ValidateDownloadedClient( stage_ / kHelperFilename, target_,
														 release_, original_hash_) ;
			// 二fileの完全検証前に新Clientを起動しない
			if( client != UpdateError::None) {
				return client ;
			}

			return ValidateDownloadedAgent( stage_ / kHelperFilename,
										   target_.parent_path() / kAgentFilename, release_) ;
		} catch( ...) {
			return UpdateError::ResourceExhausted ;
		}
	}

	/**
	 * @brief 新版を限定HANDLEで開始し、UI生成を有限時間だけ確認する。
	 * @return 新版のready／停止／生存状態。
	 */
	[[nodiscard]] UpdateRestartResult RestartAndConfirm() noexcept override
	{
		try {
			detail::KernelHandle helper_wait ;
			SECURITY_ATTRIBUTES	 security{sizeof( security), nullptr, TRUE} ;
			detail::KernelHandle ready( CreateEventW( &security, TRUE, FALSE, nullptr)) ;
			// 現在のhelperを待つ最小権限だけを新版へ継承する
			if( !ready.valid() ||
			   !InheritableHandle( GetCurrentProcess(),
								  SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, helper_wait)) {
				return {} ;
			}

			const std::wstring arguments =
				L"--zrdp-update-restarted " + HandleArgument( helper_wait.get()) + L" " +
				HandleArgument( ready.get()) + L" " + stage_.filename().native() ;
			// 検証済み固定Clientだけを起動し、shellや任意commandを使用しない
			if( !StartInternalProcess( target_, arguments, helper_wait.get(), ready.get(),
									 new_process_)) {
				return {} ;
			}

			const DWORD waited = WaitHandshake( ready.get(), new_process_.get(), {}) ;
			// 初期化済み新版だけを成功扱いにする
			if( waited == WAIT_OBJECT_0) {
				return {UpdateRestartState::Ready, UpdateError::None} ;
			}

			// 終了済み新版だけがrollback可能な状態である
			if( waited == WAIT_OBJECT_0 + 1U ||
			   WaitForSingleObject( new_process_.get(), 0U) == WAIT_OBJECT_0) {
				return {} ;
			}

			return {UpdateRestartState::Running, UpdateError::RestartFailure} ;
		} catch( ...) {
			// 起動後の内部失敗では生存の可能性を保守的に扱い、processを強制終了しない
			return new_process_.valid() &&
						   WaitForSingleObject( new_process_.get(), 0U) != WAIT_OBJECT_0
					   ? UpdateRestartResult{UpdateRestartState::Running,
											 UpdateError::RestartFailure}

					   : UpdateRestartResult{} ;
		}
	}

	/**
	 * @brief 原配置の旧版hashと署名を確認する。
	 * @return 旧版が完全に不変ならtrue。
	 */
	[[nodiscard]] bool OriginalIsInstalled() noexcept override
	{
		try {
			return IsOriginalImage( target_, original_hash_) &&
				   IsOriginalAgent( target_.parent_path() / kAgentFilename, stage_ / kHelperFilename,
								   agent_hash_) ;
		} catch( ...) {
			return false ;
		}
	}

	/**
	 * @brief 有効な旧版backupだけを原配置へ戻す。
	 * @return 完全復元できた場合だけNone。
	 */
	[[nodiscard]] UpdateError RestoreOriginal() noexcept override
	{
		try {
			const auto agent		   = target_.parent_path() / kAgentFilename ;
			const auto anchor		   = stage_ / kHelperFilename ;
			const bool client_original = IsOriginalImage( target_, original_hash_) ;
			const bool agent_original  = IsOriginalAgent( agent, anchor, agent_hash_) ;
			// 変更されたfileのbackupを双方確認するまで、一方だけの復元を開始しない
			if( (!client_original &&
				!IsOriginalImage( stage_ / kStageBackupFilename, original_hash_)) ||
			   (!agent_original &&
				!IsOriginalAgent( stage_ / kAgentBackupFilename, anchor, agent_hash_))) {
				return UpdateError::RollbackFailure ;
			}

			// 旧Agentへ戻せない状態で旧Clientを起動可能な配置へ戻さない
			if( !agent_original &&
			   RestoreClientUpdateFile( agent, stage_ / kAgentBackupFilename) != UpdateError::None) {
				return UpdateError::RollbackFailure ;
			}

			// 第一fileだけ変更された失敗では、不変のClientへ不要な書込みをしない
			if( !client_original &&
			   RestoreClientUpdateFile( target_, stage_ / kStageBackupFilename) !=
				   UpdateError::None) {
				return UpdateError::RollbackFailure ;
			}

			return OriginalIsInstalled() ? UpdateError::None : UpdateError::RollbackFailure ;
		} catch( ...) {
			return UpdateError::RollbackFailure ;
		}
	}

private:
	/// 固定Client path。Agentは同じ親からだけ導出する。
	std::filesystem::path target_ ;
	/// own helper、候補、metadata、backupを持つ限定stage。
	std::filesystem::path stage_ ;
	/// 親で同意した完全Release。
	ReleaseManifest		  release_ ;
	/// 旧版copyとbackupの不変を確認するdigest。
	Sha256				  original_hash_{} ;
	/// 元Agentとbackupの不変を確認するdigest。
	Sha256				  agent_hash_{} ;
	/// 新版の生存を確認する所有HANDLE。強制停止には使用しない。
	detail::KernelHandle  new_process_ ;
} ;

/** @brief 親終了前後で通知先を区別するhelper固定結果。 */
struct MaintenanceResult {
	/// 完了または失敗の固定分類。
	UpdateError error{UpdateError::HelperFailure} ;
	/// 親終了後に復旧通知が必要な場合だけtrue。
	bool		show_failure{false} ;
} ;

/**
 * @brief 復元済み旧版を通常権限で再起動する。
 * @param[in] target 完全検証済みの旧版Client。
 * @param[in] stage 回収を旧版workerへ移転する限定stage。
 * @return processを開始できた場合だけtrue。
 */
[[nodiscard]] bool RestartRecoveredClient( const std::filesystem::path &target,
										  const std::filesystem::path &stage)
{
	detail::KernelHandle helper_wait ;
	SECURITY_ATTRIBUTES	 security{sizeof( security), nullptr, TRUE} ;
	detail::KernelHandle ready( CreateEventW( &security, TRUE, FALSE, nullptr)) ;
	// 復旧した旧版へも限定HANDLEだけを継承し、後片付けをworkerへ移転する
	if( !ready.valid() ||
	   !InheritableHandle( GetCurrentProcess(), SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
						  helper_wait)) {
		return false ;
	}

	const std::wstring arguments = L"--zrdp-update-recovered " + HandleArgument( helper_wait.get()) +
								   L" " + HandleArgument( ready.get()) + L" " +
								   stage.filename().native() ;
	detail::KernelHandle process ;
	return StartInternalProcess( target, arguments, helper_wait.get(), ready.get(), process) ;
}

/**
 * @brief copyした同じClient EXEだけでmaintenanceを実行する。
 * @param[in] parent 限定権限で継承した親process待機HANDLE。
 * @param[in] ready 事前検証完了を親へ通知するevent。
 * @return 親終了前は親UI、親終了後はhelper UIへ渡す固定結果。
 */
[[nodiscard]] MaintenanceResult RunMaintenance( const HANDLE parent, const HANDLE ready)
{
	const auto module = ModulePath() ;
	const auto stage  = module.parent_path() ;
	const auto target = stage.parent_path() / kClientFilename ;
	// 自身の限定stageからだけ差替え先を導出し、command lineでpathを受けない
	if( module.filename() != kHelperFilename || !HasSafeStageLayout( target, stage) ||
	   !ProcessHasImage( parent, target)) {
		return {} ;
	}

	PlacementLock	  placement ;
	const UpdateError locked = placement.Acquire( target) ;
	// 競合更新では既存processを止めず、受付前に拒否する
	if( locked != UpdateError::None) {
		return {locked, false} ;
	}

	ReleaseManifest	  release ;
	Sha256			  original_hash{}, agent_hash{} ;
	const UpdateError metadata = ReadStageMetadata( stage, release, original_hash, agent_hash) ;
	// 完全な同意情報と元digestを取得できないhelperを受付済みにしない
	if( metadata != UpdateError::None) {
		return {metadata, false} ;
	}

	WindowsApplyOperations operations( target, stage, std::move( release), original_hash, agent_hash) ;
	const UpdateError	   verified = operations.ValidateBeforeReplacement() ;
	// 旧版・候補・helperが完全に有効な場合だけ親へ終了許可を返す
	if( verified != UpdateError::None) {
		return {verified, false} ;
	}

	// eventでないHANDLEや受付通知失敗では親終了もfile差替えも要求しない
	if( !SetEvent( ready)) {
		return {} ;
	}

	const ULONGLONG started = GetTickCount64() ;
	// 親processの終了を有限に待ち、他processの強制終了はしない
	while( true) {
		const DWORD waited = WaitForSingleObject( parent, kWaitSliceMilliseconds) ;
		// 親終了が実際に確認できた場合だけ原配置へ書込む
		if( waited == WAIT_OBJECT_0) {
			break ;
		}

		// HANDLE失敗または親終了期限で常駐せず安全に終了する
		if( waited != WAIT_TIMEOUT || GetTickCount64() - started >= kParentExitTimeoutMilliseconds) {
			return {UpdateError::Cancelled, false} ;
		}
	}

	const UpdateError applied = ApplyClientUpdate( operations) ;
	// 完全に起動確認された新版がbackupとstageの所有権を引き継ぐ
	if( applied == UpdateError::None) {
		return {UpdateError::None, false} ;
	}

	// rollbackで旧版を維持できた場合だけ、旧版を再起動して利用者へ復旧を知らせる
	if( operations.OriginalIsInstalled() && RestartRecoveredClient( target, stage)) {
		return {applied, false} ;
	}

	return {applied, true} ;
}

/** @brief manifestの有限memory sink。 */
class ManifestSink final : public IUpdateContentSink {
public:
	/**
	 * @brief manifestの上限内だけ所有byte列を追加する。
	 * @param[in] bytes callback中だけ有効な取得byte列。
	 * @return 完全copyできた場合だけNone。
	 */
	[[nodiscard]] UpdateError Write( const std::span<const std::byte> bytes) noexcept override
	{
		try {
			// 更新metadata以外の巨大bodyを保持しない
			if( bytes.size() > kMaximumUpdateManifestBytes - text.size()) {
				return UpdateError::DataTooLarge ;
			}

			// ASCII parserへのbyte列copyであり、文字コード変換やpayload logは行わない
			text.append( reinterpret_cast<const char *>( bytes.data()), bytes.size()) ;
			return UpdateError::None ;
		} catch( ...) {
			return UpdateError::ResourceExhausted ;
		}
	}

	/// 完全取得後に有限parserへ渡す所有manifest text。
	std::string text ;
} ;

/** @brief 候補EXEを全量memory保持せずstageへstreamingするsink。 */
class ExecutableSink final : public IUpdateContentSink {
public:
	/**
	 * @brief 新規候補fileのwrite HANDLEを所有する。
	 * @param[in] file CREATE_NEWで開いたstage候補。
	 */
	explicit ExecutableSink( detail::KernelHandle file) noexcept : file_( std::move( file))
	{
	}

	/**
	 * @brief 固定chunkを完全に書込む。
	 * @param[in] bytes 全体上限を確認した取得chunk。
	 * @return 全byteを保存できた場合だけNone。
	 */
	[[nodiscard]] UpdateError Write( const std::span<const std::byte> bytes) noexcept override
	{
		DWORD written = 0U ;
		// download側のchunk上限を独立に確認してWin32 DWORDへ渡す
		if( bytes.size() > 64U * 1024U || !file_.valid() ||
		   !WriteFile( file_.get(), bytes.data(), static_cast<DWORD>( bytes.size()), &written,
					  nullptr) ||
		   written != bytes.size()) {
			return UpdateError::FileFailure ;
		}

		return UpdateError::None ;
	}

	/**
	 * @brief 全byteをflushし、署名検証より前にwrite HANDLEを閉じる。
	 * @return 完全flushできた場合だけNone。
	 */
	[[nodiscard]] UpdateError Finish() noexcept
	{
		const bool flushed = file_.valid() && FlushFileBuffers( file_.get()) ;
		file_.reset() ;
		return flushed ? UpdateError::None : UpdateError::FileFailure ;
	}

private:
	/// 完成した候補を検証する前に必ず閉じる単独所有write HANDLE。
	detail::KernelHandle file_ ;
} ;

/**
 * @brief 新版UI生成後にhelper終了と旧版backupを検証してstageを回収する。
 * @param[in] target 固定Client配置。
 * @param[in] startup 新版へ継承した限定内部状態。
 * @param[in] stop worker終了要求。
 * @return 正常更新の一世代backupを保全できた場合だけNone。
 */
[[nodiscard]] UpdateError
FinishRestartedUpdate( const std::filesystem::path					  &target,
					  const std::shared_ptr<ClientUpdateStartupState> &startup,
					  const std::stop_token							   stop)
{
	// 通常の手動起動ではstageへ触れない
	if( startup == nullptr) {
		return UpdateError::None ;
	}

	const auto stage  = target.parent_path() / startup->stage_token ;
	const auto anchor = stage / kHelperFilename ;
	// 継承したhelperとstageの所有関係を確認するまでfileへ触れない
	if( !HasSafeStageLayout( target, stage) ||
	   !ProcessHasImage( startup->helper_process.get(), anchor)) {
		return UpdateError::HelperFailure ;
	}

	const auto started = GetTickCount64() ;
	// 生存helperを強制終了せず、有限待機後はstageを保全する
	while( !stop.stop_requested()) {
		const auto waited =
			WaitForSingleObject( startup->helper_process.get(), kWaitSliceMilliseconds) ;
		// helperが終了した場合だけ回収へ進む
		if( waited == WAIT_OBJECT_0) {
			break ;
		}

		// 待機失敗・期限では旧backupを回収しない
		if( waited != WAIT_TIMEOUT || GetTickCount64() - started >= kParentExitTimeoutMilliseconds) {
			return UpdateError::HelperFailure ;
		}
	}

	// 終了要求中に保全の新規書込みをしない
	if( stop.stop_requested()) {
		return UpdateError::Cancelled ;
	}

	DWORD result = 0U ;
	// 正常更新と完全復元の既知終了値だけを回収可能とする
	if( !startup->ready_signalled.load( std::memory_order_acquire) ||
	   !GetExitCodeProcess( startup->helper_process.get(), &result) ||
	   (!startup->recovered && result != 0U) ||
	   (startup->recovered &&
		(result == 0U || result > static_cast<DWORD>( UpdateError::ResourceExhausted)))) {
		return UpdateError::HelperFailure ;
	}

	PlacementLock placement ;
	const auto	  locked = placement.Acquire( target) ;
	// 同じ配置の次の更新と旧セット保全を直列化する
	if( locked != UpdateError::None) {
		return locked ;
	}

	ReleaseManifest release ;
	Sha256			original_hash{}, agent_hash{} ;
	// 同意manifestと元二digestが揃わないstageを削除しない
	if( ReadStageMetadata( stage, release, original_hash, agent_hash) != UpdateError::None ||
	   !IsOriginalImage( anchor, original_hash)) {
		return UpdateError::FileFailure ;
	}

	const std::array<std::filesystem::path, 2U> targets{target,
														target.parent_path() / kAgentFilename} ;
	const std::array<std::filesystem::path, 2U> backups{stage / kStageBackupFilename,
														stage / kAgentBackupFilename} ;
	const std::array<std::filesystem::path, 2U> retained{
		target.parent_path() / kRetainedBackupFilename,
		target.parent_path() / kAgentRetainedFilename} ;

	// 復元済みでは両fileが元の完全値へ一致するまで候補・backupを回収しない
	if( startup->recovered) {
		// 一方だけ戻った状態を復旧成功として扱わない
		if( !IsOriginalBundleFile( 0U, targets[0], anchor, original_hash, agent_hash) ||
		   !IsOriginalBundleFile( 1U, targets[1], anchor, original_hash, agent_hash)) {
			return UpdateError::FileFailure ;
		}

		// 部分失敗が残した元fileと同じbackupだけを回収する
		for( std::size_t index = 0U; index < backups.size(); ++index) {
			const auto attributes = GetFileAttributesW( backups[index].c_str()) ;
			// 不明なbackupの存在では削除対象にしない
			if( attributes != INVALID_FILE_ATTRIBUTES) {
				// 元の完全byteと署名を確認してから重複copyだけを回収する
				if( !IsOriginalBundleFile( index, backups[index], anchor, original_hash,
										 agent_hash) ||
				   !DeleteFileW( backups[index].c_str())) {
					return UpdateError::FileFailure ;
				}
			}
			// 不在を確認できないbackupへ削除を試みない
			else if( GetLastError() != ERROR_FILE_NOT_FOUND) {
				return UpdateError::FileFailure ;
			}
		}
	} else {
		// 稼働新版の二EXEをもう一度完全照合してから旧版を保全する
		if( ValidateDownloadedClient( anchor, target, release, original_hash) != UpdateError::None ||
		   ValidateDownloadedAgent( anchor, targets[1], release) != UpdateError::None) {
			return UpdateError::FileFailure ;
		}

		std::array<bool, 2U> pending{} ;
		// 二backupの完全値と保全先を全検証してから移動を開始する
		for( std::size_t index = 0U; index < backups.size(); ++index) {
			const auto attributes = GetFileAttributesW( backups[index].c_str()) ;
			// 前回の部分保全で移動済みのfileは同じ元hashの保全先を確認する
			if( attributes == INVALID_FILE_ATTRIBUTES) {
				// 不在以外の失敗や保全先の改変を正常と扱わない
				if( GetLastError() != ERROR_FILE_NOT_FOUND ||
				   !IsOriginalBundleFile( index, retained[index], anchor, original_hash,
										 agent_hash)) {
					return UpdateError::FileFailure ;
				}

				continue ;
			}

			// 今回の旧版に一致しないstage backupは未知fileとして残す
			if( !IsOriginalBundleFile( index, backups[index], anchor, original_hash, agent_hash)) {
				return UpdateError::FileFailure ;
			}

			pending[index]				   = true ;
			const auto previous_attributes = GetFileAttributesW( retained[index].c_str()) ;
			// 同名の無関係fileや別署名者を上書きしない
			if( previous_attributes != INVALID_FILE_ATTRIBUTES) {
				ClientImageMetadata metadata ;
				const auto read = index == 0U ? ReadClientImageMetadata( retained[index], metadata)
											  : ReadAgentImageMetadata( retained[index], metadata) ;
				// 有効な旧製品だけを一世代backupとして置換できる
				if( read != UpdateError::None || metadata.version >= release.version ||
				   VerifyClientSignaturePair( anchor, retained[index]) != UpdateError::None) {
					return UpdateError::FileFailure ;
				}
			}
			// 不在と確認できない保全先へ上書きしない
			else if( GetLastError() != ERROR_FILE_NOT_FOUND) {
				return UpdateError::FileFailure ;
			}
		}

		// 各fileを同一volumeで保全し、途中失敗では残りのstageを保持する
		for( std::size_t index = 0U; index < backups.size(); ++index) {
			// 移動済みなら再度上書きしない
			if( pending[index] && !MoveFileExW( backups[index].c_str(), retained[index].c_str(),
											  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				return UpdateError::FileFailure ;
			}
		}
	}

	startup->helper_process.reset() ;
	startup->ready_event.reset() ;
	RemoveOwnedStage( target, stage) ;
	return UpdateError::None ;
}

/** @brief 更新workerだけが使用するWindows実装。 */
class WindowsUpdateBackend final : public IClientUpdateBackend {
public:
	/**
	 * @brief 取得と適用の状態をI/Oなしで構築する。
	 * @param[in] startup 再起動時だけ持つstage回収情報。
	 */
	explicit WindowsUpdateBackend( std::shared_ptr<ClientUpdateStartupState> startup)
		: startup_( std::move( startup))
	{
	}

	/**
	 * @brief manifestのみを取得し、数値versionとwire互換性を確認する。
	 * @param[in] stop worker終了要求。
	 * @return 同意待ち候補または固定結果。
	 */
	[[nodiscard]] UpdateCheckResult Check( const std::stop_token stop) noexcept override
	{
		UpdateCheckResult result ;
		try {
			latest_.reset() ;
			target_ = ModulePath() ;
			// renameしたEXEや不明な配置を自動差替え先へ使用しない
			if( target_.empty() || target_.filename() != kClientFilename ||
			   !IsPlainDirectory( target_.parent_path())) {
				result.error = UpdateError::MetadataMismatch ;
				return result ;
			}

			ClientImageMetadata current ;
			result.error = ReadClientImageMetadata( target_, current) ;
			// 読めないversionを0として新版へ自動適用しない
			if( result.error != UpdateError::None) {
				return result ;
			}

			current_version_	   = current.version ;
			result.current_version = current.version ;
			bool recovered		   = false ;
			// 設定の無効化後も、同意済み更新のlocal backup保全を先に完了する
			if( startup_ != nullptr) {
				recovered	 = startup_->recovered ;
				result.error = FinishRestartedUpdate( target_, startup_, stop) ;
				// 検証できないstageを次の更新で上書きしない
				if( result.error != UpdateError::None) {
					return result ;
				}

				startup_.reset() ;
			}

			std::string text ;
			result.error = ReadSmallFile( target_.parent_path() / L"client.ini",
										 kMaximumUpdateConfigurationBytes, text, true) ;
			// local設定の部分読取りを更新sourceに採用しない
			if( result.error != UpdateError::None) {
				return result ;
			}

			result.error = ParseUpdateConfiguration( text, configuration_) ;
			// 明示的な無効化または不正設定では外部通信をしない
			if( result.error != UpdateError::None || !configuration_.enabled) {
				return result ;
			}

			result.configured = true ;
			// 復旧直後は同じ失敗版を自動再提示せず、次の手動確認／周期確認へ委ねる
			if( recovered) {
				return result ;
			}

			// 前回受付に失敗したhelperが生存していれば新しいupdateを進めない
			if( helper_process_.valid() &&
			   WaitForSingleObject( helper_process_.get(), 0U) != WAIT_OBJECT_0) {
				result.error = UpdateError::Busy ;
				return result ;
			}

			ClearUntransferredStage() ;
			ManifestSink  sink ;
			std::uint64_t bytes = 0U ;
			result.error		= DownloadPublicRelease( BuildManifestUrl( configuration_.repository),
														kMaximumUpdateManifestBytes, sink, stop, bytes) ;
			// HTTP失敗、404、巨大bodyを更新なしと混同して手動確認へ返さない
			if( result.error != UpdateError::None) {
				return result ;
			}

			ReleaseManifest release ;
			result.error = ParseReleaseManifest( sink.text, release) ;
			// manifest全体が有効な場合だけversionを比較する
			if( result.error != UpdateError::None) {
				return result ;
			}

			// 同版と旧版は通知もEXE取得も行わない
			if( release.version <= current_version_) {
				return result ;
			}

			// Agent更新を必要とするwire非互換Releaseは自動提示しない
			if( release.bridge_protocol != protocol::kProtocolVersion) {
				result.error = UpdateError::IncompatibleProtocol ;
				return result ;
			}

			latest_		   = release ;
			result.release = std::move( release) ;
			return result ;
		} catch( ...) {
			result.error = UpdateError::ResourceExhausted ;
			return result ;
		}
	}

	/**
	 * @brief 同意したmanifest一件だけをstageへ取得し完全検証する。
	 * @param[in] release 現在の提示値と完全一致する同意済みmanifest。
	 * @param[in] stop worker終了要求。
	 * @return sealed候補とhelperを準備できた場合だけNone。
	 */
	[[nodiscard]] UpdateError Prepare( const ReleaseManifest &release,
									  const std::stop_token	 stop) noexcept override
	{
		try {
			// backendへ直接要求されても現在提示した一件以外を取得しない
			if( stop.stop_requested()) {
				return UpdateError::Cancelled ;
			}

			// 完全なsource、最新版manifest、固定配置の前提を確認する
			if( !configuration_.enabled || !latest_.has_value() || *latest_ != release ||
			   target_.empty() || release.version <= current_version_ ||
			   release.bridge_protocol != protocol::kProtocolVersion || transferred_) {
				return UpdateError::InvalidManifest ;
			}

			PlacementLock	  placement ;
			const UpdateError locked = placement.Acquire( target_) ;
			// 同一配置の他更新とdownload／stage作成を競合させない
			if( locked != UpdateError::None) {
				return locked ;
			}

			std::string			text ;
			UpdateConfiguration latest_configuration ;
			// 同意後に利用者が更新を無効化した場合もdownloadを始めない
			if( ReadSmallFile( target_.parent_path() / L"client.ini",
							 kMaximumUpdateConfigurationBytes, text, true) != UpdateError::None ||
			   ParseUpdateConfiguration( text, latest_configuration) != UpdateError::None ||
			   !latest_configuration.enabled ||
			   latest_configuration.repository != configuration_.repository) {
				return UpdateError::InvalidConfiguration ;
			}

			const UpdateError trusted = VerifyClientSignaturePair( target_, target_) ;
			// 未署名開発buildや非信頼EXEからの自動適用ではEXE自体も取得しない
			if( trusted != UpdateError::None) {
				return trusted ;
			}

			ClientImageMetadata current ;
			// 提示後の旧版変更やbootstrap非対応を検知し、現行Agent互換性を保つ
			if( ReadClientImageMetadata( target_, current) != UpdateError::None ||
			   current.version != current_version_ || !current.update_bootstrap ||
			   !current.paired_update || current.bridge_protocol != protocol::kProtocolVersion) {
				return UpdateError::InstalledImageChanged ;
			}

			std::uint64_t	  original_bytes = 0U ;
			const UpdateError original =
				ComputeUpdateFileHash( target_, original_hash_, original_bytes) ;
			// 元の完全digestが得られない場合はstageを作らない
			if( original != UpdateError::None) {
				return original ;
			}

			const auto	  installed_agent = target_.parent_path() / kAgentFilename ;
			std::uint64_t agent_bytes	  = 0U ;
			// 元Agentが欠落・別版・未署名なら復元可能なセットと推定しない
			if( ComputeUpdateFileHash( installed_agent, original_agent_hash_, agent_bytes) !=
				   UpdateError::None ||
			   !IsOriginalAgent( installed_agent, target_, original_agent_hash_)) {
				return UpdateError::InstalledImageChanged ;
			}

			ClearUntransferredStage() ;
			const UpdateError staged = CreateStage( target_, stage_) ;
			// EXE隣接へ書けない配置では別directoryや昇格へfallbackしない
			if( staged != UpdateError::None) {
				return staged ;
			}

			UpdateError result = PrepareFiles( release, stop) ;
			// 不完全な候補は転送後でも受付可能stageとして残さない
			if( result != UpdateError::None) {
				ClearUntransferredStage() ;
				return result ;
			}

			const std::array<const wchar_t *, 6U> names{
				kCandidateFilename,	   kHelperFilename,			kStageManifestFilename,
				kOriginalHashFilename, kAgentCandidateFilename, kAgentOriginalHashFilename} ;
			// session終了待ちの間も候補・copy・metadataの書込み／rename共有を拒否する
			for( std::size_t index = 0U; index < names.size(); ++index) {
				sealed_files_[index].reset(
					CreateFileW( (stage_ / names[index]).c_str(), GENERIC_READ, FILE_SHARE_READ,
								nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) ;
				// sealに失敗した候補を完全準備済みと報告しない
				if( !sealed_files_[index].valid()) {
					ClearUntransferredStage() ;
					return UpdateError::FileFailure ;
				}
			}

			prepared_ = true ;
			return UpdateError::None ;
		} catch( ...) {
			ClearUntransferredStage() ;
			return UpdateError::ResourceExhausted ;
		}
	}

	/**
	 * @brief helperを限定HANDLEで開始し、preflight完了を待つ。
	 * @param[in] stop worker終了要求。
	 * @return 事前検証済みhelperが親終了待ちを受付けた場合だけNone。
	 */
	[[nodiscard]] UpdateError LaunchHelper( const std::stop_token stop) noexcept override
	{
		try {
			// 完全準備前／移転後／終了要求後にhelperを開始しない
			if( !prepared_ || transferred_ || stop.stop_requested() ||
			   !HasSafeStageLayout( target_, stage_)) {
				return UpdateError::HelperFailure ;
			}

			std::string			text ;
			UpdateConfiguration current_configuration ;
			// session終了待ちの間に無効化／取得元変更された更新を適用しない
			if( ReadSmallFile( target_.parent_path() / L"client.ini",
							 kMaximumUpdateConfigurationBytes, text, true) != UpdateError::None ||
			   ParseUpdateConfiguration( text, current_configuration) != UpdateError::None ||
			   !current_configuration.enabled ||
			   current_configuration.repository != configuration_.repository) {
				return UpdateError::InvalidConfiguration ;
			}

			detail::KernelHandle parent_wait ;
			SECURITY_ATTRIBUTES	 security{sizeof( security), nullptr, TRUE} ;
			detail::KernelHandle ready( CreateEventW( &security, TRUE, FALSE, nullptr)) ;
			// 他processを操作できない親待機権限と専用eventだけを複製する
			if( !ready.valid() ||
			   !InheritableHandle( GetCurrentProcess(),
								  SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, parent_wait)) {
				return UpdateError::HelperFailure ;
			}

			const std::wstring arguments = L"--zrdp-maintenance " +
										   HandleArgument( parent_wait.get()) + L" " +
										   HandleArgument( ready.get()) ;
			// 実行中Clientの検証済みcopyだけを実行する
			if( !StartInternalProcess( stage_ / kHelperFilename, arguments, parent_wait.get(),
									 ready.get(), helper_process_)) {
				return UpdateError::HelperFailure ;
			}

			const DWORD waited = WaitHandshake( ready.get(), helper_process_.get(), stop) ;
			// childがpreflightを完了した場合だけUIへ親終了を許可する
			if( waited == WAIT_OBJECT_0) {
				transferred_ = true ;
				return UpdateError::None ;
			}

			// 終了済みhelperの固定errorだけを採用し、外部error textを表示しない
			if( waited == WAIT_OBJECT_0 + 1U) {
				DWORD error = 0U ;
				// controlled helperのenum範囲を越える終了値は固定HelperFailureに縮退する
				if( GetExitCodeProcess( helper_process_.get(), &error) && error > 0U &&
				   error <= static_cast<DWORD>( UpdateError::ResourceExhausted)) {
					return static_cast<UpdateError>( error) ;
				}
			}

			return stop.stop_requested() ? UpdateError::Cancelled : UpdateError::HelperFailure ;
		} catch( ...) {
			return UpdateError::ResourceExhausted ;
		}
	}

	/** @brief worker終了時だけ未移転候補を回収し、引受済みstageはhelperへ残す。 */
	void Shutdown() noexcept override
	{
		ClearUntransferredStage() ;
		// 親終了前の読取りsealを解放し、helperが再検証後に差替えられるようにする
		for( auto &file : sealed_files_) {
			file.reset() ;
		}
	}

private:
	/**
	 * @brief 完全検証したdownloadと同じClient copyを新規stageへ準備する。
	 * @param[in] release 同意済みmanifest。
	 * @param[in] stop worker終了要求。
	 * @return 候補、copy、metadataが完全な場合だけNone。
	 */
	[[nodiscard]] UpdateError PrepareFiles( const ReleaseManifest &release,
										   const std::stop_token  stop)
	{
		const std::array<const wchar_t *, 2U> names{kCandidateFilename, kAgentCandidateFilename} ;
		const std::array<std::uint64_t, 2U>	  sizes{release.size_bytes, release.agent_size_bytes} ;
		const std::array<std::string, 2U>	  urls{
			BuildExecutableUrl( configuration_.repository, release),
			BuildAgentExecutableUrl( configuration_.repository, release)} ;
		// 同意した同じtagの二EXEを一件ずつ有限streamingし、一方の失敗でセットを拒否する
		for( std::size_t index = 0U; index < names.size(); ++index) {
			detail::KernelHandle candidate( CreateFileW( (stage_ / names[index]).c_str(),
													   GENERIC_WRITE, FILE_SHARE_READ, nullptr,
													   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) ;
			// 既存候補を再利用せず新規fileにだけ保存する
			if( !candidate.valid()) {
				return UpdateError::PermissionDenied ;
			}

			ExecutableSink sink( std::move( candidate)) ;
			std::uint64_t  received = 0U ;
			const auto	   downloaded =
				DownloadPublicRelease( urls[index], sizes[index], sink, stop, received) ;
			// 片方だけ取得できても適用を準備済みにしない
			if( downloaded != UpdateError::None) {
				return downloaded ;
			}

			// 検証前にwrite HANDLEを閉じ、上限内でもexact sizeを必須にする
			if( received != sizes[index] || sink.Finish() != UpdateError::None) {
				return UpdateError::FileFailure ;
			}
		}

		const auto client =
			ValidateDownloadedClient( target_, stage_ / kCandidateFilename, release, original_hash_) ;
		// 一方の署名・hash・版が不正なら両方を不採用にする
		if( client != UpdateError::None) {
			return client ;
		}

		const auto agent =
			ValidateDownloadedAgent( target_, stage_ / kAgentCandidateFilename, release) ;
		// 同一publisherでも別版Agentの混入を拒否する
		if( agent != UpdateError::None) {
			return agent ;
		}

		// 検証済みの旧Client copyだけをhelperにし、元Agentも不変と確認する
		if( !CopyFileW( target_.c_str(), (stage_ / kHelperFilename).c_str(), TRUE) ||
		   !IsOriginalImage( stage_ / kHelperFilename, original_hash_) ||
		   !IsOriginalAgent( target_.parent_path() / kAgentFilename, target_,
							original_agent_hash_)) {
			return UpdateError::InstalledImageChanged ;
		}

		const auto saved =
			WriteNewText( stage_ / kStageManifestFilename, SerializeManifest( release)) ;
		// 同意セットと元二digestを全てflushしてから完了を通知する
		if( saved != UpdateError::None) {
			return saved ;
		}

		const auto hash =
			WriteNewText( stage_ / kOriginalHashFilename, FormatSha256( original_hash_)) ;
		// 元Clientのdigest保存失敗や終了要求で適用へ進まない
		if( hash != UpdateError::None || stop.stop_requested()) {
			return hash != UpdateError::None ? hash : UpdateError::Cancelled ;
		}

		return WriteNewText( stage_ / kAgentOriginalHashFilename,
							FormatSha256( original_agent_hash_)) ;
	}

	/** @brief own stageをworkerで回収し、生存helperや旧版backupへ介入しない。 */
	void ClearUntransferredStage() noexcept
	{
		try {
			// 引受済みstageまたは生存helperのfileを親から削除しない
			if( transferred_ || (helper_process_.valid() &&
								WaitForSingleObject( helper_process_.get(), 0U) != WAIT_OBJECT_0)) {
				return ;
			}

			// readonly sealを解除してから自分の不完全候補だけを回収する
			for( auto &file : sealed_files_) {
				file.reset() ;
			}

			RemoveOwnedStage( target_, stage_) ;
			stage_.clear() ;
			helper_process_.reset() ;
			prepared_ = false ;
		} catch( ...) {
			// 回収失敗で削除範囲を拡張せず、有限stage上限へ残す
		}
	}

	/// 新版再起動のときだけ持つ検証・回収状態。
	std::shared_ptr<ClientUpdateStartupState> startup_ ;
	/// workerで確認したClientの固定配置。Agentは同じ親に限定する。
	std::filesystem::path					  target_ ;
	/// このbackendだけが作成したstage。引受後はhelper／新版へ移転する。
	std::filesystem::path					  stage_ ;
	/// manifestを取得した公開repository snapshot。
	UpdateConfiguration						  configuration_ ;
	/// 最新確認で提示した完全manifest。Prepareはこれとの一致を要求する。
	std::optional<ReleaseManifest>			  latest_ ;
	/// 最新確認で読んだ旧版version。提示後の変更を検知する。
	UpdateVersion							  current_version_ ;
	/// 初回確認した旧版fileの完全digest。
	Sha256									  original_hash_{} ;
	/// 元Agentの不変を確認する完全digest。
	Sha256									  original_agent_hash_{} ;
	/// session終了待ちの間だけ候補・copy・metadataを書込み不可として保持する。
	std::array<detail::KernelHandle, 6U>	  sealed_files_ ;
	/// 受付に失敗した場合も生存helperへ強制終了をしないための所有HANDLE。
	detail::KernelHandle					  helper_process_ ;
	/// 完全検証済みstageがある場合だけtrue。
	bool									  prepared_{false} ;
	/// helperが適用を引受け、stage回収責任を移転した場合だけtrue。
	bool									  transferred_{false} ;
} ;

/** @brief CommandLineToArgvWのmemory専用解放責任。 */
struct ArgumentCloser {
	/**
	 * @brief command lineの所有配列を解放する。
	 * @param[in] arguments Windowsが返した所有配列。
	 */
	void operator()( wchar_t **arguments) const noexcept
	{
		static_cast<void>( LocalFree( arguments)) ;
	}
} ;

} // namespace

ClientUpdateEntry HandleClientUpdateEntry() noexcept
{
	ClientUpdateEntry result ;
	try {
		int										   count = 0 ;
		std::unique_ptr<wchar_t *, ArgumentCloser> arguments(
			CommandLineToArgvW( GetCommandLineW(), &count)) ;
		// 通常起動ではhelper資源や任意command lineを解釈しない
		if( arguments == nullptr || count < 2) {
			return result ;
		}

		const std::wstring_view mode( arguments.get()[1]) ;
		// 通常起動の未知argumentをremote実行や更新対象として採用しない
		if( mode != L"--zrdp-maintenance" && mode != L"--zrdp-update-restarted" &&
		   mode != L"--zrdp-update-recovered") {
			return result ;
		}

		result.handled		   = true ;
		result.exit_code	   = static_cast<int>( UpdateError::HelperFailure) ;
		const bool maintenance = mode == L"--zrdp-maintenance" ;
		// 内部形式の要素数不一致を部分解釈しない
		if( (maintenance && count != 4) || (!maintenance && count != 5)) {
			return result ;
		}

		detail::KernelHandle process ;
		detail::KernelHandle ready ;
		// 有効な継承HANDLEだけを受け、任意PIDやfile pathを開かない
		if( !ReadHandleArgument( arguments.get()[2], process) ||
		   !ReadHandleArgument( arguments.get()[3], ready) || GetProcessId( process.get()) == 0U ||
		   GetProcessId( process.get()) == GetCurrentProcessId()) {
			return result ;
		}

		// maintenanceは通常RDP UIやlocalhost listenerを一切生成しない
		if( maintenance) {
			const MaintenanceResult completed = RunMaintenance( process.get(), ready.get()) ;
			result.exit_code				  = static_cast<int>( completed.error) ;
			result.show_failure				  = completed.show_failure ;
			return result ;
		}

		const std::wstring_view token( arguments.get()[4]) ;
		// 新版が回収できるstageも固定tokenだけへ制限する
		if( !IsStageName( token)) {
			return result ;
		}

		result.startup = std::make_shared<ClientUpdateStartupState>(
			std::move( process), std::move( ready), std::wstring( token),
			mode == L"--zrdp-update-recovered") ;
		result.recovered = mode == L"--zrdp-update-recovered" ;
		result.handled	 = false ;
		result.exit_code = 0 ;
		return result ;
	} catch( ...) {
		result.handled	 = true ;
		result.exit_code = static_cast<int>( UpdateError::ResourceExhausted) ;
		return result ;
	}
}

void SignalClientUpdateRestartReady(
	const std::shared_ptr<ClientUpdateStartupState> &startup) noexcept
{
	// 通常起動では確認eventを持たない
	if( startup == nullptr) {
		return ;
	}

	const HANDLE event	  = startup->ready_event.get() ;
	bool		 expected = false ;
	// HANDLE読取りをrelease通知より前に終え、worker側の回収と競合させない
	if( startup->ready_signalled.compare_exchange_strong( expected, true,
														std::memory_order_acq_rel)) {
		// event通知に失敗した場合は初期化済みとして回収しない
		if( !SetEvent( event)) {
			startup->ready_signalled.store( false, std::memory_order_release) ;
		}
	}
}

std::unique_ptr<IClientUpdateBackend>
MakeWindowsUpdateBackend( std::shared_ptr<ClientUpdateStartupState> startup)
{
	return std::make_unique<WindowsUpdateBackend>( std::move( startup)) ;
}

} // namespace zrdp::client::update
