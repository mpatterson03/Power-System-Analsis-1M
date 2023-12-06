// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#ifndef MAMBA_CORE_VALIDATE_HPP
#define MAMBA_CORE_VALIDATE_HPP

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mamba/core/timeref.hpp"
#include "mamba/core/util.hpp"
#include "mamba/fs/filesystem.hpp"
#include "mamba/validation/keys.hpp"

namespace mamba::validation
{
    [[nodiscard]] auto sha256sum(const fs::u8path& path) -> std::string_view;

    [[nodiscard]] auto md5sum(const fs::u8path& path) -> std::string_view;

    bool file_size(const fs::u8path& path, std::uintmax_t validation);

    inline constexpr std::size_t MAMBA_SHA256_SIZE_HEX = 64;
    inline constexpr std::size_t MAMBA_SHA256_SIZE_BYTES = 32;
    inline constexpr std::size_t MAMBA_MD5_SIZE_HEX = 32;
    inline constexpr std::size_t MAMBA_MD5_SIZE_BYTES = 16;
    inline constexpr std::size_t MAMBA_ED25519_KEYSIZE_HEX = 64;
    inline constexpr std::size_t MAMBA_ED25519_KEYSIZE_BYTES = 32;
    inline constexpr std::size_t MAMBA_ED25519_SIGSIZE_HEX = 128;
    inline constexpr std::size_t MAMBA_ED25519_SIGSIZE_BYTES = 64;

    int generate_ed25519_keypair(std::byte* pk, std::byte* sk);
    std::pair<std::array<std::byte, MAMBA_ED25519_KEYSIZE_BYTES>, std::array<std::byte, MAMBA_ED25519_KEYSIZE_BYTES>>
    generate_ed25519_keypair();
    std::pair<std::string, std::string> generate_ed25519_keypair_hex();

    int sign(const std::string& data, const std::byte* sk, std::byte* signature);
    int sign(const std::string& data, const std::string& sk, std::string& signature);

    std::array<std::byte, MAMBA_ED25519_SIGSIZE_BYTES>
    ed25519_sig_hex_to_bytes(const std::string& sig_hex, int& error_code) noexcept;

    std::array<std::byte, MAMBA_ED25519_KEYSIZE_BYTES>
    ed25519_key_hex_to_bytes(const std::string& key_hex, int& error_code) noexcept;

    int
    verify(const std::byte* data, std::size_t data_len, const std::byte* pk, const std::byte* signature);
    int verify(const std::string& data, const std::byte* pk, const std::byte* signature);
    int verify(const std::string& data, const std::string& pk_hex, const std::string& signature_hex);

    /**
     * Verify a GPG/PGP signature against the hash of the binary data and
     * the additional trailer added in V4 signature.
     * See RFC4880, section 5.2.4 https://datatracker.ietf.org/doc/html/rfc4880#section-5.2.4
     * This method assumes hash function to be SHA-256
     */
    int verify_gpg_hashed_msg(const std::byte* data, const std::byte* pk, const std::byte* signature);
    int
    verify_gpg_hashed_msg(const std::string& data, const std::byte* pk, const std::byte* signature);
    int
    verify_gpg_hashed_msg(const std::string& data, const std::string& pk, const std::string& signature);

    /**
     * Verify a GPG/PGP signature against the binary data and
     * the additional trailer added in V4 signature.
     * See RFC4880, section 5.2.4 https://datatracker.ietf.org/doc/html/rfc4880#section-5.2.4
     * This method assumes hash function to be SHA-256
     */
    int verify_gpg(
        const std::string& data,
        const std::string& gpg_v4_trailer,
        const std::string& pk,
        const std::string& signature
    );

    void check_timestamp_metadata_format(const std::string& ts);

    /**
     * Base class for spec implementations.
     */
    class SpecBase
    {
    public:

        virtual ~SpecBase() = default;

        std::string version_str() const;

        virtual std::string canonicalize(const nlohmann::json& j) const;

        std::string compatible_prefix() const;
        std::vector<std::string> upgrade_prefix() const;

        bool is_compatible(const fs::u8path& p) const;
        bool is_compatible(const nlohmann::json& j) const;
        bool is_compatible(const std::string& version) const;

        bool is_upgrade(const nlohmann::json& j) const;
        bool is_upgrade(const std::string& version) const;

        virtual bool upgradable() const;

        virtual std::string json_key() const = 0;
        virtual std::string expiration_json_key() const = 0;

        virtual std::set<RoleSignature> signatures(const nlohmann::json& j) const = 0;

    protected:

        SpecBase(std::string spec_version);
        SpecBase() = delete;

        std::string get_json_value(const nlohmann::json& j) const;

    private:

        std::string m_spec_version;
    };

    bool operator==(const SpecBase& sv1, const SpecBase& sv2);
    bool operator!=(const SpecBase& sv1, const SpecBase& sv2);


    /**
     * Base class for role implementation.
     */
    class RoleBase
    {
    public:

        RoleBase(const std::string& type, std::shared_ptr<SpecBase> sv);

        virtual ~RoleBase() = 0;

        std::string type() const;
        SpecBase& spec_version() const;
        std::size_t version() const;
        std::string file_ext() const;
        std::string expires() const;

        bool expired(const TimeRef& time_reference) const;

        std::set<std::string> roles() const;
        std::set<RoleSignature> signatures(const nlohmann::json& j) const;

        virtual RoleFullKeys self_keys() const = 0;
        std::map<std::string, RoleFullKeys> all_keys() const;

        friend void to_json(nlohmann::json& j, const RoleBase* r);
        friend void from_json(const nlohmann::json& j, RoleBase* r);

    protected:

        nlohmann::json read_json_file(const fs::u8path& p, bool update = false) const;

        /**
         * Check that a threshold of valid signatures is met
         * for the signed metadata of a role, using another
         * role keys (possibly the same).
         * Both signed and signatures metadata are contained
         * in 'data'.
         */
        void check_role_signatures(const nlohmann::json& data, const RoleBase& role);
        /**
         * Check that a threshold of valid signatures is met
         * for the signed metadata, using a set of keys.
         */
        void check_signatures(
            const std::string& signed_data,
            const std::set<RoleSignature>& signatures,
            const RoleFullKeys& keyring
        ) const;

        void set_spec_version(std::shared_ptr<SpecBase> sv);
        void set_expiration(const std::string& expires);

        // Forwarding to spec implementation
        std::string canonicalize(const nlohmann::json& j) const;
        // Return the spec implementation
        std::shared_ptr<SpecBase> spec_impl() const;

        // Mandatory roles defined by the current role
        virtual std::set<std::string> mandatory_defined_roles() const;
        // Optional roles defined by the current role
        virtual std::set<std::string> optionally_defined_roles() const;

        // Check role
        void check_expiration_format() const;
        void check_defined_roles(bool allow_any = false) const;

        std::map<std::string, RoleFullKeys> m_defined_roles;

    private:

        std::string m_internal_type;
        std::string m_type;
        std::shared_ptr<SpecBase> p_spec;
        std::size_t m_version = 1;
        std::string m_expires;
        std::string m_ext = "json";
    };

    // Forward declaration of RepoIndexChecker.
    class RepoIndexChecker;

    /**
     * 'root' role interface.
     */
    class RootRole : public RoleBase
    {
    public:

        virtual ~RootRole() = default;

        std::unique_ptr<RootRole> update(fs::u8path path);
        std::unique_ptr<RootRole> update(nlohmann::json j);

        std::vector<fs::u8path> possible_update_files();

        virtual std::unique_ptr<RepoIndexChecker>
        build_index_checker(Context& context, const TimeRef& time_reference, const std::string& url, const fs::u8path& cache_path) const = 0;

    protected:

        RootRole(std::shared_ptr<SpecBase> spec);

    private:

        virtual std::unique_ptr<RootRole> create_update(const nlohmann::json& j) = 0;
    };


    /**
     * Interface that performs validity checks
     * on a repository packages index.
     */
    class RepoIndexChecker
    {
    public:

        virtual ~RepoIndexChecker() = default;
        virtual void verify_index(const nlohmann::json& j) const = 0;
        virtual void verify_index(const fs::u8path& p) const = 0;
        virtual void verify_package(const nlohmann::json& signed_data, const nlohmann::json& signatures) const = 0;

    protected:

        RepoIndexChecker() = default;
    };


    /**
     * Perform security check against a repository
     * package index using cryptographic signatures.
     * Relies on multiple roles defined in TUF specification.
     */
    class RepoChecker
    {
    public:

        /**
         * Constructor.
         * @param base_url Repository base URL
         * @param ref_path Path to the reference directory, hosting trusted root metadata
         * @param cache_path Path to the cache directory
         */
        RepoChecker(
            Context& context,
            const std::string& base_url,
            const fs::u8path& ref_path,
            const fs::u8path& cache_path = ""
        );

        // Forwarding to a ``RepoIndexChecker`` implementation
        void verify_index(const nlohmann::json& j) const;
        void verify_index(const fs::u8path& p) const;
        void
        verify_package(const nlohmann::json& signed_data, const nlohmann::json& signatures) const;

        void generate_index_checker();

        const fs::u8path& cache_path();

        std::size_t root_version();

    private:

        std::string m_base_url;
        std::size_t m_root_version = 0;
        fs::u8path m_ref_path;
        fs::u8path m_cache_path;
        Context& m_context;

        fs::u8path initial_trusted_root();
        fs::u8path ref_root();
        fs::u8path cached_root();

        void persist_file(const fs::u8path& file_path);

        std::unique_ptr<RepoIndexChecker> p_index_checker;

        std::unique_ptr<RootRole> get_root_role(const TimeRef& time_reference);
    };
}
#endif
