/**
 * @file rdp_user_identity.cpp
 * @brief RDPドメインとユーザー名の有限長正規化を実装する。
 *
 * @details 実接続情報をlogへ渡さず、接続前とDPAPI履歴の両境界で同じ
 *          `DOMAIN\user`解釈を使用する。
 */
#include "client/app/rdp_user_identity.h"

#include <new>
#include <string_view>
#include <utility>

namespace zrdp::client::app {
namespace {

constexpr wchar_t kDomainSeparator = L'\\';

/**
 * @brief Win32文字列へ渡せない埋込みNULを検出する。
 * @param[in] value 検証対象のUTF-16文字列。
 * @return 埋込みNULを含む場合はtrue。
 */
[[nodiscard]] bool ContainsEmbeddedNull(
    const std::wstring_view value) noexcept {
    return value.find(L'\0') != std::wstring_view::npos;
}

}  // namespace

HRESULT NormalizeRdpUserIdentity(
    std::wstring domain,
    std::wstring user_name,
    RdpUserIdentity& normalized) noexcept {
    try {
        RdpUserIdentity candidate;

        // COMへ渡す前に文字数と埋込みNULのresource境界を強制する。
        if (domain.size() > kMaximumRdpDomainCharacters ||
            user_name.size() > kMaximumRdpUserNameCharacters ||
            ContainsEmbeddedNull(domain) ||
            ContainsEmbeddedNull(user_name)) {
            return E_INVALIDARG;
        }

        // ドメイン欄自体へ区切りを含む二重指定を許可しない。
        if (domain.find(kDomainSeparator) != std::wstring::npos) {
            return E_INVALIDARG;
        }

        if (domain.empty()) {
            const std::size_t separator = user_name.find(kDomainSeparator);
            // 旧形式がある場合だけ、空でない二要素へ安全に分離する。
            if (separator != std::wstring::npos) {
                const bool has_multiple_separators =
                    user_name.find(kDomainSeparator, separator + 1U) !=
                    std::wstring::npos;
                // 空domain、空user、複数区切りは意味が曖昧なので拒否する。
                if (separator == 0U ||
                    separator + 1U >= user_name.size() ||
                    has_multiple_separators) {
                    return E_INVALIDARG;
                }

                domain = user_name.substr(0U, separator);
                user_name.erase(0U, separator + 1U);
            }
        } else {
            // 分離domainと結合済みuserを同時指定すると認証対象が曖昧になる。
            if (user_name.empty() ||
                user_name.find(kDomainSeparator) != std::wstring::npos) {
                return E_INVALIDARG;
            }
        }

        // 旧形式から分離したdomainにも同じ上限を再適用する。
        if (domain.size() > kMaximumRdpDomainCharacters) {
            return E_INVALIDARG;
        }

        const std::size_t qualified_character_count =
            domain.empty()
                ? user_name.size()
                : domain.size() + 1U + user_name.size();
        // 既存履歴formatと旧版の有限上限を超える識別子は保存不能なので拒否する。
        if (qualified_character_count >
            kMaximumQualifiedRdpUserNameCharacters) {
            return E_INVALIDARG;
        }

        candidate.domain = std::move(domain);
        candidate.user_name = std::move(user_name);
        normalized = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        // public noexcept境界から文字列処理例外を漏らさない。
        return E_UNEXPECTED;
    }
}

HRESULT FormatQualifiedRdpUserName(
    const RdpUserIdentity& identity,
    std::wstring& qualified_user_name) noexcept {
    RdpUserIdentity normalized;
    HRESULT result = NormalizeRdpUserIdentity(
        identity.domain,
        identity.user_name,
        normalized);
    if (FAILED(result)) {
        return result;
    }

    try {
        std::wstring candidate;
        if (normalized.domain.empty()) {
            candidate = std::move(normalized.user_name);
        } else {
            candidate.reserve(
                normalized.domain.size() +
                1U +
                normalized.user_name.size());
            candidate.append(normalized.domain);
            candidate.push_back(kDomainSeparator);
            candidate.append(normalized.user_name);
        }

        qualified_user_name = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        // public noexcept境界から文字列構築例外を漏らさない。
        return E_UNEXPECTED;
    }
}

}  // namespace zrdp::client::app
