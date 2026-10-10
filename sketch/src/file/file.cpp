//=================== File System ===========================
#include "file.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "lvgl.h"
#include "task_msg/task_msg.h"
#include "ui/ui.h"
#include <Arduino.h>
#include "esp_system.h"
#include <LittleFS.h>
#include <driver/i2s_std.h>
#include <vector>
#include <Preferences.h>

int trackListLength = 0; // NEW: Definition of global track length
uint16_t trackIndex = 0;
uint8_t mediaType = 0; // 0: livestream, 1: music player, 2: chatbot, 3: config
uint8_t playMode = 0; // 0 = normal, 1 = random , 2 = repeat

static TaskHandle_t scanMusicTask = NULL;

#define PATH_BUF_LEN 512

//------------ LVGL 8.x LittleFS callbacks  -------------------------

void *fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode) {
  // 1. Allocate a File object on the heap. This will be the pointer returned to LVGL.
  File *fp = new File();
  // 2. Determine the open mode string.
  const char *open_mode = (mode == LV_FS_MODE_RD) ? "r" : "w";
  // 3. Open the file directly into the heap-allocated object (*fp).
  *fp = LittleFS.open(path, open_mode);
  // 4. Check for failure.
  if (!*fp) {
    delete fp; // Crucial: Clean up the heap allocation if opening failed.
    return nullptr;
  }
  // 5. Success. Return the heap-allocated pointer.
  return fp;
}

lv_fs_res_t fs_close(lv_fs_drv_t *drv, void *file_p) {
  if (!file_p) return LV_RES_INV;
  File *fp = (File *)file_p;
  fp->close();
  delete fp;
  return LV_FS_RES_OK;
}

lv_fs_res_t fs_read(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br) {
  if (!file_p) return LV_RES_INV;
  File *fp = (File *)file_p;
  *br = fp->read((uint8_t *)buf, btr);
  return LV_FS_RES_OK;
}

lv_fs_res_t fs_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence) {
  if (!file_p) return LV_RES_INV;
  File *fp = (File *)file_p;
  SeekMode mode;
  // 1. Map the LVGL 'whence' (reference point) to the Arduino 'SeekMode'
  switch (whence) {
  case LV_FS_SEEK_SET:
    mode = SeekSet; // Start of file
    break;
  case LV_FS_SEEK_CUR:
    mode = SeekCur; // Current position
    break;
  case LV_FS_SEEK_END:
    mode = SeekEnd; // End of file
    break;
  default: return LV_RES_INV;
  }
  // 2. Call the correct File::seek() overload
  fp->seek(pos, mode);
  return LV_FS_RES_OK;
}

//------------------------------------------------
// init LittleFS
void initLittleFS() {
  bool mounted = LittleFS.begin(false, "/littlefs", 10, "spiffs");
  if (!mounted) {
    log_w("LittleFS mount failed on 'spiffs'. Formatting...");
    mounted = LittleFS.begin(true, "/littlefs", 10, "spiffs");
    if (!mounted) {
      log_e("LittleFS format and mount failed!");
      return;
    }
    log_i("LittleFS formatted and mounted successfully.");
  } else {
    log_i("LittleFS mounted successfully.");
  }

  // Register LVGL filesystem driver only if LVGL has been initialized
  if (lv_is_initialized()) {
    static lv_fs_drv_t drv;
    static bool drv_registered = false;
    if (!drv_registered) {
      lv_fs_drv_init(&drv);
      drv.letter = 'L';
      drv.open_cb = fs_open;
      drv.close_cb = fs_close;
      drv.read_cb = fs_read;
      drv.seek_cb = fs_seek;
      lv_fs_drv_register(&drv);
      drv_registered = true;
      log_i("LVGL LittleFS driver ('L:') registered.");
    }
  }
}
//------------------------------------------------
// init sd card
void initSDCard() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  SPI.setFrequency(4000000);
  delay(100);
  if (!SD.begin(SD_CS, SPI)) {
    log_w("SD Card Mount Failed");
    updateSDCARDStatus(LV_SYMBOL_CLOSE " Card Mount Failed", 0x777777);
  } else {
    log_d("SD Card Mounted");
    updateSDCARDStatus(LV_SYMBOL_SD_CARD " SDCard Mounted", 0x00FF00);
  }
}

// ==========================================
// Global Variables for Music List
// ==========================================
// std::vector acts like a growable array.
// It will store the full path strings to your songs.
static bool endsWithIgnoreCase(const char *str, const char *suffix) {
  size_t len1 = strlen(str);
  size_t len2 = strlen(suffix);
  if (len2 > len1) return false;

  str += len1 - len2;
  while (*suffix) {
    if (tolower((unsigned char)*str++) != tolower((unsigned char)*suffix++)) return false;
  }
  return true;
}

static bool isAudioFile(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return false;

    return strcasecmp(ext, ".mp3") == 0 ||
           strcasecmp(ext, ".wav") == 0 ||
           strcasecmp(ext, ".aac") == 0 ||
           strcasecmp(ext, ".flac") == 0;
}


// ==========================================
// NEW: Recursive Directory Scanner that SAVES TO FILE
// ==========================================
static void scanDirRecursive(
    fs::FS &fs,
    const char *currentDir,
    uint8_t level,
    File &playlist,
    char *pathBuf,
    size_t pathBufLen
) {
    File dir = fs.open(currentDir);
    if (!dir || !dir.isDirectory()) {
        dir.close();
        return;
    }

    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;

        const char *entryPath = entry.name();  // FULL path on SD
        const char *base = strrchr(entryPath, '/');
        base = base ? base + 1 : entryPath;    // basename ONLY

        // Build full child path into pathBuf
        if (strcmp(currentDir, "/") == 0) {
            snprintf(pathBuf, pathBufLen, "/%s", base);
        } else {
            snprintf(pathBuf, pathBufLen, "%s/%s", currentDir, base);
        }

        if (entry.isDirectory()) {

            if (base[0] != '.' && level > 0) {
                char *childDir = (char *)heap_caps_malloc(PATH_BUF_LEN, MALLOC_CAP_SPIRAM);
                if (childDir) {
                    strncpy(childDir, pathBuf, PATH_BUF_LEN - 1);
                    childDir[PATH_BUF_LEN - 1] = '\0';

                    scanDirRecursive(
                        fs,
                        childDir,          // ✅ stable copy in PSRAM (0 bytes stack)
                        level - 1,
                        playlist,
                        pathBuf,
                        pathBufLen
                    );
                    heap_caps_free(childDir);
                }
            }

        } else {

            if (isAudioFile(base)) {
                playlist.println(pathBuf);
                log_d("-> %s", pathBuf);
            }
        }

        entry.close();
        vTaskDelay(1);
    }

    dir.close();
}




//create music_playlist.txt
void generatePlaylistFile(fs::FS &sourceFs, const char *dirname, uint8_t levels) {

    LittleFS.remove(PLAYLIST_FILE);
    File playlist = LittleFS.open(PLAYLIST_FILE, FILE_WRITE);

  
    if (!playlist) {
        log_e("Failed to open playlist file for writing!");
        return;
    }

    char *pathBuf = (char *)heap_caps_malloc(PATH_BUF_LEN, MALLOC_CAP_SPIRAM);
    if (!pathBuf) {
        log_e("Failed to allocate path buffer");
        playlist.close();
        return;
    }

    log_d("Scanning directory: %s", dirname);
    scanDirRecursive(
        sourceFs,
        dirname,      // "/" typically
        levels,
        playlist,
        pathBuf,
        PATH_BUF_LEN
    );

    heap_caps_free(pathBuf);
    playlist.close();
    log_d("Playlist file %s generation complete.", PLAYLIST_FILE);
}


// ==========================================
// Fast O(1) PSRAM Track Offset Table
// ==========================================
static uint32_t *s_track_file_offsets = nullptr;
static int s_indexed_track_count = 0;

void freeTrackOffsetIndex() {
  if (s_track_file_offsets) {
    heap_caps_free(s_track_file_offsets);
    s_track_file_offsets = nullptr;
    s_indexed_track_count = 0;
  }
}

void buildTrackOffsetIndex() {
  freeTrackOffsetIndex();

  File playlist = LittleFS.open(PLAYLIST_FILE, FILE_READ);
  if (!playlist) {
    log_w("Cannot open %s to build offset index", PLAYLIST_FILE);
    return;
  }

  // Count tracks first
  int count = 0;
  while (playlist.available()) {
    playlist.readStringUntil('\n');
    count++;
  }

  if (count <= 0) {
    playlist.close();
    trackListLength = 0;
    return;
  }

  // Allocate offset table in PSRAM (0 bytes DRAM)
  s_track_file_offsets = (uint32_t *)heap_caps_malloc(count * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  if (!s_track_file_offsets) {
    log_e("Failed to allocate %d bytes in PSRAM for track offset index", (int)(count * sizeof(uint32_t)));
    playlist.close();
    trackListLength = count;
    return;
  }

  playlist.seek(0);
  int idx = 0;
  while (playlist.available() && idx < count) {
    s_track_file_offsets[idx++] = playlist.position();
    playlist.readStringUntil('\n');
  }

  playlist.close();
  s_indexed_track_count = idx;
  trackListLength = idx;
  log_i("PSRAM Track Offset Index built: %d tracks indexed (%u bytes in PSRAM, 0 bytes DRAM)",
        s_indexed_track_count, (unsigned int)(s_indexed_track_count * sizeof(uint32_t)));
}

// Low-RAM function to count lines (tracks)
int getTrackCount() {
  File playlist = LittleFS.open(PLAYLIST_FILE, FILE_READ);
  if (!playlist) {
    log_w("Failed to open playlist file: %s", PLAYLIST_FILE);
    return 0;
  }

  int count = 0;
  while (playlist.available()) {
    playlist.readStringUntil('\n'); // Read line without storing it (low RAM)
    count++;
  }
  playlist.close();
  return count;
}

uint16_t randomIndexExcept(uint16_t count, uint16_t currentIndex) {
  if (count == 0) return 0;
  if (count == 1) return 0;

  uint32_t entropy = esp_random();
  entropy ^= micros();
  entropy ^= (uint32_t)xTaskGetTickCount() << 16;

  uint16_t next = entropy % count;
  if (next == currentIndex) {
    next = (next + 1 + ((entropy / count) % (count - 1))) % count;
  }
  return next;
}

// ==========================================
// Fast O(1) function to read a track path by index
// ==========================================
bool getTrackPath(int index, char *outBuf, size_t outBufSize) {
  if (!outBuf || outBufSize == 0 || index < 0) return false;

  File playlist = LittleFS.open(PLAYLIST_FILE, FILE_READ);
  if (!playlist) {
    log_w("Failed to open playlist file.");
    outBuf[0] = '\0';
    return false;
  }

  // O(1) Fast Seek if PSRAM index is available!
  if (s_track_file_offsets && index < s_indexed_track_count) {
    playlist.seek(s_track_file_offsets[index]);
    size_t len = playlist.readBytesUntil('\n', outBuf, outBufSize - 1);
    outBuf[len] = '\0';
    if (len > 0 && outBuf[len - 1] == '\r') {
      outBuf[len - 1] = '\0';
    }
    playlist.close();
    return true;
  }

  // Fallback sequential scan
  int lineCount = 0;
  size_t len = 0;
  while (playlist.available()) {
    len = playlist.readBytesUntil('\n', outBuf, outBufSize - 1);
    outBuf[len] = '\0';

    // Strip trailing CR (\r)
    if (len > 0 && outBuf[len - 1] == '\r') {
      outBuf[len - 1] = '\0';
    }

    if (lineCount == index) {
      playlist.close();
      return true;
    }

    lineCount++;
  }

  playlist.close();
  outBuf[0] = '\0';
  return false; // index not found
}

// The dedicated SD Scan Task
void scan_music_task(void *pvParameters) {
  SD.end();
  vTaskDelay(pdMS_TO_TICKS(50));

  if (!SD.begin(SD_CS, SPI)) {
    log_w("SD Card Mount Failed");
    updateSDCARDStatus(LV_SYMBOL_CLOSE " Card Mount Failed", 0x777777);
    trackListLength = 0;
    freeTrackOffsetIndex();
  } else {
    log_d("Indexing music library, please wait...");
    updateSDCARDStatus("Indexing music library...", 0x00FF00);
    generatePlaylistFile(SD, "/", 5); // Scan and write to LittleFS
    buildTrackOffsetIndex();          // Build O(1) PSRAM offset table and update trackListLength
    char buffer[100];
    snprintf(buffer, sizeof(buffer), LV_SYMBOL_AUDIO " Found %d songs.", trackListLength);
    updateSDCARDStatus(buffer, 0x00FF00);
    log_d("%s", buffer);

    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    log_d("{ SD Scan task stack remaining MIN: %u bytes }", hwm);
  }
  scanMusicTask = NULL;
  vTaskDelete(NULL);
}

void scanMusic() {
  if (scanMusicTask == NULL) {
    xTaskCreatePinnedToCore(scan_music_task, "SD_Scan_Task", 8 * 1024, NULL, 1, &scanMusicTask, 1);
  }
}

// load music
void initSongList() {
  log_d("Load MUSIC library");
  if (LittleFS.exists(PLAYLIST_FILE)) {
    buildTrackOffsetIndex();
    char statusMsg[100];
    snprintf(statusMsg, sizeof(statusMsg), LV_SYMBOL_AUDIO " Loaded %d songs from library", trackListLength);
    log_d("%s", statusMsg);
  } else {
    if (scanMusicTask == NULL) {
      xTaskCreatePinnedToCore(scan_music_task, "SD_Scan_Task", 8 * 1024, NULL, 1, &scanMusicTask, 1);
    }
  }
}

//------------------- Streaming Radio ----------------------------------
// load station list from files "stations.csv" or default
//radios stations[MAX_STATION_LIST_LENGTH];
radios *stations = nullptr;

int16_t stationIndex = 0;
uint8_t stationListLength = 0;
uint8_t currentRadioCatalog = RADIO_CATALOG_ONLINE_RADIO_FM;
uint8_t currentRadioLang = RADIO_LANG_HI;

// Catalog 0: https://onlineradiofm.in/ curated Indian stations
const char defaultStationsOnlineRadioFM_CSV[] PROGMEM =
    "Mirchi Top 20,https://drive.uber.radio/uber/bollywoodnow/icecast.audio\n"
    "Bollywood Hits,https://streaming.exclusive.radio/er/bollywood/icecast.audio\n"
    "Bombay Beats India,http://strm112.1.fm/bombaybeats_mobile_mp3\n"
    "Fnf.Fm Hindi,http://192.99.8.192:5032/;stream\n"
    "Ishq FM Bollywood,https://drive.uber.radio/uber/bollywoodlove/icecast.audio\n"
    "Hum FM 106.2,https://server.mediacast4u.stream/8002/stream\n"
    "MixiFy Hindi Hits,https://server.mixify.in/listen/new_hits/radio.mp3\n"
    "Radio SD 90.8 FM,http://uk2.internet-radio.com:8066/stream\n"
    "Radio Afsana,http://us9.streamingpulse.com:7058/stream\n"
    "Bollywood Mix,https://drive.uber.radio/uber/bollywoodmix/icecast.audio\n";

// Catalog 1: https://www.radioindia.in/ curated Indian stations
const char defaultStationsRadioIndia_CSV[] PROGMEM =
    "Bollywood 2000s,https://2.mystreaming.net/uber/bollywood2000s/icecast.audio\n"
    "Bollywood 2010s,https://drive.uber.radio/uber/bollywood2010s/icecast.audio\n"
    "Radio Udaan,https://stream.radioudaan.com/listen/radio_udaan/radio.mp3\n"
    "Radio Maharani,https://streamasiacdn.atc-labs.com/radiomaharani.aac\n"
    "Sangeet Radio FM,https://ice8.securenetsystems.net/SGTRADIO\n"
    "Suno Sharda 90.8,https://streamasiacdn.atc-labs.com/shardaradio.aac\n"
    "CINA 1650 AM,http://ice8.securenetsystems.net/CINA\n"
    "Ujala Radio,http://stream2.ujala.nl/stream/2/listen.mp3\n"
    "Boom FM 94.1,http://192.99.8.192:3630/stream\n"
    "NTN Radio 89.1,http://auds1.intacs.com/ntnradio\n";

// English (EN) Curated Top 10
const char defaultStationsEnglish_CSV[] PROGMEM =
    "Dance Wave!,https://dancewave.online/dance.mp3\n"
    "BBC World Service,http://stream.live.vc.bbcmedia.co.uk/bbc_world_service\n"
    "Classic Vinyl HD,https://icecast.walmradio.com:8443/classic\n"
    "WALM Old Time Radio,https://icecast.walmradio.com:8443/otr\n"
    "101 Smooth Jazz,http://jking.cdnstream1.com/b22139_128mp3\n"
    "Radio Paradise EU,http://stream-uk1.radioparadise.com/aac-320\n"
    "Classic Hits 70s 80s,https://radiopanther.radiolebowski.com/play\n"
    "WALM 2 HD,https://icecast.walmradio.com:8443/walm2\n"
    "Mango Radio EN,https://mangoradio.stream.laut.fm/mangoradio\n"
    "NPR 24/7 News,https://npr-ice.streamguys1.com/live.mp3\n";

// Spanish (ES) Curated Top 10
const char defaultStationsSpanish_CSV[] PROGMEM =
    "Cadena 100 Spain,http://cadena100-streamers-mp3.flumotion.com/cope/cadena100.mp3\n"
    "Ibiza Global Radio,http://ibizaglobalradio.streaming-pro.com:8024/\n"
    "Chocolate FM,http://streaming5.elitecomunicacion.es:8082/live.mp3\n"
    "Rock FM Spain,http://flucast02-h-cloud.flumotion.com/cope/rockfm-low.mp3\n"
    "Blu Radio Colombia,http://24503.live.streamtheworld.com/BLURADIO_SC\n"
    "80s Exitos Latino,https://80sexitos.stream.laut.fm/80sexitos\n"
    "Caracol Radio,http://27343.live.streamtheworld.com:3690/CARACOL_RADIOAAC_SC\n"
    "Los 40 Urban,https://playerservices.streamtheworld.com/api/livestream-redirect/LOS40_URBAN.mp3\n"
    "Los 40 Dance,http://playerservices.streamtheworld.com/api/livestream-redirect/LOS40_DANCE_SC\n"
    "esRadio Madrid,http://livestreaming.esradio.fm/stream64.mp3\n";

// Chinese / Mandarin (CN) Curated Top 10
const char defaultStationsChinese_CSV[] PROGMEM =
    "Asia DREAM China,http://kathy.torontocast.com:3330/stream/1/\n"
    "Hong Kong RTHK 1,http://stm.rthk.hk/radio1\n"
    "Classical FM 97.7,http://59.120.88.155:8000/live.mp3\n"
    "Chinese Radio 2,https://lhttp.qingting.fm/live/4804/64k.mp3\n"
    "CNR-1 Voice of China,https://lhttp.qtfm.cn/live/15318317/64k.mp3\n"
    "Chinese Radio 4,https://lhttp.qtfm.cn/live/20500172/64k.mp3\n"
    "Chinese Radio 5,http://lhttp.qingting.fm/live/4915/64k.mp3\n"
    "YES 933 Mandopop,http://playerservices.streamtheworld.com/api/livestream-redirect/YES933AAC.aac\n"
    "Love 972 Radio,http://playerservices.streamtheworld.com/api/livestream-redirect/LOVE972FMAAC.aac\n"
    "Jesus Is Lord Radio,https://s3.radio.co/s97f38db97/listen\n";

// German (DE) Curated Top 10 (#1 GDP non-EN/CN/ES/HI)
const char defaultStationsGerman_CSV[] PROGMEM =
    "1LIVE WDR,http://wdr-1live-live.icecast.wdr.de/wdr/1live/live/mp3/128/stream.mp3\n"
    "Antenne Bayern,http://mp3channels.webradio.antenne.de/antenne\n"
    "Rock Antenne,http://mp3channels.webradio.rockantenne.de/rockantenne\n"
    "WDR 5 Information,http://wdr-wdr5-live.icecast.wdr.de/wdr/wdr5/live/mp3/128/stream.mp3\n"
    "Sunshine Live 90er,http://stream.sunshine-live.de/90er/mp3-192/stream.sunshine-live.de\n"
    "80s80s Wave,http://streams.80s80s.de/web/mp3-192/streams.80s80s.de/\n"
    "90s90s Hits,http://streams.90s90s.de/pop/mp3-192/streams.90s90s.de/\n"
    "Rock Antenne Metal,http://mp3channels.webradio.rockantenne.de/heavy-metal\n"
    "TranceBase.FM,http://listen.trancebase.fm/tunein-aac-hd-pls\n"
    "Mango Radio DE,https://mangoradio.stream.laut.fm/mangoradio\n";

// Japanese (JA) Curated Top 10 (#2 GDP non-EN/CN/ES/HI)
const char defaultStationsJapanese_CSV[] PROGMEM =
    "Jazz Sakura Asia Dream,http://kathy.torontocast.com:3330/stream/1/?esPlayer&cb=82181.mp3\n"
    "Anime Para Ti,https://stream.zeno.fm/qpn8mkt8c4duv\n"
    "Listen.Moe J-Pop,https://listen.moe/stream\n"
    "Retro PC Game Music,http://gyusyabu.ddo.jp:8000/\n"
    "R/a/dio Anime,https://relay0.r-a-d.io/main.mp3\n"
    "J1 Gold Nostalgia,http://jenny.torontocast.com:8062/\n"
    "FM Kahoku 78.7,http://radio.kahoku.net:8000/;\n"
    "Shonan Beach FM 78.9,http://shonanbeachfm.out.airtime.pro:8000/shonanbeachfm_a\n"
    "Free FM Tokyo,https://rocafmadrid.radioca.st/\n"
    "J1 Hits Japan,http://jenny.torontocast.com:8056/\n";

const char* getRadioCatalogName(uint8_t catalogIndex) {
  if (currentRadioLang == RADIO_LANG_HI) {
    switch (catalogIndex) {
      case RADIO_CATALOG_ONLINE_RADIO_FM: return "OnlineRadioFM.in";
      case RADIO_CATALOG_RADIO_INDIA:      return "RadioIndia.in";
      default:                            return "Hindi Radio";
    }
  }
  return getRadioLanguageName(currentRadioLang);
}

const char* getRadioLanguageCode(uint8_t langIndex) {
  switch (langIndex) {
    case RADIO_LANG_HI: return "HI";
    case RADIO_LANG_EN: return "EN";
    case RADIO_LANG_ES: return "ES";
    case RADIO_LANG_CN: return "CN";
    case RADIO_LANG_DE: return "DE";
    case RADIO_LANG_JA: return "JA";
    default:            return "HI";
  }
}

const char* getRadioLanguageName(uint8_t langIndex) {
  switch (langIndex) {
    case RADIO_LANG_HI: return "Hindi";
    case RADIO_LANG_EN: return "English";
    case RADIO_LANG_ES: return "Spanish";
    case RADIO_LANG_CN: return "Chinese";
    case RADIO_LANG_DE: return "German";
    case RADIO_LANG_JA: return "Japanese";
    default:            return "Hindi";
  }
}

uint8_t getRadioLanguage() {
  return currentRadioLang;
}

bool parseCSVLine(const char *line, char *name, size_t nameSize, char *url, size_t urlSize) {
  if (!line || !name || !url) return false;

  const char *comma = strchr(line, ',');
  if (!comma) return false;

  // Copy station name
  size_t nameLen = comma - line;
  if (nameLen >= nameSize) nameLen = nameSize - 1;
  memcpy(name, line, nameLen);
  name[nameLen] = '\0';

  // Copy URL (strip CR/LF)
  const char *urlStart = comma + 1;
  size_t urlLen = strcspn(urlStart, "\r\n");
  if (urlLen >= urlSize) urlLen = urlSize - 1;
  memcpy(url, urlStart, urlLen);
  url[urlLen] = '\0';

  return true;
}

bool initStationsPSRAM() {
  if (stations) return true;   // already initialized

  stations = (radios *)heap_caps_malloc(
      sizeof(radios) * MAX_STATION_LIST_LENGTH,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );

  if (!stations) {
    log_e("PSRAM allocation failed for stations[]");
    return false;
  }

  memset(stations, 0, sizeof(radios) * MAX_STATION_LIST_LENGTH);
  log_i("%d stations[] allocated in PSRAM", MAX_STATION_LIST_LENGTH);
  return true;
}


void switchRadioLanguage(uint8_t langIndex) {
  if (langIndex > RADIO_LANG_JA) langIndex = RADIO_LANG_HI;
  currentRadioLang = langIndex;

  Preferences rpref;
  rpref.begin("tb_radio", false);
  rpref.putUChar("lang", currentRadioLang);
  rpref.end();

  if (!initStationsPSRAM()) return;

  const char *cachePath = "/radio_cat0.csv";
  const char *csvData = defaultStationsOnlineRadioFM_CSV;

  switch (currentRadioLang) {
    case RADIO_LANG_HI:
      cachePath = (currentRadioCatalog == RADIO_CATALOG_ONLINE_RADIO_FM) ? "/radio_cat0.csv" : "/radio_cat1.csv";
      csvData = (currentRadioCatalog == RADIO_CATALOG_ONLINE_RADIO_FM) ? defaultStationsOnlineRadioFM_CSV : defaultStationsRadioIndia_CSV;
      break;
    case RADIO_LANG_EN:
      cachePath = "/radio_en.csv";
      csvData = defaultStationsEnglish_CSV;
      break;
    case RADIO_LANG_ES:
      cachePath = "/radio_es.csv";
      csvData = defaultStationsSpanish_CSV;
      break;
    case RADIO_LANG_CN:
      cachePath = "/radio_cn.csv";
      csvData = defaultStationsChinese_CSV;
      break;
    case RADIO_LANG_DE:
      cachePath = "/radio_de.csv";
      csvData = defaultStationsGerman_CSV;
      break;
    case RADIO_LANG_JA:
      cachePath = "/radio_ja.csv";
      csvData = defaultStationsJapanese_CSV;
      break;
  }

  if (!LittleFS.exists(cachePath)) {
    File fc = LittleFS.open(cachePath, "w");
    if (fc) {
      fc.print(csvData);
      fc.close();
      log_i("Cached radio language %s to %s", getRadioLanguageCode(currentRadioLang), cachePath);
    }
  }

  stationListLength = 0;
  stationIndex = 0;

  File f = LittleFS.open(cachePath, "r");
  if (f) {
    char *lineBuf = (char *)heap_caps_malloc(LINE_BUF_LEN, MALLOC_CAP_SPIRAM);
    if (lineBuf) {
      while (f.available() && stationListLength < MAX_STATION_LIST_LENGTH) {
        size_t lineLen = f.readBytesUntil('\n', lineBuf, LINE_BUF_LEN - 1);
        lineBuf[lineLen] = '\0';
        if (lineLen == 0) continue;
        if (parseCSVLine(lineBuf, stations[stationListLength].name, sizeof(stations[stationListLength].name),
                         stations[stationListLength].url, sizeof(stations[stationListLength].url))) {
          stationListLength++;
        }
      }
      heap_caps_free(lineBuf);
    }
    f.close();
  }

  File actF = LittleFS.open(STATION_LIST_FILENAME, "w");
  if (actF) {
    for (uint8_t i = 0; i < stationListLength; i++) {
      actF.printf("%s,%s\n", stations[i].name, stations[i].url);
    }
    actF.close();
  }

  if (ui_MainMenu_Textarea_stationList) {
    char txt[96];
    snprintf(txt, sizeof(txt), "Total %d stations loaded (%s)", stationListLength, getRadioLanguageCode(currentRadioLang));
    lv_textarea_set_text(ui_MainMenu_Textarea_stationList, txt);
  }

  extern lv_obj_t *ui_Player_Label_trackNumber;
  extern lv_obj_t *ui_Player_Textarea_status;
  extern lv_obj_t *ui_Player_Label_Catalog;
  if (ui_Player_Label_trackNumber) {
    char status_buffer[32];
    snprintf(status_buffer, sizeof(status_buffer), "%d of %d", stationIndex + 1, stationListLength);
    lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
  }
  if (ui_Player_Textarea_status) {
    if (stationListLength > 0 && stations[stationIndex].name) {
      lv_textarea_set_text(ui_Player_Textarea_status, stations[stationIndex].name);
    }
  }
  if (ui_Player_Label_Catalog) {
    if (currentRadioLang == RADIO_LANG_HI) {
      lv_label_set_text(ui_Player_Label_Catalog,
          (currentRadioCatalog == RADIO_CATALOG_ONLINE_RADIO_FM) ? LV_SYMBOL_AUDIO " FM" : LV_SYMBOL_WIFI " IN");
    } else {
      char cbuf[16];
      snprintf(cbuf, sizeof(cbuf), LV_SYMBOL_AUDIO " %s", getRadioLanguageCode(currentRadioLang));
      lv_label_set_text(ui_Player_Label_Catalog, cbuf);
    }
  }

  log_i("[RADIO LANG] Switched to %s (%s) - %d stations loaded", getRadioLanguageCode(currentRadioLang), getRadioLanguageName(currentRadioLang), stationListLength);
}

void switchRadioCatalog(uint8_t catalogIndex) {
  if (catalogIndex > 1) catalogIndex = 0;
  currentRadioCatalog = catalogIndex;

  Preferences rpref;
  rpref.begin("tb_radio", false);
  rpref.putUChar("cat", currentRadioCatalog);
  rpref.end();

  if (currentRadioLang == RADIO_LANG_HI) {
    switchRadioLanguage(RADIO_LANG_HI);
  } else {
    switchRadioLanguage(currentRadioLang);
  }
}

void loadStationList() {
  if (!initStationsPSRAM()) {
    if (ui_MainMenu_Textarea_stationList) {
      lv_textarea_add_text(ui_MainMenu_Textarea_stationList, LV_SYMBOL_CLOSE " PSRAM allocation failed\n");
    }
    return;
  }

  if (LittleFS.exists(STATION_LIST_FILENAME)) {
    File testF = LittleFS.open(STATION_LIST_FILENAME, "r");
    if (testF) {
      String firstLine = testF.readStringUntil('\n');
      testF.close();
      if (firstLine.indexOf("Top Radio") >= 0 || firstLine.indexOf("FM93.5") >= 0 || firstLine.indexOf("Chou") >= 0) {
        log_i("Removing stale vendor legacy stations.csv");
        LittleFS.remove(STATION_LIST_FILENAME);
      }
    }
  }

  const struct { const char *path; const char *data; } langFiles[] = {
    {"/radio_cat0.csv", defaultStationsOnlineRadioFM_CSV},
    {"/radio_cat1.csv", defaultStationsRadioIndia_CSV},
    {"/radio_en.csv", defaultStationsEnglish_CSV},
    {"/radio_es.csv", defaultStationsSpanish_CSV},
    {"/radio_cn.csv", defaultStationsChinese_CSV},
    {"/radio_de.csv", defaultStationsGerman_CSV},
    {"/radio_ja.csv", defaultStationsJapanese_CSV}
  };
  for (size_t i = 0; i < sizeof(langFiles)/sizeof(langFiles[0]); i++) {
    if (!LittleFS.exists(langFiles[i].path)) {
      File fc = LittleFS.open(langFiles[i].path, "w");
      if (fc) {
        fc.print(langFiles[i].data);
        fc.close();
      }
    }
  }

  Preferences rpref;
  rpref.begin("tb_radio", false);
  currentRadioLang = rpref.getUChar("lang", RADIO_LANG_HI);
  currentRadioCatalog = rpref.getUChar("cat", RADIO_CATALOG_ONLINE_RADIO_FM);
  rpref.end();
  if (currentRadioLang > RADIO_LANG_JA) currentRadioLang = RADIO_LANG_HI;
  if (currentRadioCatalog > 1) currentRadioCatalog = 0;

  switchRadioLanguage(currentRadioLang);
}

// copy file 'stations.csv' to littleFS
bool copyStationsCSV_SD_to_LittleFS() {
  // --- Init SD ---
  if (!SD.begin(SD_CS, SPI)) {
    log_w("SD mount failed");
    return false;
  }
  // --- Check file exists on SD ---
  if (!SD.exists(STATION_LIST_FILENAME)) {
    log_w("%s not found on SD", STATION_LIST_FILENAME);
    return false;
  }
  File src = SD.open(STATION_LIST_FILENAME, "r");
  if (!src) {
    log_w("Cannot open %s on SD", STATION_LIST_FILENAME);
    return false;
  }
  // --- Open destination file in LittleFS ---
  File dst = LittleFS.open(STATION_LIST_FILENAME, "w");
  if (!dst) {
    log_w("Cannot create %s in LittleFS", STATION_LIST_FILENAME);
    src.close();
    return false;
  }
  // --- Perform buffered copy ---
  uint8_t buf[512];
  size_t len;

  while ((len = src.read(buf, sizeof(buf))) > 0) {
    dst.write(buf, len);
  }
  src.close();
  dst.close();
  log_d("%s copied from SD → LittleFS", STATION_LIST_FILENAME);
  return true;
}
