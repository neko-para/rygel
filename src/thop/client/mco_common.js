// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

let mco_common = {};
(function() {
    'use strict';

    const Catalogs = {
        'ccam': {path: 'catalogs/ccam.json', key: 'procedure'},
        'cim10': {path: 'catalogs/cim10.json', key: 'diagnosis'},
        'mco_ghm_roots': {path: 'catalogs/mco_ghm_roots.json', key: 'ghm_root'}
    };

    // Cache
    let indexes = [];
    let catalogs = {};

    function updateIndexes()
    {
        if (!indexes.length) {
            let url = thop.baseUrl('api/mco_indexes.json');
            data.get(url, function(json) {
                indexes = json;
            });
        }

        return indexes;
    }
    this.updateIndexes = updateIndexes;

    function updateCatalog(name)
    {
        let info = Catalogs[name];
        let set = catalogs[name];

        if (info && !set) {
            set = {
                concepts: [],
                map: {}
            };
            catalogs[name] = set;

            let url = thop.baseUrl(info.path);
            data.get(url, function(json) {
                set.concepts = json;
                for (const concept of json)
                    set.map[concept[info.key]] = concept;
            });
        }

        return set;
    }
    this.updateCatalog = updateCatalog;

    function refreshIndexesLine(indexes, main_index)
    {
        let svg = query('#opt_index');

        let builder = new VersionLine(svg);
        builder.anchorBuilder = function(version) {
            return thop.routeToUrl({date: version.date});
        };
        builder.changeHandler = function() {
            thop.go({date: this.object.getValue()});
        };

        for (const index of indexes) {
            let label = index.begin_date;
            if (label.endsWith('-01'))
                label = label.substr(0, label.length - 3);

            builder.addVersion(index.begin_date, label, index.begin_date, index.changed_prices);
        }
        if (main_index >= 0)
            builder.setValue(indexes[main_index].begin_date);

        builder.render();
    }
    this.refreshIndexesLine = refreshIndexesLine;

    function durationText(duration)
    {
        if (duration !== undefined) {
            return duration.toString() + (duration >= 2 ? ' nuits' : ' nuit');
        } else {
            return '';
        }
    }
    this.durationText = durationText;
}).call(mco_common);
