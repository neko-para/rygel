#!/usr/bin/python3

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see https://www.gnu.org/licenses/.

import argparse
import base64
import configparser
import os
import re
import secrets
import subprocess
import sys
from dataclasses import dataclass

GOUPILE_BINARY = '/usr/bin/goupile'

DOMAINS_DIRECTORY = '/etc/goupile/domains.d'
TEMPLATE_FILENAME = '/etc/goupile/template.ini'

CONFIG_DIRECTORY = '/run/goupile'
ROOT_DIRECTORY = '/var/lib/goupile'

GENERATOR_DIRECTORY = '/run/systemd/generator'
UNIT_FILENAME = '/usr/lib/systemd/system/goupile@.service'

FIRST_PORT = 8888

@dataclass
class DomainConfig:
    name = None
    archive_key = None
    port = None

@dataclass
class ServiceStatus:
    running = False
    inode = None

def create(args):
    if not re.match('^[a-zA-Z0-9_\-\.]+$', args.name):
        raise ValueError('Name must only contain alphanumeric, \'.\', \'-\' or \'_\' characters')
    if args.port is not None and (args.port <= 0 or args.port > 65535):
        raise ValueError(f'Invalid port value {args.port}')

    domains = load_domains(DOMAINS_DIRECTORY)
    ports = { domain.port for domain in domains }

    if args.port is not None:
        if args.port in ports:
            raise ValueError(f'Port {args.port} is already in use')

        port = args.port
    else:
        port = FIRST_PORT
        while port in ports:
            port += 1

    sk = secrets.token_bytes(32)
    decrypt_key = base64.b64encode(sk).decode('UTF-8')
    archive_key = derive_public_key(decrypt_key)

    ini_filename = os.path.join(DOMAINS_DIRECTORY, args.name) + '.ini'
    config = make_domain_config(archive_key, port)

    if os.path.exists(ini_filename) and not args.force:
        raise ValueError(f'Domain {args.name} already exists');
    with open(ini_filename, 'w' if args.force else 'x') as f:
        config.write(f)

    print(f'Domain config file: {ini_filename}', file = sys.stderr)
    print(f'HTTP port: {port}\n', file = sys.stderr)
    execute_command([GOUPILE_BINARY, 'keys', '-k', decrypt_key])

    if args.sync:
        domains = load_domains(DOMAINS_DIRECTORY)

        sync_domains(CONFIG_DIRECTORY, domains, TEMPLATE_FILENAME)
        sync_units(GENERATOR_DIRECTORY, domains)
        sync_services(domains)

def sync(args):
    domains = load_domains(DOMAINS_DIRECTORY)

    sync_domains(CONFIG_DIRECTORY, domains, TEMPLATE_FILENAME)
    sync_units(GENERATOR_DIRECTORY, domains)
    sync_services(domains)

def load_config(filename):
    config = configparser.ConfigParser()
    config.optionxform = str

    with open(filename, 'r') as f:
        config.read_file(f)

    return config

def load_domains(path):
    domains = []

    used_names = set()
    used_ports = set()

    for name in os.listdir(path):
        if not name.endswith('.ini'):
            continue

        filename = os.path.join(path, name)
        name, _ = os.path.splitext(name)

        config = load_config(filename)
        section = config['Domain']

        domain = DomainConfig()

        domain.name = name
        domain.archive_key = decode_key(section['ArchiveKey'])
        domain.port = decode_port(section['Port'])

        if domain.name in used_names:
            raise ValueError(f'Name "{domain.name}" used multiple times')
        if domain.port in used_ports:
            raise ValueError(f'Port {domain.port} used multiple times')

        used_names.add(domain.name)
        used_ports.add(domain.port)

        domains.append(domain)

    return domains

def decode_key(str):
    key = base64.b64decode(str)
    if len(key) != 32:
        raise ValueError(f'Malformed archive key "{str}"');
    return key

def decode_port(str):
    port = int(str)
    if port <= 0 or port > 65535:
        raise ValueError(f'Invalid TCP port "{str}"')
    return port

def sync_domains(path, domains, template):
    os.makedirs(path, mode = 0o755, exist_ok = True)

    for domain in domains:
        config = load_config(template)

        root_directory = os.path.join(ROOT_DIRECTORY, domain.name)
        ini_filename = os.path.join(path, domain.name) + '.ini'

        data = list(config['Data'].items())

        config['Domain']['Title'] = domain.name
        for key, value in data:
            config.remove_option('Data', key)
            config['Data'][key] = value
        config['Data']['RootDirectory'] = root_directory
        config['Archives']['PublicKey'] = base64.b64encode(domain.archive_key).decode('UTF-8')
        config['HTTP']['Port'] = str(domain.port)

        with open(ini_filename, 'w') as f:
            config.write(f)

def sync_units(path, domains):
    directory = os.path.join(path, 'multi-user.target.wants')
    prefix, _ = os.path.splitext(os.path.basename(UNIT_FILENAME))

    os.makedirs(directory, exist_ok = True)

    for name in os.listdir(directory):
        if not name.startswith(prefix):
            continue

        unit_filename = os.path.join(directory, name)
        os.unlink(unit_filename)

    for domain in domains:
        unit_filename = os.path.join(directory, prefix + domain.name) + '.service'
        os.symlink(UNIT_FILENAME, unit_filename)

def sync_services(domains):
    domains = { domain.name: domain for domain in domains }
    services = {}

    output = subprocess.check_output(['systemctl', 'list-units', '--type=service', '--all'])
    output = output.decode()

    for line in output.splitlines():
        parts = re.split(' +', line)

        if len(parts) >= 4:
            match = re.search('^goupile@([0-9A-Za-z_\\-\\.]+)\\.service$', parts[1])

            if match is not None:
                name = match.group(1)

                status = ServiceStatus()
                status.running = (parts[3] == 'active')

                if status.running:
                    try:
                        pid = int(subprocess.check_output(['systemctl', 'show', '-p', 'ExecMainPID', '--value', parts[1]]))

                        sb = os.stat(f'/proc/{pid}/exe')
                        status.inode = sb.st_ino
                    except Exception:
                        status.running = False

                services[name] = status

    binary_inode = os.stat(GOUPILE_BINARY).st_ino

    for name, status in services.items():
        domain = domains.get(name)

        if domain is None:
            run_service_command(name, 'stop')

    for domain in domains.values():
        status = services.get(domain.name)

        if status is None or not status.running or status.inode != binary_inode:
            run_service_command(domain.name, 'restart')

def run_service_command(name, cmd):
    service = f'goupile@{name}.service'
    execute_command(['systemctl', cmd, '--quiet', service])

def derive_public_key(key):
    output = subprocess.check_output([GOUPILE_BINARY, 'keys', '-k', key], stderr = subprocess.STDOUT)

    m = re.search('Public key: ([a-zA-Z0-9+/=]+)', output.decode('UTF-8'))
    archive_key = m.group(1)

    return archive_key

def make_domain_config(archive_key, port):
    config = configparser.ConfigParser()
    config.optionxform = str

    config.add_section('Domain')
    config['Domain']['ArchiveKey'] = archive_key
    config['Domain']['Port'] = str(port)

    return config

def execute_command(args):
    subprocess.run(args, check = True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description = 'Manage goupile services')
    subparsers = parser.add_subparsers(required = True)

    sub = subparsers.add_parser('create')
    sub.add_argument('name', metavar = 'name', type = str, help = 'domain name')
    sub.add_argument('-f', '--force', dest = 'force', action = 'store_true', help = 'Overwrite existing domain config')
    sub.add_argument('-p', '--port', dest = 'port', type = int, required = False, help = 'HTTP port')
    sub.add_argument('--no_sync', dest = 'sync', action = 'store_false', help = 'Skip automatic domain sync')
    sub.set_defaults(func = create)

    sub = subparsers.add_parser('sync', help = 'Sync goupile services')
    sub.set_defaults(func = sync)

    args = parser.parse_args()
    args.func(args)
