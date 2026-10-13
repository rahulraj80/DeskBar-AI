# 📖 TuneBar User Guide & Interaction Reference

TuneBar Advanced Edition transforms the Waveshare ESP32-S3-Touch-LCD-3.49 into a responsive, autonomous desktop smart assistant and media bar.

---

## 👆 Touch Gestures & Navigation Hierarchy

```
                [ CLOCK / WEATHER / ALARM ] (ui_Screen_Info)
                             │
                      Swipe UP (↑)
                             ▼
                [ MAIN MENU CAROUSEL ] (ui_Screen_MainMenu)
       [CLIMATE]  [RADIO]  [MUSIC]  [AI VOICE]  [SYSTEM]  [SETTINGS]
          │          │        │         │          │          │
          ▼          ▼        ▼         ▼          ▼          ▼
       Weather     Radio    Player  AI Voice    Utility    Settings
       Screen     Screen    Screen   Screen      Screen     Overlay
```

### From Clock / Weather / Alarm Panels (`ui_Screen_Info`):
- **Swipe Left / Right ($\leftarrow / \rightarrow$)**: Cycle seamlessly between the Nixie Clock, Live Weather & AQI, and Retro Alarm panels.
- **Swipe Up ($\uparrow$)**: Dismiss the ambient screen and open the Main Menu launcher carousel.
- **Tap Screen**: Wakes display from ambient breathing mode to full configured active brightness.

### From Main Menu Carousel (`ui_Screen_MainMenu`):
- **Drag Left / Right ($\leftarrow / \rightarrow$)**: Smoothly pan across the 6 neon application cards: **CLIMATE**, **RADIO**, **MUSIC**, **AI VOICE**, **SYSTEM**, and **SETTINGS**.
- **Tap Any Card**: Launch that application immediately.
- **Swipe Down ($\downarrow$)**: Quick return to the ambient Nixie Clock face.

### From Music & LAN Player:
- **Tap Album Card (Top Left)**: Toggle instantly between **LOCAL SD** (purple neon glow) and **LAN STREAM** (cyan neon glow).
- **Playback Controls**: Play/Pause, Next Track ($▶▶$), Previous Track ($◀◀$), Seek Forward (+5s), Seek Rewind (-5s), Shuffle mode toggle, Volume slider.
- **Swipe Up ($\uparrow$)**: Harmonized dismiss gesture — returns smoothly to the Main Menu while audio continues playing in the background.

### From Global Radio Player:
- **Tap Catalog Badge (`[ ♫ EN ]`, etc.)**: Cycle through available presets or switch catalogs.
- **Transport Buttons**: Play/Pause, Next/Previous station.
- **Swipe Up ($\uparrow$)**: Return to Main Menu while radio streaming continues in the background.

### From AI Voice Assistant:
- **Tap Microphone Icon**: Initiate voice capture. Speak naturally; hardware VAD automatically detects when you stop speaking.
- **Swipe Up ($\uparrow$)**: Dismiss assistant to Main Menu.

### From System Utility Screen:
- **Tap Torch**: Activates full 100% white screen flashlight. Tap anywhere on the screen to dismiss.
- **Tap System Info**: Opens deep hardware diagnostic dashboard (DRAM/PSRAM gauges, task high-water marks, battery metrics).
- **Tap Spirit Level**: Opens 3-panel 6-axis inclinometer and bubble leveling tool.
- **Tap Firmware OTA**: Opens on-device firmware manager and community release catalog.
- **Swipe Up ($\uparrow$)**: Return to Main Menu.

---

## ⚙️ Device Settings Suite

Access via the **SETTINGS** card on the Main Menu:

1. **Wi-Fi Tab**:
   - Live 2.4GHz network scan. Select SSID from dropdown, input password using on-screen QWERTY keyboard, and save.
   - Automatically stores up to 10 network credentials in non-volatile flash (NVS).
2. **Screen Tab (Zero-Bitmap Vector Themes)**:
   - Configure brightness presets: Low (35%), Medium (65%), High (100%).
   - Set inactivity sleep timeouts: 15s, 30s, 1m, 2m, 5m, 10m, or Never.
   - Select from **7 procedural vector themes** (`Pure Dark`, `Cyberpunk`, `Deep Ocean`, `Emerald`, `Sunset`, `Carbon`, `Midnight Blue`) rendered with zero flash footprint.
3. **Radio Tab**:
   - Change international radio catalog language (`HI`, `EN`, `ES`, `CN`, `DE`, `JA`).
   - Insert MicroSD card with `stations.csv` and click **SD Load** to import custom streams.
4. **Music Tab**:
   - Toggle default startup music source (**LOCAL** vs **LAN**).
   - Trigger MicroSD indexing scan.
   - Configure LAN HTTP streaming server URL (`IP:PORT/PATH`).
5. **Region Tab**:
   - Enter latitude/longitude coordinates or select *Auto by IP*.
   - Adjust UTC hour and minute offset wheels for instant PCF85063 RTC time sync.
   - Switch temperature display between Celsius ($^\circ\text{C}$) and Fahrenheit ($^\circ\text{F}$).
6. **User Guide Tab**:
   - Built-in on-device documentation reference.
7. **Dismiss**:
   - Tap top-right **[X]** button to persist settings to flash and return to the main carousel.
