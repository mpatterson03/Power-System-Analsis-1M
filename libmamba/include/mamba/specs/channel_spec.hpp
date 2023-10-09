// Copyright (c) 2023, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#ifndef MAMBA_SPECS_CHANNEL_SPEC_HPP
#define MAMBA_SPECS_CHANNEL_SPEC_HPP

#include <string>
#include <string_view>

#include "mamba/util/flat_set.hpp"

namespace mamba::specs
{
    /**
     * Channel specification.
     *
     * This represent the string that is passed by the user to select a channel.
     * It needs to be resolved in order to get a final URL/path.
     * This is even true when a full URL or path is given, as some authentification information
     * may come from channel alias or login database.
     *
     * Note that for a string to be considered a URL, it must have an explicit scheme.
     * So "repo.anaconda.com" is considered a name, similarily to "conda-forge" and not a URL.
     * This is because otherwise it is not possible to tell names and URL appart.
     */
    class ChannelSpec
    {
    public:

        enum class Type
        {
            /**
             * A URL to a full repo strucuture.
             *
             * Example "https://repo.anaconda.com/conda-forge".
             */
            URL,
            /**
             * A URL to a single package.
             *
             * Example "https://repo.anaconda.com/conda-forge/linux-64/pkg-0.0-bld.conda".
             */
            PackageURL,
            /**
             * An absolute path to a full repo strucuture.
             *
             * Example "/Users/name/conda-bld".
             */
            Path,
            /**
             * An absolute path to a single-package.
             *
             * Example "/tmp/pkg-0.0-bld.conda".
             */
            PackagePath,
            /**
             * A relative name.
             *
             * It needs to be resolved using a channel alias, or a custom channel.
             * Example "conda-forge", "locals", "my-channel/my-label".
             */
            Name,
        };

        inline static constexpr std::string_view default_name = "defaults";
        inline static constexpr std::string_view platform_separators = "|,;";

        [[nodiscard]] static auto parse(std::string_view str) -> ChannelSpec;

        ChannelSpec() = default;
        ChannelSpec(std::string location, util::flat_set<std::string> filters);

        [[nodiscard]] auto type() const -> Type;

        [[nodiscard]] auto location() const& -> const std::string&;
        [[nodiscard]] auto location() && -> std::string;
        [[nodiscard]] auto clear_location() -> std::string;

        [[nodiscard]] auto platform_filters() const& -> const util::flat_set<std::string>&;
        [[nodiscard]] auto platform_filters() && -> util::flat_set<std::string>;
        [[nodiscard]] auto clear_platform_filters() -> util::flat_set<std::string>;

    private:

        std::string m_location = std::string(default_name);
        util::flat_set<std::string> m_platform_filters = {};
    };
}
#endif