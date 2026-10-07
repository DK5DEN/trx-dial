// BLE peripheral side of the dial protocol v2 (see docs/protocol.md).
#pragma once
#include <Arduino.h>
#include <vector>

enum class ItemType { List, Range, Toggle, Button };

struct Option {
    String value;   // raw JSON of the option value (number or quoted string)
    String title;
    String sub;
};

struct MenuItem {
    String id, label, icon, unit;
    ItemType type = ItemType::Button;
    std::vector<Option> opts;
    double min = 0, max = 100, step = 1;
    String val;     // raw JSON of the current value, "" = unknown

    double num() const { return val.toDouble(); }
    bool on() const { return val.length() && val != "0" && val != "false"; }
    int optIndex() const {
        for (size_t i = 0; i < opts.size(); i++) if (opts[i].value == val) return (int)i;
        return -1;
    }
};

struct Home {
    String top, big, label, pill, pill2, banner;
    String accent = "green";
    String ring = "off";
    bool hasGauge = false;
    double gauge = 0, gaugeMax = 1;
    bool muted = false;
};

namespace dlink {

extern bool linked;                    // BLE connected and subscribed
extern std::vector<MenuItem> menu;
extern Home home;
extern uint32_t version;               // bumps on every received change

void begin(const char *name);
void update();                         // parse received lines, call from loop

MenuItem *item(const String &id);

void sendTurn(int detents);
void sendClick();
void sendSet(const MenuItem &it, const String &rawValue);
void sendPress(const MenuItem &it);

}  // namespace dlink
