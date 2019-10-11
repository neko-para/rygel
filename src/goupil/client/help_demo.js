// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

let help_demo = (function() {
    let self = this;

    this.assets = [
        {
            key: 'tuto',
            mimetype: 'application/x.goupil.page',
            data: `// Retirer le commentaire de la ligne suivante pour afficher les
// champs (texte, numérique, etc.) à droite du libellé.
// page.pushOptions({large: true})

page.output(html\`
    <p>Une <b>fonction</b> est composée d'un <i>nom</i> et de plusieurs <i>paramètres</i> et permet de proposer un outil de saisie (champ texte, menu déroulant ...).
    <p>Exemple : la fonction page.text("num_patient", "Numéro de patient") propose un champ de saisie texte intitulé <i>Numéro de patient</i> et le stocke dans la variable <i>num_patient</i>.
    <p>Vous pouvez copier les fonctions présentées dans la section <b>Exemples</b> dans <b>Nouvelle section</b> pour créer votre propre formulaire.
\`)

page.section("Nouvelle section", () => {
    // Copier coller les fonctions dans les lignes vides ci-dessous


})

page.section("Exemples", () => {
    page.text("nom", "Quel est votre nom ?", {mandatory: true})

    page.number("age", "Quel est votre âge ?", {min: 0, max: 120})

    page.choice("sexe", "Quel est votre sexe ?", [["M", "Homme"], ["F", "Femme"]])

    page.dropdown("csp", "Quelle est votre CSP ?", [
        [1, "Agriculteur exploitant"],
        [2, "Artisan, commerçant ou chef d'entreprise"],
        [3, "Cadre ou profession intellectuelle supérieure"],
        [4, "Profession intermédiaire"],
        [5, "Employé"],
        [6, "Ouvrier"],
        [7, "Retraité"],
        [8, "Autre ou sans activité professionnelle"]
    ])

    page.radio("lieu_vie", "Quel est votre lieu de vie ?", [
        ["maison", "Maison"],
        ["appartement", "Appartement"]
    ])

    page.multi("sommeil", "Présentez-vous un trouble du sommeil ?", [
        [1, "Troubles d’endormissement"],
        [2, "Troubles de maintien du sommeil"],
        [3, "Réveil précoce"],
        [4, "Sommeil non récupérateur"],
        [null, "Aucune de ces réponses"]
    ])
})

page.errorList()
page.buttons("save")
`
        },

        {
            key: 'complicated',
            mimetype: 'application/x.goupil.page',
            data: `page.pushOptions({large: true})

page.text("nom", "Quel est votre nom ?", {mandatory: true})
page.number("age", "Quel est votre âge ?", {min: 0, max: 120,
                                            suffix: value => value > 1 ? "ans" : "an"})

let sexe = page.choice("sexe", "Quel est votre sexe ?", [["M", "Homme"], ["F", "Femme"]])

page.dropdown("csp", "Quelle est votre CSP ?", [
    [1, "Agriculteur exploitant"],
    [2, "Artisan, commerçant ou chef d'entreprise"],
    [3, "Cadre ou profession intellectuelle supérieure"],
    [4, "Profession intermédiaire"],
    [5, "Employé"],
    [6, "Ouvrier"],
    [7, "Retraité"],
    [8, "Autre ou sans activité professionnelle"]
])

page.radio("lieu_vie", "Quel est votre lieu de vie ?", [
    ["maison", "Maison"],
    ["appartement", "Appartement"]
])

page.multi("sommeil", "Présentez-vous un trouble du sommeil ?", [
    [1, "Troubles d’endormissement"],
    [2, "Troubles de maintien du sommeil"],
    [3, "Réveil précoce"],
    [4, "Sommeil non récupérateur"],
    [null, "Aucune de ces réponses"]
])

if (sexe.value == "F") {
    page.binary("enceinte", "Êtes-vous enceinte ?")
}

page.section("Alcool", () => {
    let alcool = page.binary("alcool", "Consommez-vous de l'alcool ?")

    if (alcool.value && page.value("enceinte")) {
        alcool.error("Pensez au bébé...");
        alcool.error("On peut mettre plusieurs erreurs");
        page.error("alcool", "Et de plein de manières différentes !")
    }

    if (alcool.value) {
        page.number("alcool_qt", "Combien de verres par semaine ?", {min: 1, max: 30})
    }
})

page.section("Autres", () => {
    page.number("enfants", "Combien avez-vous d'enfants ?", {min: 0, max: 30})
    page.binary("frites", "Aimez-vous les frites ?",
                {help: "Si si, c'est important, je vous le jure !"})
})

page.output(html\`On peut aussi mettre du <b>HTML directement</b> si on veut...
                 <button class="af_button" @click=\${e => go("complicated_help")}>Afficher l'aide</button>\`)
page.output("Ou bien encore mettre du <b>texte brut</b>.")

page.errorList()
page.buttons("save")
`
        },

        {
            key: 'complicated_help',
            mimetype: 'application/x.goupil.page',
            data: `page.output("Loreum ipsum")

page.buttons([
    ["Donner l'alerte", () => alert("Alerte générale !!")],
    ["Revenir à l'auto-questionnaire", () => go("complicated")]
])
`
        }
    ];
    this.assets.sort((page1, page2) => util.compareValues(page1.key, page2.key));

    return this;
}).call({});
