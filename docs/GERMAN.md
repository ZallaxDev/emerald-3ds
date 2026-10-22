# Pokémon Smaragd-Edition auf Deutsch

Pokémon Emerald 3Ds Dual Screen arbeitet mit der sauberen deutschen ROM von
**Pokémon Smaragd-Edition (Deutschland, BPED)**, mit ihren Texten und ihren Grafiken. Die
Übersetzung wird nicht mitgeliefert: sie kommt aus deiner ROM, auf deinem Rechner oder im
Browser.

Der Datenbestand ist gegen BPED selbst geprüft: das Manifest enthält 19 336 Ersetzungen,
105 Grafik-Ressourcen, die 275 Credit-Einträge und die vier Etagen des Trainerturms.
Ein sauberer Bootstrap und der 3DS-Build sind geprüft. Das Spiel wurde aber weder auf
Konsole noch im Emulator vollständig durchgespielt; die Anzeige muss noch dort geprüft werden.

## Installieren

Sobald eine Release die deutsche Variante enthält, erkennen Web-Builder und Windows-Builder
die unterstützten ROMs und wählen die Sprache selbst. Für die schnelle Aktualisierung
(nur das 3DSX) prüft die Webseite dein `emerald3ds.pak` und bietet das passende
`Emerald3DS-de.3dsx` an, das du als `Emerald3DS.3dsx` auf die SD-Karte legst.
Ein vollständiges deutsches Release-Paket ist noch nicht geprüft. Die dafür
benötigte Symboltabelle ist inzwischen aus BPED selbst abgeleitet (siehe unten);
die Rezeptprüfung ist grün. Der lokale Entwicklungsbuild ist davon unabhängig.

| Sprache | Code | SHA-1 der sauberen ROM |
|---|---|---|
| Deutsch | BPED | `61c2eb2b380b1a75f0c94b767a2d4c26cd7ce4e3` |
| Englisch | BPEE | `f3ae088181bf583e55daf962a92bb46f4f1d07b7` |
| Französisch | BPEF | `ca666651374d89ca439007bed54d839eb7bd14d0` |
| Italienisch | BPEI | `1692db322400c3141c5de2db38469913ceb1f4d4` |
| Spanisch | BPES | `fe1558a3dcb0360ab558969e09b690888b846dd9` |

## Die deutsche Fassung aus dem Quelltext bauen

Mit den Voraussetzungen aus der [Entwicklungsanleitung](DEVELOPMENT.md):

```sh
python tools/bootstrap.py --make --german-rom "/Pfad/Pokemon - Smaragd-Edition (Germany).gba" -j8
```

Der Bootstrap wendet die Patches an, baut die Werkzeuge und die generierten Includes, führt
`tools/localize_german.py` mit deiner ROM aus und baut das 3DSX nach
`build/upstream/3ds_port/emerald3ds.3dsx`. Ein Sprachwechsel (Neustart ohne
`--german-rom`) setzt den Baum zurück und löscht die Übersetzungsobjekte und
-ressourcen, damit keine Daten einer anderen Sprache wiederverwendet werden.

Die Engine prüft, für welche Sprache sie gebaut wurde: eine deutsche Ausführungsdatei
akzeptiert nur einen Datenbestand aus BPED, und umgekehrt.

## Wie die deutsche Fassung entsteht

`tools/localize_german.py` prüft den SHA-1 der ROM und die SHA-256-Abdrücke der
Quelldateien, bevor etwas in den generierten Baum geschrieben wird. Die Manifeste in
`tools/locales/` enthalten Positionen im Quelltext, Verschiebungen und Größen in der ROM,
Symbolnamen und Adressen. Sie enthalten keine Texte, Grafiken, Klänge oder
ROM-Abschnitte.

Rohe `__()`-Felder haben kein automatisch angehängtes Endzeichen. Der Import begrenzt
ihre Größe anhand der Zeichentabelle und der Quellinitialisierung, statt bis zum nächsten
Endzeichen in der ROM zu lesen. Das gilt auch für die festen KP-Löschpuffer und Tastaturzeilen.

Der Ablauf ersetzt Texte und Codefelder durch die deiner ROM, holt 105 Grafiken heraus und
baut die 275 Credit-Einträge sowie die vier Trainerturm-Etagen neu auf. Dadurch laufen
Namen, Dialoge, Menüs, das Wörterbuch, die Bardenlieder, die Trainer-Sprüche, die Karten,
die Fragen und die Brailleschrift der Ruinen auf Deutsch.

Deutsches:

- die fünf Schriften (und die übrigen 100 Grafiken) kommen aus der deutschen ROM;
- das KP-Zeichen der Kampfanzeige kommt aus der vollständigen BPED-Kacheltabelle;
- die Namenseingabe übernimmt alle 108 Tastaturbytes und die Spaltenpositionen aus BPED.
  Anzeige und Auswahl benutzen neun Buchstabenspalten einschließlich aller Umlaute;
  die Symbolseite behält ihre sechs auswählbaren Tasten. Der Datenimport ersetzt ganze
  Tabellen statt rohe Acht-Byte-Zeilen zu überfüllen. Der reine Strukturpatch
  `0082-german-naming-keyboard.patch` lässt die anderen Sprachen unverändert;
- der Bildschirm « ENDE » benutzt die vorgerenderte Tilemap der deutschen ROM
  (`graphics/credits/german_the_end.bin.lz`), über
  `patches/pokeemerald/0080-german-ending-tilemap.patch`;
- metrische Pokédex-Werte mit Dezimalkomma, die Beerenanzahl (AMRENA + BEERE in einem
  Wort, also « SINELBEERE »), die deutsche Fassung der Team-EP-Meldung des Ports und die
  verbreiterte Messspalte, über `patches/pokeemerald/0081-german-layout.patch` («EP» ist
  die eigene Abkürzung des Spiels, wie im deutschen Item-Namen von `EP-TEILER`).

Noch nicht fertig, und deshalb offen ausgewiesen:

- die Variante **übersetzt und bindet**: der Port baut auf einem frisch lokalisierten
  Baum, mit 6393/6393 geprüften Assets und ohne ungelöste Verweise;
- die vier alphabetischen Easy-Chat-ID-Listen werden aus BPED übernommen; ihre Positionen
  sind über die Zeiger der gemeinsamen Gruppentabelle und die vollständigen ID-Mengen geprüft;
- die **37 bisher offenen Quellenstellen sind geklärt**: 28 haben eine belegte
  ROM-Zuordnung, acht Tastaturzeilen werden über die vollständige Retail-Tabelle
  übernommen, ein ungenutzter leerer Platzhalter bleibt bewusst unverändert.
  Das Manifest enthält für jede Stelle eine ausdrückliche Entscheidung;
- der **Konsolentest** steht noch aus;
- die Symboltabelle (`tools/locales/german-symbols.s.gz`) ist aus BPED selbst
  rekonstruiert, nicht aus einem GBA-Build dieses Baums: Strukturen über ihre Form,
  unveränderte Objekte über ihren Inhalt, Texte über die Adressinvarianz des Manifests.
  Sie ist **nicht vollständig** (15 083 von 48 858 Namen mit belegter Adresse). Die
  Rezeptprüfung verwendet weiterhin die unveränderten Grenzen für Literale; alle
  rekonstruierten Dateien und Generator-Eingaben werden byte-genau verglichen.

Die Menüs des Ports (EINSTELL., EXTRAS, CHEATS und ihre Werte) sind über
`CTR_TEXT` in `3ds_port/include/3ds_locale.h` an sechs Oberflächensprachen gebunden, auf Deutsch
eingetragen; die Umlaute und das scharfe ß wandelt `3ds_locale_text.h` in die Zeichencodes
der internationalen Tabelle um (`Ä`=F1, `Ö`=F2, `Ü`=F3, `ä`=F4, `ö`=F5, `ü`=F6, `ß`=15).

Wenn eine Änderung des Ports eine Quelldatei des Manifests verändert, hält die
Lokalisierung an (« Source differs from the pinned patched tree »), bis ihre Positionen und
Abdrücke aktualisiert sind.

## Fotos für die Durchsicht

Die Variante muss noch auf einer Konsole ausprobiert werden. Den unteren Bildschirm
vollständig fotografieren, ohne Ränder oder Tabs zu beschneiden:

- EINSTELLUNGEN, Karteikarte der Optionen: die zwölf Zellen, die Werte und die Tabs.
- VERBESSERUNGEN: die Optionen, einschließlich der vom Port ergänzten.
- CHEATS: die beiden Seiten, mit Seitenzahl und sichtbaren Werten.
- Aus POKéMON die Zusammenfassung eines Pokémon: Name, Typen, Werte, Wesen, Fähigkeit und
  Attacken mit AP; am besten lange Namen.
- Im Kampf die Attackenwahl: die vier Felder mit Namen, Typen und AP.
- Der Pokédex: Größe und Gewicht eines Pokémon, um Meter, Kilogramm und das Dezimalkomma zu
  prüfen.
- Die Credits und der Schlussbildschirm « ENDE ».

Den gebauten Commit und das Konsolenmodell zu den Fotos notieren. Breitenprüfungen am
Rechner ersetzen diese Fotos und die Versuche auf der Konsole nicht.

Der Port benutzt feste SD-Pfade (`/3ds/emerald3ds/`) für die Daten, die Einstellungen und
den Spielstand, auch wenn das 3DSX aus einem anderen Ordner gestartet wird. Für einen
Testlauf ohne eine bestehende Installation anzufassen, eine eigene Test-SD-Karte benutzen.

## Tests ohne ROM

```sh
python -m unittest discover -s builder/tests -v
```

## Autor

Deutsche Fassung und Lokalisierungswerkzeuge von andyst-dev.
