import tkinter as tk
import requests
from datetime import datetime, timedelta
from collections import deque

# ==============================================================================
# --- FELHASZNÁLÓI BEÁLLÍTÁSOK (KONFIGURÁCIÓ) ---
# ==============================================================================
# HÁLÓZATI ÉS API BEÁLLÍTÁSOK
CHANNEL_NUMBER = "xxxxxxxxx"
READ_API_KEY = "xxxxxxxxxxx"

# METEOROLÓGIAI KÜSZÖBÉRTÉKEK (A TRÉNING ALAPJÁN FINOMHANGOLHATÓ)
STORM_THRESHOLD         = -1.8    # Viharjelzés küszöbérték (hPa / 5 perc)
BAD_WEATHER_THRESHOLD   = -2.0    # Időjárás-romlás küszöb (hPa / 60 perc)
SUNNY_CLEAR_THRESHOLD   = 0.5     # Tiszta/napos idő küszöb (hPa / 60 perc)
SLOW_IMPROV_THRESHOLD   = 0.2     # Lassú javulás küszöb (hPa / 60 perc)
SEASONAL_OFFSET         = -0.43   # Szezonális eltolás mértéke
WIND_MULTIPLIER         = 0.3     # V VÉRMEZŐRE OPTIMALIZÁLT SZÉLIRÁNY-SZORZÓ V (Javítva: 0.3)

# ABSZOLÚT NYOMÁSI ZÓNÁK (hPa)
PRESSURE_EXTREME_HIGH   = 1030.3  # Extrém magas nyomás határ (Anticiklon)
PRESSURE_STANDARD_MID   = 1016.7  # Standard tengerszinti alapérték
PRESSURE_EXTREME_LOW    = 1003.8  # Extrém alacsony nyomás határ (Ciklon)

# IDŐZÍTÉSEK ÉS MATEMATIKAI ABLAKOK (EZREDMÁSODPERCBEN - MS)
UPDATE_INTERVAL_MS     = 60000    # ThingSpeak adatletöltési gyakoriság (60 mp)
WIND_CHECK_INTERVAL_MS = 900000   # Szélirány és szezon frissítése (15 perc)
SCREEN_1_DURATION_MS   = 7000     # 1. képernyő (Alapadatok) láthatósága (7 mp)
SCREEN_2_DURATION_MS   = 4000     # 2. képernyő (Előrejelzés) láthatósága (4 mp)
MA_WINDOW              = 5        # Szenzorzaj-szűrés mozgóátlag ablaka (elem)
# ==============================================================================

# --- GLOBAL VARIABLES ---
in_temp = 0.0
out_temp = 0.0
pressure = 0.0
pressure_12h = 0.0 
pressure_6h = 0.0 # ÚJ: Sager 6 órás időablakhoz

# A C++ 61 és 11 elemű tömbjeinek megfelelő dequek (automatikus léptetéssel)
pressure_history = deque([0.0] * 61, maxlen=61)
pressure_ma = deque([0.0] * 61, maxlen=61)
temp_in_history = deque([0.0] * 11, maxlen=11)
temp_out_history = deque([0.0] * 11, maxlen=11)

current_wind_dir = "N"
is_barometric_crash = False
history_ready = False
is_summer = True
current_screen = 1

# Boot Screen státuszok
wifi_status_str = "WAIT"
open_meteo_str  = "WAIT"
ts_status_str   = "WAIT"

# --- FUNCTIONS ---

def update_season():
    global is_summer
    month = datetime.now().month
    is_summer = (3 <= month <= 9)

def fetch_wind_direction():
    global current_wind_dir, open_meteo_str
    try:
        url = "http://api.open-meteo.com/v1/forecast?latitude=47.4979&longitude=19.0402&current_weather=true"
        response = requests.get(url, timeout=5)
        if response.status_code == 200:
            data = response.json()
            wind_deg = data["current_weather"]["winddirection"]
            
            if 337.5 <= wind_deg or wind_deg < 22.5:       current_wind_dir = "N"
            elif 22.5 <= wind_deg and wind_deg < 67.5:     current_wind_dir = "NE"
            elif 67.5 <= wind_deg and wind_deg < 112.5:    current_wind_dir = "E"
            elif 112.5 <= wind_deg and wind_deg < 157.5:   current_wind_dir = "SE"
            elif 157.5 <= wind_deg and wind_deg < 202.5:   current_wind_dir = "S"
            elif 202.5 <= wind_deg and wind_deg < 247.5:   current_wind_dir = "SW"
            elif 247.5 <= wind_deg and wind_deg < 292.5:   current_wind_dir = "W"
            else:                                          current_wind_dir = "NW"
            
            open_meteo_str = "OK"
            return True
        else:
            open_meteo_str = f"HTTP {response.status_code}"
    except Exception as e:
        open_meteo_str = "NOK (No Net)"
    return False

def get_temp_trend_char(current, past):
    if past == 0.0: return '-'
    delta = current - past
    if delta >= 0.20: return '^'
    if delta <= -0.20: return 'v'
    return '-'

def get_press_trend_char(current, past):
    if past == 0.0: return '-'
    delta = current - past
    if delta >= 0.10: return '^'
    if delta <= -0.10: return 'v'
    return '-'

def fetch_latest_data():
    global in_temp, out_temp, pressure, pressure_12h, pressure_6h, ts_status_str
    try:
        indoor_found = False
        outdoor_found = False
        pressure_found = False
        
        # 1. Mikro-trend lekérése (4 perces ablak)
        url_window = f"https://api.thingspeak.com/channels/{CHANNEL_NUMBER}/feeds.json?api_key={READ_API_KEY}&minutes=4"
        response = requests.get(url_window, timeout=5)
        
        if response.status_code == 200:
            data = response.json()
            feeds = data.get("feeds", [])
            
            if feeds:
                # Legutolsó elem a hőmérsékletekhez
                last_feed = feeds[-1]
                if last_feed.get("field1") is not None and str(last_feed["field1"]).lower() != "null":
                    in_temp = float(str(last_feed["field1"]).replace(',', '.'))
                    indoor_found = True
                    
                if last_feed.get("field5") is not None and str(last_feed["field5"]).lower() != "null":
                    out_temp = float(str(last_feed["field5"]).replace(',', '.'))
                    outdoor_found = True
                    
                # 4 perces ablak átlagolása a nyomáshoz
                valid_pressures = []
                for f in feeds:
                    if f.get("field3") is not None and str(f["field3"]).lower() != "null":
                        p = float(str(f["field3"]).replace(',', '.'))
                        if 950.0 < p < 1050.0:
                            valid_pressures.append(p)
                            
                if valid_pressures:
                    pressure = sum(valid_pressures) / len(valid_pressures)
                    pressure_found = True

        # Makro-trend lekérése (12 órás) ISO 8601 UTC szerint (ZAMBRETTI-HEZ)
        try:
            time_12h_ago = (datetime.utcnow() - timedelta(hours=12)).strftime('%Y-%m-%dT%H:%M:%SZ')
            url_macro = f"https://api.thingspeak.com/channels/{CHANNEL_NUMBER}/fields/3.json?api_key={READ_API_KEY}&end={time_12h_ago}&results=1"
            res_macro = requests.get(url_macro, timeout=5)
            if res_macro.status_code == 200:
                data_macro = res_macro.json()
                feeds_macro = data_macro.get("feeds", [])
                if feeds_macro and feeds_macro[0].get("field3") is not None and str(feeds_macro[0]["field3"]).lower() != "null":
                    p12 = float(str(feeds_macro[0]["field3"]).replace(',', '.'))
                    if 950.0 < p12 < 1050.0:
                        pressure_12h = p12
        except Exception as e:
            pass

        # ÚJ: 6 órás trend lekérése ISO 8601 UTC szerint (SAGER-HEZ)
        try:
            time_6h_ago = (datetime.utcnow() - timedelta(hours=6)).strftime('%Y-%m-%dT%H:%M:%SZ')
            url_6h = f"https://api.thingspeak.com/channels/{CHANNEL_NUMBER}/fields/3.json?api_key={READ_API_KEY}&end={time_6h_ago}&results=1"
            res_6h = requests.get(url_6h, timeout=5)
            if res_6h.status_code == 200:
                data_6h = res_6h.json()
                feeds_6h = data_6h.get("feeds", [])
                if feeds_6h and feeds_6h[0].get("field3") is not None and str(feeds_6h[0]["field3"]).lower() != "null":
                    p6 = float(str(feeds_6h[0]["field3"]).replace(',', '.'))
                    if 950.0 < p6 < 1050.0:
                        pressure_6h = p6
        except Exception as e:
            pass

        # 2. Biztonsági Háló (Fallback) - Pontosan mint a C++ kódban
        if not indoor_found:
            res = requests.get(f"https://api.thingspeak.com/channels/{CHANNEL_NUMBER}/fields/1/last.json?api_key={READ_API_KEY}", timeout=3)
            if res.status_code == 200 and res.json().get("field1") not in [None, "null"]:
                in_temp = float(res.json()["field1"])
                indoor_found = True
                
        if not outdoor_found:
            res = requests.get(f"https://api.thingspeak.com/channels/{CHANNEL_NUMBER}/fields/5/last.json?api_key={READ_API_KEY}", timeout=3)
            if res.status_code == 200 and res.json().get("field5") not in [None, "null"]:
                out_temp = float(res.json()["field5"])
                outdoor_found = True
                
        if not pressure_found:
            res = requests.get(f"https://api.thingspeak.com/channels/{CHANNEL_NUMBER}/fields/3/last.json?api_key={READ_API_KEY}", timeout=3)
            if res.status_code == 200 and res.json().get("field3") not in [None, "null"]:
                p3 = float(res.json()["field3"])
                if 950.0 < p3 < 1050.0:
                    pressure = p3
                    pressure_found = True

        if indoor_found or outdoor_found or pressure_found:
            ts_status_str = "OK"
            return True
        else:
            ts_status_str = "ERROR"
            return False
            
    except Exception as e:
        ts_status_str = "NOK (No Net)"
        return False

def update_local_history():
    global history_ready, is_barometric_crash
    
    # A deque automatikusan eltolja az elemeket, ahogy a C++ for ciklusai teszik
    temp_in_history.append(in_temp)
    temp_out_history.append(out_temp)
    pressure_history.append(pressure)

    # Mozgóátlag számítása a MA_WINDOW (5) alapján
    sum_p = 0.0
    count = 0
    # pressure_history listává alakítva az indexeléshez
    p_list = list(pressure_history)
    for i in range(61 - MA_WINDOW, 61):
        if p_list[i] > 950.0:
            sum_p += p_list[i]
            count += 1
    
    ma_val = sum_p / count if count > 0 else pressure
    pressure_ma.append(ma_val)

    # Barometric crash 5 perces ablakban
    p_ma_list = list(pressure_ma)
    short_trend = p_ma_list[60] - p_ma_list[55]
    if short_trend <= STORM_THRESHOLD and p_ma_list[55] > 950.0:
        is_barometric_crash = True
    else:
        is_barometric_crash = False

    valid = sum(1 for p in p_list if p > 950.0)
    if valid >= MA_WINDOW:
        history_ready = True

def get_forecast_text():
    p_ma_list = list(pressure_ma)
    if not history_ready or p_ma_list[60] < 950.0: return "COLLECTING..."
    if is_barometric_crash: return "STORM WARNING"
    
    p = p_ma_list[60]
    raw_trend = p_ma_list[60] - p_ma_list[0]
    
    wind_mod = 0.0
    if current_wind_dir in ["S", "SW"]: wind_mod = 2.0
    elif current_wind_dir in ["SE", "W"]: wind_mod = 0.5
    elif current_wind_dir == "NW": wind_mod = 0.6 if raw_trend < 0 else -0.2
    elif current_wind_dir in ["E", "NE", "N"]: wind_mod = -0.6
    
    trend = raw_trend - (wind_mod * WIND_MULTIPLIER)
    seasonal_factor = -SEASONAL_OFFSET if is_summer else SEASONAL_OFFSET
    
    # 12 ÓRÁS MAKRO DÖNTÉSI MÁTRIX INTEGRÁCIÓ
    if pressure_12h > 950.0:
        macro_trend = p - pressure_12h
        if macro_trend <= -3.0 and trend <= BAD_WEATHER_THRESHOLD: return "CYCLONE / STORM"
        if macro_trend >= 3.0 and trend <= BAD_WEATHER_THRESHOLD:  return "PASSING FRONT"
        if macro_trend <= -3.0 and trend >= 0.5:                   return "SLOW IMPROV."
        
    if trend <= STORM_THRESHOLD + seasonal_factor: return "STORMY RAIN" if p < 1005.0 else "RAIN/WEATHER"
    if trend <= BAD_WEATHER_THRESHOLD: return "BAD WEATHER"
    if trend >= SUNNY_CLEAR_THRESHOLD + seasonal_factor: return "SUNNY/CLEAR"
    if trend >= SLOW_IMPROV_THRESHOLD: return "SLOW IMPROV."
    
    if p >= PRESSURE_EXTREME_HIGH: return "STABLE SUNNY" if is_summer else "COLD/FOGGY ANTI"
    if p >= PRESSURE_STANDARD_MID: return "SUNNY/DRY" if is_summer else "CLOUDY/DRY"
    if p <= PRESSURE_EXTREME_LOW:  return "LOW/HEAVY STORM"
    
    return "STABLE/FAIR"

# ÚJ FÜGGVÉNY: Sager-féle Mátrix Kiértékelése
def get_sager_forecast_text():
    if pressure_6h < 950.0 or pressure < 950.0:
        return "COLLECTING 6H DATA..."

    diff_6h = pressure - pressure_6h
    
    # Zónák meghatározása dinamikusan a konfigurációd alapján
    p_level = "Normál"
    if pressure < (PRESSURE_STANDARD_MID + PRESSURE_EXTREME_LOW) / 2:
        p_level = "Alacsony"
    elif pressure > (PRESSURE_STANDARD_MID + PRESSURE_EXTREME_HIGH) / 2:
        p_level = "Magas"
        
    abs_d6h = abs(diff_6h)
    speed = "Lassú"
    if 1.0 <= abs_d6h < 2.0: speed = "Mérsékelt"
    elif 2.0 <= abs_d6h < 3.5: speed = "Gyors"
    elif abs_d6h >= 3.5: speed = "Drasztikus"

    if diff_6h <= -0.2: # Trend csökken
        if p_level == "Magas":
            return "Szép idő vége,\nnövekvő felhőzet" if speed in ["Mérsékelt", "Lassú"] else "Erősödő szél,\neső közeledik"
        elif p_level == "Normál":
            return "Időjárás-romlás,\nszeles eső" if speed in ["Gyors", "Drasztikus"] else "Lassú felhősödés,\ncsapadék jöhet"
        else:
            return "Viharos, tartós\nrossz idő" if speed in ["Gyors", "Drasztikus"] else "Eső, instabil\nzivataros légkör"
            
    elif diff_6h >= 0.2: # Trend emelkedik
        if p_level == "Alacsony":
            return "Vihar elvonult,\nszeles tisztulás" if speed in ["Gyors", "Drasztikus"] else "Lassú, bizonytalan\njavulás"
        elif p_level == "Normál":
            return "Határozott tisztulás,\nszép idő jön" if speed in ["Gyors", "Drasztikus"] else "Lassan javuló\nfeltételek"
        else:
            return "Tartósan stabil,\nderült, száraz idő"
            
    else: # Stagnál
        return "Stabil helyzet,\nnincs változás (24h)"

# --- TIMERS & UI UPDATES ---

def draw_boot_screen(current_action):
    text = (
        f"--- SYSTEM START ---\n"
        f"-------------------------------------\n\n"
        f"WiFi Network: {wifi_status_str}\n"
        f"Open-Meteo:   {open_meteo_str}\n"
        f"ThingSpeak:   {ts_status_str}\n\n"
        f"-------------------------------------\n"
        f">{current_action}"
    )
    lbl_display.config(text=text)
    root.update()

def network_update_loop():
    if fetch_latest_data():
        update_local_history()
    update_display()
    root.after(UPDATE_INTERVAL_MS, network_update_loop)

def wind_update_loop():
    fetch_wind_direction()
    update_season()
    root.after(WIND_CHECK_INTERVAL_MS, wind_update_loop)

def screen_switch_loop():
    global current_screen
    # ÚJ LOGIKA: A kijelző 3 képernyő között vált (Szenzorok -> Zambretti -> Sager)
    if current_screen == 1:
        current_screen = 2
        delay = SCREEN_2_DURATION_MS
    elif current_screen == 2:
        current_screen = 3
        delay = SCREEN_2_DURATION_MS # A Sager előrejelzés is annyi ideig látszik, mint a Zambretti
    else:
        current_screen = 1
        delay = SCREEN_1_DURATION_MS
    update_display()
    root.after(delay, screen_switch_loop)

def update_display():
    if current_screen == 1:
        t_in_list = list(temp_in_history)
        t_out_list = list(temp_out_history)
        p_ma_list = list(pressure_ma)
        
        trend_in = get_temp_trend_char(t_in_list[10], t_in_list[0])
        trend_out = get_temp_trend_char(t_out_list[10], t_out_list[0])
        trend_p = get_press_trend_char(p_ma_list[60], p_ma_list[50])
        
        diff = in_temp - out_temp
        diff_sign = "+" if diff >= 0 else ""
        
        text = (
            f"Indoor:  {in_temp:.1f} C {trend_in}\n\n"
            f"Outdoor: {out_temp:.1f} C {trend_out}\n\n"
            f"Delta:   {diff_sign}{diff:.1f} C\n\n"
            f"Baro:    {pressure:.1f} hPa {trend_p}"
        )
    elif current_screen == 2:
        season_str = "ZAMB(SUMMER)" if is_summer else "ZAMB(WINTER)"
        text = (
            f"{season_str}|Wind:{current_wind_dir}\n"
            f"-------------------------------------\n\n"
            f"{get_forecast_text()}"
        )
    elif current_screen == 3: # ÚJ KÉPERNYŐ A SAGERNEK
        diff_6h = pressure - pressure_6h if pressure_6h > 950 else 0.0
        
        # JAVÍTVA: Mikroszkopikus változások kerekítése 0.0-ra (A -0.0 hiba elkerülése)
        if abs(diff_6h) < 0.05:
            diff_6h = 0.0
            
        trend_sign = "+" if diff_6h >= 0 else ""
        text = (
            f"SAGER | 6h Tr:{trend_sign}{diff_6h:.1f}\n"
            f"-------------------------------------\n\n"
            f"{get_sager_forecast_text()}"
        )
        
    lbl_display.config(text=text)

def boot_sequence():
    global wifi_status_str
    
    draw_boot_screen("System booting...")
    root.after(1000)
    
    # WiFi szimulálása
    wifi_status_str = "OK"
    draw_boot_screen("WiFi connected!")
    root.after(500)
    
    draw_boot_screen("Syncing NTP Time...")
    update_season()
    root.after(500)
    
    draw_boot_screen("ThingSpeak fetch...")
    fetch_latest_data()
    root.after(500)
    
    draw_boot_screen("Open-Meteo fetch...")
    fetch_wind_direction()
    root.after(500)
    
    draw_boot_screen("All ready!")
    if ts_status_str == "OK":
        # Kezdeti feltöltés (11 és 61 elem)
        global temp_in_history, temp_out_history, pressure_history, pressure_ma
        temp_in_history = deque([in_temp] * 11, maxlen=11)
        temp_out_history = deque([out_temp] * 11, maxlen=11)
        pressure_history = deque([pressure] * 61, maxlen=61)
        pressure_ma = deque([pressure] * 61, maxlen=61)
        
    root.after(2000, start_loops)

def start_loops():
    update_display()
    root.after(UPDATE_INTERVAL_MS, network_update_loop)
    root.after(WIND_CHECK_INTERVAL_MS, wind_update_loop)
    root.after(SCREEN_1_DURATION_MS, screen_switch_loop)

# --- GUI SETUP (Szimulált OLED) ---
root = tk.Tk()
# JAVÍTVA: Az ablak címe most már illeszkedik a projekt kontextusához
root.title("Pasarét Meteorológiai Állomás - OLED Szimulátor")
root.geometry("380x240")
root.configure(bg="black")
root.resizable(False, False)

lbl_display = tk.Label(
    root, 
    text="--- SYSTEM START ---\n\nSystem booting...", 
    font=("Consolas", 16, "bold"), 
    fg="cyan", 
    bg="black", 
    justify="left",
    anchor="nw",
    padx=20,
    pady=20
)
lbl_display.pack(fill="both", expand=True)

# Boot szekvencia indítása 0.5 mp után
root.after(500, boot_sequence)
root.mainloop()
