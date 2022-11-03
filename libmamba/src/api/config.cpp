// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <iostream>

#include "mamba/api/configuration.hpp"


namespace mamba
{
    void config_describe()
    {
        auto& config = Configuration::instance();

        config.at("use_target_prefix_fallback").set_value(true);
        config.at("show_banner").set_value(false);
        config.at("target_prefix_checks")
            .set_value(MAMBA_ALLOW_EXISTING_PREFIX | MAMBA_ALLOW_MISSING_PREFIX
                       | MAMBA_ALLOW_NOT_ENV_PREFIX | MAMBA_NOT_EXPECT_EXISTING_PREFIX);
        config.load();

        auto show_group
            = config.at("show_config_groups").value<bool>() ? MAMBA_SHOW_CONFIG_GROUPS : 0;
        auto show_long_desc = config.at("show_config_long_descriptions").value<bool>()
                                  ? MAMBA_SHOW_CONFIG_LONG_DESCS
                                  : 0;
        auto specs = config.at("specs").value<std::vector<std::string>>();
        int dump_opts = MAMBA_SHOW_CONFIG_DESCS | show_long_desc | show_group;

        Console::stream() << config.dump(dump_opts, specs) << '\n';

        config.operation_teardown();
    }

    void config_list()
    {
        auto& config = Configuration::instance();

        config.at("use_target_prefix_fallback").set_value(true);
        config.at("show_banner").set_value(false);
        config.at("target_prefix_checks")
            .set_value(MAMBA_ALLOW_EXISTING_PREFIX | MAMBA_ALLOW_MISSING_PREFIX
                       | MAMBA_ALLOW_NOT_ENV_PREFIX | MAMBA_NOT_EXPECT_EXISTING_PREFIX);
        config.load();

        auto show_sources
            = config.at("show_config_sources").value<bool>() ? MAMBA_SHOW_CONFIG_SRCS : 0;
        auto show_all = config.at("show_all_configs").value<bool>() ? MAMBA_SHOW_ALL_CONFIGS : 0;
        auto show_all_rcs
            = config.at("show_all_rc_configs").value<bool>() ? MAMBA_SHOW_ALL_RC_CONFIGS : 0;
        auto show_group
            = config.at("show_config_groups").value<bool>() ? MAMBA_SHOW_CONFIG_GROUPS : 0;
        auto show_desc
            = config.at("show_config_descriptions").value<bool>() ? MAMBA_SHOW_CONFIG_DESCS : 0;
        auto show_long_desc = config.at("show_config_long_descriptions").value<bool>()
                                  ? MAMBA_SHOW_CONFIG_LONG_DESCS
                                  : 0;
        auto specs = config.at("specs").value<std::vector<std::string>>();
        int dump_opts = MAMBA_SHOW_CONFIG_VALUES | show_sources | show_desc | show_long_desc
                        | show_group | show_all_rcs | show_all;

        Console::stream() << config.dump(dump_opts, specs) << '\n';

        config.operation_teardown();
    }

    void config_sources()
    {
        auto& config = Configuration::instance();

        config.at("use_target_prefix_fallback").set_value(true);
        config.at("show_banner").set_value(false);
        config.at("target_prefix_checks")
            .set_value(MAMBA_ALLOW_EXISTING_PREFIX | MAMBA_ALLOW_MISSING_PREFIX
                       | MAMBA_ALLOW_NOT_ENV_PREFIX | MAMBA_NOT_EXPECT_EXISTING_PREFIX);
        config.load();

        auto& no_rc = config.at("no_rc").value<bool>();

        if (no_rc)
        {
            Console::stream() << "Configuration files disabled by --no-rc flag" << '\n';
        }
        else
        {
            Console::stream() << "Configuration files (by precedence order):" << '\n';

            auto srcs = config.sources();
            auto valid_srcs = config.valid_sources();

            for (auto s : srcs)
            {
                auto found_s = std::find(valid_srcs.begin(), valid_srcs.end(), s);
                if (found_s != valid_srcs.end())
                {
                    Console::stream() << env::shrink_user(s).string() << '\n';
                }
                else
                {
                    Console::stream() << env::shrink_user(s).string() << " (invalid)" << '\n';
                }
            }
        }

        config.operation_teardown();
    }
}
