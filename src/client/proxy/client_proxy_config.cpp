//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_proxy_config.cpp
 * @brief EXE隣接`client.ini`の有限読込みと厳格なport解析を実装する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Clientの同意付き更新と設定の分離を追加。
 * </pre>
 */
//------------------------------------------------------------------------------

#include <Windows.h>

#include "client/proxy/client_proxy_config.h"

#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace zrdp::client::proxy {
namespace {

// EXEと同じdirectoryからだけ読み込むoptional Proxy設定file名
constexpr wchar_t  kClientProxyConfigFileName[] = L"client.ini" ;

/**
 * @brief Windows HANDLEを一所有者で閉じる。
 */
class UniqueHandle final {
public:
	/**
	 * @brief 初期状態の管理instanceを構築し、所有資源の解放責任を固定する。
	 */
	UniqueHandle() noexcept = default ;

	/**
	 * @brief 初期状態の管理instanceを構築し、所有資源の解放責任を固定する。
	 * @param[in] handle 受け取る資源handle。無効値を許容し、有効な場合だけ終了時に解放する。
	 */
	explicit UniqueHandle( const HANDLE handle) noexcept : handle_( handle)
	{
	}

	/**
	 * @brief 所有資源を終了経路で解放し、後続の参照を残さない。
	 */
	~UniqueHandle()
	{
		Reset() ;
	}

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	UniqueHandle( const UniqueHandle &)				= delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	UniqueHandle &operator=( const UniqueHandle &)	= delete ;
	/**
	 * @brief 稼働中の所有資源とlifecycleの移動を禁止する。
	 */
	UniqueHandle( UniqueHandle &&)					= delete ;
	/**
	 * @brief 稼働中の所有資源とlifecycleの移動を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	UniqueHandle		&operator=( UniqueHandle &&) = delete ;

	/**
	 * @brief 現在の非owning HANDLE値を返す。
	 * @return 未所有時は`INVALID_HANDLE_VALUE`。
	 */
	[[nodiscard]] HANDLE Get() const noexcept
	{
		return handle_ ;
	}

	/**
	 * @brief 所有HANDLEを一度だけ閉じる。
	 */
	void Reset() noexcept
	{
		// 設定file handleを所有している場合だけ閉じてsentinelへ戻す
		if( handle_ != INVALID_HANDLE_VALUE) {
			static_cast<void>( CloseHandle( handle_)) ;
			handle_ = INVALID_HANDLE_VALUE ;
		}
	}

private:
	/// このRAII instanceが単独所有するWin32 handle。無効値はAPIの契約に従う。
	HANDLE handle_{INVALID_HANDLE_VALUE} ;
} ;

/**
 * @brief ASCII空白だけを両端から除外する。
 * @param[in] value 対象範囲。
 * @return 同じ所有bufferを参照するtrim済み範囲。
 */
[[nodiscard]] std::string_view TrimAscii( std::string_view value) noexcept
{
	std::size_t first = 0U ;
	// INI tokenの前置きspaceとtabだけを構文外として読み飛ばす
	while( first < value.size() && (value[first] == ' ' || value[first] == '\t')) {
		++first ;
	}

	std::size_t last = value.size() ;
	// INI tokenの後置きspaceとtabだけを構文外として読み飛ばす
	while( last > first && (value[last - 1U] == ' ' || value[last - 1U] == '\t')) {
		--last ;
	}

	return value.substr( first, last - first) ;
}

/**
 * @brief 一つのdecimal portを範囲検証する。
 * @param[in] value trim済みtoken。
 * @param[out] port 成功時の1..65535。
 * @return 全文字をdecimalとして消費できた場合はtrue。
 */
[[nodiscard]] bool ParsePort( const std::string_view value, std::uint16_t &port) noexcept
{
	// 空のport指定は既定値へ読み替えず設定不正として拒否する
	if( value.empty()) {
		return false ;
	}

	std::uint32_t parsed = 0U ;
	const auto	  result = std::from_chars( value.data(), value.data() + value.size(), parsed, 10) ;
	// 部分parse、overflow、0、TCP port上限超過を設定として受け付けない
	if( result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0U ||
	   parsed > std::numeric_limits<std::uint16_t>::max()) {
		return false ;
	}

	port = static_cast<std::uint16_t>( parsed) ;
	return true ;
}

/**
 * @brief 実行中moduleの絶対pathを取得する。
 * @param[out] path 成功時のNULを含まないpath。
 * @return 完全pathを取得できた場合はtrue。
 */
[[nodiscard]] bool ResolveExecutablePath( std::wstring &path) noexcept
{
	try {
		std::array<wchar_t, 32768U> buffer{} ;
		const DWORD					length =
			GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>( buffer.size())) ;
		// 0またはbuffer全長は失敗／切詰めであり固定設定pathを確定できない
		if( length == 0U || static_cast<std::size_t>( length) >= buffer.size()) {
			return false ;
		}

		path.assign( buffer.data(), length) ;
	} catch( ...) {
		path.clear() ;
		return false ;
	}

	return true ;
}

} // namespace

ClientProxyConfigurationResult
ParseClientProxyConfiguration( std::string_view			text,
							  ClientProxyConfiguration &configuration) noexcept
{
	configuration = ClientProxyConfiguration{} ;
	ClientProxyConfiguration parsed_configuration{} ;
	// file reader以外から呼ぶunit testでも同じ全体上限を適用する
	if( text.size() > kMaximumClientProxyConfigBytes) {
		return {
			ClientProxyConfigurationError::FileTooLarge,
			0U,
		} ;
	}

	// UTF-8 BOMだけを任意に除き、data自体はASCII subsetへ制限する
	if( text.starts_with( "\xEF\xBB\xBF")) {
		text.remove_prefix( 3U) ;
	}

	// 埋込みNULはsectionを問わずfileの多義的な解釈を防ぐため拒否する
	for( const unsigned char character : text) {
		// NULを終端として部分設定を採用しない
		if( character == 0U) {
			return {
				ClientProxyConfigurationError::UnsupportedEncoding,
				0U,
			} ;
		}
	}

	bool		proxy_section_seen	= false ;
	bool		in_proxy_section	= false ;
	bool		in_update_section	= false ;
	bool		update_section_seen = false ;
	bool		port_seen			= false ;
	std::size_t offset				= 0U ;
	// 改行単位で有限長を検証し、最後の改行がない一行も同じ規則で解析する
	while( offset <= text.size()) {
		const std::size_t line_end = text.find( '\n', offset) ;
		const std::size_t end	   = line_end == std::string_view::npos ? text.size() : line_end ;
		// 一行の上限超過では有限parserの許容量を越えて保持しない
		if( end - offset > kMaximumClientProxyConfigLineBytes) {
			return {
				ClientProxyConfigurationError::LineTooLong,
				0U,
			} ;
		}

		std::string_view line = text.substr( offset, end - offset) ;
		// CRLFのCRを除いて行単位の設定解析を統一する
		if( !line.empty() && line.back() == '\r') {
			line.remove_suffix( 1U) ;
		}

		line = TrimAscii( line) ;
		// 更新sectionの値だけは専用parserへ委譲し、更新設定異常でProxyを停止しない
		if( !in_update_section || (!line.empty() && line.front() == '[')) {
			// Proxy構文とsection名には従来どおりASCIIだけを許可する
			for( const unsigned char character : line) {
				// Unicodeの別表記をProxy設定名やportに解釈しない
				if( character > 0x7FU) {
					return {ClientProxyConfigurationError::UnsupportedEncoding, 0U} ;
				}
			}
		}

		// 空行とcomment行を除いた設定行だけを解析する
		if( !line.empty() && line.front() != ';' && line.front() != '#') {
			// section宣言はkey-valueとは分けて許可sectionを検証する
			if( line.front() == '[') {
				// 更新sectionの再宣言や不正値は更新parserだけが拒否する
				if( line == "[update]") {
					update_section_seen = true ;
					in_update_section	= true ;
					in_proxy_section	= false ;
				} else {
					// 閉じ括弧以外のsuffixや未知sectionを黙って無視しない
					if( line != "[proxy]") {
						return {
							ClientProxyConfigurationError::UnknownSection,
							0U,
						} ;
					}

					// proxy sectionの再宣言は曖昧な設定として拒否する
					if( proxy_section_seen) {
						return {
							ClientProxyConfigurationError::MalformedLine,
							0U,
						} ;
					}

					proxy_section_seen = true ;
					in_proxy_section   = true ;
					in_update_section  = false ;
				}
			}
			// 更新専用値を除いた行だけを既存Proxy portとして解析する
			else if( !in_update_section) {
				const std::size_t separator = line.find( '=') ;
				// section外、区切りなし、複数区切りを曖昧なkeyとして扱わない
				if( !in_proxy_section || separator == std::string_view::npos ||
				   line.find( '=', separator + 1U) != std::string_view::npos) {
					return {
						ClientProxyConfigurationError::MalformedLine,
						0U,
					} ;
				}

				const std::string_view key	 = TrimAscii( line.substr( 0U, separator)) ;
				const std::string_view value = TrimAscii( line.substr( separator + 1U)) ;
				// 未知のkeyを無視せず入力誤りとしてfail-closedにする
				if( key != "port") {
					return {
						ClientProxyConfigurationError::UnknownKey,
						0U,
					} ;
				}

				// portが複数定義された場合は優先順位を推測せず拒否する
				if( port_seen) {
					return {
						ClientProxyConfigurationError::DuplicatePort,
						0U,
					} ;
				}

				std::uint16_t port = 0U ;
				// 不正なport値では別portへ自動fallbackせずlistenerを停止する
				if( !ParsePort( value, port)) {
					return {
						ClientProxyConfigurationError::InvalidPort,
						0U,
					} ;
				}

				parsed_configuration.port			  = port ;
				parsed_configuration.loaded_from_file = true ;
				port_seen							  = true ;
			}
		}

		// 最終行を処理したら設定textの走査を終了する
		if( line_end == std::string_view::npos) {
			break ;
		}

		offset = line_end + 1U ;
	}

	// 存在するfileは明示portを一件だけ持つ完全設定に限定する
	if( (!proxy_section_seen || !port_seen) && !(update_section_seen && !proxy_section_seen)) {
		return {
			ClientProxyConfigurationError::MissingPort,
			0U,
		} ;
	}

	configuration = parsed_configuration ;
	return {} ;
}

ClientProxyConfigurationResult LoadClientProxyConfigurationFromExecutableDirectory(
	ClientProxyConfiguration &configuration) noexcept
{
	configuration = ClientProxyConfiguration{} ;
	std::wstring executable_path ;
	// EXE配置を特定できなければ他directoryの設定へfallbackしない
	if( !ResolveExecutablePath( executable_path)) {
		return {
			ClientProxyConfigurationError::ExecutablePathUnavailable,
			static_cast<std::uint32_t>( GetLastError()),
		} ;
	}

	const std::size_t separator = executable_path.find_last_of( L"\\/") ;
	// directoryなしのmodule pathから相対設定fileを推測しない
	if( separator == std::wstring::npos) {
		return {
			ClientProxyConfigurationError::ExecutablePathUnavailable,
			0U,
		} ;
	}

	try {
		executable_path.resize( separator + 1U) ;
		executable_path.append( kClientProxyConfigFileName) ;
	} catch( ...) {
		return {
			ClientProxyConfigurationError::ResourceExhausted,
			0U,
		} ;
	}

	UniqueHandle file( CreateFileW( executable_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
								  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) ;
	// fileを開けない場合は任意の読込み失敗とoptional file不在を区別する
	if( file.Get() == INVALID_HANDLE_VALUE) {
		const DWORD error = GetLastError() ;
		// optional file欠落だけは既定portを使う正常経路とする
		if( error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
			return {} ;
		}

		return {
			ClientProxyConfigurationError::FileOpenFailed,
			static_cast<std::uint32_t>( error),
		} ;
	}

	LARGE_INTEGER file_size{} ;
	// file長の取得失敗では部分設定を採用しない
	if( !GetFileSizeEx( file.Get(), &file_size)) {
		return {
			ClientProxyConfigurationError::FileReadFailed,
			static_cast<std::uint32_t>( GetLastError()),
		} ;
	}

	// 負値、64 KiB超過、DWORDへ安全に変換できないfileを読まない
	if( file_size.QuadPart < 0LL ||
	   static_cast<unsigned long long>( file_size.QuadPart) > kMaximumClientProxyConfigBytes) {
		return {
			ClientProxyConfigurationError::FileTooLarge,
			0U,
		} ;
	}

	std::vector<char> bytes ;
	try {
		bytes.resize( static_cast<std::size_t>( file_size.QuadPart)) ;
	} catch( ...) {
		return {
			ClientProxyConfigurationError::ResourceExhausted,
			0U,
		} ;
	}

	DWORD bytes_read = 0U ;
	// 非空fileのread失敗または短いreadは欠落設定として拒否する
	if( !bytes.empty() && (!ReadFile( file.Get(), bytes.data(), static_cast<DWORD>( bytes.size()),
									&bytes_read, nullptr) ||
						  static_cast<std::size_t>( bytes_read) != bytes.size())) {
		return {
			ClientProxyConfigurationError::FileReadFailed,
			static_cast<std::uint32_t>( GetLastError()),
		} ;
	}

	return ParseClientProxyConfiguration( std::string_view( bytes.data(), bytes.size()),
										 configuration) ;
}

} // namespace zrdp::client::proxy
