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

#include "../../core/libcc/libcc.hh"
#include "domain.hh"
#include "goupile.hh"
#include "instance.hh"
#include "user.hh"
#include "../../../vendor/libsodium/src/libsodium/include/sodium.h"

namespace RG {

static http_SessionManager sessions;

const Token *Session::GetToken(const InstanceHolder *instance) const
{
    if (instance) {
        Token *token;
        {
            std::shared_lock<std::shared_mutex> lock_shr(tokens_lock);
            token = tokens_map.Find(instance->unique);
        }

        if (!token) {
            do {
                sq_Statement stmt;
                if (!gp_domain.db.Prepare(R"(SELECT permissions FROM dom_permissions
                                             WHERE userid = ?1 AND instance = ?2)", &stmt))
                    break;
                sqlite3_bind_int64(stmt, 1, userid);
                sqlite3_bind_text(stmt, 2, instance->key.ptr, (int)instance->key.len, SQLITE_STATIC);
                if (!stmt.Next())
                    break;

                uint32_t permissions = sqlite3_column_int(stmt, 0);

                std::lock_guard<std::shared_mutex> lock_excl(tokens_lock);

                token = tokens_map.SetDefault(instance->unique);
                token->permissions = permissions;
            } while (false);

            // User is not assigned to this instance, cache this information
            if (!token) {
                std::lock_guard<std::shared_mutex> lock_excl(tokens_lock);
                token = tokens_map.SetDefault(instance->unique);
            }
        }

        return token->permissions ? token : nullptr;
    } else {
        return nullptr;
    }
}

static void WriteProfileJson(const Session *session, const Token *token, json_Writer *out_json)
{
    char buf[128];

    out_json->StartObject();

    if (session) {
        out_json->Key("userid"); out_json->Int64(session->userid);
        out_json->Key("username"); out_json->String(session->username);
        out_json->Key("admin"); out_json->Bool(session->IsAdmin());
        out_json->Key("demo"); out_json->Bool(session->demo);

        if (token) {
            out_json->Key("permissions"); out_json->StartObject();
            for (Size i = 0; i < RG_LEN(UserPermissionNames); i++) {
                Span<const char> key = ConvertToJsonName(UserPermissionNames[i], buf);
                out_json->Key(key.ptr, (size_t)key.len); out_json->Bool(token->permissions & (1 << i));
            }
            out_json->EndObject();
        }

        out_json->Key("local_key"); out_json->String(session->local_key);
    }

    out_json->EndObject();
}

static RetainPtr<Session> CreateUserSession(const http_RequestInfo &request, http_IO *io,
                                            int64_t userid, const char *username, const char *local_key)
{
    Size username_len = strlen(username);
    Size len = RG_SIZE(Session) + username_len + 1;
    Session *session = (Session *)Allocator::Allocate(nullptr, len, (int)Allocator::Flag::Zero);

    new (session) Session;
    RetainPtr<Session> ptr(session, [](Session *session) {
        session->~Session();
        Allocator::Release(nullptr, session, -1);
    });

    session->userid = userid;
    session->username = (char *)session + RG_SIZE(Session);
    CopyString(username, MakeSpan((char *)session->username, username_len + 1));
    if (!CopyString(local_key, session->local_key)) {
        // Should never happen, but let's be careful
        LogError("User local key is too big");
        return {};
    }

    return ptr;
}

RetainPtr<const Session> GetCheckedSession(const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<Session> session = sessions.Find<Session>(request, io);

    if (gp_domain.config.demo_user && !session) {
        static RetainPtr<Session> demo_session = [&]() {
            sq_Statement stmt;
            if (!gp_domain.db.Prepare("SELECT userid, local_key FROM dom_users WHERE username = ?1", &stmt))
                return RetainPtr<Session>(nullptr);
            sqlite3_bind_text(stmt, 1, gp_domain.config.demo_user, -1, SQLITE_STATIC);

            if (!stmt.Next()) {
                if (stmt.IsValid()) {
                    LogError("Demo user '%1' does not exist", gp_domain.config.demo_user);
                }
                return RetainPtr<Session>(nullptr);
            }

            int64_t userid = sqlite3_column_int64(stmt, 0);
            const char *local_key = (const char *)sqlite3_column_text(stmt, 1);
            RetainPtr<Session> session = CreateUserSession(request, io, userid, gp_domain.config.demo_user, local_key);

            if (RG_LIKELY(session)) {
                session->demo = true;
            }

            return session;
        }();

        session = demo_session;
    }

    return session;
}

// XXX: This is a quick and dirty way to redirect the user but we need to do better
static bool RedirectToSlave(InstanceHolder *instance, const Session *session, http_IO *io)
{
    // Try to redirect user to a slave instance he is allowed to access (if any)
    if (session && instance && instance->GetSlaveCount()) {
        sq_Statement stmt;
        if (!gp_domain.db.Prepare(R"(SELECT i.instance FROM dom_instances i
                                     INNER JOIN dom_permissions p ON (p.instance = i.instance)
                                     WHERE p.userid = ?1 AND i.master = ?2
                                     ORDER BY i.instance)", &stmt))
            return false;
        sqlite3_bind_int64(stmt, 1, session->userid);
        sqlite3_bind_text(stmt, 2, instance->key.ptr, (int)instance->key.len, SQLITE_STATIC);

        if (stmt.Next()) {
            const char *key = (const char *)sqlite3_column_text(stmt, 0);

            char redirect[512];
            Fmt(redirect, "/%1/", key);

            io->AddHeader("Location", redirect);
            io->AttachNothing(302);

            return true;
        }
    }

    return false;
}

void HandleUserLogin(InstanceHolder *instance, const http_RequestInfo &request, http_IO *io)
{
    io->RunAsync([=]() {
        // Read POST values
        const char *username;
        const char *password;
        {
            HashMap<const char *, const char *> values;
            if (!io->ReadPostValues(&io->allocator, &values)) {
                io->AttachError(422);
                return;
            }

            username = values.FindValue("username", nullptr);
            password = values.FindValue("password", nullptr);
            if (!username || !password) {
                LogError("Missing 'username' or 'password' parameter");
                io->AttachError(422);
                return;
            }
        }

        // We use this to extend/fix the response delay in case of error
        int64_t now = GetMonotonicTime();

        sq_Statement stmt;
        if (instance) {
            if (!gp_domain.db.Prepare(R"(SELECT u.userid, u.password_hash, u.admin, u.local_key FROM dom_users u
                                         INNER JOIN dom_permissions p ON (p.userid = u.userid)
                                         INNER JOIN dom_instances i ON (i.instance = p.instance)
                                         WHERE u.username = ?1 AND
                                               (i.instance = ?2 OR i.master = ?2) AND
                                               p.permissions > 0)", &stmt))
                return;
            sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, instance->key.ptr, (int)instance->key.len, SQLITE_STATIC);
        } else {
            if (!gp_domain.db.Prepare(R"(SELECT userid, password_hash, admin, local_key FROM dom_users
                                         WHERE username = ?1 AND admin = 1)", &stmt))
                return;
            sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        }

        if (stmt.Next()) {
            int64_t userid = sqlite3_column_int64(stmt, 0);
            const char *password_hash = (const char *)sqlite3_column_text(stmt, 1);
            bool admin = (sqlite3_column_int(stmt, 2) == 1);
            const char *local_key = (const char *)sqlite3_column_text(stmt, 3);

            if (crypto_pwhash_str_verify(password_hash, password, strlen(password)) == 0) {
                int64_t time = GetUnixTime();

                if (!gp_domain.db.Run(R"(INSERT INTO adm_events (time, address, type, username)
                                         VALUES (?1, ?2, ?3, ?4))",
                                      time, request.client_addr, "login", username))
                    return;

                RetainPtr<Session> session = CreateUserSession(request, io, userid, username, local_key);

                if (RG_LIKELY(session)) {
                    if (admin && !instance) {
                        // Require regular relogin (every 20 minutes) to access admin panel
                        session->admin_until = GetMonotonicTime() + 1200 * 1000;
                    }

                    sessions.Open(request, io, session);

                    const Token *token = session->GetToken(instance);

                    if (RedirectToSlave(instance, session.GetRaw(), io))
                        return;

                    http_JsonPageBuilder json(request.compression_type);
                    WriteProfileJson(session.GetRaw(), token, &json);
                    json.Finish(io);
                }

                return;
            }
        }

        if (stmt.IsValid()) {
            // Enforce constant delay if authentification fails
            int64_t safety_delay = std::max(2000 - GetMonotonicTime() + now, (int64_t)0);
            WaitDelay(safety_delay);

            LogError("Invalid username or password");
            io->AttachError(403);
        }
    });
}

void HandleUserLogout(InstanceHolder *, const http_RequestInfo &request, http_IO *io)
{
    sessions.Close(request, io);
    io->AttachText(200, "Done!");
}

void HandleUserProfile(InstanceHolder *instance, const http_RequestInfo &request, http_IO *io)
{
    RetainPtr<const Session> session = GetCheckedSession(request, io);
    const Token *token = session ? session->GetToken(instance) : nullptr;

    if (RedirectToSlave(instance, session.GetRaw(), io))
        return;

    http_JsonPageBuilder json(request.compression_type);
    WriteProfileJson(session.GetRaw(), token, &json);
    json.Finish(io);
}

}
