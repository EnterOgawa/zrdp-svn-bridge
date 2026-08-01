/**
 * @file source_comment_contract_test.cpp
 * @brief 全C++／resource sourceがZRdpのコメント形式を維持するか検証する。
 *
 * @details source本文を変更せず、file契約、loop目的、test entry契約、
 *          手書き履歴禁止を静的に確認する。実endpointやpayloadは扱わない。
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// 監査対象と固定規約
// -----------------------------------------------------------------------------

constexpr std::array<std::wstring_view, 3> kSourceExtensions{
    L".h",
    L".cpp",
    L".rc",
};

constexpr std::size_t kFileContractSearchLines = 30U;
constexpr std::size_t kEntryContractSearchLines = 16U;
constexpr std::string_view kForbiddenAuthorTag{"@" "author"};
constexpr std::string_view kForbiddenVersionTag{"@" "version"};
constexpr std::string_view kForbiddenManualHistory{"更新" "履歴"};

/**
 * @brief 一source fileの監査失敗分類。
 */
enum class CommentContractError {
    None,
    FileReadFailed,
    MissingFileContract,
    MissingLoopPurpose,
    MissingTestEntryContract,
    ForbiddenManualHistory,
};

/**
 * @brief 監査結果と失敗位置を保持する。
 */
struct CommentContractResult {
    CommentContractError error{CommentContractError::None};  ///< 失敗分類。
    std::size_t line{0U};  ///< 1 originの失敗行。file全体の失敗は0。
};

// -----------------------------------------------------------------------------
// 文字列とfile読込み
// -----------------------------------------------------------------------------

/**
 * @brief ASCII whitespaceだけを除いた非所有viewを返す。
 * @param[in] text 一行のsource text。
 * @return 前後のspace、tab、CRを除いたview。
 */
[[nodiscard]] std::string_view TrimAscii(
    const std::string_view text) noexcept {
    constexpr std::string_view whitespace{" \t\r"};
    const std::size_t first = text.find_first_not_of(whitespace);
    // 空白行をnposのままsubviewへ変換しない。
    if (first == std::string_view::npos) {
        return {};
    }

    const std::size_t last = text.find_last_not_of(whitespace);
    return text.substr(first, last - first + 1U);
}

/**
 * @brief 指定prefixで始まるかC++20だけで判定する。
 * @param[in] text 判定対象。
 * @param[in] prefix 必須prefix。
 * @return prefixが完全一致する場合はtrue。
 */
[[nodiscard]] bool StartsWith(
    const std::string_view text,
    const std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
        text.substr(0U, prefix.size()) == prefix;
}

/**
 * @brief コメントだけの一行またはblock comment終端か判定する。
 * @param[in] line trim済みsource行。
 * @return loop直前の目的commentとして認識できる場合はtrue。
 */
[[nodiscard]] bool IsCommentLine(
    const std::string_view line) noexcept {
    return StartsWith(line, "//") ||
        StartsWith(line, "/*") ||
        StartsWith(line, "*") ||
        (line.size() >= 2U &&
         line.substr(line.size() - 2U) == "*/");
}

/**
 * @brief 監査対象拡張子か確認する。
 * @param[in] path 候補file path。
 * @return C++ header、implementation、resource sourceならtrue。
 */
[[nodiscard]] bool IsAuditedSource(
    const std::filesystem::path& path) {
    const std::wstring extension = path.extension().wstring();
    // 許可した三種類だけを監査し、binaryや生成物を読まない。
    return std::ranges::find(
               kSourceExtensions,
               std::wstring_view(extension)) !=
        kSourceExtensions.end();
}

/**
 * @brief source fileを改行を除く所有行へ読込む。
 * @param[in] path 読込み対象。
 * @param[out] lines 成功時の全source行。
 * @return 最後まで読込めた場合はtrue。
 */
[[nodiscard]] bool ReadSourceLines(
    const std::filesystem::path& path,
    std::vector<std::string>& lines) {
    lines.clear();
    std::ifstream input(path, std::ios::binary);
    // 開けないsourceを監査済みとして扱わない。
    if (!input.is_open()) {
        return false;
    }

    std::string line;
    // file全体を一行ずつ保持し、後続の直前comment検査で順序を維持する。
    while (std::getline(input, line)) {
        lines.push_back(std::move(line));
    }

    return input.eof();
}

/**
 * @brief 指定行範囲にtokenが存在するか確認する。
 * @param[in] lines 全source行。
 * @param[in] begin inclusive開始index。
 * @param[in] end exclusive終了index。
 * @param[in] token 検索するASCII token。
 * @return 範囲内のいずれかの行にtokenがあればtrue。
 */
[[nodiscard]] bool ContainsToken(
    const std::vector<std::string>& lines,
    const std::size_t begin,
    const std::size_t end,
    const std::string_view token) {
    const std::size_t bounded_end = std::min(end, lines.size());
    // 呼出側が指定した有限範囲だけを順方向に検索する。
    for (std::size_t index = std::min(begin, bounded_end);
         index < bounded_end;
         ++index) {
        if (lines[index].find(token) != std::string::npos) {
            return true;
        }
    }

    return false;
}

// -----------------------------------------------------------------------------
// コメント契約の判定
// -----------------------------------------------------------------------------

/**
 * @brief source行がloop開始か判定する。
 * @param[in] line 一行のsource text。
 * @return `for`、`while`、`do`の開始行ならtrue。
 */
[[nodiscard]] bool IsLoopStart(
    const std::string_view line) noexcept {
    const std::string_view trimmed = TrimAscii(line);
    return StartsWith(trimmed, "for (") ||
        StartsWith(trimmed, "for(") ||
        StartsWith(trimmed, "while (") ||
        StartsWith(trimmed, "while(") ||
        trimmed == "do {" ||
        trimmed == "do{";
}

/**
 * @brief test entry pointの宣言開始行か判定する。
 * @param[in] line 一行のsource text。
 * @return `int main`または`int wmain`の開始行ならtrue。
 */
[[nodiscard]] bool IsTestEntryPoint(
    const std::string_view line) noexcept {
    const std::string_view trimmed = TrimAscii(line);
    return StartsWith(trimmed, "int main(") ||
        StartsWith(trimmed, "int main (") ||
        StartsWith(trimmed, "int wmain(") ||
        StartsWith(trimmed, "int wmain (");
}

/**
 * @brief 指定行の直前に目的commentがあるか確認する。
 * @param[in] lines 全source行。
 * @param[in] line_index 判定対象の0 origin行。
 * @return 直前の非空行がcommentならtrue。
 */
[[nodiscard]] bool HasImmediatePurposeComment(
    const std::vector<std::string>& lines,
    const std::size_t line_index) {
    std::size_t previous = line_index;
    // 空行だけを遡り、別statementを越えて遠いcommentを流用しない。
    while (previous > 0U) {
        --previous;
        const std::string_view trimmed =
            TrimAscii(lines[previous]);
        if (trimmed.empty()) {
            continue;
        }

        return IsCommentLine(trimmed);
    }

    return false;
}

/**
 * @brief 一source fileの全コメント契約を監査する。
 * @param[in] path 監査対象source。
 * @param[in] test_source tests配下のsourceならtrue。
 * @return 成功または最初の失敗分類と行番号。
 */
[[nodiscard]] CommentContractResult AuditSource(
    const std::filesystem::path& path,
    const bool test_source) {
    std::vector<std::string> lines;
    // 読込み失敗時は内容不明のためfail-closedにする。
    if (!ReadSourceLines(path, lines)) {
        return {CommentContractError::FileReadFailed, 0U};
    }

    // 全sourceは冒頭30行以内にfile名と責務を明示する。
    if (!ContainsToken(
            lines,
            0U,
            kFileContractSearchLines,
            "@file") ||
        !ContainsToken(
            lines,
            0U,
            kFileContractSearchLines,
            "@brief")) {
        return {CommentContractError::MissingFileContract, 0U};
    }

    // Gitと重複する個人名、手書きversion、変更記録をsourceへ持ち込まない。
    if (ContainsToken(
            lines,
            0U,
            lines.size(),
            kForbiddenAuthorTag) ||
        ContainsToken(
            lines,
            0U,
            lines.size(),
            kForbiddenVersionTag) ||
        ContainsToken(
            lines,
            0U,
            lines.size(),
            kForbiddenManualHistory)) {
        return {CommentContractError::ForbiddenManualHistory, 0U};
    }

    // source順を維持し、最初の不足位置を再現可能な結果として返す。
    for (std::size_t index = 0U;
         index < lines.size();
         ++index) {
        if (IsLoopStart(lines[index]) &&
            !HasImmediatePurposeComment(lines, index)) {
            return {
                CommentContractError::MissingLoopPurpose,
                index + 1U,
            };
        }

        // test実行入口は検証目的とreturn code契約を直前に持たせる。
        if (test_source &&
            IsTestEntryPoint(lines[index])) {
            const std::size_t begin =
                index > kEntryContractSearchLines
                    ? index - kEntryContractSearchLines
                    : 0U;
            if (!ContainsToken(
                    lines,
                    begin,
                    index,
                    "@brief") ||
                !ContainsToken(
                    lines,
                    begin,
                    index,
                    "@return")) {
                return {
                    CommentContractError::
                        MissingTestEntryContract,
                    index + 1U,
                };
            }
        }
    }

    return {};
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief `src`、`tests`、icon resourceの全source comment契約を検証する。
 * @param[in] argument_count 実行fileを含む引数個数。
 * @param[in] arguments 第1引数にproject source rootを受け取る。
 * @return 全sourceが契約を満たす場合は0、不足時は分類済み非0 code。
 */
int wmain(
    const int argument_count,
    wchar_t* arguments[]) {
    // CTestからproject rootが一件だけ渡された場合に監査を開始する。
    if (argument_count != 2 ||
        arguments[1] == nullptr) {
        return 1;
    }

    const std::filesystem::path project_root(arguments[1]);
    const std::array<std::filesystem::path, 3> roots{
        project_root / L"src",
        project_root / L"tests",
        project_root / L"assets" / L"icons",
    };

    std::size_t audited_files = 0U;
    // source、test、icon resourceの三rootだけを再帰走査する。
    for (const std::filesystem::path& root : roots) {
        std::error_code iterator_error;
        std::filesystem::recursive_directory_iterator iterator(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            iterator_error);
        // root自体を走査できない場合は未監査sourceを残さない。
        if (iterator_error) {
            return 2;
        }

        const std::filesystem::recursive_directory_iterator end;
        // directory entryを一度ずつ確認し、対象拡張子だけを監査する。
        while (iterator != end) {
            const std::filesystem::directory_entry& entry = *iterator;
            std::error_code entry_error;
            const bool regular_file =
                entry.is_regular_file(entry_error);
            if (entry_error) {
                return 3;
            }

            if (regular_file &&
                IsAuditedSource(entry.path())) {
                const bool test_source =
                    entry.path().lexically_normal().wstring().find(
                        (project_root / L"tests")
                            .lexically_normal()
                            .wstring()) == 0U;
                const CommentContractResult result =
                    AuditSource(entry.path(), test_source);
                if (result.error !=
                    CommentContractError::None) {
                    return 10 +
                        static_cast<int>(result.error);
                }

                ++audited_files;
            }

            iterator.increment(iterator_error);
            if (iterator_error) {
                return 4;
            }
        }
    }

    // 空のroot指定や誤ったpathを成功扱いにしない。
    if (audited_files == 0U) {
        return 5;
    }

    return 0;
}
