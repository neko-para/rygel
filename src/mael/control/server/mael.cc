// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#include "../../../core/libcc/libcc.hh"
#include "config.hh"
#include "../../../core/libnet/libnet.hh"
#include "../../teensy/protocol.hh"
#include "../../../../vendor/libhs/libhs.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../../../../vendor/miniz/miniz.h"
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
#endif
#include <thread>

namespace RG {

static Config mael_config;

static hs_monitor *monitor = nullptr;
static std::thread monitor_thread;
#ifdef _WIN32
static HANDLE monitor_event;
#else
static int monitor_pfd[2] = {-1, -1};
#endif

static std::mutex comm_mutex;
static hs_device *comm_dev = nullptr;
static hs_port *comm_port = nullptr;

bool recv_started = false;
alignas(uint64_t) static LocalArray<uint8_t, 65536> recv_buf;

struct Client {
    Client *prev;
    Client *next;

    StreamReader reader;
    StreamWriter writer;
};

static std::mutex clients_mutex;
static Client clients_root = {&clients_root, &clients_root};

static const hs_match_spec DeviceSpecs[] = {
    HS_MATCH_VID_PID(0x16C0, 0x0483, nullptr)
};

static int DeviceCallback(hs_device *dev, void *)
{
    std::lock_guard<std::mutex> lock(comm_mutex);

    switch (dev->status) {
        case HS_DEVICE_STATUS_ONLINE: {
            if (comm_dev) {
                LogError("Ignoring supplementary device '%1'", dev->location);
                return 0;
            }

            LogInfo("Acquired control device '%1'", dev->location);
            comm_dev = hs_device_ref(dev);
        } break;

        case HS_DEVICE_STATUS_DISCONNECTED: {
            if (dev == comm_dev) {
                LogInfo("Lost control device '%1'", dev->location);

                hs_device_unref(comm_dev);
                hs_port_close(comm_port);
                comm_dev = nullptr;
                comm_port = nullptr;
            }
        } break;
    }

    return 0;
}

// Returns true until there is nothing to do
static bool ReceivePacket()
{
    // Find packet start
    if (!recv_started) {
        Size i = 0;

        while (i < recv_buf.len) {
            if (recv_buf[i++] == 0xA) {
                recv_started = true;
                break;
            }
        }

        recv_buf.len -= i;
        memmove(recv_buf.data, recv_buf.data + i, recv_buf.len);

        if (!recv_started)
            return false;
    }

    // Rewrite packet (escaped bytes)
    Span<uint8_t> pkt;
    Size end;
    {
        Size i = 0;
        Size len = 0;

        for (; i < recv_buf.len; i++, len++) {
            recv_buf[len] = recv_buf[i];

            if (recv_buf[i] == 0xA) {
                recv_started = false;
                break;
            } else if (recv_buf[i] == 0xD) {
                if (i >= recv_buf.len - 1)
                    break;

                recv_buf[len] = recv_buf[++i] ^ 0x8;
            }
        }
        if (recv_started)
            return false;

        pkt = MakeSpan(recv_buf.data, len);
        end = i + !!len;
    }

    // Clear first packet after processing
    RG_DEFER {
        recv_buf.len -= end;
        memmove(recv_buf.data, recv_buf.data + end, recv_buf.len);
    };
    if (pkt.len < RG_SIZE(PacketHeader)) {
        LogError("Truncated packet");
        return true;
    }

    const PacketHeader &hdr = *(const PacketHeader *)pkt.ptr;

    // Check integrity
    if (hdr.payload > pkt.len - RG_SIZE(PacketHeader)) {
        LogError("Invalid payload length");
        return true;
    }
    if (hdr.type >= RG_LEN(PacketSizes)) {
        LogError("Invalid packet type");
        return true;
    }
    if (hdr.payload != PacketSizes[hdr.type]) {
        LogError("Mis-sized packet payload");
        return true;
    }
    if (hdr.crc32 != mz_crc32(MZ_CRC32_INIT, pkt.ptr + 4, hdr.payload + 4)) {
        LogError("Packet failed CRC32 check");
        return true;
    }

    // Dispatch to all clients
    {
        std::lock_guard<std::mutex> lock(clients_mutex);

        for (Client *client = clients_root.next; client != &clients_root; client = client->next) {
            client->writer.Write(pkt);
        }
    }

    return true;
}

static void RunMonitorThread()
{
    LocalArray<hs_poll_source, 3> sources = {
#ifdef _WIN32
        {monitor_event},
#else
        {monitor_pfd[0]},
#endif
        {hs_monitor_get_poll_handle(monitor)}
    };

    do {
        // Try to open device
        if (!comm_port) {
            std::lock_guard<std::mutex> lock(comm_mutex);

            hs_device *dev = comm_dev ? hs_device_ref(comm_dev) : nullptr;
            RG_DEFER { hs_device_unref(dev); };

            if (dev) {
                hs_port_open(dev, HS_PORT_MODE_RW, &comm_port);
            }
        }

        // Poll the controller if it is plugged
        sources.len = 2;
        if (comm_port) {
            hs_handle h = hs_port_get_poll_handle(comm_port);
            sources.Append({h});
        }

        // Wait for something to happen
        if (hs_poll(sources.data, sources.len, -1) < 0) {
            SignalWaitFor();
            return;
        }

        // Refresh known devices
        if (sources[1].ready && hs_monitor_refresh(monitor, DeviceCallback, nullptr) < 0) {
            SignalWaitFor();
            return;
        }

        if (comm_port && sources[2].ready) {
            Span<uint8_t> buf = recv_buf.TakeAvailable();
            buf.len = hs_serial_read(comm_port, buf.ptr, buf.len, 0);

            if (buf.len >= 0) {
                recv_buf.len += buf.len;
                while (ReceivePacket());
            } else {
                recv_buf.Clear();

                hs_port_close(comm_port);
                comm_port = nullptr;
            }
        }
    } while (!sources[0].ready);
}

static void StopMonitor();
static bool InitMonitor()
{
    RG_DEFER_N(err_guard) { StopMonitor(); };

#ifdef _WIN32
    monitor_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!monitor_event) {
        LogError("CreateEvent() failed: %1", GetWin32ErrorString());
        return false;
    }
#else
    if (!CreatePipe(monitor_pfd))
        return false;
#endif

    if (hs_monitor_new(DeviceSpecs, RG_LEN(DeviceSpecs), &monitor) < 0)
        return false;
    if (hs_monitor_start(monitor) < 0)
        return false;

    if (hs_monitor_list(monitor, DeviceCallback, nullptr) < 0)
        return false;
    monitor_thread = std::thread(RunMonitorThread);

    err_guard.Disable();
    return true;
}

static void StopMonitor()
{
    if (monitor) {
#ifdef _WIN32
        SetEvent(monitor_event);
#else
        char dummy = 0;
        write(monitor_pfd[1], &dummy, 1);
#endif

        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }
        monitor_thread = {};

        hs_monitor_stop(monitor);
    }

    hs_monitor_free(monitor);
    monitor = nullptr;

#ifdef _WIN32
    if (monitor_event) {
        CloseHandle(monitor_event);
        monitor_event = nullptr;
    }
#else
    close(monitor_pfd[0]);
    close(monitor_pfd[1]);
    monitor_pfd[0] = -1;
    monitor_pfd[1] = -1;
#endif

    hs_port_close(comm_port);
    hs_device_unref(comm_dev);
    comm_dev = nullptr;
    comm_port = nullptr;
}

static void HandleWebSocket(const http_RequestInfo &request, http_IO *io)
{
    io->RunAsync([=]() {
        Client client;

        // Upgrade connection
        if (!io->UpgradeToWS(0))
            return;
        io->OpenForReadWS(&client.reader);
        io->OpenForWriteWS(&client.writer);

        // Register client
        {
            std::lock_guard<std::mutex> lock(clients_mutex);

            client.prev = clients_root.prev;
            client.prev->next = &client;
            clients_root.prev = &client;
            client.next = &clients_root;
        }
        RG_DEFER {
            std::lock_guard<std::mutex> lock(clients_mutex);

            client.next->prev = client.prev;
            client.prev->next = client.next;
        };

        // Read in blocking mode
        while (!client.reader.IsEOF()) {
            LocalArray<uint8_t, 1024> buf;
            buf.len = client.reader.Read(buf.data);
            if (buf.len < 0)
                break;

            if (buf.len) {
                Span<const char> text = MakeSpan((const char *)buf.data, buf.len);
                LogDebug("Received: '%1'", text);
            }
        }
    });
}

static void HandleRequest(const http_RequestInfo &request, http_IO *io)
{
    if (mael_config.require_host) {
        const char *host = request.GetHeaderValue("Host");

        if (!host) {
            LogError("Request is missing required Host header");
            io->AttachError(400);
            return;
        }
        if (!TestStr(host, mael_config.require_host)) {
            LogError("Unexpected Host header '%1'", host);
            io->AttachError(403);
            return;
        }
    }

    // Send these headers whenever possible
    io->AddHeader("Referrer-Policy", "no-referrer");
    io->AddHeader("Cross-Origin-Opener-Policy", "same-origin");
    io->AddHeader("X-Robots-Tag", "noindex");
    io->AddHeader("Permissions-Policy", "interest-cohort=()");

    // Serve page
    if (TestStr(request.url, "/")) {
        const char *text = Fmt(&io->allocator, "Mael %1", FelixVersion).ptr;
        io->AttachText(200, text);
    } else if (TestStr(request.url, "/api/ws")) {
        HandleWebSocket(request, io);
    } else {
        io->AttachError(404);
    }
}

int Main(int argc, char **argv)
{
    // Options
    const char *config_filename = nullptr;

    const auto print_usage = [=](FILE *fp) {
        PrintLn(fp, R"(Usage: %!..+%1 [options]%!0

Options:
    %!..+-C, --config_file <file>%!0     Set configuration file

        %!..+--port <port>%!0            Change web server port
                                 %!D..(default: %2)%!0)",
                FelixTarget, mael_config.http.port);
    };

    // Handle version
    if (argc >= 2 && TestStr(argv[1], "--version")) {
        PrintLn("%!R..%1%!0 %!..+%2%!0", FelixTarget, FelixVersion);
        PrintLn("Compiler: %1", FelixCompiler);
        return 0;
    }

    // Find config filename
    {
        OptionParser opt(argc, argv, (int)OptionParser::Flag::SkipNonOptions);

        while (opt.Next()) {
            if (opt.Test("--help")) {
                print_usage(stdout);
                return 0;
            } else if (opt.Test("-C", "--config_file", OptionType::Value)) {
                config_filename = opt.current_value;
            } else if (opt.TestHasFailed()) {
                return 1;
            }
        }
    }

    // Load config file
    if (config_filename && !LoadConfig(config_filename, &mael_config))
        return 1;

    // Parse arguments
    {
        OptionParser opt(argc, argv);

        while (opt.Next()) {
            if (opt.Test("-C", "--config_file", OptionType::Value)) {
                // Already handled
            } else if (opt.Test("--port", OptionType::Value)) {
                if (!ParseInt(opt.current_value, &mael_config.http.port))
                    return 1;
            } else {
                opt.LogUnknownError();
                return 1;
            }
        }
    }

    // Init device access
    LogInfo("Init device monitor");
    hs_log_set_handler([](hs_log_level level, int, const char *msg, void *) {
        switch (level) {
            case HS_LOG_ERROR:
            case HS_LOG_WARNING: { LogError("%1", msg); } break;
            case HS_LOG_DEBUG: { LogDebug("%1", msg); } break;
        }
    }, nullptr);
    if (!InitMonitor())
        return 1;
    RG_DEFER { StopMonitor(); };

    // Run!
    LogInfo("Init HTTP server");
    http_Daemon daemon;
    if (!daemon.Start(mael_config.http, HandleRequest))
        return 1;
    if (mael_config.http.sock_type == SocketType::Unix) {
        LogInfo("Listening on socket '%1' (Unix stack)", mael_config.http.unix_path);
    } else {
        LogInfo("Listening on port %1 (%2 stack)",
                mael_config.http.port, SocketTypeNames[(int)mael_config.http.sock_type]);
    }

#ifdef __linux__
    if (!NotifySystemd())
        return 1;
#endif

    // Run until exit
    if (WaitForInterrupt() == WaitForResult::Interrupt) {
        LogInfo("Exit requested");
    }
    LogDebug("Stop HTTP server");
    daemon.Stop();

    return 0;
}

}

// C++ namespaces are stupid
int main(int argc, char **argv) { return RG::Main(argc, argv); }
