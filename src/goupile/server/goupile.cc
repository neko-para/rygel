// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "../../core/libcc/libcc.hh"
#include "admin.hh"
#include "domain.hh"
#include "files.hh"
#include "goupile.hh"
#include "instance.hh"
#include "js.hh"
#include "records.hh"
#include "user.hh"
#include "../../web/libhttp/libhttp.hh"
#include "../../../vendor/libsodium/src/libsodium/include/sodium.h"
#ifndef _WIN32
    #include <sys/time.h>
    #include <sys/resource.h>
#endif
#ifdef __GLIBC__
    #include <malloc.h>
#endif

namespace RG {

DomainHolder goupile_domain;

static void HandleEvents(InstanceHolder *, const http_RequestInfo &request, http_IO *io)
{
    // Do this to renew session and clear invalid session cookies
    GetCheckedSession(request, io);

    io->AttachText(200, "{}", "application/json");
}

static void AttachAsset(const http_RequestInfo &request, const AssetInfo &asset, const char *etag, http_IO *io)
{
    const char *client_etag = request.GetHeaderValue("If-None-Match");

    if (client_etag && TestStr(client_etag, etag)) {
        MHD_Response *response = MHD_create_response_from_buffer(0, nullptr, MHD_RESPMEM_PERSISTENT);
        io->AttachResponse(304, response);
    } else {
        const char *mimetype = http_GetMimeType(GetPathExtension(asset.name));
        io->AttachBinary(200, asset.data, mimetype, asset.compression_type);

        io->AddCachingHeaders(goupile_domain.config.max_age, etag);
        if (asset.source_map) {
            io->AddHeader("SourceMap", asset.source_map);
        }
    }
}

static void HandleRequest(const http_RequestInfo &request, http_IO *io)
{
 #ifndef NDEBUG
    {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);

        if (ReloadAssets()) {
            LogInfo("Reload assets");

            InitAdminAssets();
            goupile_domain.InitAssets();
        }
    }
#endif

    // Send these headers whenever possible
    io->AddHeader("Referrer-Policy", "no-referrer");

    // Separate base URL and path
    Span<const char> instance_key;
    const char *instance_path;
    {
        Size offset = SplitStr(request.url + 1, '/').len + 1;

        if (request.url[offset] != '/') {
            if (offset == 1) {
                io->AttachError(404);
            } else {
                const char *redirect = Fmt(&io->allocator, "%1/", request.url).ptr;
                io->AddHeader("Location", redirect);
                io->AttachNothing(301);
            }
            return;
        }

        instance_key = MakeSpan(request.url + 1, offset - 1);
        instance_path = request.url + offset;
    }

    // If new base URLs are added besides "/admin", RunCreateInstance() must be modified
    // to forbid the instance key.
    if (instance_key == "admin") {
        // Try static assets
        {
            const AssetInfo *asset = admin_assets_map.FindValue(instance_path, nullptr);

            if (asset) {
                AttachAsset(request, *asset, admin_etag, io);
                return;
            }
        }

        // And now, API endpoints
        if (TestStr(instance_path, "/api/events") && request.method == http_RequestMethod::Get) {
            HandleEvents(nullptr, request, io);
        } else if (TestStr(instance_path, "/api/user/profile") && request.method == http_RequestMethod::Get) {
            HandleUserProfile(nullptr, request, io);
        } else if (TestStr(instance_path, "/api/user/login") && request.method == http_RequestMethod::Post) {
            HandleUserLogin(nullptr, request, io);
        } else if (TestStr(instance_path, "/api/user/logout") && request.method == http_RequestMethod::Post) {
            HandleUserLogout(nullptr, request, io);
        } else if (TestStr(instance_path, "/api/instances/create") && request.method == http_RequestMethod::Post) {
            HandleCreateInstance(request, io);
        } else if (TestStr(instance_path, "/api/instances/delete") && request.method == http_RequestMethod::Post) {
            HandleDeleteInstance(request, io);
        } else if (TestStr(instance_path, "/api/instances/configure") && request.method == http_RequestMethod::Post) {
            HandleConfigureInstance(request, io);
        } else if (TestStr(instance_path, "/api/instances/list") && request.method == http_RequestMethod::Get) {
            HandleListInstances(request, io);
        } else if (TestStr(instance_path, "/api/users/create") && request.method == http_RequestMethod::Post) {
            HandleCreateUser(request, io);
        } else if (TestStr(instance_path, "/api/users/delete") && request.method == http_RequestMethod::Post) {
            HandleDeleteUser(request, io);
        } else if (TestStr(instance_path, "/api/users/assign") && request.method == http_RequestMethod::Post) {
            HandleAssignUser(request, io);
        } else if (TestStr(instance_path, "/api/users/list") && request.method == http_RequestMethod::Get) {
            HandleListUsers(request, io);
        } else {
            io->AttachError(404);
        }
    } else {
        bool reload;
        InstanceHolder *instance = goupile_domain.Ref(instance_key, &reload);
        if (!instance) {
            io->AttachError(reload ? 503 : 404);
            return;
        }
        io->AddFinalizer([=]() { instance->Unref(); });

        // Try application files
        if (request.method == http_RequestMethod::Get && HandleFileGet(instance, request, io))
            return;

        // Try static assets
        if (request.method == http_RequestMethod::Get) {
            const AssetInfo *asset;
            if (TestStr(instance_path, "/") || StartsWith(instance_path, "/app/")) {
                asset = instance->assets_map.FindValue("/index.html", nullptr);
                RG_ASSERT(asset);
            } else {
                asset = instance->assets_map.FindValue(instance_path, nullptr);
            }

            if (asset) {
                AttachAsset(request, *asset, instance->etag, io);
                return;
            }
        }

        // And now, API endpoints
        if (TestStr(instance_path, "/api/events") && request.method == http_RequestMethod::Get) {
            HandleEvents(instance, request, io);
        } else if (TestStr(instance_path, "/api/user/profile") && request.method == http_RequestMethod::Get) {
            HandleUserProfile(instance, request, io);
        } else if (TestStr(instance_path, "/api/user/login") && request.method == http_RequestMethod::Post) {
            HandleUserLogin(instance, request, io);
        } else if (TestStr(instance_path, "/api/user/logout") && request.method == http_RequestMethod::Post) {
            HandleUserLogout(instance, request, io);
        } else if (TestStr(instance_path, "/api/files/list") && request.method == http_RequestMethod::Get) {
             HandleFileList(instance, request, io);
        } else if (TestStr(instance_path, "/api/files/static") && request.method == http_RequestMethod::Get) {
            HandleFileStatic(instance, request, io);
        } else if (StartsWith(instance_path, "/files/") && request.method == http_RequestMethod::Put) {
            HandleFilePut(instance, request, io);
        } else if (StartsWith(instance_path, "/files/") && request.method == http_RequestMethod::Delete) {
            HandleFileDelete(instance, request, io);
        } else if (TestStr(instance_path, "/api/records/load") && request.method == http_RequestMethod::Get) {
            HandleRecordLoad(instance, request, io);
        } else if (TestStr(instance_path, "/api/records/columns") && request.method == http_RequestMethod::Get) {
            HandleRecordColumns(instance, request, io);
        } else if (TestStr(instance_path, "/api/records/sync") && request.method == http_RequestMethod::Post) {
            HandleRecordSync(instance, request, io);
        } else if (TestStr(instance_path, "/api/records/recompute") && request.method == http_RequestMethod::Post) {
            HandleRecordRecompute(instance, request, io);
        } else {
            io->AttachError(404);
        }
    }
}

static int RunServe(Span<const char *> arguments)
{
    const char *config_filename = "goupile.ini";

    const auto print_usage = [&](FILE *fp) {
        PrintLn(fp, R"(Usage: %!..+%1 serve [options]%!0

Options:
    %!..+-C, --config_file <file>%!0     Set configuration file
                                 %!D..(default: %2)%!0

        %!..+--port <port>%!0            Change web server port
                                 %!D..(default: %3)%!0

Other commands:
    %!..+init%!0                         Create new domain
    %!..+migrate%!0                      Migrate existing domain

For help about those commands, type: %!..+%1 <command> --help%!0)",
                FelixTarget, config_filename, goupile_domain.config.http.port);
    };

    // Find config filename
    {
        OptionParser opt(arguments, (int)OptionParser::Flag::SkipNonOptions);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-C", "--config_file", OptionType::Value)) {
                config_filename = opt.current_value;
            }
        }
    }

#ifndef _WIN32
    {
        const rlim_t max_nofile = 16384;
        struct rlimit lim;

        // Increase maximum number of open file descriptors
        if (getrlimit(RLIMIT_NOFILE, &lim) >= 0) {
            if (lim.rlim_cur < max_nofile) {
                lim.rlim_cur = std::min(max_nofile, lim.rlim_max);

                if (setrlimit(RLIMIT_NOFILE, &lim) >= 0) {
                    if (lim.rlim_cur < max_nofile) {
                        LogError("Maximum number of open descriptors is low: %1 (recommended: %2)", lim.rlim_cur, max_nofile);
                    }
                } else {
                    LogError("Could not raise RLIMIT_NOFILE to %1: %2", max_nofile, strerror(errno));
                }
            }
        } else {
            LogError("getrlimit(RLIMIT_NOFILE) failed: %1", strerror(errno));
        }
    }
#endif

    LogInfo("Init instances");
    InitAdminAssets();
    if (!goupile_domain.Open(config_filename))
        return 1;

    LogInfo("Init JS");
    InitJS();

    // Parse arguments
    {
        OptionParser opt(arguments);

        while (opt.Next()) {
            if (opt.Test("-C", "--config_file", OptionType::Value)) {
                // Already handled
            } else if (opt.Test("--port", OptionType::Value)) {
                if (!ParseInt(opt.current_value, &goupile_domain.config.http.port))
                    return 1;
            } else {
                LogError("Cannot handle option '%1'", opt.current_option);
                return 1;
            }
        }

        // We may have changed some stuff (such as HTTP port), so revalidate
        if (!goupile_domain.config.Validate())
            return 1;
    }

    // Run!
    http_Daemon daemon;
    if (!daemon.Start(goupile_domain.config.http, HandleRequest))
        return 1;
#ifndef _WIN32
    if (goupile_domain.config.http.sock_type == SocketType::Unix) {
        LogInfo("Listening on socket '%1' (Unix stack)", goupile_domain.config.http.unix_path);
    } else
#endif
    LogInfo("Listening on port %1 (%2 stack)",
            goupile_domain.config.http.port, SocketTypeNames[(int)goupile_domain.config.http.sock_type]);

    for (;;) {
        int timeout = goupile_domain.IsSynced() ? -1 : 30000;

        // Respond to SIGUSR1
        if (WaitForInterrupt(timeout) == WaitForResult::Interrupt)
            break;

        goupile_domain.Sync();

#ifdef __GLIBC__
        // Actually release memory to the OS, because for some reason glibc doesn't want to
        // do this automatically even after 98% of the resident memory pool has been freed.
        malloc_trim(0);
#endif
    }

    daemon.Stop();
    LogInfo("Exit");

    return 0;
}

int Main(int argc, char **argv)
{
    // Handle help and version arguments
    if (argc >= 2) {
        if (TestStr(argv[1], "--help") || TestStr(argv[1], "help")) {
            if (argc >= 3 && argv[2][0] != '-') {
                argv[1] = argv[2];
                argv[2] = const_cast<char *>("--help");
            } else {
                const char *args[] = {"--help"};
                return RunServe(args);
            }
        } else if (TestStr(argv[1], "--version")) {
            PrintLn("%!R..%1%!0 %2", FelixTarget, FelixVersion);
            return 0;
        }
    }

    int (*cmd_func)(Span<const char *> arguments);
    Span<const char *> arguments;
    if (argc >= 2) {
        const char *cmd = argv[1];

        if (TestStr(cmd, "init")) {
            cmd_func = RunInit;
            arguments = MakeSpan((const char **)argv + 2, argc - 2);
        } else if (TestStr(cmd, "migrate")) {
            cmd_func = RunMigrate;
            arguments = MakeSpan((const char **)argv + 2, argc - 2);
        } else if (TestStr(cmd, "serve")) {
            cmd_func = RunServe;
            arguments = MakeSpan((const char **)argv + 2, argc - 2);
        } else if (cmd[0] == '-') {
            cmd_func = RunServe;
            arguments = MakeSpan((const char **)argv + 1, argc - 1);
        } else {
            LogError("Unknown command '%1'", cmd);
            return 1;
        }
    } else {
        cmd_func = RunServe;
        arguments = {};
    }

    return cmd_func(arguments);
}

}

// C++ namespaces are stupid
int main(int argc, char **argv) { return RG::Main(argc, argv); }
