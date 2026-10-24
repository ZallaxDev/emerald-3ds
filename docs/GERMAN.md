# Pokémon Smaragd auf Deutsch

Emerald 3DS läuft mit dem sauberen deutschen ROM von
**Pokémon Smaragd-Edition (Deutschland, BPED)**, mit dessen Texten und Grafiken. Die
Übersetzung wird nicht mitgeliefert: Sie kommt aus deinem eigenen ROM, auf deinem
Computer oder im Browser.

Die Variante ist **experimentell**. Sie wurde kompiliert und gelinkt, und das Datenpaket
lässt sich aus BPED bauen. Auf einer Konsole oder in einem Emulator ist sie noch nicht
getestet, und das Abenteuer wurde noch nicht durchgespielt.

## Dein Modul auslesen

Du brauchst ein unverändertes Abbild deines eigenen Moduls, zum Beispiel mit einem
GBxCart RW oder einem Nintendo DS Lite mit GBA-Steckplatz und einem Backup-Programm.
Das Abbild muss genau diese Prüfsumme haben:

| Sprache | Code | SHA-1 des sauberen ROMs |
|---|---|---|
| Deutsch | BPED | `61c2eb2b380b1a75f0c94b767a2d4c26cd7ce4e3` |
| Englisch | BPEE | `f3ae088181bf583e55daf962a92bb46f4f1d07b7` |

Unter Windows zeigt `certutil -hashfile smaragd.gba SHA1` die Prüfsumme an, unter
Linux und macOS `sha1sum smaragd.gba` bzw. `shasum smaragd.gba`.

## Installieren

Wie bei der englischen Version: Der Builder nimmt jedes unterstützte ROM an und wählt die
Sprache selbst. Jede Release enthält die Varianten mit ihrem eigenen Rezept; die deutsche
3DSX heißt `Emerald3DS-de.3dsx` und kommt als `Emerald3DS.3dsx` auf die SD-Karte.

Der Motor prüft die Sprache, mit der er kompiliert wurde: Eine deutsche 3DSX nimmt nur
ein Datenpaket an, das aus BPED gebaut wurde, und umgekehrt.

## Die deutsche Version aus dem Quellcode bauen

Mit den Voraussetzungen aus der [Entwicklungsanleitung](DEVELOPMENT.md):

```sh
python tools/bootstrap.py --make --german-rom "/pfad/zu/smaragd.gba" -j8
```

Der Bootstrap wendet die Patches an, baut die Werkzeuge und die erzeugten Includes,
führt `tools/localize_german.py` mit deinem ROM aus und baut die 3DSX nach
`build/upstream/3ds_port/emerald3ds.3dsx`. Ein Sprachwechsel (ohne `--german-rom`
neu starten) setzt den Baum zurück und löscht kompilierte Objekte und lokalisierte Daten.

## Wie die deutsche Version entsteht

`tools/localize_german.py` prüft die SHA-1 des ROMs und die SHA-256 der Quelldateien,
bevor es etwas in den erzeugten Baum schreibt. `tools/locales/german.json.gz` enthält nur
Positionen im Quelltext, Offsets und Größen im ROM und Symbolnamen, aber keinen Text,
keine Grafik und keinen Ton.

Das Manifest erzeugt `tools/gen_german_manifest.py` aus zwei Builds der deutschen
Dekompilierung [pokeemerald-german](https://github.com/WalterWoshid/pokeemerald-german),
die BPEE und BPED Byte für Byte nachbaut. Die Textstellen sind die des französischen,
italienischen und spanischen Manifests: Jede wird im englischen Build gefunden und über
dasselbe Symbol im deutschen Build zugeordnet. So kommen etwa 17.000 Texte, 2.300
Datenfelder, 109 Grafiken, der Abspann und die Ebenen des Trainerhügels aus deinem ROM.
25 Textstellen ließen sich nicht eindeutig zuordnen und bleiben wie im Quelltext; sie
stehen im Manifest unter `unmapped_source_texts`. Keine davon ist im Spiel als englischer
Text zu sehen: Es sind Zeichen, die im Deutschen gleich sind (♂, ♀, !, ?, :, …, Pfeile),
leere Platzhalter und ungenutzte Texte.

Deutsche Besonderheiten (Patch `0080-german-layout.patch`):

- breitere Fenster für die längeren deutschen Bezeichnungen: Kampf, Beutel,
  Beerenetikett, PokéNav-Listen, Kampfzone, Beerenmixer und mehr, nach den Werten des
  deutschen ROMs;
- der Pokédex zeigt Meter und Kilogramm mit **Komma**;
- die Namenstastatur hat neun Spalten und die Umlaute **Ä Ö Ü**;
- Trainerklassen nach Geschlecht (etwa die Schülerin) und Sätze, die um einen Namen
  herum gebaut werden („{Name}s Geheimbasis“);
- der Titel der Wettbewerbsergebnisse als ein Stück, das deutsche „ENDE“ im Abspann und
  das Timing des deutschen Abspanns;
- die eigenen Meldungen des Ports (Team-EP, VM einsetzen, noch einen Schutz benutzen,
  lokale Drahtlosverbindung) sind auf Deutsch geschrieben;
- die Port-Menüs (OPTIONEN, EXTRAS, CHEATS, Beuteltaschen, Werte) sind übersetzt, und das
  Touch-Menü kann Umlaute darstellen.

Begleiter-Pokémon sprechen Deutsch: Der Builder holt die deutschen Texte aus HeartGold
und SoulSilver (`de_msg.txt` des gepinnten poke-corpus).

Ändert ein Port-Patch eine Quelldatei aus einem Manifest, verschiebt
`tools/rebase_locale_manifests.py` die Stellen der anderen Sprachen; das deutsche
Manifest wird neu erzeugt.

## Fotos zur Prüfung

Bitte den unteren Bildschirm vollständig fotografieren:

- OPTIONEN, EXTRAS und CHEATS mit allen Seiten;
- die Statusseite eines Pokémon mit langem Namen und die Attackenwahl im Kampf;
- Größe und Gewicht im Pokédex;
- die Namenseingabe mit Umlauten;
- den Abspann und den Bildschirm „ENDE“.

## Tests ohne ROM

```sh
python -m unittest discover -s builder/tests -v
```
