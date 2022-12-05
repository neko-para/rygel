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
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "src/core/libcc/libcc.hh"
#include "ssh.hh"

namespace RG {

bool ssh_Config::Validate() const
{
    bool valid = true;

    if (!host) {
        LogError("Missing SFTP host name");
        valid = false;
    }
    if (!username) {
        LogError("Missing SFTP username");
        valid = false;
    }

    if (!known_hosts && !host_hash) {
        LogError("Cannot use SFTP without KnownHosts and no valid server hash");
        valid = false;
    }
    if (!password && !keyfile) {
        LogError("Missing SFTP password and/or keyfile");
        valid = false;
    }

    return valid;
}

static bool SetStringOption(ssh_session ssh, ssh_options_e type, const char *str)
{
    int ret = ssh_options_set(ssh, type, str);
    return (ret == SSH_OK);
}

template <typename T>
bool SetIntegerOption(ssh_session ssh, ssh_options_e type, T value)
{
    int ret = ssh_options_set(ssh, type, &value);
    return (ret == SSH_OK);
}

ssh_session ssh_Connect(const ssh_Config &config)
{
    if (!config.Validate())
        return nullptr;

    ssh_session ssh = ssh_new();
    if (!ssh)
        throw std::bad_alloc();
    RG_DEFER_N(err_guard) { ssh_free(ssh); };

    // Set options
    {
        bool success = true;

        success &= SetStringOption(ssh, SSH_OPTIONS_HOST, config.host);
        success &= SetIntegerOption(ssh, SSH_OPTIONS_PORT, 22);
        success &= SetStringOption(ssh, SSH_OPTIONS_USER, config.username);

        if (!success)
            return nullptr;
    }

    // Connect SSH
    if (ssh_connect(ssh) != SSH_OK) {
        LogError("Failed to connect to '%1': %2", config.host, ssh_get_error(ssh));
        return nullptr;
    }
    RG_DEFER { ssh_disconnect(ssh); };

    // Authenticate server
    {
        ssh_key pk;
        Span<uint8_t> hash;

        if (ssh_get_server_publickey(ssh, &pk) < 0) {
            LogError("Failed to retrieve SSH public key of '%1': %2", config.host, ssh_get_error(ssh));
            return nullptr;
        }
        RG_DEFER { ssh_key_free(pk); };

        size_t hash_len;
        if (ssh_get_publickey_hash(pk, SSH_PUBLICKEY_HASH_SHA256, &hash.ptr, &hash_len) < 0) {
            LogError("Failed to hash SSH public key of '%1': %1", config.host, ssh_get_error(ssh));
            return nullptr;
        }
        hash.len = (Size)hash_len;
        RG_DEFER { ssh_clean_pubkey_hash(&hash.ptr); };

        ssh_known_hosts_e state = ssh_session_is_known_server(ssh);

        switch (state) {
            case SSH_KNOWN_HOSTS_OK: { LogInfo("OK"); } break;
            case SSH_KNOWN_HOSTS_CHANGED: { LogInfo("Changed"); } break;
            case SSH_KNOWN_HOSTS_OTHER: { LogInfo("Other"); } break;
            case SSH_KNOWN_HOSTS_NOT_FOUND: { LogInfo("Not Found"); } break;
            case SSH_KNOWN_HOSTS_UNKNOWN: {
                char hex[256];
                Fmt(hex, "%1", FmtSpan(hash, FmtType::SmallHex, ":"));

                LogInfo("The server is unknown. Do you trust the host key?");
                LogInfo("Public key hash: %1", hex);
            } break;
            case SSH_KNOWN_HOSTS_ERROR: { LogInfo("Error: %1", ssh_get_error(ssh)); } break;
        }
    }

    // Authenticate user
    if (ssh_userauth_password(ssh, nullptr, config.password) != SSH_AUTH_SUCCESS) {
        LogError("Failed to authenticate to '%1@%2': %3", config.host, config.username, ssh_get_error(ssh));
        return nullptr;
    }

    err_guard.Disable();
    return ssh;
}

}
