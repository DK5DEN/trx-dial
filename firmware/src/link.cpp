#include "link.h"
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

namespace dlink {

static const char *SVC_UUID = "4a415954-5258-4449-414c-000000000000";
static const char *RX_UUID  = "4a415954-5258-4449-414c-000000000001";  // host -> dial
static const char *TX_UUID  = "4a415954-5258-4449-414c-000000000002";  // dial -> host

bool linked = false;
std::vector<MenuItem> menu;
Home home;
uint32_t version = 0;

static NimBLECharacteristic *txChar = nullptr;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static String rxBuf;                    // filled from the BLE task
static volatile bool connected = false;
static volatile bool subscribed = false;

static void send(const String &json) {
    if (!txChar || !subscribed) return;
    String s = json + "\n";
    size_t chunk = 20;
    uint16_t mtu = NimBLEDevice::getServer()->getPeerMTU(0);
    if (mtu > 23) chunk = mtu - 3;
    for (size_t i = 0; i < s.length(); i += chunk) {
        String part = s.substring(i, i + chunk);
        txChar->setValue((const uint8_t *)part.c_str(), part.length());
        txChar->notify();
    }
}

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s, ble_gap_conn_desc *desc) override {
        connected = true;
        s->updateConnParams(desc->conn_handle, 12, 24, 0, 400);
    }
    void onDisconnect(NimBLEServer *) override {
        connected = false;
        subscribed = false;
        NimBLEDevice::startAdvertising();
    }
};

class RxCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string v = c->getValue();
        portENTER_CRITICAL(&mux);
        rxBuf.concat(v.data(), v.size());
        portEXIT_CRITICAL(&mux);
    }
};

class TxCb : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic *, ble_gap_conn_desc *, uint16_t subValue) override {
        subscribed = subValue != 0;
    }
};

void begin(const char *name) {
    NimBLEDevice::init(name);
    NimBLEDevice::setMTU(247);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEServer *srv = NimBLEDevice::createServer();
    srv->setCallbacks(new ServerCb());
    NimBLEService *svc = srv->createService(SVC_UUID);
    NimBLECharacteristic *rx = svc->createCharacteristic(
        RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCb());
    txChar = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    txChar->setCallbacks(new TxCb());
    svc->start();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SVC_UUID);
    adv->setScanResponse(true);
    adv->start();
}

MenuItem *item(const String &id) {
    for (auto &it : menu) if (it.id == id) return &it;
    return nullptr;
}

static String raw(JsonVariantConst v) {
    String s;
    serializeJson(v, s);
    return s;
}

static String str(JsonVariantConst v) {
    if (v.is<const char *>()) return String(v.as<const char *>());
    return v.isNull() ? String("") : raw(v);
}

static ItemType typeOf(const char *t) {
    if (!strcmp(t, "list")) return ItemType::List;
    if (!strcmp(t, "range")) return ItemType::Range;
    if (!strcmp(t, "toggle")) return ItemType::Toggle;
    return ItemType::Button;
}

static void parseMenu(JsonArrayConst items) {
    std::vector<MenuItem> next;
    for (JsonObjectConst o : items) {
        MenuItem it;
        it.id = str(o["id"]);
        if (!it.id.length()) continue;
        it.label = str(o["label"]);
        it.icon = str(o["icon"]);
        it.unit = str(o["unit"]);
        it.type = typeOf(o["type"] | "button");
        it.min = o["min"] | 0.0;
        it.max = o["max"] | 100.0;
        it.step = o["step"] | 1.0;
        if (it.step <= 0) it.step = 1;
        for (JsonArrayConst row : o["opts"].as<JsonArrayConst>())
            it.opts.push_back({raw(row[0]), str(row[1]), str(row[2])});
        MenuItem *old = item(it.id);          // keep the known value
        if (old) it.val = old->val;
        next.push_back(it);
    }
    menu.swap(next);
}

static void parseHome(JsonObjectConst o) {
    auto take = [&](const char *k, String &dst) { if (!o[k].isNull()) dst = str(o[k]); };
    take("top", home.top);
    take("big", home.big);
    take("label", home.label);
    take("pill", home.pill);
    take("pill2", home.pill2);
    take("banner", home.banner);
    take("accent", home.accent);
    take("ring", home.ring);
    if (!o["muted"].isNull()) home.muted = (o["muted"] | 0) != 0;
    if (!o["gauge"].isNull()) {
        JsonArrayConst g = o["gauge"].as<JsonArrayConst>();
        home.hasGauge = g.size() == 2;
        if (home.hasGauge) { home.gauge = g[0] | 0.0; home.gaugeMax = g[1] | 1.0; }
    }
}

static void handleLine(const String &line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return;
    const char *t = doc["t"] | "";
    if (!strcmp(t, "menu")) parseMenu(doc["items"].as<JsonArrayConst>());
    else if (!strcmp(t, "home")) parseHome(doc.as<JsonObjectConst>());
    else if (!strcmp(t, "val")) {
        for (JsonPairConst kv : doc["v"].as<JsonObjectConst>()) {
            MenuItem *it = item(String(kv.key().c_str()));
            if (it) it->val = raw(kv.value());
        }
    } else return;
    version++;
}

void update() {
    bool now = connected && subscribed;
    if (now && !linked) send("{\"c\":\"hello\",\"v\":2}");
    if (!now && linked) { menu.clear(); home = Home(); version++; }
    linked = now;

    String chunk;
    portENTER_CRITICAL(&mux);
    chunk = rxBuf;
    rxBuf = "";
    portEXIT_CRITICAL(&mux);
    static String acc;
    acc += chunk;
    int nl;
    while ((nl = acc.indexOf('\n')) >= 0) {
        String line = acc.substring(0, nl);
        acc.remove(0, nl + 1);
        if (line.length()) handleLine(line);
    }
    if (acc.length() > 16384) acc = "";
}

static String quoted(const String &s) {
    JsonDocument d;
    d.set(s);
    return raw(d.as<JsonVariantConst>());
}

void sendTurn(int d) { send("{\"c\":\"turn\",\"d\":" + String(d) + "}"); }
void sendClick() { send("{\"c\":\"click\"}"); }
void sendSet(const MenuItem &it, const String &rawValue) {
    send("{\"c\":\"set\",\"id\":" + quoted(it.id) + ",\"v\":" + rawValue + "}");
}
void sendPress(const MenuItem &it) { send("{\"c\":\"press\",\"id\":" + quoted(it.id) + "}"); }

}  // namespace dlink
