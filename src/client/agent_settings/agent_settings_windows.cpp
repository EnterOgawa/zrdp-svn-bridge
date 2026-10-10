//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_windows.cpp
 * @brief current-user DPAPIとlocal fileの原子的保存でAgent設定を保護する。
 * @version 01.00.03
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  暗号化profile保存とAgent INIの入出力を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * </pre>
 *
 * @note file path、profile名、実endpointをlogへ出さず、すべてworkerで実行する。
 */
//------------------------------------------------------------------------------
#include "client/agent_settings/agent_settings_service.h"
#include "client/update/update_windows_handles.h"

#include <Windows.h>
#include <ShlObj.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <array>
#include <utility>

namespace zrdp::client::agent_settings {
namespace {

// 利用者profile内の固定暗号化保存file名
constexpr wchar_t kSettingsFilename[] = L"agent-settings.bin" ;
// 他のclient.iniやsourceをINI出力で上書きしないための唯一の名前
constexpr wchar_t kIniFilename[]	  = L"agent.ini" ;

/** @brief DPAPI所有memoryを内容消去して解放する責任。 */
struct ProtectedBlob {
	/// WindowsがLocalAllocで返す所有byte列。
	DATA_BLOB value{} ;
	/** @brief 暗号化／復号dataを保存処理のscope終了時に消去する。 */
	~ProtectedBlob()
	{
		// 実体がある場合だけWindows memoryを消去して解放する
		if( value.pbData != nullptr) {
			SecureZeroMemory( value.pbData, value.cbData) ;
			LocalFree( value.pbData) ;
		}
	}
} ;

/** @brief 平文bufferをscope終了時に消去する責任。 */
struct PlaintextWiper {
	/// 呼出し元が所有し、wiperより長く生存するbuffer。
	std::vector<std::byte> &bytes ;
	/** @brief 実endpointを含む平文byte列を消去する。 */
	~PlaintextWiper()
	{
		// 空bufferでない場合だけ所有領域を消去する
		if( !bytes.empty()) {
			SecureZeroMemory( bytes.data(), bytes.size()) ;
		}
	}
} ;

/**
 * @brief Windows ordinal規則で固定file名へ一致するか確認する。
 * @param[in] path 判定対象。
 * @param[in] filename 固定名。
 * @return basenameだけが大小文字差を除き一致する場合はtrue。
 */
[[nodiscard]] bool HasFilename( const std::filesystem::path &path, const std::wstring_view filename)
{
	const auto name = path.filename().native() ;
	return !name.empty() && name.size() <= 32767U &&
		   CompareStringOrdinal( name.data(), static_cast<int>( name.size()), filename.data(),
								static_cast<int>( filename.size()), TRUE) == CSTR_EQUAL ;
}

/**
 * @brief local volumeの通常directoryへ限定し、linkやshareへfallbackしない。
 * @param[in] directory 完全な親directory。
 * @return fixed／removable volumeの通常directoryならtrue。
 */
[[nodiscard]] bool IsLocalDirectory( const std::filesystem::path &directory)
{
	// 未解決の相対pathを保存先として解釈しない
	if( !directory.is_absolute() || directory.native().size() > 32767U) {
		return false ;
	}

	const DWORD					attributes = GetFileAttributesW( directory.c_str()) ;
	std::array<wchar_t, 32768U> volume{} ;
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
		   (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U &&
		   GetVolumePathNameW( directory.c_str(), volume.data(),
							  static_cast<DWORD>( volume.size())) &&
		   (GetDriveTypeW( volume.data()) == DRIVE_FIXED ||
			GetDriveTypeW( volume.data()) == DRIVE_REMOVABLE) ;
}

/**
 * @brief 読取りfileがsingle-linkの通常disk fileか確認する。
 * @param[in] file 所有者が保持する非所有HANDLE。
 * @param[in] maximum byte上限。
 * @param[out] size 完全なfile byte数。
 * @return 正常な有限fileの場合だけtrue。
 */
[[nodiscard]] bool ReadableFile( const HANDLE file, const std::size_t maximum, DWORD &size) noexcept
{
	BY_HANDLE_FILE_INFORMATION info{} ;
	// reparse、directory、hard link、巨大fileを設定入力として使わない
	if( GetFileType( file) != FILE_TYPE_DISK || !GetFileInformationByHandle( file, &info) ||
	   info.nNumberOfLinks != 1U ||
	   (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U ||
	   info.nFileSizeHigh != 0U || info.nFileSizeLow > maximum) {
		return false ;
	}

	size = info.nFileSizeLow ;
	return true ;
}

/**
 * @brief 利用者が指定したlocal fileだけを有限に読み取る。
 * @param[in] path 固定保存fileまたは明示INI。
 * @param[in] maximum allocation前に判定するbyte上限。
 * @param[in] missing_ok 固定保存fileの不在だけを空一覧として許可するか。
 * @param[out] bytes 完全な入力。失敗時は空。
 * @return 正常読込みまたは許可した不在なら成功。
 */
[[nodiscard]] AgentSettingsResult ReadFileBytes( const std::filesystem::path &path,
												const std::size_t maximum, const bool missing_ok,
												std::vector<std::byte> &bytes)
{
	bytes.clear() ;
	// network shareや不明な親directoryを設定入力として使わない
	if( !IsLocalDirectory( path.parent_path())) {
		return {AgentSettingsError::InvalidPath} ;
	}

	update::detail::KernelHandle file( CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ,
												  nullptr, OPEN_EXISTING,
												  FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) ;
	// 保存fileの不在だけを新規設定として扱う
	if( !file.valid()) {
		return missing_ok && GetLastError() == ERROR_FILE_NOT_FOUND
				   ? AgentSettingsResult{}

				   : AgentSettingsResult{AgentSettingsError::FileFailure} ;
	}

	DWORD size = 0U ;
	// 実size検証前にmemoryへ取り込まない
	if( !ReadableFile( file.get(), maximum, size)) {
		return {AgentSettingsError::InvalidData} ;
	}

	bytes.resize( size) ;
	DWORD received = 0U ;
	// 空の明示INIはparserで拒否し、途中までの入力は採用しない
	if( size != 0U &&
	   (!ReadFile( file.get(), bytes.data(), size, &received, nullptr) || received != size)) {
		bytes.clear() ;
		return {AgentSettingsError::FileFailure} ;
	}

	return {} ;
}

/**
 * @brief 固定fileへ新規一時fileから一括置換する。
 * @param[in] path 完全検証済みの固定名。
 * @param[in] bytes 暗号化dataまたは完全生成したINI。
 * @param[in] overwrite 利用者の明示操作または固定profile保存の上書き許可。
 * @param[in] stop worker終了要求。
 * @return 全byteを書込みflushし、同volumeで置換できた場合だけ成功。
 */
[[nodiscard]] AgentSettingsResult WriteAtomic( const std::filesystem::path	  &path,
											  const std::span<const std::byte> bytes,
											  const bool overwrite, const std::stop_token stop)
{
	// 有限のlocal file以外へ出力範囲を拡大しない
	if( bytes.empty() || bytes.size() > kMaximumAgentSettingsCiphertext ||
	   !IsLocalDirectory( path.parent_path())) {
		return {AgentSettingsError::InvalidPath} ;
	}

	const DWORD attributes = GetFileAttributesW( path.c_str()) ;
	// 既存targetは明示上書き許可と通常fileの両方を必須にする
	if( attributes != INVALID_FILE_ATTRIBUTES) {
		update::detail::KernelHandle original(
			CreateFileW( path.c_str(), FILE_READ_ATTRIBUTES,
						FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
						OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) ;
		DWORD ignored = 0U ;
		// directoryやlink、read-only対象をconfig出力で変更しない
		if( !overwrite || (attributes & FILE_ATTRIBUTE_READONLY) != 0U || !original.valid() ||
		   !ReadableFile( original.get(), kMaximumAgentSettingsCiphertext, ignored)) {
			return {AgentSettingsError::FileFailure} ;
		}
	}
	// 不在を確認できないtargetは書込み可能と推測しない
	else if( GetLastError() != ERROR_FILE_NOT_FOUND) {
		return {AgentSettingsError::FileFailure} ;
	}

	std::array<UCHAR, 16U> random{} ;
	// 一時file名はcurrent-user directory内の推測困難な新規名とする
	if( BCryptGenRandom( nullptr, random.data(), static_cast<ULONG>( random.size()),
					   BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		return {AgentSettingsError::ResourceExhausted} ;
	}

	std::wstring name = L".zrdp-agent-settings-" ;
	// CSPRNG byteをASCII hexへ変換し、任意path表現を入れない
	for( const UCHAR value : random) {
		name.push_back( L"0123456789abcdef"[value >> 4U]) ;
		name.push_back( L"0123456789abcdef"[value & 0x0fU]) ;
	}

	const auto					 temporary = path.parent_path() / (name + L".tmp") ;
	update::detail::KernelHandle file( CreateFileW( temporary.c_str(), GENERIC_WRITE, 0U, nullptr,
												  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) ;
	// 衝突や権限不足で既存fileを再利用しない
	if( !file.valid()) {
		return {AgentSettingsError::FileFailure} ;
	}

	DWORD	   written = 0U ;
	const bool complete =
		!stop.stop_requested() &&
		WriteFile( file.get(), bytes.data(), static_cast<DWORD>( bytes.size()), &written, nullptr) &&
		written == bytes.size() && FlushFileBuffers( file.get()) ;
	file.reset() ;
	// 書込み中の取消や部分保存ではtargetへ一時fileを公開しない
	if( !complete || stop.stop_requested()) {
		static_cast<void>( DeleteFileW( temporary.c_str())) ;
		return {stop.stop_requested() ? AgentSettingsError::Cancelled
									  : AgentSettingsError::FileFailure} ;
	}

	const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0U) ;
	// 完全な同directoryの自作一時fileだけを移し、別pathへfallbackしない
	if( !MoveFileExW( temporary.c_str(), path.c_str(), flags)) {
		static_cast<void>( DeleteFileW( temporary.c_str())) ;
		return {AgentSettingsError::FileFailure} ;
	}

	return {} ;
}

/** @brief current-userの固定保存先または隔離testの一件だけを扱う実装。 */
class WindowsAgentSettingsBackend final : public IAgentSettingsBackend {
public:
	/**
	 * @brief I/Oを行わず固定保存先を保持する。
	 * @param[in] path testだけの明示保存先。通常起動では空。
	 */
	explicit WindowsAgentSettingsBackend( std::filesystem::path path) : storage_( std::move( path))
	{
	}

	/**
	 * @brief 復号と形式検証の両方に成功した一覧だけを返す。
	 * @param[out] profiles 完全一覧。不在／失敗時は空。
	 * @param[in] stop worker終了要求。
	 * @return 固定分類。
	 */
	[[nodiscard]] AgentSettingsResult Load( AgentSettingsProfiles &profiles,
										   const std::stop_token  stop) noexcept override
	{
		profiles.clear() ;
		try {
			const auto prepared = StoragePath() ;
			// 保存先を検証できなければ別directoryへfallbackしない
			if( !prepared.succeeded()) {
				return prepared ;
			}

			// 終了要求後に暗号化fileを読まない
			if( stop.stop_requested()) {
				return {AgentSettingsError::Cancelled} ;
			}

			std::vector<std::byte> bytes ;
			const auto read = ReadFileBytes( storage_, kMaximumAgentSettingsCiphertext, true, bytes) ;
			// 読めないfileは空設定と混同しない
			if( !read.succeeded()) {
				return read ;
			}

			// 保存fileがない場合は正常な空一覧で開始する
			if( bytes.empty()) {
				// 実在する空fileは破損として扱い、不在と区別する
				if( GetFileAttributesW( storage_.c_str()) != INVALID_FILE_ATTRIBUTES) {
					return {AgentSettingsError::InvalidData} ;
				}

				// 不在の再確認が権限等で失敗した場合も空設定へ置換しない
				if( GetLastError() != ERROR_FILE_NOT_FOUND) {
					return {AgentSettingsError::FileFailure} ;
				}

				return {} ;
			}

			DATA_BLOB	  input{static_cast<DWORD>( bytes.size()),
							reinterpret_cast<BYTE *>( bytes.data())} ;
			ProtectedBlob decoded ;
			// machine scopeや独自keyへfallbackせずcurrent-user DPAPIで読む
			if( !CryptUnprotectData( &input, nullptr, nullptr, nullptr, nullptr,
								   CRYPTPROTECT_UI_FORBIDDEN, &decoded.value) ||
			   decoded.value.cbData > kMaximumAgentSettingsPlaintext) {
				return {AgentSettingsError::ProtectionFailed} ;
			}

			// 復号後にも終了要求を確認して部分一覧を公開しない
			if( stop.stop_requested()) {
				return {AgentSettingsError::Cancelled} ;
			}

			return DecodeAgentSettingsProfiles(
				std::span<const std::byte>(
					reinterpret_cast<const std::byte *>( decoded.value.pbData),
					decoded.value.cbData),
				profiles) ;
		} catch( ...) {
			return {AgentSettingsError::ResourceExhausted} ;
		}
	}

	/**
	 * @brief 有限profile一覧を暗号化し固定fileへ原子的に保存する。
	 * @param[in] profiles 全体保存する一覧。
	 * @param[in] stop worker終了要求。
	 * @return 固定分類。
	 */
	[[nodiscard]] AgentSettingsResult Save( const AgentSettingsProfiles &profiles,
										   const std::stop_token		stop) noexcept override
	{
		try {
			std::vector<std::byte> plain ;
			const PlaintextWiper   wiper{plain} ;
			const auto			   encoded = EncodeAgentSettingsProfiles( profiles, plain) ;
			// 不正一覧で旧保存fileを変更しない
			if( !encoded.succeeded()) {
				return encoded ;
			}

			const auto prepared = StoragePath() ;
			// 固定保存先の検証失敗時は保存しない
			if( !prepared.succeeded()) {
				return prepared ;
			}

			// 終了後に新しい暗号化を開始しない
			if( stop.stop_requested()) {
				return {AgentSettingsError::Cancelled} ;
			}

			DATA_BLOB	  input{static_cast<DWORD>( plain.size()),
							reinterpret_cast<BYTE *>( plain.data())} ;
			ProtectedBlob encoded_blob ;
			// endpointを平文保存せず、Windowsユーザーscopeで保護する
			if( !CryptProtectData( &input, L"ZRdp SVN Bridge Agent Settings", nullptr, nullptr,
								 nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encoded_blob.value) ||
			   encoded_blob.value.cbData > kMaximumAgentSettingsCiphertext) {
				return {AgentSettingsError::ProtectionFailed} ;
			}

			return WriteAtomic( storage_,
							   std::span<const std::byte>(
								   reinterpret_cast<const std::byte *>( encoded_blob.value.pbData),
								   encoded_blob.value.cbData),
							   true, stop) ;
		} catch( ...) {
			return {AgentSettingsError::ResourceExhausted} ;
		}
	}

	/**
	 * @brief local INI全体を共有Agent parserへ渡す。
	 * @param[in] path 明示INI path。
	 * @param[in] name 有効な設定名。
	 * @param[out] profile 用途と承認を推定しないdraft。
	 * @param[in] stop worker終了要求。
	 * @return 固定分類。
	 */
	[[nodiscard]] AgentSettingsResult Import( const std::filesystem::path &path,
											 const std::wstring_view	  name,
											 AgentSettingsProfile		 &profile,
											 const std::stop_token		  stop) noexcept override
	{
		profile = {} ;
		try {
			// 終了後の読込みや任意拡張子の大きなfile入力をしない
			if( stop.stop_requested()) {
				return {AgentSettingsError::Cancelled} ;
			}

			// 利用者のINI指定に限定し、EXE等を設定入力として読まない
			const auto extension = path.extension().native() ;
			// 大小文字差だけを許可し、別file種別を読まない
			if( extension.size() != 4U ||
			   CompareStringOrdinal( extension.data(), 4, L".ini", 4, TRUE) != CSTR_EQUAL) {
				return {AgentSettingsError::InvalidPath} ;
			}

			std::vector<std::byte> bytes ;
			const PlaintextWiper   wiper{bytes} ;
			const auto			   read =
				ReadFileBytes( path, agent::network::kMaximumAllowlistFileBytes, false, bytes) ;
			// 不完全なfileを部分採用しない
			if( !read.succeeded()) {
				return read ;
			}

			// 取得後の終了要求では結果を公開しない
			if( stop.stop_requested()) {
				return {AgentSettingsError::Cancelled} ;
			}

			return ParseAgentSettingsIni(
				std::string_view( reinterpret_cast<const char *>( bytes.data()), bytes.size()), name,
				profile) ;
		} catch( ...) {
			return {AgentSettingsError::ResourceExhausted} ;
		}
	}

	/**
	 * @brief 承認済み設定だけを固定agent.iniへ出力する。
	 * @param[in] path 標準dialogで選ばれた固定名。
	 * @param[in] profile 完全に検証する設定。
	 * @param[in] overwrite_confirmed 利用者が既存fileの上書きを承認したか。
	 * @param[in] stop worker終了要求。
	 * @return 固定分類。
	 */
	[[nodiscard]] AgentSettingsResult Export( const std::filesystem::path &path,
											 const AgentSettingsProfile	 &profile,
											 const bool					  overwrite_confirmed,
											 const std::stop_token		  stop) noexcept override
	{
		try {
			// 生成対象をagent.iniだけへ限定する
			if( !HasFilename( path, kIniFilename)) {
				return {AgentSettingsError::InvalidPath} ;
			}

			std::string text ;
			const auto	generated = GenerateAgentSettingsIni( profile, text) ;
			// 未承認や不正設定で既存INIを変更しない
			if( !generated.succeeded()) {
				return generated ;
			}

			const auto result =
				WriteAtomic( path, std::as_bytes( std::span( text.data(), text.size())),
							overwrite_confirmed, stop) ;
			SecureZeroMemory( text.data(), text.size()) ;
			return result ;
		} catch( ...) {
			return {AgentSettingsError::ResourceExhausted} ;
		}
	}

private:
	/**
	 * @brief 初回だけcurrent-userの固定directoryをworker上で確定する。
	 * @return 通常保存先または隔離test先が有効な場合だけ成功。
	 */
	[[nodiscard]] AgentSettingsResult StoragePath()
	{
		// 通常backendだけがWindowsユーザーのKnownFolderを導出する
		if( storage_.empty()) {
			PWSTR		  raw = nullptr ;
			const HRESULT result =
				SHGetKnownFolderPath( FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw) ;
			// user profileを読めない場合はEXE隣接等へfallbackしない
			if( FAILED( result) || raw == nullptr) {
				CoTaskMemFree( raw) ;
				return {AgentSettingsError::FileFailure} ;
			}

			std::filesystem::path base ;
			try {
				base = raw ;
			} catch( ...) {
				CoTaskMemFree( raw) ;
				throw ;
			}

			CoTaskMemFree( raw) ;
			const auto directory = base / L"ZRdpSvnBridge" ;
			// user専用親が正常なら固定のアプリdirectoryだけを作る
			if( !IsLocalDirectory( base) || (!CreateDirectoryW( directory.c_str(), nullptr) &&
										   GetLastError() != ERROR_ALREADY_EXISTS)) {
				return {AgentSettingsError::FileFailure} ;
			}

			storage_ = directory / kSettingsFilename ;
		}

		return HasFilename( storage_, kSettingsFilename) && IsLocalDirectory( storage_.parent_path())
				   ? AgentSettingsResult{}

				   : AgentSettingsResult{AgentSettingsError::InvalidPath} ;
	}

	/** @brief 既存Agent設定workerから製品情報と承認済み配置を準備する。
	 * @param[in] profile 全承認済みsnapshot。
	 * @param[out] package 完全成功時の二ファイル。
	 * @param[in] stop worker終了要求。
	 * @return 固定名Agentと設定を検証できた場合だけ成功。
	 */
	[[nodiscard]] AgentSettingsResult
	PrepareDeployment( const AgentSettingsProfile					   &profile,
					  std::shared_ptr<const agent_deployment::Package> &package,
					  std::stop_token									stop) noexcept override
	{
		return agent_deployment::PreparePackage( profile, package, stop) ;
	}

	/// workerだけが導出・使用する唯一の暗号化保存file。
	std::filesystem::path storage_ ;
} ;

} // namespace

std::unique_ptr<IAgentSettingsBackend> MakeWindowsAgentSettingsBackend()
{
	return std::make_unique<WindowsAgentSettingsBackend>( std::filesystem::path{}) ;
}

std::unique_ptr<IAgentSettingsBackend>
MakeFileAgentSettingsBackend( std::filesystem::path storage_path)
{
	return std::make_unique<WindowsAgentSettingsBackend>( std::move( storage_path)) ;
}

} // namespace zrdp::client::agent_settings
