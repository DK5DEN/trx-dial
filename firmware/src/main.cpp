// TRX-Dial: generic BLE rotary controller (protocol v2, docs/protocol.md).
//
// The host describes a home screen and a menu; this firmware only draws them
// and reports turns, clicks and chosen values. Home screen: turning and
// clicking are passed to the host. A long press or a tap on the screen opens
// the menu; host items come first, then the dial's own settings. Every other
// screen falls back to the home screen after a few seconds idle.
#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include "board.h"
#include "link.h"

// ---- fonts: numbers in DejaVu (ASCII), text in efont (Unicode, umlauts) ----
#define FONT_HUGE  (&fonts::DejaVu56)
#define FONT_NUM   (&fonts::DejaVu40)
#define FONT_NUMS  (&fonts::DejaVu24)
#define FONT_BIG   (&fonts::efontJA_24_b)
#define FONT_TEXT  (&fonts::efontJA_16_b)
#define FONT_SMALL (&fonts::efontJA_12)

// ---- palette (RGB565) ----
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static const uint16_t C_BG      = rgb(8, 10, 14);
static const uint16_t C_PANEL   = rgb(24, 28, 36);
static const uint16_t C_TRACK   = rgb(40, 46, 58);
static const uint16_t C_FG      = rgb(236, 240, 245);
static const uint16_t C_DIM     = rgb(130, 140, 155);
static const uint16_t C_FAINT   = rgb(70, 78, 92);
static const uint16_t C_GREEN   = rgb(52, 211, 120);
static const uint16_t C_RED     = rgb(239, 68, 68);
static const uint16_t C_BLUE    = rgb(56, 132, 255);
static const uint16_t C_ORANGE  = rgb(251, 146, 60);
static const uint16_t C_YELLOW  = rgb(250, 204, 21);

static const int CX = 120, CY = 120;    // M5Dial: 240x240

static uint16_t named(const String &n) {
    if (n == "yellow") return C_YELLOW;
    if (n == "orange") return C_ORANGE;
    if (n == "red") return C_RED;
    if (n == "blue") return C_BLUE;
    return C_GREEN;
}

// ---- screens and menu entries ----
enum class Mode { Home, Menu, List, Range, Bright };
enum class Local { None, Bright, Sound, Back };

struct Entry {
    int host;                   // index into dlink::menu, or -1 for a local entry
    Local local;
    Entry(int h, Local l) : host(h), local(l) {}
};

static M5Canvas canvas(&board::display());
static Preferences prefs;
static Mode mode = Mode::Home;
static std::vector<Entry> entries;
static int menuSel = 0;
static String openId;               // host item shown in List/Range
static int listSel = 0;
static double rangeVal = 0;
static bool rangeDirty = false;
static uint32_t rangeSentAt = 0;
static String pendingVal;           // list value sent, until the host confirms
static uint32_t pendingAt = 0;
static int brightness = 140;
static bool soundOn = true;
static int turnAcc = 0;
static uint32_t turnSentAt = 0;
static uint32_t lastInput = 0;
static uint32_t seenVersion = 0xFFFFFFFF;
static bool dirty = true;
static int injSteps = 0;            // serial test input
static bool injClick = false, injLong = false;

static const uint32_t IDLE_TIMEOUT_MS = 8000;

static void beep(uint16_t f, uint16_t ms) {
    if (soundOn) board::beep(f, ms);
}

static MenuItem *openItem() { return openId.length() ? dlink::item(openId) : nullptr; }

static void buildEntries() {
    entries.clear();
    for (size_t i = 0; i < dlink::menu.size(); i++) entries.emplace_back((int)i, Local::None);
    entries.emplace_back(-1, Local::Bright);
    entries.emplace_back(-1, Local::Sound);
    entries.emplace_back(-1, Local::Back);
    if (menuSel >= (int)entries.size()) menuSel = 0;
}

static void setMode(Mode m) {
    mode = m;
    if (m == Mode::Home || m == Mode::Menu) openId = "";
    dirty = true;
}

// ------------------------------------------------------------- primitives

static void utf8Chop(String &t) {
    int i = t.length() - 1;
    while (i > 0 && (t[i] & 0xC0) == 0x80) i--;
    t.remove(i);
}

static void text(const String &s, int x, int y, const lgfx::IFont *f, uint16_t col,
                 int maxW = 200, textdatum_t datum = middle_center) {
    canvas.setFont(f);
    canvas.setTextColor(col);
    canvas.setTextDatum(datum);
    String t = s;
    bool cut = false;
    while (t.length() > 1 && canvas.textWidth(t) > maxW) { utf8Chop(t); cut = true; }
    if (cut && t.length() > 1) { utf8Chop(t); t += "…"; }
    canvas.drawString(t, x, y);
}

static void centered(const String &s, int y, const lgfx::IFont *f, uint16_t col, int maxW = 200) {
    text(s, CX, y, f, col, maxW);
}

/// Largest of the given fonts in which s fits into maxW.
static const lgfx::IFont *fit(const String &s, int maxW, std::initializer_list<const lgfx::IFont *> fonts) {
    const lgfx::IFont *last = nullptr;
    for (auto f : fonts) {
        canvas.setFont(f);
        last = f;
        if (canvas.textWidth(s) <= maxW) return f;
    }
    return last;
}

static bool asciiOnly(const String &s) {
    for (size_t i = 0; i < s.length(); i++) if ((uint8_t)s[i] >= 0x80) return false;
    return true;
}

/// Big centred value: DejaVu for plain ASCII (numbers, frequencies), efont otherwise.
static void bigValue(const String &s, int y, uint16_t col, int maxW) {
    const lgfx::IFont *f = asciiOnly(s) ? fit(s, maxW, {FONT_HUGE, FONT_NUM, FONT_NUMS})
                                        : fit(s, maxW, {FONT_BIG, FONT_TEXT});
    centered(s, y, f, col, maxW);
}

/// Segmented 270° gauge (gap at the bottom).
static void gauge(double value, double max, uint16_t on, int r0, int r1) {
    int n = max >= 1 && max <= 20 ? (int)max : 20;
    float span = 270.0f / n;
    int lit = max > 0 ? (int)(value / max * n + 0.5) : 0;
    if (value > 0 && lit == 0) lit = 1;
    for (int i = 0; i < n; i++) {
        float a0 = 135 + i * span + 1.2f, a1 = 135 + (i + 1) * span - 1.2f;
        canvas.fillArc(CX, CY, r0, r1, a0, a1, i < lit ? on : C_TRACK);
    }
}

static void pill(int y, int h, uint16_t bg, const String &label, const lgfx::IFont *f, uint16_t fg,
                 int maxW = 150) {
    canvas.setFont(f);
    int w = canvas.textWidth(label);
    if (w > maxW) w = maxW;
    int pw = w + 24;
    canvas.fillSmoothRoundRect(CX - pw / 2, y - h / 2, pw, h, h / 2, bg);
    text(label, CX, y + 1, f, fg, maxW);
}

static String fmtNum(double v, double step) {
    if (step >= 1 || step <= 0) return String((long)lround(v));
    int dec = step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3;
    return String(v, dec);
}

// ------------------------------------------------------------------ icons

static void iconSpeaker(int x, int y, uint16_t c, bool crossed) {
    canvas.fillRect(x - 16, y - 7, 9, 14, c);
    canvas.fillTriangle(x - 8, y - 7, x + 2, y - 16, x + 2, y + 16, c);
    canvas.fillTriangle(x - 8, y - 7, x - 8, y + 7, x + 2, y + 16, c);
    if (crossed) {
        canvas.drawWideLine(x + 8, y - 8, x + 20, y + 8, 2.0f, c);
        canvas.drawWideLine(x + 8, y + 8, x + 20, y - 8, 2.0f, c);
    } else {
        canvas.drawArc(x + 2, y, 10, 8, -45, 45, c);
        canvas.drawArc(x + 2, y, 17, 15, -50, 50, c);
    }
}

static void iconList(int x, int y, uint16_t c) {
    for (int i = -1; i <= 1; i++) {
        canvas.fillSmoothCircle(x - 14, y + i * 11, 3, c);
        canvas.drawWideLine(x - 6, y + i * 11, x + 16, y + i * 11, 2.0f, c);
    }
}

static void iconLock(int x, int y, uint16_t c, bool open) {
    canvas.fillSmoothRoundRect(x - 14, y - 2, 28, 20, 4, c);
    int sx = open ? x + 6 : x;
    canvas.drawArc(sx, y - 6, 10, 7, 180, 360, c);
    canvas.fillRect(sx - 10, y - 6, 3, open ? 2 : 6, c);
    canvas.fillRect(sx + 7, y - 6, 3, 6, c);
    canvas.fillSmoothCircle(x, y + 7, 3, C_PANEL);
}

static void iconSun(int x, int y, uint16_t c) {
    canvas.fillSmoothCircle(x, y, 8, c);
    for (int i = 0; i < 8; i++) {
        float a = i * PI / 4;
        canvas.drawWideLine(x + cosf(a) * 13, y + sinf(a) * 13, x + cosf(a) * 19, y + sinf(a) * 19, 1.6f, c);
    }
}

static void iconNote(int x, int y, uint16_t c, bool crossed) {
    canvas.fillSmoothCircle(x - 6, y + 10, 6, c);
    canvas.drawWideLine(x - 1, y + 10, x - 1, y - 14, 1.6f, c);
    canvas.drawWideLine(x - 1, y - 14, x + 11, y - 8, 2.2f, c);
    if (crossed) canvas.drawWideLine(x - 16, y - 16, x + 16, y + 16, 2.2f, C_RED);
}

static void iconBack(int x, int y, uint16_t c) {
    canvas.drawWideLine(x - 12, y, x + 14, y, 2.4f, c);
    canvas.drawWideLine(x - 12, y, x - 2, y - 10, 2.4f, c);
    canvas.drawWideLine(x - 12, y, x - 2, y + 10, 2.4f, c);
}

static void iconWave(int x, int y, uint16_t c) {
    int px = x - 18, py = y;
    for (int i = 1; i <= 36; i++) {
        int nx = x - 18 + i, ny = y - (int)(sinf(i * PI / 9) * 10);
        canvas.drawWideLine(px, py, nx, ny, 1.4f, c);
        px = nx; py = ny;
    }
}

static void iconBand(int x, int y, uint16_t c) {
    for (int i = 0; i < 4; i++) {
        int h = 8 + i * 6;
        canvas.fillSmoothRoundRect(x - 17 + i * 9, y + 14 - h, 6, h, 2, c);
    }
}

static void iconFilter(int x, int y, uint16_t c) {
    canvas.drawWideLine(x - 20, y + 12, x - 10, y + 12, 1.6f, c);
    canvas.drawWideLine(x - 10, y + 12, x - 5, y - 10, 1.6f, c);
    canvas.drawWideLine(x - 5, y - 10, x + 5, y - 10, 1.6f, c);
    canvas.drawWideLine(x + 5, y - 10, x + 10, y + 12, 1.6f, c);
    canvas.drawWideLine(x + 10, y + 12, x + 20, y + 12, 1.6f, c);
}

static void iconMemory(int x, int y, uint16_t c) {
    canvas.drawRoundRect(x - 10, y - 16, 24, 28, 3, c);
    canvas.fillSmoothRoundRect(x - 15, y - 11, 24, 28, 3, c);
    canvas.fillRect(x - 10, y - 4, 14, 2, C_PANEL);
    canvas.fillRect(x - 10, y + 2, 14, 2, C_PANEL);
}

static void iconMode(int x, int y, uint16_t c) {
    text("Mode", x, y, FONT_SMALL, c, 50);
    canvas.drawRoundRect(x - 22, y - 11, 44, 22, 6, c);
}

static void iconGear(int x, int y, uint16_t c) {
    for (int i = 0; i < 8; i++) {
        float a = i * PI / 4;
        canvas.drawWideLine(x + cosf(a) * 10, y + sinf(a) * 10, x + cosf(a) * 17, y + sinf(a) * 17, 3.0f, c);
    }
    canvas.fillSmoothCircle(x, y, 12, c);
    canvas.fillSmoothCircle(x, y, 5, C_PANEL);
}

static void iconDot(int x, int y, uint16_t c) {
    canvas.fillSmoothCircle(x, y, 9, c);
}

static void hostIcon(const MenuItem &it, int x, int y, uint16_t c) {
    const String &n = it.icon;
    if (n == "speaker") iconSpeaker(x - 2, y, c, false);
    else if (n == "mute") iconSpeaker(x - 2, y, c, it.type == ItemType::Toggle && it.on());
    else if (n == "list") iconList(x, y, c);
    else if (n == "lock") iconLock(x, y, c, !(it.type == ItemType::Toggle && it.on()));
    else if (n == "wave") iconWave(x, y, c);
    else if (n == "band") iconBand(x, y, c);
    else if (n == "filter") iconFilter(x, y, c);
    else if (n == "memory") iconMemory(x, y, c);
    else if (n == "mode") iconMode(x, y, c);
    else if (n == "gear") iconGear(x, y, c);
    else iconDot(x, y, c);
}

static void entryIcon(const Entry &e, int x, int y, uint16_t c) {
    if (e.host >= 0) { hostIcon(dlink::menu[e.host], x, y, c); return; }
    if (e.local == Local::Bright) iconSun(x, y, c);
    else if (e.local == Local::Sound) iconNote(x, y, c, !soundOn);
    else iconBack(x, y, c);
}

static String entryLabel(const Entry &e) {
    if (e.host >= 0) return dlink::menu[e.host].label;
    if (e.local == Local::Bright) return "Helligkeit";
    if (e.local == Local::Sound) return "Quittungstöne";
    return "Zurück";
}

// ----------------------------------------------------------------- screens

static void drawRing() {
    const String &r = dlink::home.ring;
    uint16_t c = C_FAINT;
    int th = 3;
    if (!dlink::linked || r == "off") { c = C_FAINT; th = 3; }
    else if (r == "tx") { c = C_RED; th = 8; }
    else if (r == "rx") { c = C_GREEN; th = 8; }
    else { c = C_BLUE; th = 3; }
    canvas.fillArc(CX, CY, 119, 119 - th, 0, 360, c);
}

/// Top line: banner while receiving/sending, otherwise the host's status line.
static void drawHeader() {
    const Home &h = dlink::home;
    if (h.banner.length() && (h.ring == "rx" || h.ring == "tx")) {
        bool tx = h.ring == "tx";
        pill(40, 24, tx ? C_RED : C_GREEN, h.banner, FONT_TEXT, tx ? C_FG : C_BG, 150);
    } else if (h.top.length()) {
        centered(h.top, 40, FONT_SMALL, C_DIM, 150);
    }
}

static void drawHome() {
    const Home &h = dlink::home;
    uint16_t ac = named(h.accent);
    if (h.hasGauge) gauge(h.gauge, h.gaugeMax, h.muted ? C_ORANGE : ac, 108, 96);
    drawHeader();
    if (h.muted) {
        iconSpeaker(CX - 2, CY - 6, C_ORANGE, true);
        centered("STUMM", CY + 28, FONT_TEXT, C_ORANGE);
    } else {
        bigValue(h.big, CY - 4, C_FG, h.hasGauge ? 170 : 200);
        if (h.label.length()) centered(h.label, CY + 28, FONT_SMALL, C_DIM, 160);
    }
    if (h.pill.length() || h.pill2.length()) {
        int y = CY + 54;
        canvas.setFont(FONT_TEXT);
        int w1 = canvas.textWidth(h.pill);
        canvas.setFont(FONT_SMALL);
        int w2 = h.pill2.length() ? canvas.textWidth(h.pill2) + 8 : 0;
        int w = min(w1 + w2, 130);
        canvas.fillSmoothRoundRect(CX - w / 2 - 12, y - 15, w + 24, 30, 15, C_PANEL);
        int x = CX - w / 2;
        text(h.pill, x, y + 1, FONT_TEXT, ac, 130, middle_left);
        if (h.pill2.length()) text(h.pill2, x + w1 + 8, y + 1, FONT_SMALL, C_FG, max(10, 130 - w1 - 8), middle_left);
    }
}

/// State of an entry as a short pill under its label in the menu.
static String entryState(const Entry &e, bool &on, bool &isToggle) {
    isToggle = false;
    if (e.host < 0) {
        if (e.local == Local::Sound) { isToggle = true; on = soundOn; return on ? "AN" : "AUS"; }
        if (e.local == Local::Bright) return String(brightness * 100 / 255) + " %";
        return "";
    }
    const MenuItem &it = dlink::menu[e.host];
    switch (it.type) {
        case ItemType::Toggle: isToggle = true; on = it.on(); return on ? "AN" : "AUS";
        case ItemType::Range:  return it.val.length() ? fmtNum(it.num(), it.step) + (it.unit.length() ? " " + it.unit : "") : "";
        case ItemType::List: {
            int i = it.optIndex();
            if (i < 0) return "";
            const Option &o = it.opts[i];
            return o.sub.length() && o.title.length() <= 6 ? o.title + " " + o.sub : o.title;
        }
        default: return "";
    }
}

static void drawMenu() {
    int n = entries.size();
    for (int i = 0; i < n; i++) {         // position dots along the upper arc
        float step = n > 9 ? 90.0f / (n - 1) : 11;
        float a = (-90 + (i - (n - 1) / 2.0f) * step) * DEG_TO_RAD;
        int x = CX + cosf(a) * 100, y = CY + sinf(a) * 100;
        canvas.fillSmoothCircle(x, y, i == menuSel ? 4 : 2, i == menuSel ? C_YELLOW : C_FAINT);
    }
    auto at = [&](int off) -> const Entry & { return entries[((menuSel + off) % n + n) % n]; };
    entryIcon(at(-1), CX - 78, CY - 8, C_FAINT);
    entryIcon(at(1), CX + 78, CY - 8, C_FAINT);
    const Entry &e = at(0);
    canvas.fillSmoothCircle(CX, CY - 8, 36, C_PANEL);
    canvas.drawArc(CX, CY - 8, 37, 35, 0, 360, C_YELLOW);
    entryIcon(e, CX, CY - 8, C_FG);
    centered(entryLabel(e), CY + 46, FONT_BIG, C_YELLOW, 180);
    bool on = false, isToggle = false;
    String st = entryState(e, on, isToggle);
    if (st.length()) {
        if (isToggle) pill(CY + 76, 20, on ? C_GREEN : C_TRACK, st, FONT_SMALL, on ? C_BG : C_DIM);
        else centered(st, CY + 76, FONT_SMALL, C_DIM, 150);
    }
}

static void drawList() {
    MenuItem *it = openItem();
    if (!it || it->opts.empty()) { centered("keine Einträge", CY, FONT_TEXT, C_DIM); return; }
    int n = it->opts.size();
    int cur = it->optIndex();
    auto opt = [&](int off) -> const Option & { return it->opts[((listSel + off) % n + n) % n]; };
    auto col = [&](int idx, uint16_t other) { return idx == cur ? C_GREEN : other; };

    centered(it->label, 38, FONT_SMALL, C_DIM, 150);
    if (n > 1) {                          // scroll indicator on the right edge
        float span = 100.0f, a0 = -span / 2, seg = span / n, s0 = a0 + listSel * seg;
        canvas.fillArc(CX, CY, 110, 107, a0, a0 + span, C_TRACK);
        canvas.fillArc(CX, CY, 111, 106, s0, s0 + (seg < 6 ? 6 : seg), C_YELLOW);
        const Option &p = opt(-1), &q = opt(1);
        centered(p.title + (p.sub.length() ? "  " + p.sub : ""), CY - 50, FONT_SMALL,
                 col(((listSel - 1) % n + n) % n, C_FAINT), 150);
        centered(q.title + (q.sub.length() ? "  " + q.sub : ""), CY + 62, FONT_SMALL,
                 col((listSel + 1) % n, C_FAINT), 150);
    }
    const Option &o = opt(0);
    bool isCur = listSel == cur;
    bool isPending = pendingVal.length() && o.value == pendingVal;
    canvas.fillSmoothRoundRect(22, CY - 30, 196, 72, 18, C_PANEL);
    uint16_t c = isCur ? C_GREEN : isPending ? C_ORANGE : C_FG;
    bigValue(o.title, CY - 8, c, 180);
    if (o.sub.length()) centered(o.sub, CY + 25, FONT_TEXT, C_FG, 180);
    String foot = isCur ? "aktiv" : isPending ? "wechsle …" : "Klick = wählen";
    centered(foot, CY + 86, FONT_SMALL, isCur ? C_GREEN : C_DIM);
}

static void drawRange() {
    MenuItem *it = openItem();
    if (!it) return;
    double span = it->max - it->min;
    gauge(rangeVal - it->min, span > 0 ? span : 1, C_GREEN, 108, 96);
    centered(it->label, 38, FONT_SMALL, C_DIM, 150);
    bigValue(fmtNum(rangeVal, it->step), CY - 4, C_FG, 160);
    if (it->unit.length()) centered(it->unit, CY + 28, FONT_SMALL, C_DIM);
}

static void drawBright() {
    gauge(brightness, 255, C_YELLOW, 108, 96);
    iconSun(CX, CY - 22, C_YELLOW);
    centered(String(brightness * 100 / 255) + " %", CY + 18, FONT_BIG, C_FG);
    centered("Helligkeit", CY + 48, FONT_SMALL, C_DIM);
}

static void drawOffline() {
    static uint8_t phase = 0;
    phase++;
    for (int i = 0; i < 3; i++)
        canvas.fillSmoothCircle(CX - 16 + i * 16, CY + 28, 4, (phase / 4) % 3 == i ? C_BLUE : C_TRACK);
    centered("TRX-Dial", CY - 12, FONT_BIG, C_FG);
    centered("warte auf Verbindung", CY + 56, FONT_SMALL, C_DIM);
}

static void render() {
    canvas.fillScreen(C_BG);
    drawRing();
    if (!dlink::linked) drawOffline();
    else switch (mode) {
        case Mode::Home:   drawHome(); break;
        case Mode::Menu:   drawMenu(); break;
        case Mode::List:   drawList(); break;
        case Mode::Range:  drawRange(); break;
        case Mode::Bright: drawBright(); break;
    }
    canvas.pushSprite(0, 0);
}

// ------------------------------------------------------------ serial tools

/// "shot" dumps the frame as base64 RGB565; "rot N", "click", "long" fake input.
static void serialCommands() {
    static String line;
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch != '\n' && ch != '\r') { line += ch; continue; }
        if (line.startsWith("rot ")) injSteps += line.substring(4).toInt();
        if (line == "click") injClick = true;
        if (line == "long") injLong = true;
        if (line == "shot") {
            const uint8_t *buf = (const uint8_t *)canvas.getBuffer();
            size_t len = (size_t)canvas.width() * canvas.height() * 2;
            Serial.printf("RAW %d %d\n", canvas.width(), canvas.height());
            static uint8_t out[4100];
            for (size_t i = 0; i < len; i += 3072) {
                size_t n = len - i < 3072 ? len - i : 3072, olen = 0;
                mbedtls_base64_encode(out, sizeof(out), &olen, buf + i, n);
                Serial.write(out, olen);
                Serial.write('\n');
                Serial.flush();
            }
            Serial.println("END");
        }
        line = "";
    }
}

// ---------------------------------------------------------------- behaviour

static void openMenu() {
    buildEntries();
    menuSel = 0;
    setMode(Mode::Menu);
    beep(2000, 30);
}

static void runEntry(const Entry &e) {
    beep(2400, 30);
    if (e.host < 0) {
        if (e.local == Local::Bright) setMode(Mode::Bright);
        else if (e.local == Local::Sound) { soundOn = !soundOn; prefs.putBool("sound", soundOn); beep(2400, 30); }
        else setMode(Mode::Home);
        return;
    }
    MenuItem &it = dlink::menu[e.host];
    switch (it.type) {
        case ItemType::List:
            openId = it.id;
            listSel = max(0, it.optIndex());
            pendingVal = "";
            mode = Mode::List;
            break;
        case ItemType::Range:
            openId = it.id;
            rangeVal = it.val.length() ? it.num() : it.min;
            mode = Mode::Range;
            break;
        case ItemType::Toggle:
            dlink::sendSet(it, it.on() ? "0" : "1");
            break;
        case ItemType::Button:
            dlink::sendPress(it);
            break;
    }
    dirty = true;
}

static void handleInput() {
    uint32_t now = millis();
    int steps = board::encoderSteps() + injSteps;
    injSteps = 0;
    bool click = board::clicked() || injClick;
    injClick = false;
    bool lng = board::longPressed() || injLong;
    injLong = false;
    int tx = 0, ty = 0;
    bool tap = board::tapped(&tx, &ty);
    if (steps || click || lng || tap) { lastInput = now; dirty = true; }
    if (!dlink::linked) return;

    if (tap && mode != Mode::Menu) { openMenu(); return; }
    if (tap) {                              // in the menu: tap an item to open it, elsewhere to close
        auto near = [&](int x, int y, int r) { return (tx - x) * (tx - x) + (ty - y) * (ty - y) <= r * r; };
        int off = 99;
        if (near(CX, CY - 8, 46) || (abs(tx - CX) < 90 && ty > CY + 28 && ty < CY + 90)) off = 0;
        else if (near(CX - 78, CY - 8, 34)) off = -1;
        else if (near(CX + 78, CY - 8, 34)) off = 1;
        if (off == 99) { setMode(Mode::Home); beep(2000, 30); return; }
        int n = entries.size();
        menuSel = ((menuSel + off) % n + n) % n;
        runEntry(entries[menuSel]);
        return;
    }
    if (lng) {
        if (mode == Mode::Menu) { setMode(Mode::Home); beep(2000, 30); }
        else openMenu();
        return;
    }

    switch (mode) {
        case Mode::Home:
            turnAcc += steps;
            if (click) { dlink::sendClick(); beep(1500, 30); }
            break;

        case Mode::Menu: {
            int n = entries.size();
            if (steps) { menuSel = ((menuSel + steps) % n + n) % n; beep(3200, 4); }
            if (click) runEntry(entries[menuSel]);
            break;
        }

        case Mode::List: {
            MenuItem *it = openItem();
            if (!it || it->opts.empty()) { if (click) setMode(Mode::Home); break; }
            int n = it->opts.size();
            if (steps) { listSel = ((listSel + steps) % n + n) % n; beep(3200, 4); }
            if (click) {
                if (listSel == it->optIndex()) { setMode(Mode::Home); break; }
                pendingVal = it->opts[listSel].value;
                pendingAt = now;
                dlink::sendSet(*it, pendingVal);
                beep(2400, 40);
            }
            break;
        }

        case Mode::Range: {
            MenuItem *it = openItem();
            if (!it) break;
            if (steps) {
                rangeVal = constrain(rangeVal + steps * it->step, it->min, it->max);
                rangeDirty = true;
            }
            if (click) setMode(Mode::Home);
            break;
        }

        case Mode::Bright:
            if (steps) {
                brightness = constrain(brightness + steps * 8, 16, 255);
                board::setBrightness(brightness);
            }
            if (click) { prefs.putUChar("bright", brightness); setMode(Mode::Home); }
            break;
    }
}

static void tick() {
    uint32_t now = millis();
    if (turnAcc && now - turnSentAt > 40) {        // home turns, batched
        dlink::sendTurn(turnAcc);
        turnAcc = 0;
        turnSentAt = now;
    }
    if (rangeDirty && now - rangeSentAt > 80) {    // range values, throttled
        MenuItem *it = openItem();
        if (it) dlink::sendSet(*it, fmtNum(rangeVal, it->step));
        rangeDirty = false;
        rangeSentAt = now;
    }
    if (pendingVal.length()) {
        MenuItem *it = openItem();
        if (it && it->val == pendingVal) {         // host confirmed: back home
            pendingVal = "";
            setMode(Mode::Home);
        } else if (now - pendingAt > 4000) {
            pendingVal = "";
            beep(400, 150);
            dirty = true;
        }
    }
    if (mode != Mode::Home && !pendingVal.length() && now - lastInput > IDLE_TIMEOUT_MS) {
        if (mode == Mode::Bright) prefs.putUChar("bright", brightness);
        setMode(Mode::Home);
    }
    if (dlink::version != seenVersion) {
        seenVersion = dlink::version;
        if (mode == Mode::Menu) buildEntries();
        if ((mode == Mode::List || mode == Mode::Range) && !openItem()) setMode(Mode::Home);
        if (mode == Mode::Range && !rangeDirty && now - lastInput > 1000) {
            MenuItem *it = openItem();
            if (it && it->val.length()) rangeVal = it->num();
        }
        dirty = true;
    }
    static uint32_t animAt = 0;                    // offline animation
    if (!dlink::linked && now - animAt > 120) { animAt = now; dirty = true; }
}

void setup() {
    Serial.setTxTimeoutMs(500);
    Serial.begin(115200);
    board::begin();
    prefs.begin("dial", false);
    brightness = prefs.getUChar("bright", 140);
    soundOn = prefs.getBool("sound", true);
    board::setBrightness(brightness);
    canvas.setColorDepth(16);
    canvas.createSprite(board::width(), board::height());
    dlink::begin("TRX-Dial");
    buildEntries();
}

void loop() {
    static bool lastLinked = false;
    board::update();
    dlink::update();
    serialCommands();
    handleInput();
    tick();
    if (dlink::linked != lastLinked) {
        lastLinked = dlink::linked;
        if (!lastLinked) setMode(Mode::Home);
        dirty = true;
    }
    if (dirty) { dirty = false; render(); }
    delay(5);
}
