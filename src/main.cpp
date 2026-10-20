#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DFRobotDFPlayerMini.h>

// Set to 0 to build without Bluetooth (e.g. while debugging power problems)
#define ENABLE_BLE 1

#if ENABLE_BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#endif

// ==================== OLED ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C

// ==================== DFPlayer ====================
#define DF_RX 27  // ESP32 RX2 <- DFPlayer TX
#define DF_TX 26  // ESP32 TX2 -> DFPlayer RX

// ==================== Buttons ====================
#define BTN_VOL_UP   18
#define BTN_VOL_DOWN 19
#define BTN_NEXT     23
#define BTN_PREV     25
#define BTN_PAUSE    32

// ==================== Playlist ====================
// Files must be inside the /MP3 folder on the SD card: 0001.mp3, 0002.mp3, ...
// The number of files is detected automatically at startup.
int totalTracks = 6; // updated automatically from the SD card at startup

// ==================== Objects ====================
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini player;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==================== Bluetooth hooks (defined further down) ====================
#if ENABLE_BLE
void bleInit();
void handleBle();
void bleNotifyStatus();
bool bleIsConnected();
#else
inline void bleInit() {}
inline void handleBle() {}
inline void bleNotifyStatus() {}
inline bool bleIsConnected() { return false; }
#endif

// ==================== Player state ====================
int currentTrack = 1;
int volumeLevel = 15;

bool playing = false;
bool paused = false;

unsigned long playStartedAt = 0;
unsigned long accumulatedMs = 0;
unsigned long lastDisplayUpdate = 0;

// Time of the last play request (used to ignore duplicate "finished" events
// and to know whether a FileMismatch error belongs to our request)
unsigned long lastPlayRequestAt = 0;
int mismatchCount = 0;

const unsigned long DISPLAY_INTERVAL = 200;
const unsigned long DEBOUNCE_MS = 35;
const unsigned long FINISH_GUARD_MS = 1500;   // ignore "finished" right after a play request
const unsigned long RESTART_THRESHOLD_MS = 3000; // PREV restarts track if played longer than this

// ==================== Buttons ====================
struct Button {
    uint8_t pin;
    bool stableState;
    bool lastReading;
    unsigned long changedAt;
    unsigned long pressedAt;
    unsigned long lastRepeat;
};

Button buttons[] = {
    {BTN_VOL_UP,   HIGH, HIGH, 0, 0, 0},
    {BTN_VOL_DOWN, HIGH, HIGH, 0, 0, 0},
    {BTN_NEXT,     HIGH, HIGH, 0, 0, 0},
    {BTN_PREV,     HIGH, HIGH, 0, 0, 0},
    {BTN_PAUSE,    HIGH, HIGH, 0, 0, 0}
};

enum ButtonIndex {
    VOL_UP,
    VOL_DOWN,
    NEXT_TRACK,
    PREV_TRACK,
    PAUSE_TRACK
};

// ==================== Time helpers ====================
unsigned long elapsedMilliseconds() {
    if (playing && !paused) {
        return accumulatedMs + (millis() - playStartedAt);
    }
    return accumulatedMs;
}

void printTime(int x, int y, unsigned long seconds) {
    display.setCursor(x, y);

    unsigned long minutes = seconds / 60;
    unsigned long secs = seconds % 60;

    if (minutes < 10) display.print('0');
    display.print(minutes);
    display.print(':');
    if (secs < 10) display.print('0');
    display.print(secs);
}

// ==================== OLED ====================
void drawPartsBar() {
    // One segment per file. Done = filled, current = blinking (solid when paused),
    // upcoming = outline. Works for any number of files.
    const int y = 52;
    const int h = 11;

    // Too many files for separate segments: draw one continuous bar
    if (totalTracks > 40) {
        display.drawRect(0, y, SCREEN_WIDTH, h, SSD1306_WHITE);
        int w = ((SCREEN_WIDTH - 4) * currentTrack) / totalTracks;
        display.fillRect(2, y + 2, w, h - 4, SSD1306_WHITE);
        return;
    }

    const int gap = (totalTracks > 12) ? 1 : 2;
    int segW = (SCREEN_WIDTH - (totalTracks - 1) * gap) / totalTracks;
    int digits = (totalTracks >= 10) ? 2 : 1;
    bool showNumbers = segW >= (digits * 6 + 2);
    int startX = (SCREEN_WIDTH - (totalTracks * segW + (totalTracks - 1) * gap)) / 2;

    bool blinkOn = ((millis() / 500) % 2) == 0;

    for (int i = 1; i <= totalTracks; i++) {
        int x = startX + (i - 1) * (segW + gap);
        bool filled;

        if (i < currentTrack) {
            filled = true;
        } else if (i == currentTrack) {
            filled = paused || !playing || blinkOn;
        } else {
            filled = false;
        }

        if (filled) {
            display.fillRect(x, y, segW, h, SSD1306_WHITE);
            display.setTextColor(SSD1306_BLACK);
        } else {
            display.drawRect(x, y, segW, h, SSD1306_WHITE);
            display.setTextColor(SSD1306_WHITE);
        }

        if (showNumbers) {
            int numDigits = (i >= 10) ? 2 : 1;
            display.setTextSize(1);
            display.setCursor(x + (segW - numDigits * 6) / 2 + 1, y + 2);
            display.print(i);
        }
    }

    display.setTextColor(SSD1306_WHITE);
}

void showScreen() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    // Header
    display.setCursor(0, 0);
    display.print("Quran Player");
    if (bleIsConnected()) {
        display.setCursor(76, 0);
        display.print("BT");
    }
    display.setCursor(91, 0);
    display.print("V:");
    display.print(volumeLevel);
    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    // Part x / N + file name
    display.setCursor(0, 14);
    display.print("Part ");
    display.print(currentTrack);
    display.print("/");
    display.print(totalTracks);

    char filename[12];
    snprintf(filename, sizeof(filename), "%04d.mp3", currentTrack);
    display.setCursor(80, 14);
    display.print(filename);

    // Elapsed time (big)
    display.setTextSize(2);
    printTime(0, 29, elapsedMilliseconds() / 1000);

    // Status
    display.setTextSize(1);
    display.setCursor(72, 34);
    if (paused) {
        display.print("PAUSED");
    } else if (playing) {
        display.print("PLAYING");
    } else {
        display.print("STOPPED");
    }

    // Parts progress bar
    drawPartsBar();

    display.display();
}

// ==================== Playback ====================
void playTrack(int track) {
    // Wrap around the playlist: after the last file go back to the first
    if (track > totalTracks) track = 1;
    if (track < 1) track = totalTracks;

    currentTrack = track;
    accumulatedMs = 0;
    playStartedAt = millis();
    lastPlayRequestAt = millis();

    paused = false;
    playing = true;

    player.playMp3Folder(currentTrack);

    Serial.print("Requesting MP3 file: ");
    Serial.println(currentTrack);

    showScreen();
    bleNotifyStatus();
}

void nextTrack() {
    playTrack(currentTrack + 1);
}

void previousTrack() {
    // Normal player behaviour: restart the current track if it already
    // played for a few seconds, otherwise go to the previous one.
    if (elapsedMilliseconds() > RESTART_THRESHOLD_MS) {
        playTrack(currentTrack);
    } else {
        playTrack(currentTrack - 1);
    }
}

void togglePause() {
    if (!playing && !paused) {
        playTrack(currentTrack);
        return;
    }

    if (playing && !paused) {
        accumulatedMs = elapsedMilliseconds();
        player.pause();

        paused = true;
        playing = false;

        Serial.println("Paused");
    } else if (paused) {
        player.start();

        playStartedAt = millis();
        paused = false;
        playing = true;

        Serial.println("Resumed");
    }

    showScreen();
    bleNotifyStatus();
}

void changeVolume(int amount) {
    int newVolume = constrain(volumeLevel + amount, 0, 30);
    if (newVolume == volumeLevel) return;

    volumeLevel = newVolume;
    player.volume(volumeLevel);

    Serial.print("Volume: ");
    Serial.println(volumeLevel);

    showScreen();
    bleNotifyStatus();
}

// ==================== Bluetooth (BLE) ====================
// Nordic UART Service (NUS): works with nRF Connect and "Serial Bluetooth Terminal"
// (choose the BLE mode in that app). Device name: "Quran Player".
//
// Commands (one per write, case-insensitive):
//   PLAY | PAUSE | TOGGLE | NEXT | PREV
//   VOL+ | VOL- | VOL=<0-30>
//   TRACK=<n>
//   STATUS
// Replies/notifications are short lines such as "PLAYING 5/161 V15".
#if ENABLE_BLE

#define BLE_DEVICE_NAME  "Quran Player"
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // phone -> ESP32
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // ESP32 -> phone

BLECharacteristic *bleTx = nullptr;
volatile bool bleConnected = false;
volatile bool bleRestartAdv = false;
volatile unsigned long bleRestartAt = 0;

// BLE callbacks run in another task, so commands are queued here and
// executed from loop() (the display and DFPlayer code is not thread-safe).
const int BLE_QUEUE_SIZE = 4;
const int BLE_CMD_LEN = 24;
char bleQueue[BLE_QUEUE_SIZE][BLE_CMD_LEN];
volatile int bleHead = 0;
volatile int bleTail = 0;
portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;

bool bleIsConnected() {
    return bleConnected;
}

void bleSend(const char *text) {
    if (!bleConnected || bleTx == nullptr) return;
    bleTx->setValue((uint8_t *)text, strlen(text));
    bleTx->notify();
}

void bleNotifyStatus() {
    if (!bleConnected) return;

    // Kept to 20 bytes max so it fits in one notification with the default MTU
    char msg[24];
    const char *state = paused ? "PAUSED" : (playing ? "PLAYING" : "STOPPED");
    snprintf(msg, sizeof(msg), "%s %d/%d V%d\n", state, currentTrack, totalTracks, volumeLevel);
    bleSend(msg);
}

void bleQueuePush(const char *cmd) {
    portENTER_CRITICAL(&bleMux);
    int next = (bleHead + 1) % BLE_QUEUE_SIZE;
    if (next != bleTail) {            // drop the command if the queue is full
        strncpy(bleQueue[bleHead], cmd, BLE_CMD_LEN - 1);
        bleQueue[bleHead][BLE_CMD_LEN - 1] = '\0';
        bleHead = next;
    }
    portEXIT_CRITICAL(&bleMux);
}

bool bleQueuePop(char *out) {
    bool got = false;
    portENTER_CRITICAL(&bleMux);
    if (bleTail != bleHead) {
        strncpy(out, bleQueue[bleTail], BLE_CMD_LEN);
        bleTail = (bleTail + 1) % BLE_QUEUE_SIZE;
        got = true;
    }
    portEXIT_CRITICAL(&bleMux);
    return got;
}

class QuranServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override {
        bleConnected = true;
        Serial.println("BLE client connected");
    }

    void onDisconnect(BLEServer *server) override {
        bleConnected = false;
        bleRestartAt = millis() + 300;
        bleRestartAdv = true;          // restart advertising from loop()
        Serial.println("BLE client disconnected");
    }
};

class QuranRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) override {
        auto value = characteristic->getValue(); // std::string or String depending on core version

        char cmd[BLE_CMD_LEN];
        int len = 0;

        for (size_t i = 0; i < value.length(); i++) {
            char c = (char)value[i];
            if (c == '\n' || c == '\r') {
                if (len > 0) {
                    cmd[len] = '\0';
                    bleQueuePush(cmd);
                    len = 0;
                }
            } else if (len < BLE_CMD_LEN - 1) {
                cmd[len++] = c;
            }
        }

        if (len > 0) {
            cmd[len] = '\0';
            bleQueuePush(cmd);
        }
    }
};

void executeBleCommand(char *cmd) {
    // Trim spaces and upper-case the command
    char *start = cmd;
    while (*start == ' ') start++;
    int n = strlen(start);
    while (n > 0 && start[n - 1] == ' ') start[--n] = '\0';
    for (int i = 0; i < n; i++) start[i] = toupper((unsigned char)start[i]);

    Serial.print("BLE command: ");
    Serial.println(start);

    if (strcmp(start, "PLAY") == 0) {
        if (paused) togglePause();
        else if (!playing) playTrack(currentTrack);
        else bleNotifyStatus();
    } else if (strcmp(start, "PAUSE") == 0) {
        if (playing && !paused) togglePause();
        else bleNotifyStatus();
    } else if (strcmp(start, "TOGGLE") == 0) {
        togglePause();
    } else if (strcmp(start, "NEXT") == 0) {
        nextTrack();
    } else if (strcmp(start, "PREV") == 0) {
        previousTrack();
    } else if (strcmp(start, "VOL+") == 0) {
        changeVolume(1);
    } else if (strcmp(start, "VOL-") == 0) {
        changeVolume(-1);
    } else if (strncmp(start, "VOL=", 4) == 0) {
        int v = constrain(atoi(start + 4), 0, 30);
        if (v != volumeLevel) changeVolume(v - volumeLevel);
        else bleNotifyStatus();
    } else if (strncmp(start, "TRACK=", 6) == 0) {
        int t = atoi(start + 6);
        if (t >= 1 && t <= totalTracks) playTrack(t);
        else bleSend("ERR range\n");
    } else if (strcmp(start, "STATUS") == 0) {
        bleNotifyStatus();
    } else {
        bleSend("ERR unknown\n");
    }
}

void bleInit() {
    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setPower(ESP_PWR_LVL_N3); // lower radio power = lower current spikes

    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(new QuranServerCallbacks());

    BLEService *service = server->createService(NUS_SERVICE_UUID);

    bleTx = service->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    bleTx->addDescriptor(new BLE2902());

    BLECharacteristic *rx = service->createCharacteristic(
        NUS_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new QuranRxCallbacks());

    service->start();

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(NUS_SERVICE_UUID);
    advertising->setScanResponse(true);
    BLEDevice::startAdvertising();

    Serial.println("BLE ready: " BLE_DEVICE_NAME);
}

void handleBle() {
    if (bleRestartAdv && (long)(millis() - bleRestartAt) >= 0) {
        bleRestartAdv = false;
        BLEDevice::startAdvertising();
        Serial.println("BLE advertising restarted");
    }

    char cmd[BLE_CMD_LEN];
    while (bleQueuePop(cmd)) {
        executeBleCommand(cmd);
    }
}

#endif // ENABLE_BLE

// ==================== Button handling ====================
bool buttonPressed(Button &button) {
    bool reading = digitalRead(button.pin);
    unsigned long now = millis();

    if (reading != button.lastReading) {
        button.lastReading = reading;
        button.changedAt = now;
    }

    if ((now - button.changedAt) >= DEBOUNCE_MS) {
        if (reading != button.stableState) {
            button.stableState = reading;

            // INPUT_PULLUP: LOW means pressed.
            if (button.stableState == LOW) {
                button.pressedAt = now;
                button.lastRepeat = now;
                return true;
            }
        }
    }

    return false;
}

// Auto-repeat while the button is held (used for volume)
bool buttonRepeat(Button &button) {
    if (button.stableState != LOW) return false;

    unsigned long now = millis();
    if ((now - button.pressedAt) > 500 && (now - button.lastRepeat) >= 120) {
        button.lastRepeat = now;
        return true;
    }
    return false;
}

void handleButtons() {
    if (buttonPressed(buttons[VOL_UP]) || buttonRepeat(buttons[VOL_UP])) {
        changeVolume(1);
    }

    if (buttonPressed(buttons[VOL_DOWN]) || buttonRepeat(buttons[VOL_DOWN])) {
        changeVolume(-1);
    }

    if (buttonPressed(buttons[NEXT_TRACK])) {
        nextTrack();
    }

    if (buttonPressed(buttons[PREV_TRACK])) {
        previousTrack();
    }

    if (buttonPressed(buttons[PAUSE_TRACK])) {
        togglePause();
    }
}

// ==================== DFPlayer events ====================
void handlePlayerEvents() {
    while (player.available()) {
        uint8_t type = player.readType();
        int value = player.read();

        if (type == DFPlayerPlayFinished) {
            Serial.print("Finished: ");
            Serial.println(value);

            // DFPlayer often sends this event twice. Ignore anything that
            // arrives right after we requested a track, otherwise parts get skipped.
            if (millis() - lastPlayRequestAt < FINISH_GUARD_MS) {
                Serial.println("Duplicate finish event ignored");
                continue;
            }

            if (playing && !paused) {
                mismatchCount = 0;
                nextTrack();
            }
        } else if (type == DFPlayerError) {
            Serial.print("DFPlayer error: ");
            Serial.println(value);

            // Only react to a missing file if it belongs to our latest request
            if (value == FileMismatch && (millis() - lastPlayRequestAt) < 3000) {
                mismatchCount++;
                Serial.println("File missing or unreadable.");

                if (mismatchCount >= totalTracks) {
                    // Nothing playable, stop instead of looping forever
                    playing = false;
                    paused = false;
                    accumulatedMs = 0;
                    Serial.println("No playable files. Stopped.");
                } else if (currentTrack == totalTracks) {
                    playTrack(1);
                } else {
                    nextTrack();
                }
            }
        } else if (type == DFPlayerCardOnline) {
            Serial.println("SD card online");
        } else if (type == DFPlayerCardRemoved) {
            Serial.println("SD card removed");
        }
    }
}

// ==================== Setup ====================
void setup() {
    Serial.begin(115200);

    pinMode(BTN_VOL_UP, INPUT_PULLUP);
    pinMode(BTN_VOL_DOWN, INPUT_PULLUP);
    pinMode(BTN_NEXT, INPUT_PULLUP);
    pinMode(BTN_PREV, INPUT_PULLUP);
    pinMode(BTN_PAUSE, INPUT_PULLUP);

    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000); // faster OLED refresh, less time blocking the loop

    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("OLED initialization failed");
        while (true) {
            delay(1000);
        }
    }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.println("Starting Quran Player");
    display.display();

    dfSerial.begin(9600, SERIAL_8N1, DF_RX, DF_TX);
    delay(1000);

    if (!player.begin(dfSerial)) {
        Serial.println("DFPlayer initialization failed");

        display.clearDisplay();
        display.setCursor(0, 20);
        display.println("DFPlayer Error");
        display.display();

        while (true) {
            delay(1000);
        }
    }

    player.setTimeOut(500);
    player.outputDevice(DFPLAYER_DEVICE_SD);
    delay(300);

    // Count the files on the SD card (retry a few times, the module can be slow)
    int count = -1;
    for (int attempt = 0; attempt < 5 && count <= 0; attempt++) {
        count = player.readFileCounts();
        if (count <= 0) delay(300);
    }
    Serial.print("Files found on SD card: ");
    Serial.println(count);
    if (count > 0) {
        totalTracks = count;
    } else {
        Serial.println("Could not read file count, using default");
    }

    player.volume(volumeLevel);
    player.disableLoop();

    // Start Bluetooth before playback so the radio start-up current spike
    // doesn't coincide with the DFPlayer starting to play.
    bleInit();
    delay(300);

    playTrack(1);

    Serial.println("Quran Player ready");
}

// ==================== Main loop ====================
void loop() {
    handleButtons();
    handlePlayerEvents();
    handleBle();

    if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL) {
        lastDisplayUpdate = millis();
        showScreen();
    }
}
