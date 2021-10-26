// Copyright (c) 2019, QuantStack and Mamba Contributors
//
// Distributed under the terms of the BSD 3-Clause License.
//
// The full license is in the file LICENSE, distributed with this software.

#include "mamba/version.hpp"
#include "mamba/core/fetch.hpp"
#include "mamba/core/context.hpp"
#include "mamba/core/thread_utils.hpp"
#include "mamba/core/util.hpp"
#include "mamba/core/url.hpp"

#include <string_view>
#include <thread>
#include <regex>


namespace mamba
{
    void init_curl_ssl()
    {
        auto& ctx = Context::instance();

        if (!ctx.curl_initialized)
        {
            if (ctx.ssl_verify == "<false>")
            {
                LOG_DEBUG << "'ssl_verify' not activated, skipping cURL SSL init";
                ctx.curl_initialized = true;
                return;
            }

#ifdef MICROMAMBA_STATIC_LINK
            CURLsslset sslset_res;
            const curl_ssl_backend** available_backends;

            if (on_linux)
            {
                sslset_res
                    = curl_global_sslset(CURLSSLBACKEND_OPENSSL, nullptr, &available_backends);
            }
            else if (on_mac)
            {
                sslset_res = curl_global_sslset(
                    CURLSSLBACKEND_SECURETRANSPORT, nullptr, &available_backends);
            }
            else if (on_win)
            {
                sslset_res
                    = curl_global_sslset(CURLSSLBACKEND_SCHANNEL, nullptr, &available_backends);
            }

            if (sslset_res == CURLSSLSET_TOO_LATE)
            {
                LOG_ERROR << "cURL SSL init called too late, that is a bug.";
            }
            else if (sslset_res == CURLSSLSET_UNKNOWN_BACKEND
                     || sslset_res == CURLSSLSET_NO_BACKENDS)
            {
                LOG_WARNING
                    << "Could not use preferred SSL backend (Linux: OpenSSL, OS X: SecureTransport, Win: SChannel)"
                    << std::endl;
                LOG_WARNING << "Please check the cURL library configuration that you are using."
                            << std::endl;
            }

            CURL* handle = curl_easy_init();
            if (handle)
            {
                const struct curl_tlssessioninfo* info = NULL;
                CURLcode res = curl_easy_getinfo(handle, CURLINFO_TLS_SSL_PTR, &info);
                if (info && !res)
                {
                    if (info->backend == CURLSSLBACKEND_OPENSSL)
                    {
                        LOG_INFO << "Using OpenSSL backend";
                    }
                    else if (info->backend == CURLSSLBACKEND_SECURETRANSPORT)
                    {
                        LOG_INFO << "Using macOS SecureTransport backend";
                    }
                    else if (info->backend == CURLSSLBACKEND_SCHANNEL)
                    {
                        LOG_INFO << "Using Windows Schannel backend";
                    }
                    else if (info->backend != CURLSSLBACKEND_NONE)
                    {
                        LOG_INFO << "Using an unknown (to mamba) SSL backend";
                    }
                    else if (info->backend == CURLSSLBACKEND_NONE)
                    {
                        LOG_WARNING
                            << "No SSL backend found! Please check how your cURL library is configured.";
                    }
                }
                curl_easy_cleanup(handle);
            }
#endif

            if (!ctx.ssl_verify.size() && std::getenv("REQUESTS_CA_BUNDLE") != nullptr)
            {
                ctx.ssl_verify = std::getenv("REQUESTS_CA_BUNDLE");
                LOG_INFO << "Using REQUESTS_CA_BUNDLE " << ctx.ssl_verify;
            }
            else if (ctx.ssl_verify == "<system>" && on_linux)
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
                        ctx.ssl_verify = loc;
                        found = true;
                    }
                }

                if (!found)
                {
                    LOG_ERROR << "No CA certificates found on system";
                    throw std::runtime_error("Aborting.");
                }
            }

            ctx.curl_initialized = true;
        }
    }

    /*********************************
     * DownloadTarget implementation *
     *********************************/

    DownloadTarget::DownloadTarget(const std::string& name,
                                   const std::string& url,
                                   const std::string& filename)
        : m_name(name)
        , m_filename(filename)
        , m_url(unc_url(url))
    {
        m_handle = curl_easy_init();

        init_curl_ssl();
        init_curl_target(m_url);
    }

    void DownloadTarget::init_curl_handle(CURL* handle, const std::string& url)
    {
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);

        // DO NOT SET TIMEOUT as it will also take into account multi-start time and
        // it's just wrong curl_easy_setopt(m_handle, CURLOPT_TIMEOUT,
        // Context::instance().read_timeout_secs);

        // TODO while libcurl in conda now _has_ http2 support we need to fix mamba to
        // work properly with it this includes:
        // - setting the cache stuff correctly
        // - fixing how the progress bar works
        curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

        // if the request is slower than 30b/s for 60 seconds, cancel.
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 30L);

        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, Context::instance().connect_timeout_secs);

        std::string ssl_no_revoke_env
            = std::getenv("MAMBA_SSL_NO_REVOKE") ? std::getenv("MAMBA_SSL_NO_REVOKE") : "0";
        if (Context::instance().ssl_no_revoke || ssl_no_revoke_env != "0")
        {
            curl_easy_setopt(handle, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NO_REVOKE);
        }

        std::string& ssl_verify = Context::instance().ssl_verify;
        if (ssl_verify.size())
        {
            if (ssl_verify == "<false>")
            {
                curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
            }
            else if (ssl_verify == "<system>")
            {
#ifdef MICROMAMBA_STATIC_LINK
                curl_easy_setopt(handle, CURLOPT_CAINFO, nullptr);
#endif
            }
            else
            {
                if (!fs::exists(ssl_verify))
                {
                    throw std::runtime_error("ssl_verify does not contain a valid file path.");
                }
                else
                {
                    curl_easy_setopt(handle, CURLOPT_CAINFO, ssl_verify.c_str());
                }
            }
        }
    }

    void DownloadTarget::init_curl_target(const std::string& url)
    {
        init_curl_handle(m_handle, url);

        curl_easy_setopt(m_handle, CURLOPT_ERRORBUFFER, m_errbuf);

        curl_easy_setopt(m_handle, CURLOPT_HEADERFUNCTION, &DownloadTarget::header_callback);
        curl_easy_setopt(m_handle, CURLOPT_HEADERDATA, this);

        curl_easy_setopt(m_handle, CURLOPT_WRITEFUNCTION, &DownloadTarget::write_callback);
        curl_easy_setopt(m_handle, CURLOPT_WRITEDATA, this);

        m_headers = nullptr;
        if (ends_with(url, ".json"))
        {
            curl_easy_setopt(
                m_handle, CURLOPT_ACCEPT_ENCODING, "gzip, deflate, compress, identity");
            m_headers = curl_slist_append(m_headers, "Content-Type: application/json");
        }

        static std::string user_agent
            = std::string("User-Agent: mamba/" LIBMAMBA_VERSION_STRING " ") + curl_version();

        m_headers = curl_slist_append(m_headers, user_agent.c_str());
        curl_easy_setopt(m_handle, CURLOPT_HTTPHEADER, m_headers);
        curl_easy_setopt(m_handle, CURLOPT_VERBOSE, Context::instance().verbosity >= 2);
    }

    bool DownloadTarget::can_retry()
    {
        return m_retries < size_t(Context::instance().max_retries) && http_status >= 500
               && !starts_with(m_url, "file://");
    }

    CURL* DownloadTarget::retry()
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= m_next_retry)
        {
            if (fs::exists(m_filename))
            {
                fs::remove(m_filename);
            }
            init_curl_target(m_url);
            if (m_has_progress_bar)
            {
                curl_easy_setopt(
                    m_handle, CURLOPT_XFERINFOFUNCTION, &DownloadTarget::progress_callback);
                curl_easy_setopt(m_handle, CURLOPT_XFERINFODATA, this);
            }
            m_retry_wait_seconds = m_retry_wait_seconds * Context::instance().retry_backoff;
            m_next_retry = now + std::chrono::seconds(m_retry_wait_seconds);
            m_retries++;
            return m_handle;
        }
        else
        {
            return nullptr;
        }
    }

    DownloadTarget::~DownloadTarget()
    {
        curl_easy_cleanup(m_handle);
        curl_slist_free_all(m_headers);
    }

    size_t DownloadTarget::write_callback(char* ptr, size_t size, size_t nmemb, void* self)
    {
        auto* s = reinterpret_cast<DownloadTarget*>(self);
        if (!s->m_file.is_open())
        {
            s->m_file = std::ofstream(s->m_filename, std::ios::binary);
            if (!s->m_file)
            {
                LOG_ERROR << "Could not open file for download " << s->m_filename << ": "
                          << strerror(errno);
                exit(1);
            }
        }

        s->m_file.write(ptr, size * nmemb);

        if (!s->m_file)
        {
            LOG_ERROR << "Could not write to file " << s->m_filename << ": " << strerror(errno);
            exit(1);
        }
        return size * nmemb;
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
            value = header.substr(colon_idx, header.size() - colon_idx - 2);
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

    int DownloadTarget::progress_callback(
        void*, curl_off_t total_to_download, curl_off_t now_downloaded, curl_off_t, curl_off_t)
    {
        if (Context::instance().quiet || Context::instance().json)
        {
            return 0;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - m_progress_throttle_time < std::chrono::milliseconds(150))
        {
            return 0;
        }
        m_progress_throttle_time = now;

        if (total_to_download != 0 && now_downloaded == 0 && m_expected_size != 0)
        {
            now_downloaded = total_to_download;
            total_to_download = m_expected_size;
        }

        if ((total_to_download != 0 || m_expected_size != 0) && now_downloaded != 0)
        {
            std::stringstream postfix;
            postfix << std::setw(6);
            to_human_readable_filesize(postfix, now_downloaded);
            postfix << " / ";
            postfix << std::setw(6);
            to_human_readable_filesize(postfix, total_to_download);
            postfix << " (";
            postfix << std::setw(6);
            to_human_readable_filesize(postfix, get_speed(), 2);
            postfix << "/s)";
            m_progress_bar.set_progress(now_downloaded, total_to_download);
            m_progress_bar.set_postfix(postfix.str());
        }
        if (now_downloaded == 0 && total_to_download != 0)
        {
            std::stringstream postfix;
            to_human_readable_filesize(postfix, total_to_download);
            postfix << " / ?? (";
            to_human_readable_filesize(postfix, get_speed(), 2);
            postfix << "/s)";
            m_progress_bar.set_progress(SIZE_MAX, SIZE_MAX);
            m_progress_bar.set_postfix(postfix.str());
        }
        return 0;
    }

    void DownloadTarget::set_mod_etag_headers(const nlohmann::json& mod_etag)
    {
        auto to_header = [](const std::string& key, const std::string& value) {
            return std::string(key + ": " + value);
        };

        if (mod_etag.find("_etag") != mod_etag.end())
        {
            m_headers = curl_slist_append(m_headers,
                                          to_header("If-None-Match", mod_etag["_etag"]).c_str());
        }
        if (mod_etag.find("_mod") != mod_etag.end())
        {
            m_headers = curl_slist_append(m_headers,
                                          to_header("If-Modified-Since", mod_etag["_mod"]).c_str());
        }
    }

    void DownloadTarget::set_progress_bar(ProgressProxy progress_proxy)
    {
        m_has_progress_bar = true;
        m_progress_bar = progress_proxy;
        curl_easy_setopt(m_handle, CURLOPT_XFERINFOFUNCTION, &DownloadTarget::progress_callback);
        curl_easy_setopt(m_handle, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(m_handle, CURLOPT_NOPROGRESS, 0L);
    }

    void DownloadTarget::set_expected_size(std::size_t size)
    {
        m_expected_size = size;
    }

    const std::string& DownloadTarget::name() const
    {
        return m_name;
    }

    static size_t discard(char* ptr, size_t size, size_t nmemb, void*)
    {
        return size * nmemb;
    }


    bool DownloadTarget::resource_exists()
    {
        auto handle = curl_easy_init();

        init_curl_ssl();
        init_curl_handle(handle, m_url);

        curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
        if (curl_easy_perform(handle) == CURLE_OK)
            return true;

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
            return false;
    }

    bool DownloadTarget::perform()
    {
        LOG_INFO << "Downloading to filename: " << m_filename;

        result = curl_easy_perform(m_handle);
        set_result(result);
        return m_finalize_callback ? m_finalize_callback() : true;
    }

    CURL* DownloadTarget::handle()
    {
        return m_handle;
    }

    curl_off_t DownloadTarget::get_speed()
    {
        curl_off_t speed;
        CURLcode res = curl_easy_getinfo(m_handle, CURLINFO_SPEED_DOWNLOAD_T, &speed);
        return res == CURLE_OK ? speed : 0;
    }

    void DownloadTarget::set_result(CURLcode r)
    {
        result = r;
        if (r != CURLE_OK)
        {
            char* effective_url = nullptr;
            curl_easy_getinfo(m_handle, CURLINFO_EFFECTIVE_URL, &effective_url);

            std::stringstream err;
            err << "Download error (" << result << ") " << curl_easy_strerror(result) << " ["
                << effective_url << "]\n";
            if (m_errbuf[0] != '\0')
            {
                err << m_errbuf;
            }
            LOG_INFO << err.str();

            m_next_retry
                = std::chrono::steady_clock::now() + std::chrono::seconds(m_retry_wait_seconds);

            if (m_has_progress_bar)
            {
                m_progress_bar.set_progress(0, 1);
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
        char* effective_url = nullptr;

        auto cres = curl_easy_getinfo(m_handle, CURLINFO_SPEED_DOWNLOAD_T, &avg_speed);
        if (cres != CURLE_OK)
        {
            avg_speed = 0;
        }

        curl_easy_getinfo(m_handle, CURLINFO_RESPONSE_CODE, &http_status);
        curl_easy_getinfo(m_handle, CURLINFO_EFFECTIVE_URL, &effective_url);
        curl_easy_getinfo(m_handle, CURLINFO_SIZE_DOWNLOAD_T, &downloaded_size);

        LOG_INFO << "Transfer finalized, status: " << http_status << " [" << effective_url << "] "
                 << downloaded_size << " bytes";

        if (http_status >= 500 && can_retry())
        {
            // this request didn't work!
            m_next_retry
                = std::chrono::steady_clock::now() + std::chrono::seconds(m_retry_wait_seconds);
            std::stringstream msg;
            msg << "Failed (" << http_status << "), retry in " << m_retry_wait_seconds << "s";
            m_progress_bar.set_progress(0, downloaded_size);
            m_progress_bar.set_postfix(msg.str());
            return false;
        }

        m_file.close();

        final_url = effective_url;
        if (m_finalize_callback)
        {
            return m_finalize_callback();
        }
        else
        {
            if (m_has_progress_bar)
            {
                m_progress_bar.mark_as_completed("Downloaded " + m_name);
            }
        }
        return true;
    }

    /**************************************
     * MultiDownloadTarget implementation *
     **************************************/

    MultiDownloadTarget::MultiDownloadTarget()
    {
        m_handle = curl_multi_init();
        curl_multi_setopt(
            m_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, Context::instance().max_parallel_downloads);
    }

    MultiDownloadTarget::~MultiDownloadTarget()
    {
        curl_multi_cleanup(m_handle);
    }

    void MultiDownloadTarget::add(DownloadTarget* target)
    {
        if (!target)
            return;
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

        while ((msg = curl_multi_info_read(m_handle, &msgs_in_queue)))
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
            if (msg->data.result != CURLE_OK)
            {
                if (current_target->can_retry())
                {
                    curl_multi_remove_handle(m_handle, current_target->handle());
                    m_retry_targets.push_back(current_target);
                    continue;
                }
            }

            if (msg->msg == CURLMSG_DONE)
            {
                LOG_INFO << "Transfer done ...";
                // We are only interested in messages about finished transfers
                curl_multi_remove_handle(m_handle, current_target->handle());

                // flush file & finalize transfer
                if (!current_target->finalize())
                {
                    // transfer did not work! can we retry?
                    if (current_target->can_retry())
                    {
                        LOG_INFO << "Adding target to retry!";
                        m_retry_targets.push_back(current_target);
                    }
                    else
                    {
                        if (failfast && current_target->ignore_failure() == false)
                        {
                            throw std::runtime_error("Multi-download failed.");
                        }
                    }
                }
            }
        }
        return true;
    }

    bool MultiDownloadTarget::download(bool failfast)
    {
        LOG_INFO << "Starting to download targets";

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

            if (curl_timeout == 0)  // No wait
                continue;

            if (curl_timeout < 0 || curl_timeout > max_wait_msecs)  // Wait no more than 1s
                curl_timeout = max_wait_msecs;

            int numfds;
            code = curl_multi_wait(m_handle, NULL, 0, curl_timeout, &numfds);
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
            Console::print("Download interrupted");
            curl_multi_cleanup(m_handle);
            return false;
        }
        return true;
    }
}  // namespace mamba
