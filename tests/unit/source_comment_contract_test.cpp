//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file source_comment_contract_test.cpp
 * @brief 自作sourceのコメント形式とmanager controlの共通font契約を検証する。
 * @version 01.00.02
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details source本文を変更せず、file契約、loop目的、test entry契約、
 *          file metadataと分岐目的を静的に確認する。実endpointやpayloadは扱わない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  managerの生成controlとfont適用対象の一致を検証。
 * Ver.01.00.02   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// 監査対象と固定規約
// -----------------------------------------------------------------------------

// 自作C++とresourceだけを監査する拡張子集合。生成物は含めない
constexpr std::array<std::wstring_view, 3> kSourceExtensions{
	L".h",
	L".cpp",
	L".rc",
} ;

// 更新履歴が増えても有限なfile headerだけを監査する上限
constexpr std::size_t					  kFileContractSearchLines	= 256U ;
// test入口の直前契約を検索する有限行数
constexpr std::size_t					  kEntryContractSearchLines = 16U ;
// file先頭の有限範囲内で確認する、初回移行後の必須metadata
constexpr std::array<std::string_view, 6> kRequiredMetadata{
	"Copyright (c) 2026 EnterSystem CO.,LTD.",
	"SPDX-License-Identifier: Apache-2.0",
	"@version",
	"@date",
	"@author Shunji Ogawa",
	"@par 更新履歴",
} ;

/**
 * @brief 一source fileの監査失敗分類。
 */
enum class CommentContractError {
	/// 異常は検出されておらず、要求の処理を継続できる。
	None,
	/// file全体を正常に読めず、部分設定を採用しない。
	FileReadFailed,
	/// file名または責務のDoxygen契約を取得できない。
	MissingFileContract,
	/// 反復の目的と終了条件の直前commentがない。
	MissingLoopPurpose,
	/// test entry pointの検証目的または戻り値契約がない。
	MissingTestEntryContract,
	/// 権利表示、制作者、日付、file版または履歴節がない。
	MissingFileMetadata,
	/// file版・日付の形式、履歴の単調増加または最新版の一致が不正。
	InvalidFileHistory,
	/// 条件成立時の処理意図が直前にない。
	MissingConditionPurpose,
	/// switchの選択目的またはcaseの意味が直前にない。
	MissingSwitchPurpose,
	/// fieldまたはenum値に禁止された行末Doxygen形式を使っている。
	TrailingMemberComment,
} ;

/**
 * @brief 監査結果と失敗位置を保持する。
 */
struct CommentContractResult {
	/// 失敗分類。
	CommentContractError error{CommentContractError::None} ;
	/// 1 originの失敗行。file全体の失敗は0。
	std::size_t			 line{0U} ;
} ;

// -----------------------------------------------------------------------------
// 文字列とfile読込み
// -----------------------------------------------------------------------------

/**
 * @brief ASCII whitespaceだけを除いた非所有viewを返す。
 * @param[in] text 一行のsource text。
 * @return 前後のspace、tab、CRを除いたview。
 */
[[nodiscard]] std::string_view TrimAscii( const std::string_view text) noexcept
{
	constexpr std::string_view whitespace{" \t\r"} ;
	const std::size_t		   first = text.find_first_not_of( whitespace) ;
	// 空白行をnposのままsubviewへ変換しない
	if( first == std::string_view::npos) {
		return {} ;
	}

	const std::size_t last = text.find_last_not_of( whitespace) ;
	return text.substr( first, last - first + 1U) ;
}

/**
 * @brief 指定prefixで始まるかC++20だけで判定する。
 * @param[in] text 判定対象。
 * @param[in] prefix 必須prefix。
 * @return prefixが完全一致する場合はtrue。
 */
[[nodiscard]] bool StartsWith( const std::string_view text, const std::string_view prefix) noexcept
{
	return text.size() >= prefix.size() && text.substr( 0U, prefix.size()) == prefix ;
}

/**
 * @brief コメントだけの一行またはblock comment終端か判定する。
 * @param[in] line trim済みsource行。
 * @return loop直前の目的commentとして認識できる場合はtrue。
 */
[[nodiscard]] bool IsCommentLine( const std::string_view line) noexcept
{
	return StartsWith( line, "//") || StartsWith( line, "/*") ||
		   (line.size() >= 2U && line.substr( line.size() - 2U) == "*/") ;
}

/**
 * @brief 監査対象拡張子か確認する。
 * @param[in] path 候補file path。
 * @return C++ header、implementation、resource sourceならtrue。
 */
[[nodiscard]] bool IsAuditedSource( const std::filesystem::path &path)
{
	const std::wstring extension = path.extension().wstring() ;
	// 許可した三種類だけを監査し、binaryや生成物を読まない
	return std::ranges::find( kSourceExtensions, std::wstring_view( extension)) !=
		   kSourceExtensions.end() ;
}

/**
 * @brief source fileを改行を除く所有行へ読込む。
 * @param[in] path 読込み対象。
 * @param[out] lines 成功時の全source行。
 * @return 最後まで読込めた場合はtrue。
 */
[[nodiscard]] bool ReadSourceLines( const std::filesystem::path &path,
								   std::vector<std::string>	   &lines)
{
	lines.clear() ;
	std::ifstream input( path, std::ios::binary) ;
	// 開けないsourceを監査済みとして扱わない
	if( !input.is_open()) {
		return false ;
	}

	std::string line ;
	// file全体を一行ずつ保持し、後続の直前comment検査で順序を維持する
	while( std::getline( input, line)) {
		lines.push_back( std::move( line)) ;
	}

	return input.eof() ;
}

/**
 * @brief 指定行範囲にtokenが存在するか確認する。
 * @param[in] lines 全source行。
 * @param[in] begin inclusive開始index。
 * @param[in] end exclusive終了index。
 * @param[in] token 検索するASCII token。
 * @return 範囲内のいずれかの行にtokenがあればtrue。
 */
[[nodiscard]] bool ContainsToken( const std::vector<std::string> &lines, const std::size_t begin,
								 const std::size_t end, const std::string_view token)
{
	const std::size_t bounded_end = std::min( end, lines.size()) ;
	// 呼出側が指定した有限範囲だけを順方向に検索する
	for( std::size_t index = std::min( begin, bounded_end); index < bounded_end; ++index) {
		// 指定tokenを含む最初の行で検索を完了する
		if( lines[index].find( token) != std::string::npos) {
			return true ;
		}
	}

	return false ;
}

/**
 * @brief file版と更新履歴の形式・順序・最新一致を検証する。
 * @param[in] lines 全source行。
 * @return metadataと履歴が規約を満たす場合はtrue。
 * @note 製品versionとは比較せず、独立したfile版だけを扱う。
 */
[[nodiscard]] bool HasValidFileHistory( const std::vector<std::string> &lines)
{
	const std::regex version_pattern( R"(@version ([0-9]{2}\.[0-9]{2}\.[0-9]{2})\s*$)") ;
	const std::regex date_pattern( R"(@date [0-9]{4}/(0[1-9]|1[0-2])/(0[1-9]|[12][0-9]|3[01])\s*$)") ;
	const std::regex history_pattern(
		R"(^\s*\* Ver\.([0-9]{2}\.[0-9]{2}\.[0-9]{2})\s+Shunji Ogawa\s+[0-9]{4}/(0[1-9]|1[0-2])/(0[1-9]|[12][0-9]|3[01])\s+\S.*$)") ;
	std::string file_version ;
	std::string last_version ;
	bool		valid_date		   = false ;
	bool		in_current_history = false ;
	// file headerだけを走査し、本文やtest fixtureのtagを流用しない
	for( std::size_t index = 0U; index < std::min( kFileContractSearchLines, lines.size()); ++index) {
		const std::string &line = lines[index] ;
		// 最初のfile契約終端より後のfixtureや本文からmetadataを流用しない
		if( line.find( "*/") != std::string::npos) {
			break ;
		}

		// 現行履歴だけを採用し、移行前の製品版履歴は比較に含めない
		if( line.find( "@par") != std::string::npos) {
			in_current_history = line.find( "@par 更新履歴") != std::string::npos ;
		}

		std::smatch match ;
		// 現在のfile版を固定幅表記のまま取得する
		if( std::regex_search( line, match, version_pattern)) {
			file_version = match[1].str() ;
		}

		// 作成日の表記は年・月・日の三項目を要求する
		if( std::regex_search( line, date_pattern)) {
			valid_date = true ;
		}

		// 新形式履歴だけを比較し、旧製品版の記録とは混同しない
		if( in_current_history && std::regex_match( line, match, history_pattern)) {
			const std::string version = match[1].str() ;
			// 固定幅なので文字列順が版の数値順となり、重複も拒否できる
			if( !last_version.empty() && version <= last_version) {
				return false ;
			}

			last_version = version ;
		}
	}

	return valid_date && !file_version.empty() && !last_version.empty() &&
		   file_version == last_version ;
}

// -----------------------------------------------------------------------------
// コメント契約の判定
// -----------------------------------------------------------------------------

/**
 * @brief source行がloop開始か判定する。
 * @param[in] line 一行のsource text。
 * @return `for`、`while`、`do`の開始行ならtrue。
 */
[[nodiscard]] bool IsLoopStart( const std::string_view line) noexcept
{
	const std::string_view trimmed = TrimAscii( line) ;
	return StartsWith( trimmed, "for (") || StartsWith( trimmed, "for(") ||
		   StartsWith( trimmed, "while (") || StartsWith( trimmed, "while(") || trimmed == "do {" ||
		   trimmed == "do{" ;
}

/**
 * @brief test entry pointの宣言開始行か判定する。
 * @param[in] line 一行のsource text。
 * @return `int main`または`int wmain`の開始行ならtrue。
 */
[[nodiscard]] bool IsTestEntryPoint( const std::string_view line) noexcept
{
	const std::string_view trimmed = TrimAscii( line) ;
	return StartsWith( trimmed, "int main(") || StartsWith( trimmed, "int main (") ||
		   StartsWith( trimmed, "int wmain(") || StartsWith( trimmed, "int wmain (") ;
}

/**
 * @brief 指定行の直前に目的commentがあるか確認する。
 * @param[in] lines 全source行。
 * @param[in] line_index 判定対象の0 origin行。
 * @return 直前の非空行がcommentならtrue。
 */
[[nodiscard]] bool HasImmediatePurposeComment( const std::vector<std::string> &lines,
											  const std::size_t				  line_index)
{
	std::size_t previous = line_index ;
	// 空行だけを遡り、別statementを越えて遠いcommentを流用しない
	while( previous > 0U) {
		--previous ;
		const std::string_view trimmed = TrimAscii( lines[previous]) ;
		// 改行による視覚的な区切りは許容するがstatementは越えない
		if( trimmed.empty()) {
			continue ;
		}

		return IsCommentLine( trimmed) ;
	}

	return false ;
}

/**
 * @brief 読込み済みsourceのfile metadata・制御文目的・test入口契約を監査する。
 * @param[in] lines 監査対象の全source行。
 * @param[in] test_source tests配下のsourceならtrue。
 * @return 成功または最初の失敗分類と行番号。
 */
[[nodiscard]] CommentContractResult AuditLines( const std::vector<std::string> &lines,
											   const bool					   test_source)
{
	std::size_t file_contract_end = std::min( kFileContractSearchLines, lines.size()) ;
	// 本文の文字列fixtureがheader metadataの不足を隠さないよう最初の契約で区切る
	for( std::size_t index = 0U; index < file_contract_end; ++index) {
		// Doxygen file headerの終端を含む有限範囲だけをmetadata検索に渡す
		if( lines[index].find( "*/") != std::string::npos) {
			file_contract_end = index + 1U ;
			break ;
		}
	}

	// 全sourceは有限な冒頭範囲にfile名と責務を明示する
	if( !ContainsToken( lines, 0U, file_contract_end, "@file") ||
	   !ContainsToken( lines, 0U, file_contract_end, "@brief")) {
		return {CommentContractError::MissingFileContract, 0U} ;
	}

	// 指定された権利・制作者・独立file版を冒頭の契約に要求する
	for( const std::string_view tag : kRequiredMetadata) {
		// 一項目でも不足したfileはmetadata未整備として報告する
		if( !ContainsToken( lines, 0U, file_contract_end, tag)) {
			return {CommentContractError::MissingFileMetadata, 0U} ;
		}
	}

	// file版と最新履歴が不整合なら履歴を監査済みにしない
	if( !HasValidFileHistory( lines)) {
		return {CommentContractError::InvalidFileHistory, 0U} ;
	}

	// source順を維持し、最初の不足位置を再現可能な結果として返す
	for( std::size_t index = 0U; index < lines.size(); ++index) {
		// 反復の目的がない開始行を最初の不足位置として報告する
		if( IsLoopStart( lines[index]) && !HasImmediatePurposeComment( lines, index)) {
			return {
				CommentContractError::MissingLoopPurpose,
				index + 1U,
			} ;
		}

		const std::string_view line		 = TrimAscii( lines[index]) ;
		const bool			   condition = StartsWith( line, "if(") || StartsWith( line, "if (") ||
							   StartsWith( line, "if constexpr(") ||
							   StartsWith( line, "if constexpr (") || StartsWith( line, "else if(") ||
							   StartsWith( line, "else if (") || StartsWith( line, "} else if(") ||
							   StartsWith( line, "} else if (") ;
		// 条件成立時の意図をstatementごとに確認する
		if( condition && !HasImmediatePurposeComment( lines, index)) {
			return {CommentContractError::MissingConditionPurpose, index + 1U} ;
		}

		const bool switch_or_case = StartsWith( line, "switch(") || StartsWith( line, "switch (") ||
									StartsWith( line, "case ") || StartsWith( line, "default:") ;
		// 選択全体と各分岐の意味を独立して確認する
		if( switch_or_case && !HasImmediatePurposeComment( lines, index)) {
			return {CommentContractError::MissingSwitchPurpose, index + 1U} ;
		}

		// Doxygenのfield説明は直前形式へ統一する
		if( line.find( "/"
					 "/"
					 "/<") != std::string_view::npos) {
			return {CommentContractError::TrailingMemberComment, index + 1U} ;
		}

		// test実行入口は検証目的とreturn code契約を直前に持たせる
		if( test_source && IsTestEntryPoint( lines[index])) {
			const std::size_t begin =
				index > kEntryContractSearchLines ? index - kEntryContractSearchLines : 0U ;
			// 検証対象と終了値の意味が揃わないentry pointは拒否する
			if( !ContainsToken( lines, begin, index, "@brief") ||
			   !ContainsToken( lines, begin, index, "@return")) {
				return {
					CommentContractError::MissingTestEntryContract,
					index + 1U,
				} ;
			}
		}
	}

	return {} ;
}

/**
 * @brief 一source fileを読込み、そのコメント契約を監査する。
 * @param[in] path 監査対象source。
 * @param[in] test_source tests配下のsourceならtrue。
 * @return 成功または最初の失敗分類と行番号。
 */
[[nodiscard]] CommentContractResult AuditSource( const std::filesystem::path &path,
												const bool					 test_source)
{
	std::vector<std::string> lines ;
	// 読込み失敗時は内容不明のためfail-closedにする
	if( !ReadSourceLines( path, lines)) {
		return {CommentContractError::FileReadFailed, 0U} ;
	}

	return AuditLines( lines, test_source) ;
}

/**
 * @brief 監査器がmetadata不足、履歴不整合、目的comment不足を拒否するか確認する。
 * @return 正常fixtureの受理と各異常fixtureの拒否が一致した場合はtrue。
 * @note 実fileを書かず、読み込まれた行と同じinterfaceだけを使う。
 */
[[nodiscard]] bool VerifyAuditFixtures()
{
	const std::vector<std::string> valid{
		"/**",
		" * Copyright (c) 2026 EnterSystem CO.,LTD.",
		" * SPDX-License-Identifier: Apache-2.0",
		" * @file fixture.cpp",
		" * @brief 監査fixture。",
		" * @version 01.00.01",
		" * @date 2026/10/07",
		" * @author Shunji Ogawa",
		" * @par 更新履歴",
		" * <pre>",
		" * Ver.01.00.00   Shunji Ogawa  2026/10/07  初回作成。",
		" * Ver.01.00.01   Shunji Ogawa  2026/10/07  監査経路を検証。",
		" * </pre>",
		" */",
		"// 境界外入力を拒否する",
		"if( invalid) {",
		"}",
		"// 要素を有限個だけ確認する",
		"for( int i = 0; i < 1; ++i) {",
		"}",
		"// 状態に応じた操作を選ぶ",
		"switch( state) {",
		"// 通常状態だけを受け付ける",
		"case 1:",
		"break ;",
		"// 未知状態は操作しない",
		"default:",
		"break ;",
		"}",
	} ;
	// 全契約を持つ入力を拒否する誤検知がないことを先に確認する
	if( AuditLines( valid, false).error != CommentContractError::None) {
		return false ;
	}

	const std::array<std::pair<std::size_t, CommentContractError>, 5> omissions{
		std::pair{1U, CommentContractError::MissingFileMetadata},
		std::pair{11U, CommentContractError::InvalidFileHistory},
		std::pair{14U, CommentContractError::MissingConditionPurpose},
		std::pair{17U, CommentContractError::MissingLoopPurpose},
		std::pair{22U, CommentContractError::MissingSwitchPurpose},
	} ;
	// 必須要素を一つずつ落とし、分類を変えずに不足を検出できるか確認する
	for( const auto &[index, expected] : omissions) {
		std::vector<std::string> invalid = valid ;
		invalid[index]					 = "int unrelated ;" ;
		// 対象の不足を別分類や成功として扱う監査器は採用しない
		if( AuditLines( invalid, false).error != expected) {
			return false ;
		}
	}

	std::vector<std::string> duplicate = valid ;
	duplicate[11]					   = duplicate[10] ;
	// 同じ版数を再利用した履歴は単調増加の契約違反として拒否する
	if( AuditLines( duplicate, false).error != CommentContractError::InvalidFileHistory) {
		return false ;
	}

	std::vector<std::string> dereference = valid ;
	dereference[14]						 = "*output = nullptr ;" ;
	// pointerへの代入をblock commentの継続行と誤認しない
	return AuditLines( dereference, false).error == CommentContractError::MissingConditionPurpose ;
}

/**
 * @brief managerの生成必須control一覧と共通font配布一覧の一致を検証する。
 * @param[in] source manager implementationまたは自作fixtureの文字列。
 * @return 二一覧が重複なく一致し、接続とAgent設定buttonを含む場合true。
 * @note 実Window、history、設定、RDPを生成せずsourceだけを確認する。
 */
[[nodiscard]] bool VerifyManagerFontControls( const std::string &source)
{
	const std::regex arrays( R"(std::array<HWND,\s*([0-9]+)>\s+controls\s*\{([^}]+)\})") ;
	const std::regex identifier( "[A-Za-z_][A-Za-z0-9_]*") ;
	std::array<std::vector<std::string>, 2U> lists{} ;
	std::size_t								 index = 0U ;
	// 生成一覧とfont配布一覧の二つだけを比較し、別型の一覧は扱わない
	for( auto match = std::sregex_iterator( source.begin(), source.end(), arrays);
		match != std::sregex_iterator(); ++match) {
		// 想定外の同名HWND一覧を見落として成功にしない
		if( index >= lists.size()) {
			return false ;
		}

		const std::string fields = (*match)[2].str() ;
		auto			 &list	 = lists[index] ;
		// 列挙されたcontrol名だけを比較用に取得する
		for( auto field = std::sregex_iterator( fields.begin(), fields.end(), identifier);
			field != std::sregex_iterator(); ++field) {
			list.push_back( field->str()) ;
		}

		std::sort( list.begin(), list.end()) ;
		// 件数・重複・主要二buttonの欠落を明示的に拒否する
		if( std::to_string( list.size()) != (*match)[1].str() ||
		   std::adjacent_find( list.begin(), list.end()) != list.end() ||
		   std::find( list.begin(), list.end(), "connect_button_") == list.end() ||
		   std::find( list.begin(), list.end(), "agent_settings_button_") == list.end()) {
			return false ;
		}

		++index ;
	}

	return index == lists.size() && lists[0] == lists[1] ;
}

/**
 * @brief font監査の正常・適用漏れ・両一覧の欠落を自作文字列だけで確認する。
 * @return font適用順に依存せず、欠落と件数違反を拒否する場合true。
 */
[[nodiscard]] bool VerifyManagerFontFixtures()
{
	const std::string created =
		"const std::array<HWND, 2> controls{connect_button_,agent_settings_button_,};" ;
	const std::string reordered =
		"const std::array<HWND, 2> controls{agent_settings_button_,connect_button_,};" ;
	const std::string missing = "const std::array<HWND, 1> controls{connect_button_,};" ;
	return VerifyManagerFontControls( created + reordered) &&
		   !VerifyManagerFontControls( created + missing) &&
		   !VerifyManagerFontControls( missing + missing) && !VerifyManagerFontControls( created) ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief `src`、`tests`、icon resourceの全source comment契約を検証する。
 * @param[in] argument_count 実行fileを含む引数個数。
 * @param[in] arguments 第1引数にproject source rootを受け取る。
 * @return 全sourceが契約を満たす場合は0、不足時は分類済み非0 code。
 */
int wmain( const int argument_count, wchar_t *arguments[])
{
	// 監査器自身の拒否経路が壊れていればsourceの監査を開始しない
	if( !VerifyAuditFixtures() || !VerifyManagerFontFixtures()) {
		return 6 ;
	}

	// CTestからproject rootが一件だけ渡された場合に監査を開始する
	if( argument_count != 2 || arguments[1] == nullptr) {
		return 1 ;
	}

	const std::filesystem::path project_root( arguments[1]) ;
	std::ifstream	  manager_source( project_root / L"src/client/app/connection_manager_window.cpp",
									 std::ios::binary) ;
	const std::string manager_text{std::istreambuf_iterator<char>( manager_source),
								   std::istreambuf_iterator<char>()} ;
	// sourceを読めない場合や必須controlへfontが配られない場合は失敗する
	if( !manager_source || !VerifyManagerFontControls( manager_text)) {
		std::cerr << "Manager font control lists do not match\n" ;
		return 20 ;
	}

	const std::array<std::filesystem::path, 3> roots{
		project_root / L"src",
		project_root / L"tests",
		project_root / L"assets" / L"icons",
	} ;

	std::size_t audited_files = 0U ;
	// source、test、icon resourceの三rootだけを再帰走査する
	for( const std::filesystem::path &root : roots) {
		std::error_code								  iterator_error ;
		std::filesystem::recursive_directory_iterator iterator(
			root, std::filesystem::directory_options::skip_permission_denied, iterator_error) ;
		// root自体を走査できない場合は未監査sourceを残さない
		if( iterator_error) {
			return 2 ;
		}

		const std::filesystem::recursive_directory_iterator end ;
		// directory entryを一度ずつ確認し、対象拡張子だけを監査する
		while( iterator != end) {
			const std::filesystem::directory_entry &entry = *iterator ;
			std::error_code							entry_error ;
			const bool regular_file = entry.is_regular_file( entry_error) ;
			// entryの種別が不明なまま監査対象を除外しない
			if( entry_error) {
				return 3 ;
			}

			// 対象sourceだけを読み、生成物や他拡張子を除外する
			if( regular_file && IsAuditedSource( entry.path())) {
				const bool test_source =
					entry.path().lexically_normal().wstring().find(
						(project_root / L"tests").lexically_normal().wstring()) == 0U ;
				const CommentContractResult result = AuditSource( entry.path(), test_source) ;
				// 失敗したfileと行を表示し、最初の不足を再現可能にする
				if( result.error != CommentContractError::None) {
					std::wcerr << entry.path().wstring() << L":" << result.line << L" error "
							   << static_cast<int>( result.error) << L"\n" ;
					return 10 + static_cast<int>( result.error) ;
				}

				++audited_files ;
			}

			iterator.increment( iterator_error) ;
			// 走査途中の失敗も未監査sourceを残した成功にはしない
			if( iterator_error) {
				return 4 ;
			}
		}
	}

	// 空のroot指定や誤ったpathを成功扱いにしない
	if( audited_files == 0U) {
		return 5 ;
	}

	return 0 ;
}
