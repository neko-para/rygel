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

async function exportRecords(stores) {
    if (typeof XSLX === 'undefined')
        await net.loadScript(`${ENV.urls.static}sheetjs/xlsx.mini.min.js`);

    let entries = await net.get(`${ENV.urls.instance}api/records/list`);

    // Load records
    let threads = [];
    for (let entry of entries) {
        let url = util.pasteURL(`${ENV.urls.instance}api/records/get`, { tid: entry.tid });
        let thread = await net.get(url);

        threads.push(thread);
    }

    // Create data worksheets
    let worksheets = stores.map(store => {
        let variables = orderVariables(store, threads);
        let columns = expandColumns(variables);

        let ws = XLSX.utils.aoa_to_sheet([columns.map(column => column.name)]);

        for (let thread of threads) {
            let entry = thread.entries[store];

            if (entry == null)
                continue;

            let row = columns.map(column => {
                let result = column.read(entry.data);
                if (result == null)
                    return 'NA';
                return result;
            });
            XLSX.utils.sheet_add_aoa(ws, [row], {origin: -1});
        }

        return [store, ws];
    });

    // Create workbook...
    let wb = XLSX.utils.book_new();
    let wb_name = `export_${dates.today()}`;
    for (let [store, ws] of worksheets) {
        XLSX.utils.book_append_sheet(wb, ws, store);
    }

    // ... and export it!
    let filename = `${ENV.key}_${wb_name}.xlsx`;
    XLSX.writeFile(wb, filename);
}

function orderVariables(store, threads) {
    let variables = [];

    // Use linked list and map for fast inserts and to avoid potential O^2 behavior
    let first_head = null;
    let last_head = null;
    let heads_map = new Map;

    // Reconstitute logical order
    for (let thread of threads) {
        let entry = thread.entries[store];

        if (entry == null)
            continue;
        if (entry.meta.notes.variables == null)
            continue;

        let notes = entry.meta.notes;
        let keys = Object.keys(notes.variables);

        for (let i = 0; i < keys.length; i++) {
            let key = keys[i];

            if (!heads_map.has(key)) {
                let previous = heads_map.get(keys[i - 1]);

                let head = {
                    variable: { key: key, ...notes.variables[key] },
                    previous: previous,
                    next: null
                };

                if (previous == null) {
                    first_head = head;
                } else {
                    head.next = previous.next;
                }
                if (last_head != null)
                    last_head.next = head;
                last_head = head;

                heads_map.set(key, head);
            }
        }
    }

    // Transform linked list to simple array
    for (let head = first_head; head != null; head = head.next)
        variables.push(head.variable);

    return variables;
}

function expandColumns(variables) {
    let columns = [];

    for (let variable of variables) {
        if (variable.type == 'multi') {
            for (let prop of variable.props) {
                if (prop.value == null)
                    continue;

                let column = {
                    name: variable.key + '.' + prop.value,
                    read: data => {
                        let value = data[variable.key];
                        if (!Array.isArray(value))
                            return null;
                        return 0 + value.includes(prop.value);
                    }
                };

                columns.push(column);
            }
        } else {
            let column = {
                name: variable.key,
                read: data => data[variable.key]
            };

            columns.push(column);
        }
    }

    return columns;
}
