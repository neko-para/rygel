// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

let report = (function() {
    let self = this;

    function createRow(title, value)
    {
        return html('tr',
            html('th', title),
            html('td', value)
        );
    }

    this.refreshReport = function(rows) {
        let plid = query('#inpl_option_plid').value;
        let row = rows.find(row => row.rdv_plid == plid);

        let report = query('#inpl_report');

        report.replaceContent(
            html('h1', 'Identité'),
            row ? html('div',
                html('table',
                    createRow('Nom', row.consultant_nom),
                    createRow('Prénom', row.consultant_prenom),
                    createRow('Sexe', {M: 'Homme', F: 'Femme'}[row.consultant_sexe]),
                    createRow('Date de naissance', row.consultant_date_naissance)
                )
            ) : null,
            html('h1', 'Biologie médicale'),
            html('h1', 'Examen général'),
            html('h1', 'Électrocardiogramme (ECG)'),
            html('h1', 'Examen de la vision'),
            html('h1', 'Examen de l\'audition'),
            html('h1', 'Examen de spirométrie'),
            html('h1', 'Examen de densitométrie osseuse'),
            html('h1', 'Entretien avec neuro-psychologue'),
            html('h1', 'Entretien avec nutritionniste'),
            html('h1', 'Entretien avec EMS'),
            row ? html('button', {style: 'display: block; margin: 0 auto;',
                                  click: (e) => generateDocument(row)}, 'Générer un Compte-Rendu') : null
        );
    };

    function generateDocument(row)
    {
        let file = query('#inpl_option_template').files[0];

        if (file && row) {
            let reader = new FileReader();

            // Shortcut functions
            function label(value, var_name)
            {
                if (value instanceof Object && !Array.isArray(value))
                    value = value[var_name];

                if (value !== null) {
                    let var_info = bridge.getVarInfo(var_name);
                    let dict = bridge.getDictInfo(var_info.dict_name)

                    return dict[value];
                } else {
                    return null;
                }
            }
            function test(data, test_name)
            {
                switch (test_name) {
                    case 'demo_rachis': return tests.testRachis(data).text;
                    case 'demo_col': return tests.testFemoralNeck(data).text;
                    case 'demo_hanche': return tests.testHip(data).text;
                    case 'demo_avbras': return tests.testForearm(data).text;
                    case 'demo_sarcopenie': return tests.testSarcopenia(data).text;

                    case 'diet_diversite': return tests.testDiversity(data).text;
                    case 'diet_proteines': return tests.testProteinIntake(data).text;
                    case 'diet_calcium': return tests.testCalciumIntake(data).text;
                    case 'diet_comportement': return tests.testBehavior(data).text;

                    case 'ems_mobilite': return tests.testMobility(data).text;
                    case 'ems_force': return tests.testStrength(data).text;
                    case 'ems_fractures': return tests.testFractureRisk(data).text;

                    case 'neuropsy_efficience': return tests.neuroEfficiency(data).text;
                    case 'neuropsy_memoire': return tests.neuroMemory(data).text;
                    case 'neuropsy_execution': return tests.neuroExecution(data).text;
                    case 'neuropsy_attention': return tests.neuroAttention(data).text;
                    case 'neuropsy_cognition': return tests.neuroCognition(data).text;
                    case 'neuropsy_had': return tests.neuroDepressionAnxiety(data).text;
                    case 'neuropsy_sommeil': return tests.neuroSleep(data).text;

                    case 'constantes_hta_ortho': return tests.cardioOrthostaticHypotension(data).text;
                    case 'constantes_vop': return tests.cardioRigidity(data).text;

                    case 'audition_surdite_gauche': return tests.surdityLeft(data).text;
                    case 'audition_surdite_droite': return tests.surdityRight(data).text;

                    default: throw `Unknown test \'${test_name}\'`;
                }
            }
            function calc(data, calc_name)
            {
                switch (calc_name) {
                    case 'aq_epices': return tests.aqEpices(data);

                    default: throw `Unknown calculated variable \'${calc_name}\'`;
                }
            }
            function sex(data, male_text, female_text)
            {
                switch (data.consultant_sexe) {
                    case 'M': return male_text;
                    case 'F': return female_text;
                    default: return null;
                }
            }

            reader.onload = function() {
                let zip = new JSZip(reader.result);
                let doc = new Docxtemplater().loadZip(zip);

                try {
                    doc.setOptions({
                        parser: function(tag) {
                            tag = tag.replace(/[’‘]/g, "'");
                            return { get: (it) => {
                                let ret = eval(tag);
                                if (ret === null || ret === undefined)
                                    ret = '????';
                                return ret;
                            }};
                        },
                        paragraphLoop: true,
                        linebreaks: true
                    });
                    doc.setData(row);
                    doc.render();

                    let out = doc.getZip().generate({
                        type: 'blob',
                        mimeType: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document'
                    });

                    saveBlob(out, `CR_${row.rdv_plid}.docx`);
                } catch (e) {
                    let error_count = 0;
                    if (e.properties.id === 'multi_error') {
                        e.properties.errors.forEach(function(err) {
                            console.log(err);
                            error_count++;
                        });
                    } else {
                        console.log(e);
                        error_count = 1;
                    }

                    alert(`Got ${error_count} error(s), open the JS console to get more information`);
                }
            };

            reader.readAsBinaryString(file);
        } else if (row) {
            alert('You must specify a template document');
        }
    }

    return this;
}).call({});
