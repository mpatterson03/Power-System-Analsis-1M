// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include <string_view>

#include <spdlog/spdlog.h>

#include "mamba/core/context.hpp"
#include "mamba/core/fetch.hpp"
#include "mamba/core/output.hpp"
#include "mamba/core/thread_utils.hpp"
#include "mamba/core/url.hpp"
#include "mamba/core/util_string.hpp"
#include "mamba/version.hpp"

#include "compression.hpp"
#include "curl.hpp"
#include "progress_bar_impl.hpp"

namespace mamba
{
    /*********************************
     * DownloadTarget implementation *
     *********************************/

    DownloadTarget::DownloadTarget(const std::string& name, const std::string& url, const std::string& filename)
        : m_name(name)
        , m_filename(filename)
        , m_url(unc_url(url))
    {
        m_curl_handle = std::make_unique<CURLHandle>();
        init_curl_ssl();
        init_curl_target(m_url);
    }

    DownloadTarget::~DownloadTarget()
    {
    }

    std::size_t DownloadTarget::get_default_retry_timeout()
    {
        return static_cast<std::size_t>(Context::instance().remote_fetch_info.retry_timeout);
    }

    void DownloadTarget::init_curl_handle(CURL* handle, const std::string& url)
    {
        // if the request is slower than 30b/s for 60 seconds, cancel.
        std::string no_low_speed_limit = std::getenv("MAMBA_NO_LOW_SPEED_LIMIT")
                                             ? std::getenv("MAMBA_NO_LOW_SPEED_LIMIT")
                                             : "0";

        std::string ssl_no_revoke_env = std::getenv("MAMBA_SSL_NO_REVOKE")
                                            ? std::getenv("MAMBA_SSL_NO_REVOKE")
                                            : "0";
        bool set_ssl_no_revoke = (Context::instance().remote_fetch_info.ssl_no_revoke || ssl_no_revoke_env != "0");

        curl::configure_curl_handle(
            handle,
            url,
            (no_low_speed_limit == "0"),
            Context::instance().remote_fetch_info.connect_timeout_secs,
            set_ssl_no_revoke,
            proxy_match(url),
            Context::instance().remote_fetch_info.ssl_verify
        );
    }

    int
    curl_debug_callback(CURL* /* handle */, curl_infotype type, char* data, size_t size, void* userptr)
    {
        auto* logger = reinterpret_cast<spdlog::logger*>(userptr);
        auto log = Console::hide_secrets(std::string_view(data, size));
        switch (type)
        {
            case CURLINFO_TEXT:
                logger->info(fmt::format("* {}", log));
                break;
            case CURLINFO_HEADER_OUT:
                logger->info(fmt::format("> {}", log));
                break;
            case CURLINFO_HEADER_IN:
                logger->info(fmt::format("< {}", log));
                break;
            default:
                break;
        }
        return 0;
    }

    void DownloadTarget::init_curl_ssl()
    {
        auto& ctx = Context::instance();

        if (!ctx.remote_fetch_info.curl_initialized)
        {
            if (ctx.remote_fetch_info.ssl_verify == "<false>")
            {
                LOG_DEBUG << "'ssl_verify' not activated, skipping cURL SSL init";
                ctx.remote_fetch_info.curl_initialized = true;
                return;
            }

#ifdef LIBMAMBA_STATIC_DEPS
            auto init_res = m_curl_handle->get_ssl_backend_info();
            switch (init_res.second)
            {
                case CurlLogLevel::kInfo:
                {
                    LOG_INFO << init_res.first;
                    break;
                }
                case CurlLogLevel::kWarning:
                {
                    LOG_WARNING << init_res.first;
                    break;
                }
                case CurlLogLevel::kError:
                {
                    LOG_ERROR << init_res.first;
                    break;
                }
            }
#endif

            if (!ctx.remote_fetch_info.ssl_verify.size()
                && std::getenv("REQUESTS_CA_BUNDLE") != nullptr)
            {
                ctx.remote_fetch_info.ssl_verify = std::getenv("REQUESTS_CA_BUNDLE");
                LOG_INFO << "Using REQUESTS_CA_BUNDLE " << ctx.remote_fetch_info.ssl_verify;
            }
            else if (ctx.remote_fetch_info.ssl_verify == "<system>" && on_linux)
            {
                std::array<std::string, 6> cert_locations{
                    "/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu/Gentoo etc.
                    "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora/RHEL 6
                    "/etc/ssl/ca-bundle.pem",              // OpenSUSE
                    "/etc/pki/tls/cacert.pem",             // OpenELEC
                    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // CentOS/RHEL 7
                    "/etc/ssl/cert.pem",                                  // Alpine Linux
                };
                bool found = false;

                for (const auto& loc : cert_locations)
                {
                    if (fs::exists(loc))
                    {
                        ctx.remote_fetch_info.ssl_verify = loc;
                        found = true;
                    }
                }

                if (!found)
                {
                    LOG_ERROR << "No CA certificates found on system";
                    throw std::runtime_error("Aborting.");
                }
            }

            ctx.remote_fetch_info.curl_initialized = true;
        }
    }

    void DownloadTarget::init_curl_target(const std::string& url)
    {
        init_curl_handle(m_curl_handle->handle(), url);

        m_curl_handle->set_opt(CURLOPT_HEADERFUNCTION, &DownloadTarget::header_callback);
        m_curl_handle->set_opt(CURLOPT_HEADERDATA, this);

        if (ends_with(url, ".json.zst"))
        {
            m_zstd_stream = std::make_unique<ZstdStream>(&DownloadTarget::write_callback, this);
            if (ends_with(m_filename, ".zst"))
            {
                m_filename = m_filename.substr(0, m_filename.size() - 4);
            }
            m_curl_handle->set_opt(CURLOPT_WRITEFUNCTION, ZstdStream::write_callback);
            m_curl_handle->set_opt(CURLOPT_WRITEDATA, m_zstd_stream.get());
        }
        else if (ends_with(url, ".json.bz2"))
        {
            m_bzip2_stream = std::make_unique<Bzip2Stream>(&DownloadTarget::write_callback, this);
            if (ends_with(m_filename, ".bz2"))
            {
                m_filename = m_filename.substr(0, m_filename.size() - 4);
            }
            m_curl_handle->set_opt(CURLOPT_WRITEFUNCTION, Bzip2Stream::write_callback);
            m_curl_handle->set_opt(CURLOPT_WRITEDATA, m_bzip2_stream.get());
        }
        else
        {
            m_curl_handle->set_opt(CURLOPT_WRITEFUNCTION, &DownloadTarget::write_callback);
            m_curl_handle->set_opt(CURLOPT_WRITEDATA, this);
        }

        if (ends_with(url, ".json"))
        {
            // accept all encodings supported by the libcurl build
            m_curl_handle->set_opt(CURLOPT_ACCEPT_ENCODING, "");
            m_curl_handle->add_header("Content-Type: application/json");
        }

        std::string user_agent = fmt::format(
            "User-Agent: {} {}",
            Context::instance().remote_fetch_info.user_agent,
            curl_version()
        );

        m_curl_handle->add_header(user_agent);
        m_curl_handle->set_opt_header();
        m_curl_handle->set_opt(CURLOPT_VERBOSE, Context::instance().output_info.verbosity >= 2);

        auto logger = spdlog::get("libcurl");
        m_curl_handle->set_opt(CURLOPT_DEBUGFUNCTION, curl_debug_callback);
        m_curl_handle->set_opt(CURLOPT_DEBUGDATA, logger.get());
    }

    bool DownloadTarget::can_retry()
    {
        switch (result)
        {
            case CURLE_ABORTED_BY_CALLBACK:
            case CURLE_BAD_FUNCTION_ARGUMENT:
            case CURLE_CONV_REQD:
            case CURLE_COULDNT_RESOLVE_PROXY:
            case CURLE_FILESIZE_EXCEEDED:
            case CURLE_INTERFACE_FAILED:
            case CURLE_NOT_BUILT_IN:
            case CURLE_OUT_OF_MEMORY:
            // See RhBug: 1219817
            // case CURLE_RECV_ERROR:
            // case CURLE_SEND_ERROR:
            case CURLE_SSL_CACERT_BADFILE:
            case CURLE_SSL_CRL_BADFILE:
            case CURLE_WRITE_ERROR:
            case CURLE_OPERATION_TIMEDOUT:
                return false;
                break;
            default:
                // Other error are not considered fatal
                break;
        }

        return m_retries < size_t(Context::instance().remote_fetch_info.max_retries)
               && (http_status == 413 || http_status == 429 || http_status >= 500)
               && !starts_with(m_url, "file://");
    }

    CURL* DownloadTarget::retry()
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= m_next_retry)
        {
            if (m_file.is_open())
            {
                m_file.close();
            }
            if (fs::exists(m_filename))
            {
                fs::remove(m_filename);
            }
            init_curl_target(m_url);  // Not sure this is needed, TODO to remove?
            if (m_has_progress_bar)
            {
                m_curl_handle->set_opt(CURLOPT_XFERINFOFUNCTION, &DownloadTarget::progress_callback);
                m_curl_handle->set_opt(CURLOPT_XFERINFODATA, this);
            }
            m_retry_wait_seconds = m_retry_wait_seconds
                                   * static_cast<std::size_t>(
                                       Context::instance().remote_fetch_info.retry_backoff
                                   );
            m_next_retry = now + std::chrono::seconds(m_retry_wait_seconds);
            m_retries++;
            return m_curl_handle->handle();
        }
        else
        {
            return nullptr;
        }
    }

    size_t DownloadTarget::write_callback(char* ptr, size_t size, size_t nmemb, void* self)
    {
        auto* s = reinterpret_cast<DownloadTarget*>(self);
        auto expected_write_size = size * nmemb;
        if (!s->m_file.is_open())
        {
            s->m_file = open_ofstream(s->m_filename, std::ios::binary);
            if (!s->m_file)
            {
                LOG_ERROR << "Could not open file for download " << s->m_filename << ": "
                          << strerror(errno);
                // Return a size _different_ than the expected write size to signal an error
                return expected_write_size + 1;
            }
        }

        s->m_file.write(ptr, static_cast<std::streamsize>(expected_write_size));

        if (!s->m_file)
        {
            LOG_ERROR << "Could not write to file " << s->m_filename << ": " << strerror(errno);
            // Return a size _different_ than the expected write size to signal an error
            return expected_write_size + 1;
        }
        return expected_write_size;
    }

    size_t DownloadTarget::header_callback(char* buffer, size_t size, size_t nitems, void* self)
    {
        auto* s = reinterpret_cast<DownloadTarget*>(self);

        std::string_view header(buffer, size * nitems);
        auto colon_idx = header.find(':');
        if (colon_idx != std::string_view::npos)
        {
            std::string_view key, value;
            key = header.substr(0, colon_idx);
            colon_idx++;
            // remove spaces
            while (std::isspace(header[colon_idx]))
            {
                ++colon_idx;
            }

            // remove \r\n header ending
            auto header_end = header.find_first_of("\r\n");
            value = header.substr(colon_idx, (header_end > colon_idx) ? header_end - colon_idx : 0);

            // http headers are case insensitive!
            std::string lkey = to_lower(key);
            if (lkey == "etag")
            {
                s->etag = value;
            }
            else if (lkey == "cache-control")
            {
                s->cache_control = value;
            }
            else if (lkey == "last-modified")
            {
                s->mod = value;
            }
        }
        return nitems * size;
    }

    std::function<void(ProgressBarRepr&)> DownloadTarget::download_repr()
    {
        return [&](ProgressBarRepr& r) -> void
        {
            r.current.set_value(fmt::format(
                "{:>7}",
                to_human_readable_filesize(static_cast<double>(m_progress_bar.current()), 1)
            ));

            std::string total_str;
            if (!m_progress_bar.total()
                || (m_progress_bar.total() == std::numeric_limits<std::size_t>::max()))
            {
                total_str = "??.?MB";
            }
            else
            {
                total_str = to_human_readable_filesize(static_cast<double>(m_progress_bar.total()), 1);
            }
            r.total.set_value(fmt::format("{:>7}", total_str));

            auto speed = m_progress_bar.speed();
            r.speed.set_value(fmt::format(
                "@ {:>7}/s",
                speed ? to_human_readable_filesize(static_cast<double>(speed), 1) : "??.?MB"
            ));

            r.separator.set_value("/");
        };
    }

    std::chrono::steady_clock::time_point DownloadTarget::progress_throttle_time() const
    {
        return m_progress_throttle_time;
    }

    void DownloadTarget::set_progress_throttle_time(const std::chrono::steady_clock::time_point& time)
    {
        m_progress_throttle_time = time;
    }

    int DownloadTarget::progress_callback(
        void* f,
        curl_off_t total_to_download,
        curl_off_t now_downloaded,
        curl_off_t,
        curl_off_t
    )
    {
        auto* target = static_cast<DownloadTarget*>(f);

        auto now = std::chrono::steady_clock::now();
        if (now - target->progress_throttle_time() < std::chrono::milliseconds(50))
        {
            return 0;
        }
        else
        {
            target->set_progress_throttle_time(now);
        }

        if (!total_to_download && !target->expected_size())
        {
            target->m_progress_bar.activate_spinner();
        }
        else
        {
            target->m_progress_bar.deactivate_spinner();
        }

        if (!total_to_download && target->expected_size())
        {
            target->m_progress_bar.update_current(static_cast<std::size_t>(now_downloaded));
        }
        else
        {
            target->m_progress_bar.update_progress(
                static_cast<std::size_t>(now_downloaded),
                static_cast<std::size_t>(total_to_download)
            );
        }

        target->m_progress_bar.set_speed(static_cast<std::size_t>(target->get_speed()));

        return 0;
    }

    void DownloadTarget::set_mod_etag_headers(const std::string& lmod, const std::string& letag)
    {
        auto to_header = [](const std::string& key, const std::string& value)
        { return key + ": " + value; };

        if (!letag.empty())
        {
            m_curl_handle->add_header(to_header("If-None-Match", letag));
        }
        if (!lmod.empty())
        {
            m_curl_handle->add_header(to_header("If-Modified-Since", lmod));
        }
    }

    void DownloadTarget::set_progress_bar(ProgressProxy progress_proxy)
    {
        m_has_progress_bar = true;
        m_progress_bar = progress_proxy;
        m_progress_bar.set_repr_hook(download_repr());

        m_curl_handle->set_opt(CURLOPT_XFERINFOFUNCTION, &DownloadTarget::progress_callback);
        m_curl_handle->set_opt(CURLOPT_XFERINFODATA, this);
        m_curl_handle->set_opt(CURLOPT_NOPROGRESS, 0L);
    }

    void DownloadTarget::set_expected_size(std::size_t size)
    {
        m_expected_size = size;
    }

    void DownloadTarget::set_head_only(bool yes)
    {
        m_curl_handle->set_opt(CURLOPT_NOBODY, yes);
    }

    const std::string& DownloadTarget::name() const
    {
        return m_name;
    }

    const std::string& DownloadTarget::url() const
    {
        return m_url;
    }

    std::size_t DownloadTarget::expected_size() const
    {
        return m_expected_size;
    }

    static size_t discard(char*, size_t size, size_t nmemb, void*)
    {
        return size * nmemb;
    }

    bool DownloadTarget::resource_exists()
    {
        init_curl_ssl();
        auto handle = curl_easy_init();
        init_curl_handle(handle, m_url);

        curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);

        if (curl_easy_perform(handle) == CURLE_OK)
        {
            return true;
        }

        long response_code;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);

        if (response_code == 405)
        {
            // Method not allowed
            // Some servers don't support HEAD, try a GET if the HEAD fails
            curl_easy_setopt(handle, CURLOPT_NOBODY, 0L);
            // Prevent output of data
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &discard);
            return curl_easy_perform(handle) == CURLE_OK;
        }
        else
        {
            return false;
        }
    }

    bool DownloadTarget::perform()
    {
        LOG_INFO << "Downloading to filename: " << m_filename;

        result = curl_easy_perform(m_curl_handle->handle());
        set_result(result);
        return finalize();
    }

    CURL* DownloadTarget::handle()
    {
        return m_curl_handle->handle();
    }

    curl_off_t DownloadTarget::get_speed()
    {
        auto speed = m_curl_handle->get_info<long>(CURLINFO_SPEED_DOWNLOAD_T);
        // TODO Should we just drop all code below with progress_bar and use value_or(0) in get_info
        // above instead?
        if (!speed.has_value())
        {
            if (m_has_progress_bar)
            {
                return static_cast<curl_off_t>(m_progress_bar.avg_speed());
            }
            else
            {
                return 0;
            }
        }
        return speed.value();
    }

    void DownloadTarget::set_result(CURLcode r)
    {
        result = r;
        if (r != CURLE_OK)
        {
            auto leffective_url = m_curl_handle->get_info<char*>(CURLINFO_EFFECTIVE_URL).value();

            std::stringstream err;
            err << "Download error (" << result << ") " << curl_easy_strerror(result) << " ["
                << leffective_url << "]\n";
            if (m_curl_handle->get_error_buffer()[0] != '\0')
            {
                err << m_curl_handle->get_error_buffer();
            }
            LOG_INFO << err.str();

            m_next_retry = std::chrono::steady_clock::now()
                           + std::chrono::seconds(m_retry_wait_seconds);

            if (m_has_progress_bar)
            {
                m_progress_bar.update_progress(0, 1);
                // m_progress_bar.set_elapsed_time();
                m_progress_bar.set_postfix(curl_easy_strerror(result));
            }
            if (!m_ignore_failure && !can_retry())
            {
                throw std::runtime_error(err.str());
            }
        }
    }

    bool DownloadTarget::finalize()
    {
        avg_speed = get_speed();
        http_status = m_curl_handle->get_info<int>(CURLINFO_RESPONSE_CODE).value_or(10000);
        effective_url = m_curl_handle->get_info<char*>(CURLINFO_EFFECTIVE_URL).value();
        downloaded_size = m_curl_handle->get_info<long>(CURLINFO_SIZE_DOWNLOAD_T).value_or(0);

        LOG_INFO << get_transfer_msg();

        if (can_retry())
        {
            // this request didn't work!

            // respect Retry-After header if present, otherwise use default timeout
            m_retry_wait_seconds = m_curl_handle->get_info<std::size_t>(CURLINFO_RETRY_AFTER)
                                       .value_or(0);
            if (!m_retry_wait_seconds)
            {
                m_retry_wait_seconds = get_default_retry_timeout();
            }

            m_next_retry = std::chrono::steady_clock::now()
                           + std::chrono::seconds(m_retry_wait_seconds);
            std::stringstream msg;
            msg << "Failed (" << http_status << "), retry in " << m_retry_wait_seconds << "s";
            if (m_has_progress_bar)
            {
                m_progress_bar.update_progress(0, static_cast<std::size_t>(downloaded_size));
                m_progress_bar.set_postfix(msg.str());
            }
            return false;
        }

        m_file.close();
        final_url = effective_url;

        if (m_has_progress_bar)
        {
            m_progress_bar.set_speed(static_cast<std::size_t>(avg_speed));
            m_progress_bar.set_total(static_cast<std::size_t>(downloaded_size));
            m_progress_bar.set_full();
            m_progress_bar.set_postfix("Downloaded");
        }

        bool ret = true;
        if (m_finalize_callback)
        {
            ret = m_finalize_callback(*this);
        }
        else
        {
            if (m_has_progress_bar)
            {
                m_progress_bar.mark_as_completed();
            }
            else
            {
                Console::instance().print(name() + " completed");
            }
        }

        if (m_has_progress_bar)
        {
            // make sure total value is up-to-date
            m_progress_bar.update_repr(false);
            // select field to display and make sure they are
            // properly set if not yet printed by the progress bar manager
            ProgressBarRepr r = m_progress_bar.repr();
            r.prefix.set_format("{:<50}", 50);
            r.progress.deactivate();
            r.current.deactivate();
            r.separator.deactivate();

            auto console_stream = Console::stream();
            r.print(console_stream, 0, false);
        }

        return ret;
    }

    std::string DownloadTarget::get_transfer_msg()
    {
        std::stringstream ss;
        ss << "Transfer finalized, status: " << http_status << " [" << effective_url << "] "
           << downloaded_size << " bytes";
        return ss.str();
    }

    /**************************************
     * MultiDownloadTarget implementation *
     **************************************/

    MultiDownloadTarget::MultiDownloadTarget()
    {
        m_handle = curl_multi_init();
        curl_multi_setopt(m_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, Context::instance().download_threads);
    }

    MultiDownloadTarget::~MultiDownloadTarget()
    {
        curl_multi_cleanup(m_handle);
    }

    void MultiDownloadTarget::add(DownloadTarget* target)
    {
        if (!target)
        {
            return;
        }
        CURLMcode code = curl_multi_add_handle(m_handle, target->handle());
        if (code != CURLM_CALL_MULTI_PERFORM)
        {
            if (code != CURLM_OK)
            {
                throw std::runtime_error(curl_multi_strerror(code));
            }
        }
        m_targets.push_back(target);
    }

    bool MultiDownloadTarget::check_msgs(bool failfast)
    {
        int msgs_in_queue;
        CURLMsg* msg;

        while ((msg = curl_multi_info_read(m_handle, &msgs_in_queue)) != nullptr)
        {
            // TODO maybe refactor so that `msg` is passed to current target?
            DownloadTarget* current_target = nullptr;
            for (const auto& target : m_targets)
            {
                if (target->handle() == msg->easy_handle)
                {
                    current_target = target;
                    break;
                }
            }

            if (!current_target)
            {
                throw std::runtime_error("Could not find target associated with multi request");
            }

            current_target->set_result(msg->data.result);
            if (msg->data.result != CURLE_OK && current_target->can_retry())
            {
                curl_multi_remove_handle(m_handle, current_target->handle());
                m_retry_targets.push_back(current_target);
                continue;
            }

            if (msg->msg == CURLMSG_DONE)
            {
                LOG_INFO << "Transfer done for '" << current_target->name() << "'";
                // We are only interested in messages about finished transfers
                curl_multi_remove_handle(m_handle, current_target->handle());

                // flush file & finalize transfer
                if (!current_target->finalize())
                {
                    // transfer did not work! can we retry?
                    if (current_target->can_retry())
                    {
                        LOG_INFO << "Setting retry for '" << current_target->name() << "'";
                        m_retry_targets.push_back(current_target);
                    }
                    else
                    {
                        if (failfast && current_target->ignore_failure() == false)
                        {
                            throw std::runtime_error(
                                "Multi-download failed. Reason: " + current_target->get_transfer_msg()
                            );
                        }
                    }
                }
            }
        }
        return true;
    }

    bool MultiDownloadTarget::download(int options)
    {
        bool failfast = options & MAMBA_DOWNLOAD_FAILFAST;
        bool sort = options & MAMBA_DOWNLOAD_SORT;
        bool no_clear_progress_bars = options & MAMBA_NO_CLEAR_PROGRESS_BARS;

        auto& ctx = Context::instance();

        if (m_targets.empty())
        {
            LOG_INFO << "All targets to download are cached";
            return true;
        }

        if (sort)
        {
            std::sort(
                m_targets.begin(),
                m_targets.end(),
                [](DownloadTarget* a, DownloadTarget* b) -> bool
                { return a->expected_size() > b->expected_size(); }
            );
        }

        LOG_INFO << "Starting to download targets";

        auto& pbar_manager = Console::instance().progress_bar_manager();
        interruption_guard g([]() { Console::instance().progress_bar_manager().terminate(); });

        // be sure the progress bar manager was not already started
        // it would mean this code is part of a larger process using progress bars
        bool pbar_manager_started = pbar_manager.started();
        if (!(ctx.internal_design_info.no_progress_bars || ctx.output_info.json
              || ctx.output_info.quiet || pbar_manager_started))
        {
            pbar_manager.watch_print();
        }

        int still_running, repeats = 0;
        const long max_wait_msecs = 1000;
        do
        {
            CURLMcode code = curl_multi_perform(m_handle, &still_running);

            if (code != CURLM_OK)
            {
                throw std::runtime_error(curl_multi_strerror(code));
            }
            check_msgs(failfast);

            if (!m_retry_targets.empty())
            {
                auto it = m_retry_targets.begin();
                while (it != m_retry_targets.end())
                {
                    CURL* curl_handle = (*it)->retry();
                    if (curl_handle != nullptr)
                    {
                        curl_multi_add_handle(m_handle, curl_handle);
                        it = m_retry_targets.erase(it);
                        still_running = 1;
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            long curl_timeout = -1;  // NOLINT(runtime/int)
            code = curl_multi_timeout(m_handle, &curl_timeout);
            if (code != CURLM_OK)
            {
                throw std::runtime_error(curl_multi_strerror(code));
            }

            if (curl_timeout == 0)
            {  // No wait
                continue;
            }

            if (curl_timeout < 0 || curl_timeout > max_wait_msecs)
            {  // Wait no more than 1s
                curl_timeout = max_wait_msecs;
            }

            int numfds;
            code = curl_multi_wait(m_handle, NULL, 0, static_cast<int>(curl_timeout), &numfds);
            if (code != CURLM_OK)
            {
                throw std::runtime_error(curl_multi_strerror(code));
            }

            if (!numfds)
            {
                repeats++;  // count number of repeated zero numfds
                if (repeats > 1)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            else
            {
                repeats = 0;
            }
        } while ((still_running || !m_retry_targets.empty()) && !is_sig_interrupted());

        if (is_sig_interrupted())
        {
            Console::instance().print("Download interrupted");
            return false;
        }

        if (!(ctx.internal_design_info.no_progress_bars || ctx.output_info.json
              || ctx.output_info.quiet || pbar_manager_started))
        {
            pbar_manager.terminate();
            if (!no_clear_progress_bars)
            {
                pbar_manager.clear_progress_bars();
            }
        }

        return true;
    }
}  // namespace mamba
