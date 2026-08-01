/**
 * @file rdp_user_identity.h
 * @brief RDPドメインとユーザー名の検証・正規化契約を定義する。
 *
 * @details passwordを扱わず、Microsoft RDP ActiveXへ渡す非機密識別子だけを
 *          有限長で管理する。
 */
#pragma once

#include <Windows.h>

#include <cstddef>
#include <string>

namespace zrdp::client::app {

/// DNS形式を含むRDPドメイン入力のapplication上限。
inline constexpr std::size_t kMaximumRdpDomainCharacters = 255U;

/// Microsoft標準credential UIへ渡すユーザー名のapplication上限。
inline constexpr std::size_t kMaximumRdpUserNameCharacters = 512U;

/// 旧履歴formatとの互換性を維持する`DOMAIN\user`全体の上限。
inline constexpr std::size_t kMaximumQualifiedRdpUserNameCharacters = 512U;

/**
 * @brief Microsoft RDP ActiveXへ別propertyとして渡すlogon識別子。
 *
 * passwordや認証tokenは意図的にmemberへ含めない。
 */
struct RdpUserIdentity {
    /// 任意のRDPドメイン。空ならWindows標準UIでの選択を許容する。
    std::wstring domain;

    /// RDPユーザー名。ドメインとは別にActiveXへ設定する。
    std::wstring user_name;
};

/**
 * @brief 分離入力または旧`DOMAIN\user`入力を一つの正規形へ変換する。
 * @param[in] domain ドメイン欄から取得した所有文字列。
 * @param[in] user_name ユーザー名欄または旧形式の所有文字列。
 * @param[out] normalized 成功時だけ置換する分離済み識別子。
 * @return 有効な有限長識別子を構築できた場合はS_OK。
 *
 * ドメインが空の場合だけ、一つの`\`を持つ旧形式を分離する。UPN形式は
 * ユーザー名として維持する。埋込みNULや曖昧な複数区切りは拒否する。
 */
[[nodiscard]] HRESULT NormalizeRdpUserIdentity(
    std::wstring domain,
    std::wstring user_name,
    RdpUserIdentity& normalized) noexcept;

/**
 * @brief 分離済み識別子を旧履歴互換の`DOMAIN\user`表現へ変換する。
 * @param[in] identity 検証対象の分離識別子。
 * @param[out] qualified_user_name 成功時だけ置換する履歴用文字列。
 * @return 有効な有限長文字列を構築できた場合はS_OK。
 *
 * passwordを結合または保存せず、ドメインとユーザー名だけを扱う。
 */
[[nodiscard]] HRESULT FormatQualifiedRdpUserName(
    const RdpUserIdentity& identity,
    std::wstring& qualified_user_name) noexcept;

}  // namespace zrdp::client::app
