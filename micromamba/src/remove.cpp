// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include "mamba/api/configuration.hpp"
#include "mamba/api/remove.hpp"

#include "common_options.hpp"


using namespace mamba;  // NOLINT(build/namespaces)

void
set_remove_command(CLI::App* subcom)
{
    using string_list = std::vector<std::string>;
    init_general_options(subcom);
    init_prefix_options(subcom);

    auto& config = Configuration::instance();

    auto& specs = config.at("specs");
    subcom->add_option(
        "specs",
        specs.get_cli_config<string_list>(),
        "Specs to remove from the environment"
    );

    static bool remove_all = false, force = false, prune = true;
    subcom->add_flag("-a,--all", remove_all, "Remove all packages in the environment");
    subcom->add_flag(
        "-f,--force",
        force,
        "Force removal of package (note: consistency of environment is not guaranteed!"
    );
    subcom->add_flag("--prune,!--no-prune", prune, "Prune dependencies (default)");

    subcom->callback(
        []
        {
            int flags = 0;
            if (prune)
            {
                flags |= MAMBA_REMOVE_PRUNE;
            }
            if (force)
            {
                flags |= MAMBA_REMOVE_FORCE;
            }
            if (remove_all)
            {
                flags |= MAMBA_REMOVE_ALL;
            }
            remove(flags);
        }
    );
}
