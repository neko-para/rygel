// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

let demo = (function() {
    let self = this;

    this.testRachis = function(data) {
        if (data.demo_dmo_rachis === null)
            return null;

        if (data.demo_dmo_rachis <= -2.5) {
            return ScreeningResult.Bad;
        } else if (data.demo_dmo_rachis <= -1.0) {
            return ScreeningResult.Fragile;
        } else {
            return ScreeningResult.Good;
        }
    };

    this.testFemoralNeck = function(data) {
        if (data.demo_dmo_col === null)
            return null;

        if (data.demo_dmo_col <= -2.5) {
            return ScreeningResult.Bad;
        } else if (data.demo_dmo_col <= -1.0) {
            return ScreeningResult.Fragile;
        } else {
            return ScreeningResult.Good;
        }
    };

    this.testHip = function(data) {
        if (data.demo_dmo_hanche === null)
            return null;

        if (data.demo_dmo_hanche <= -2.5) {
            return ScreeningResult.Bad;
        } else if (data.demo_dmo_hanche <= -1.0) {
            return ScreeningResult.Fragile;
        } else {
            return ScreeningResult.Good;
        }
    };

    this.testForearm = function(data) {
        if (data.demo_dmo_avb1 === null)
            return null;

        if (data.demo_dmo_avb1 <= -2.5) {
            return ScreeningResult.Bad;
        } else if (data.demo_dmo_avb1 <= -1.0) {
            return ScreeningResult.Fragile;
        } else {
            return ScreeningResult.Good;
        }
    }

    this.testSarcopenia = function(data) {
        if (data.consultant_sexe === null || data.demo_dxa_indice_mm === null)
            return null;

        let treshold;
        switch (data.consultant_sexe) {
            case 'M': {
                if (data.demo_dxa_indice_mm <= 7.23) {
                    return ScreeningResult.Fragile;
                } else {
                    return ScreeningResult.Good;
                }
            } break;

            case 'F': {
                if (data.demo_dmo_avb1 <= 5.67) {
                    return ScreeningResult.Fragile;
                } else {
                    return ScreeningResult.Good;
                }
            } break;
        }
    }

    return this;
}).call({});
