# GeoVoice Studio für Windows

Mit GeoVoice Studio bereitest du am PC Offlinekarten, Routen und Ansagen für den [GeoVoice-Player](https://github.com/huettnerTobias/RasberryGeoVoice) vor. Die fertige [GeoVoiceStudio.exe für Windows x64](https://github.com/huettnerTobias/GeoVoiceStudio/raw/refs/heads/master/dist/GeoVoiceStudio.exe) benötigt weder Java noch CLion.

## Erste Route

1. Starte die EXE und wähle **Neue Route**. Bei der ersten Bearbeitung richtest du lokal ein Admin-Passwort und einen Route-Code ein. Der Code schaltet die Bearbeitung für die Sitzung frei. Über **Werkzeuge → Bearbeitung sperren** kannst du sie wieder sperren.
2. Öffne **Offlinekarten**. Du kannst eine vorhandene Mapsforge-Datei oder einen XYZ-Ordner anzeigen, ein Länderpaket auf den PC herunterladen oder einen XYZ-Ausschnitt für den Player erstellen. Für einen XYZ-Download werden die URL eines für Offline-Downloads freigegebenen Anbieters, die Quellenangabe, die Mitte, der Radius und die Zoomstufen benötigt.
3. Setze per Doppelklick auf die Karte einen Punkt oder gib unter **Hinzufügen** Breiten- und Längengrad ein. Werte mit Dezimalpunkt und Dezimalkomma werden angenommen. Gib dem Punkt einen Namen und weise ihm eine WAV- oder MP3-Datei zu. **Audio importieren** kopiert die Datei in die Studio-Bibliothek.
4. Die Punktliste bestimmt die Reihenfolge der Ansagen. Mit **Nach oben** und **Nach unten** änderst du die Reihenfolge. **Weiter** zeigt den nächsten Punkt, **Anhören** spielt die zugeordnete Datei ab. Ein Entwurf darf Punkte ohne Ansage enthalten. Für den Export braucht jeder Punkt einen Namen und eine vorhandene Audiodatei.
5. Wähle **Route exportieren** und speichere das `.geovoice`-Paket auf dem USB-Stick. Unter **Datei** kannst du auch alle Routen zusammen exportieren. Auf dem Player importierst du das Paket unter **Mehr → Wartung → Inhalte**. Die Karte separat kopieren. Eine `.map` lässt sich am Player importieren. Einen XYZ-Ordner zuerst auf das Gerät kopieren und dort auswählen.

## Luftbilder für Österreich

1. Wähle eine Route mit mindestens einem Punkt. Unter **Offlinekarten → Luftbild für die Route** zeigt Studio den Namen der ausgewählten Route an.
2. Für ein detailreiches Luftbild wähle **Luftbild laden …**. Studio lädt die Originalkacheln von basemap.at entlang der Verbindungen zwischen den Routenpunkten. Die höchste Zoomstufe ist einstellbar von 17 bis 20, voreingestellt ist 19. Ein kleinerer Wert spart Downloadzeit und Platz. Die Detailauflösung hängt vom Aufnahmegebiet ab. [Orthofoto von basemap.at](https://basemap.at/orthofoto/) · [Anleitung des Anbieters zur Offline-Nutzung](https://cdn.basemap.at/basemap-downloader-Anleitung.pdf)
3. Wenn das offizielle Offlinepaket schon auf dem PC liegt, entpacke die ZIP-Datei und wähle **Original-MBTiles importieren …**. Dieser Weg funktioniert ohne Internet. Studio übernimmt die im Paket verfügbaren Zoomstufen. Das österreichweite MBTiles-Paket `L00bisL16` reicht bis Zoom 16. [Offizieller Downloadkatalog](https://www.data.gv.at/katalog/dataset/7cb3fb29-6b14-477b-a88b-a6a526b59b40)
4. Wähle den Speicherort, zum Beispiel einen USB-Stick. Studio legt dort einen neuen Ordner `Orthofoto-<Routenname>` an und öffnet die fertige Karte. Kopiere **den ganzen Ordner** auf den Player und wähle ihn unter **Mehr → Wartung → Inhalte → XYZ-Kartenordner auswählen**. Die Route und ihre Ansagen überträgst du separat als `.geovoice`.

Studio und Player zeigen die verlinkte Quellenangabe **Datenquelle: basemap.at** und die Lizenz **CC BY 4.0** an. Im Kartenordner stehen die Quellenangabe, beide Links und ein Hinweis auf den Routenausschnitt. Gib diese Dateien zusammen mit den Kacheln weiter. [Nutzungsbedingungen von basemap.at](https://basemap.at/) · [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)

## Bestehende Inhalte

Über **Datei → Bestehende Java-Bibliothek übernehmen** lassen sich die Routen und ihre zugeordneten Ansagen aus dem bisherigen Studio übernehmen. Schließe dazu die alte Anwendung. Der übliche Quellordner unter Windows ist `%APPDATA%\GeoVoiceStudio`. Weitere WAV-/MP3-Dateien kannst du über die **Audiobibliothek** auch gemeinsam importieren. Alte `.geovoice`-Pakete öffnest du direkt über **Paket importieren**.

Bestätigte Änderungen speichert das Studio unter `%APPDATA%\GeoVoiceStudioC`. Zur Sicherung das Studio schließen und diesen Ordner kopieren. Die gewählte Kartenquelle wird für das Windows-Benutzerkonto gespeichert. Direkt geöffnete Karten bleiben an ihrem ursprünglichen Speicherort. Für einen getrennten Routendatenbestand kann vor dem Start die Umgebungsvariable `GEOVOICE_STUDIO_DIR` auf einen absoluten Verzeichnispfad gesetzt werden.

Der Route-Code schützt die Bearbeitung in der Oberfläche. Den Zugriff auf die Bibliothek selbst regeln die Windows-Dateiberechtigungen. Bestehende Zugangscodes funktionieren weiter. Wenn du sie unter **Werkzeuge → Zugangscodes verwalten** neu speicherst, verwendet das Studio für beide Prüfsummen PBKDF2-HMAC-SHA‑256 mit 600.000 Iterationen und jeweils eigenem zufälligem Salt.

Beim Import prüft das Studio die SHA-256-Prüfsummen der Audiodateien gegen das Paketmanifest. Die Herkunft eines USB-Pakets lässt sich damit nicht bestätigen.

## In CLion bauen

Öffne die `CMakeLists.txt` in CLion und wähle die mitgelieferte MinGW-Toolchain für Windows x64. Das Startziel heißt **GeoVoiceStudio**. Für die verteilbare EXE verwende das Profil **Release**.

Mit CMake, Ninja und MinGW-w64 im Suchpfad:

```powershell
cmake -S . -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release
ctest --test-dir cmake-build-release --output-on-failure
```

Die EXE liegt danach in `cmake-build-release/GeoVoiceStudio.exe`. ZIP und JSON werden durch mitgelieferte C-Bibliotheken verarbeitet. Die Lizenztexte sind auch als Ressourcen in der EXE enthalten. [Quellen und Lizenztexte](vendor/SOURCES.md)
