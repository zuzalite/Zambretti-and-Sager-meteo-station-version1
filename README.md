# Zambretti-and-Sager-meteo-station-version1

# Mikrokontrolleres Meteorológiai Állomás (Zambretti & Sager Előrejelzéssel)

Ez a projekt egy ESP32/Pico W alapú, OLED kijelzővel (SSD1306) ellátott intelligens időjárás állomás. A rendszer nemcsak a helyi szenzoradatokat (hőmérséklet, légnyomás) gyűjti és jeleníti meg a ThingSpeak API-n keresztül, hanem az Open-Meteo API szélirány-adatait, valamint visszamenőleges (6 és 12 órás) légnyomás-trendeket felhasználva komplex meteorológiai előrejelzéseket (Zambretti és Sager algoritmusok) készít, speciálisan a helyi viszonyokra (pl. Vérmező) finomhangolva.

---

## 1. Korábbi hibák és az alkalmazott javítások

A kód jelenlegi verziója az alábbi kritikus matematikai, hálózati és logikai finomításokat tartalmazza:

### 🛠️ 1. Hálózati stabilitás és API limitációk elkerülése
* **Hiba:** A ThingSpeak szerverei hajlamosak megszakítani a kapcsolatot, ha túl gyorsan, egymás után kapják a lekéréseket (Rate Limiting).
* **Javítás:** Biztonsági hálózati szünetek (`delay(200)` és `delay(1000)`) kerültek beépítésre a historikus és az egyedi (Fallback) HTTP kérések közé, így a letöltés stabil és megbízható maradt.

### 🛠️ 2. ISO 8601 Időbélyeg szabványosítás
* **Hiba:** A ThingSpeak API visszamenőleges (macro) adatlekérése (12 és 6 órás adatok) pontatlan vagy sikertelen lehetett a rosszul formázott dátumok miatt.
* **Javítás:** A kód most már szigorúan az ISO 8601 szabványt használja (`%Y-%m-%dT%H:%M:%SZ`), a `T` és a `Z` (UTC) azonosítók beépítésével, így az API mindig hajszálpontosan a kívánt múltbeli adatot adja vissza.

### 🛠️ 3. Matematikai ablakok és tömbméretek korrekciója
* **Hiba:** A percalapú historikus adatok tárolásánál a tömbök méretezése nem fedte le teljesen a vizsgálni kívánt időablakot (pl. a 60 perces trendhez nem elég 60 elem, ha a kezdő- és végpontot is pontosan akarjuk kivonni).
* **Javítás:** A nyomástörténeti tömbök mérete `61` elemre, a hőmérsékleti tömböké `11` elemre módosult. Így a tiszta időkülönbség (delta) matematikai kiszámítása torzításmentes lett.

### 🛠️ 4. Sager kijelzési anomália (A "-0.0" hiba)
* **Hiba:** Ha a légnyomásváltozás 6 óra alatt mikroszkopikus mértékű volt, de negatív irányú, a kijelzőn zavaró `-0.0` érték jelent meg.
* **Javítás:** Beépítésre került egy küszöbérték-vizsgálat (`abs(diff_6h) < 0.05`), ami ezeket a jelentéktelen fluktuációkat tiszta `0.0`-ra kerekíti.

### 🛠️ 5. Biztonságos JSON feldolgozás
* **Hiba:** A nyers API válaszok manuális (String alapú) darabolása sérülékeny és memóriaszivárgást okozhat.
* **Javítás:** Az ipari standard `ArduinoJson` könyvtár integrálása, amely robusztusan és hibatűrően bontja ki az Open-Meteo és a ThingSpeak bonyolult JSON válaszait.

---

## 2. A kód működése és architektúrája

A program egy végtelen ciklusban (`loop`) futó állapotgép, amely időzítők (`millis()`) alapján ütemezi a feladatait a blokkoló (megakasztó) várakozások elkerülése érdekében.

### A működési folyamat lépései:

1. **Rendszerindítás (Boot folyamat):**
   * Az OLED kijelzőn megjelenik egy státuszképernyő.
   * A rendszer csatlakozik a WiFi-hez.
   * Szinkronizálja a belső órát NTP szervereken keresztül (Google, Pool.ntp).
   * Megállapítja az aktuális évszakot (nyár/tél a Zambretti kalibrációhoz).
   * Letölti a kezdeti adatokat (ThingSpeak és Open-Meteo).

2. **Adatgyűjtési ciklusok (Polling):**
   * **60 másodpercenként:** Letölti a legfrissebb beltéri, kültéri és légnyomás adatokat a ThingSpeak-ről. Kiszámolja a zajszűrt mozgóátlagot (`MA_WINDOW`).
   * **15 percenként:** Frissíti a szélirányt az Open-Meteo API-ról, amely kritikus a lokális előrejelzésekhez (a kód tartalmaz egy egyedi, helyi viszonyokra szabott `WIND_MULTIPLIER` változót).

3. **Előrejelzési algoritmusok:**
   * **Zambretti:** A jelenlegi nyomás, a rövid távú (1 órás) trend, a 12 órával ezelőtti makro-trend, a szélirány és az évszak (szezonális eltolás) kombinációjából szöveges prognózist generál.
   * **Sager:** Egy specifikusabb módszer, amely szigorúan a 6 órás légnyomásváltozás (`pressure6h`) meredekségét és a nyomás abszolút szintjét (magas, normál, alacsony) vizsgálva ad előrejelzést a következő 24 órára.

4. **Kijelző (OLED) rotációja:**
   Az adatok három külön képernyőn, automatikusan váltakozva jelennek meg:
   * **1. Képernyő (Alapadatok - 7 mp):** Beltéri/kültéri hőmérséklet, hőmérséklet-különbség (Delta), aktuális légnyomás és apró trendnyilak (`^`, `v`, `-`).
   * **2. Képernyő (Zambretti - 4 mp):** Aktuális szélirány és a Zambretti algoritmus által számolt szöveges előrejelzés.
   * **3. Képernyő (Sager - 4 mp):** A 6 órás nyomáskülönbség pontos értéke és a Sager algoritmus diagnózisa.

---

## 3. Konfiguráció (Felhasználói beállítások)

A kód elején található konstansok szabadon módosíthatók a helyi mikroklíma további finomhangolásához:
* `STORM_THRESHOLD` / `BAD_WEATHER_THRESHOLD`: A nyomásesés sebességének riasztási határai.

* ---

## Hardware Setup & Wiring

The system uses an **SSD1306 128x64 OLED display** communicating via the **I2C protocol**. By default, the Arduino wire library on the Raspberry Pi Pico W initializes the default `I2C0` bus.

Connect your OLED display to the Pico W using the following pinout configuration:

| OLED Pin | Pico W Pin Name | Pico W Physical Pin | Description |
| :--- | :--- | :--- | :--- |
| **VCC** | 3V3(OUT) | Pin 36 | 3.3V Power Supply |
| **GND** | GND | Pin 38 (or any GND) | Ground |
| **SDA** | GP4 | Pin 6 | I2C Data Line |
| **SCL** | GP5 | Pin 7 | I2C Clock Line |

---

* `PRESSURE_EXTREME_HIGH` / `PRESSURE_EXTREME_LOW`: Az adott tengerszint feletti magasságra jellemző extrém nyomásértékek.
* `WIND_MULTIPLIER`: Speciális szorzó, amely a helyi (pl. völgyekben vagy domboldalakon jelentkező) szélcsatorna-hatásokat kompenzálja.
