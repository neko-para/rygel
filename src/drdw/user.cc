// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "drdw.hh"
#ifdef _WIN32
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
#endif
#include "../../lib/libsodium/src/libsodium/include/sodium.h"

static const int64_t SessionIdleDelay = 4 * 3600000;

struct Session {
    char session_key[129];
    char client_addr[65];
    char user_agent[134];
    int64_t last_seen;

    const User *user;

    HASH_TABLE_HANDLER_T(Session, const char *, session_key);
};

static std::mutex sessions_mutex;
static HashTable<const char *, Session> sessions;

static bool GetClientAddress(MHD_Connection *conn, Span<char> out_address)
{
    int family;
    void *addr;
    {
        sockaddr *saddr =
            MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS)->client_addr;

        family = saddr->sa_family;
        switch (saddr->sa_family) {
            case AF_INET: { addr = &((sockaddr_in *)saddr)->sin_addr; } break;
            case AF_INET6: { addr = &((sockaddr_in6 *)saddr)->sin6_addr; } break;
            default: { Assert(false); } break;
        }
    }

    // TODO: Make sure inet_ntop always NUL-terminates
    if (!inet_ntop(family, addr, out_address.ptr, out_address.len)) {
        LogError("Cannot convert network address to text");
        return false;
    }

    return true;
}

// Call with sessions_mutex locked
static Session *FindSession(MHD_Connection *conn)
{
    uint64_t now = GetMonotonicTime();

    char address[65];
    if (!GetClientAddress(conn, address))
        return nullptr;

    const char *session_key = MHD_lookup_connection_value(conn, MHD_COOKIE_KIND, "session_key");
    const char *user_agent = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "User-Agent");
    if (!session_key || !user_agent)
        return nullptr;

    Session *session = sessions.Find(session_key);
    if (!session )
        return nullptr;
    if (!TestStr(session->client_addr, address))
        return nullptr;
    if (!TestStr(session->user_agent, user_agent))
        return nullptr;
    // TODO: Remove once DropStaleSessions() works
    if (now - session->last_seen >= SessionIdleDelay)
        return nullptr;

    session->last_seen = now;
    return session;
}

// Call with sessions_mutex locked
static void DropStaleSessions()
{
    // FIXME: Don't have a good way to remove stuff from HashTable while iterating it
}

const User *CheckSessionUser(MHD_Connection *conn)
{
    std::lock_guard<std::mutex> lock(sessions_mutex);
    DropStaleSessions();

    Session *session = FindSession(conn);
    return session ? session->user : nullptr;
}

Response HandleConnect(MHD_Connection *conn, const User *user, const char *url,
                       CompressionType compression_type)
{
    if (user) {
        Session *session = FindSession(conn);
        sessions.Remove(session);
    }

    char address[512];
    if (!GetClientAddress(conn, address))
        return CreateErrorPage(500);

    // FIXME: Switch to POST method
    const char *username = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "username");
    const char *password = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "password");
    const char *user_agent = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "User-Agent");
    if (!username || !password || !user_agent)
        return CreateErrorPage(422);

    user = drdw_user_set.FindUser(username);
    if (!user || !user->password_hash ||
            crypto_pwhash_str_verify(user->password_hash, password, strlen(password)) != 0)
        return CreateErrorPage(404);

    char session_key[129];
    {
        uint64_t buf[8];
        randombytes_buf(buf, SIZE(buf));
        Fmt(session_key, "%1%2%3%4%5%6%7%8",
            FmtHex(buf[0]).Pad0(-16), FmtHex(buf[1]).Pad0(-16),
            FmtHex(buf[2]).Pad0(-16), FmtHex(buf[3]).Pad0(-16),
            FmtHex(buf[4]).Pad0(-16), FmtHex(buf[5]).Pad0(-16),
            FmtHex(buf[6]).Pad0(-16), FmtHex(buf[7]).Pad0(-16));
    }

    // Create session
    {
        Session session = {};

        StaticAssert(SIZE(session.session_key) >= SIZE(session_key));
        strncpy(session.session_key, session_key, SIZE(session.session_key) - 1);
        strncpy(session.client_addr, address, SIZE(session.client_addr) - 1);
        strncpy(session.user_agent, user_agent, SIZE(session.user_agent) - 1);
        session.user = user;

        std::lock_guard<std::mutex> lock(sessions_mutex);
        DropStaleSessions();

        if (!sessions.Append(session).second) {
            LogError("Generated duplicate session key");
            return CreateErrorPage(500);
        }
    }

    Response response;
    {
        response = ProduceUser(conn, user, url, compression_type);

        char buf[512];
        Fmt(buf, "session_key=%1; Max-Age=14400", session_key);
        MHD_add_response_header(response.response, "Set-Cookie", buf);
    }

    return response;
}

Response HandleDisconnect(MHD_Connection *conn, const User *user, const char *url,
                          CompressionType compression_type)
{
    std::lock_guard<std::mutex> lock(sessions_mutex);
    DropStaleSessions();

    Session *session = FindSession(conn);
    sessions.Remove(session);

    // TODO: Check that the cookie gets removed with this
    Response response = ProduceUser(conn, nullptr, url, compression_type);
    MHD_add_response_header(response.response, "Set-Cookie", "session_key=; Max-Age=-1");

    return response;
}

Response ProduceUser(MHD_Connection *, const User *user, const char *,
                     CompressionType compression_type)
{
    Response response = {};

    response.code = 200;
    response.response = BuildJson(compression_type,
                                  [&](rapidjson::Writer<JsonStreamWriter> &writer) {
        writer.StartObject();
        if (user) {
            writer.Key("username"); writer.String(user->name);
        }
        writer.EndObject();

        return true;
    });

    return response;
}
