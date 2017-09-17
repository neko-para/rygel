from collections import OrderedDict, defaultdict
import sys
from datetime import datetime, date
import csv
import json
import traceback
import io
import operator

# -------------------------------------------------------------------------
# Constants
# -------------------------------------------------------------------------

FINESS_JURIDIQUE = 590780193

# -------------------------------------------------------------------------
# Utility
# -------------------------------------------------------------------------

def date_value(date_str):
    return datetime.strptime(date_str, "%d%m%Y").date()
def int_optional(int_str, default = None):
    int_str = int_str.strip()
    return int(int_str) if int_str else default

def write_json(data, dest):
    class ExtendedJsonEncoder(json.JSONEncoder):
        def default(self, obj):
            if isinstance(obj, date):
                return obj.strftime('%Y-%m-%d')
            return json.JSONEncoder.default(self, obj)

    json.dump(data, dest, sort_keys = True, indent = 4, cls = ExtendedJsonEncoder)

# -------------------------------------------------------------------------
# Converters
# -------------------------------------------------------------------------

def parse_rums(filename):
    def int_optional(int_str, default = None):
        int_str = int_str.strip()
        return int(int_str) if int_str else default
    def date(date_str):
        date_str = date_str.decode()
        return datetime.strptime(date_str, "%d%m%Y").date()
    def date_optional(date_str, default = None):
        date_str = date_str.strip()
        return date(date_str) if date_str else default

    with open(filename, 'rb') as f:
        for line in f:
            buf = io.BytesIO(line)

            try:
                rum = {}

                buf.seek(2, io.SEEK_CUR) # Skip GHM Version
                rum['test_ghm'] = buf.read(6).decode()
                buf.seek(1, io.SEEK_CUR) # Skip filler byte
                rss_version = int(buf.read(3))
                if rss_version < 116:
                    raise ValueError(f'Unsupported RSS format {rss_version}')
                rum['test_error'] = int_optional(buf.read(3))
                buf.seek(12, io.SEEK_CUR) # Skip FINESS and RUM version
                rum['bill_id'] = int(buf.read(20))
                rum['stay_id'] = int(buf.read(20))
                buf.seek(10, io.SEEK_CUR) # Skip RUM identifier
                rum['birthdate'] = date(buf.read(8))
                rum['sex'] = int(buf.read(1))
                rum['unit'] = int(buf.read(4))
                rum['bed_authorization'] = int_optional(buf.read(2))
                rum['entry_date'] = date(buf.read(8))
                rum['entry_mode'] = int(buf.read(1))
                rum['entry_origin'] = buf.read(1).decode().upper()
                if rum['entry_origin'] != 'R':
                    rum['entry_origin'] = int_optional(rum['entry_origin'])
                rum['exit_date'] = date(buf.read(8))
                rum['exit_mode'] = int(buf.read(1))
                rum['exit_destination'] = int_optional(buf.read(1))
                buf.seek(5, io.SEEK_CUR) # Skip postal code
                rum['newborn_weight'] = int_optional(buf.read(4))
                rum['gestational_age'] = int_optional(buf.read(2))
                rum['last_menstrual_period'] = date_optional(buf.read(8))
                rum['session_count'] = int_optional(buf.read(2))
                das_count = int(buf.read(2))
                dad_count = int(buf.read(2))
                proc_count = int(buf.read(3))
                rum['dp'] = buf.read(8).decode().strip()
                rum['dr'] = buf.read(8).decode().strip()
                if not rum['dr']:
                    rum['dr'] = None
                rum['igs2'] = int_optional(buf.read(3))
                buf.seek(33, io.SEEK_CUR) # Skip confirmation, innovation, etc.

                if das_count:
                    rum['das'] = []
                    for i in range(0, das_count):
                        rum['das'].append(buf.read(8).decode().strip())
                buf.seek(dad_count * 8, io.SEEK_CUR)

                if proc_count:
                    rum['procedures'] = []
                    for i in range(0, proc_count):
                        proc = {}
                        proc['date'] = date(buf.read(8))
                        proc['code'] = buf.read(7).decode().strip()
                        if rss_version >= 117:
                            buf.seek(3, io.SEEK_CUR) # Skip CCAM code extension
                        proc['phase'] = int(buf.read(1))
                        proc['activity'] = int(buf.read(1))
                        buf.seek(7, io.SEEK_CUR) # Skip modificators, doc extension, etc.
                        proc['count'] = int(buf.read(2))
                        rum['procedures'].append(proc)

                rum = {k: v for k, v in rum.items() if v is not None}
                yield rum
            except Exception as e:
                traceback.print_exc()

def parse_structure(filename):
    units = {}

    with open(filename, newline = '') as f:
        reader = csv.DictReader(f, delimiter = ';')
        for row in reader:
            # TODO: We want to keep units with patients, this is not enough
            if row['REF - Libellé long UF'].startswith('FERME'):
                continue
            if int(row['GEO - Matricule Finess']) == FINESS_JURIDIQUE:
                continue

            uf = int(row['REF - Code UF'])

            unit = {}
            unit['facility'] = int(row['GEO - Matricule Finess'])

            units[uf] = unit

    return units

def parse_ficum(filename):
    authorizations = defaultdict(list)

    with open(filename) as f:
        for line in f:
            try:
                type = line[13:16].strip()
                if type == '00':
                    continue

                authorization = {}
                authorization['date'] = date_value(line[16:24])
                if line[0:4] == '$$$$':
                    authorization['facility'] = int(line[4:13])
                else:
                    authorization['unit'] = int(line[0:4])

                authorizations[type].append(authorization)
            except Exception as e:
                print(e, file = sys.stderr)

    return authorizations

# -------------------------------------------------------------------------
# Main
# -------------------------------------------------------------------------

MAIN_USAGE = \
"""Usage: convert_chu_lille.py command options

Commands:
    authorizations      Convert structure and authorization data
    stays               Convert RUM files"""
STAYS_USAGE = \
"""
"""
UNITS_USAGE = \
"""
"""

def process_stays(rum_filename):
    rums = list(parse_rums(rum_filename))
    rums.sort(key = operator.itemgetter('bill_id'))

    def update_rss_count(first, last):
        for rum in rums[first:last]:
            rum['test_cluster_len'] = last - first

    first_rss_rum = 0
    for i, rum in enumerate(rums):
        if rums[first_rss_rum]['bill_id'] != rum['bill_id']:
            update_rss_count(first_rss_rum, i)
            first_rss_rum = i
    update_rss_count(first_rss_rum, len(rums))

    write_json(rums, sys.stdout)

def process_units(structure_filename, ficum_filename):
    write_json({
        'units': parse_structure(structure_filename),
        'authorizations': parse_ficum(ficum_filename)
    }, sys.stdout)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(MAIN_USAGE, file = sys.stderr)
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == "stays":
        if len(sys.argv) < 3:
            print(STAYS_USAGE, file = sys.stderr)
            sys.exit(1)

        process_stays(sys.argv[2])
    elif cmd == "authorizations":
        if len(sys.argv) < 4:
            print(UNITS_USAGE, file = sys.stderr)
            sys.exit(1)

        process_units(sys.argv[2], sys.argv[3])
    else:
        print(f"Unknown command '{cmd}'", file = sys.stderr)
        print(MAIN_USAGE, file = sys.stderr)
        sys.exit(1)

    sys.exit(0)
