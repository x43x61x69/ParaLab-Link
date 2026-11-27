#include "core.h"
#include "button_state.h"
#include "catalog_filter.h"
#include "ui_fonts.h"
#include "item_names.h"
#include "panel.h"
#include "link_version.h"
#include "ui_theme.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <Preferences.h>
#include <esp_heap_caps.h>

namespace {
enum class Scene { Home, Gifts, Worlds, Catalog, Detail, Confirm, Result, More, Bridge, Guide, Settings, ItemLanguage, About, LastItem };
enum class Action { None, Back, Home, Open, Previous, Next, Favorite, Confirm, Submit, Stop, Rotate, Brightness, ItemLanguage };
struct Page {
    Scene scene = Scene::Home;
    int kind = 0, filter = -1;
    unsigned number = 0, item = 0;
    Job job = Job::Item;
};
struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
struct Target { Rect bounds; Action action; unsigned value; };
constexpr unsigned choicePageSize = 6;
constexpr unsigned catalogPageSize = 4;
constexpr int bootPin = 0;
constexpr int headerHeight = 50;
constexpr int bodyTop = 54, rowHeight = 88, rowGap = 4;
constexpr int contentLeft = 12, columnGap = 4;
constexpr int pageButtonWidth = 80, rightInset = 8;
constexpr int pageButtonLeft = panel::width - rightInset - pageButtonWidth;
constexpr int pagedContentRight = pageButtonLeft - columnGap;
constexpr uint32_t backHoldMs = 650;
constexpr uint32_t displayIdleMs = 30000;
constexpr Rect versionArea{318, 0, 118, 50};
constexpr Rect previousPage{pageButtonLeft, bodyTop, pageButtonWidth, rowHeight};
constexpr Rect nextPage{pageButtonLeft, bodyTop + rowHeight + rowGap, pageButtonWidth, rowHeight};
constexpr Rect primaryAction{416, bodyTop, 112, rowHeight};
constexpr Rect secondaryAction{416, bodyTop + rowHeight + rowGap, 112, rowHeight};
constexpr int halfActionHeight = (rowHeight - rowGap) / 2;
constexpr Rect detailContinue{secondaryAction.x, secondaryAction.y, secondaryAction.w, halfActionHeight};
constexpr Rect detailFavorite{secondaryAction.x, secondaryAction.y + halfActionHeight + rowGap, secondaryAction.w, halfActionHeight};

const char* const homeChoices[] = {"Gifts", "Lab Items", "Playdates", "Lab Codes", "Coins", "More"};
const char* const giftChoices[] = {"Food & Snacks", "Toys & Decor", "Accessories", "Neon Planets", "Favorites"};
const char* const worldChoices[] = {"Lab", "Land", "Water", "Sky", "Jade Forest", "Tropical", "Glacier", "Neon Planets"};
enum MoreEntry { Favorites, LastItem, Settings, UsbBridge, LinkTest, Guide };
const char* const moreChoices[] = {"Favorites", "Last Item", "Settings", "USB Bridge", "Link Test", "Guide"};
const char* const homeHints[] = {"Food and toys", "Custom objects", "Invite a visitor", "Research records", "Money Boost", "Tools & settings"};
constexpr uint16_t homeColors[] = {rgb565(0xffbdad), CYAN, ACCENT, rgb565(0xcbd0ff), YELLOW, MUTED};
const char* guideTitles[] = {"Wiring", "Lab Items", "Gifts", "Playdates", "Lab Codes", "Coins"};
const char* guideCopy[] = {
    "Connect TX43 to Tama RX, RX44 to Tama TX, and GND to GND. Leave 3V3 open. Power both devices separately.",
    "On Tama, open Connect > Download. Wait for the receiving screen, then tap Send Item.",
    "Tap Send Gift, then open Connect > Gift on Tama and press B.",
    "Choose a visitor and tap Invite. Open Playdate on Tama and begin the connection.",
    "Open the microscope and the code page in Tama research records. Enter the displayed code on Tama.",
    "Download Money Boost using Connect > Download, then sell the item in the Tama shop for 9999 G.",
};
const uint8_t homeIcons[][8] = {
    {0x24,0x5a,0xff,0x99,0xff,0x5a,0x5a,0x7e}, // Gift box
    {0x3c,0x18,0x18,0x24,0x42,0x7e,0x7e,0x3c}, // Flask
    {0x66,0xff,0xff,0xff,0x7e,0x3c,0x18,0x00}, // Heart
    {0x7e,0x42,0x5a,0x42,0x5a,0x42,0x42,0x7e}, // Code card
    {0x3c,0x42,0x99,0xa1,0x85,0x99,0x42,0x3c}, // Coin
    {0x00,0x00,0x00,0xdb,0xdb,0x00,0x00,0x00}, // More
};

Page page, history[6];
unsigned depth, targetCount, pageCount;
Target targets[10];
Status status;
std::vector<unsigned> items;
Preferences prefs;
ButtonState bootButton;
uint16_t* frame;
uint8_t favorites[(catalogCount + 7) / 8]{};
uint16_t imagePixels[32 * 32];
bool rotated, dirty, touching, dragged, ignoreUntilRelease;
bool screenOn, wakeTouchGuard;
uint32_t lastActivityMs, lastWakeContact;
unsigned brightness;
unsigned holdStep;
unsigned itemLanguage = names::english;
int startX, startY, lastX, lastY;
uint32_t touchStart;
Target pressed{};

uint32_t codepoint(const char*& p) {
    const uint8_t first = *p++;
    if (first < 128) return first;
    int count = first < 224 ? 1 : first < 240 ? 2 : 3;
    uint32_t code = first & ((1 << (6 - count)) - 1);
    while (count-- && *p) code = (code << 6) | (uint8_t(*p++) & 63);
    return code;
}

const uifont::Face& typeface(int size) {
    if (size == 3) return uifont::Heading;
    return size == 2 ? uifont::Body : uifont::Compact;
}

const uifont::Glyph& glyph(const uifont::Face& face, uint32_t code) {
    const auto* end = face.glyphs + face.count;
    const auto* entry = std::lower_bound(face.glyphs, end, code,
        [](const uifont::Glyph& value, uint32_t key) { return value.code < key; });
    return entry != end && entry->code == code ? *entry : face.glyphs['?' - ' '];
}

void fill(Rect r, uint16_t color) {
    if (r.x < 0 || r.y < 0 || r.x + r.w > panel::width || r.y + r.h > panel::height) return;
    for (int y = r.y; y < r.y + r.h; ++y) {
        auto* start = frame + y * panel::width + r.x;
        std::fill_n(start, r.w, color);
    }
}

void surface(Rect r, uint16_t color) {
    constexpr int corner[] = {4, 2, 1, 1};
    fill({r.x, r.y + 4, r.w, r.h - 8}, color);
    for (int i = 0; i < 4; ++i) {
        const int inset = corner[i];
        fill({r.x + inset, r.y + i, r.w - 2 * inset, 1}, color);
        fill({r.x + inset, r.y + r.h - 1 - i, r.w - 2 * inset, 1}, color);
    }
}

uint16_t blendText(uint16_t background, uint16_t foreground, unsigned coverage) {
    const unsigned remaining = 15 - coverage;
    const unsigned red = ((background >> 11) * remaining + (foreground >> 11) * coverage + 7) / 15;
    const unsigned green = (((background >> 5) & 63) * remaining + ((foreground >> 5) & 63) * coverage + 7) / 15;
    const unsigned blue = ((background & 31) * remaining + (foreground & 31) * coverage + 7) / 15;
    return (red << 11) | (green << 5) | blue;
}

void drawGlyph(const uifont::Face& face, const uifont::Glyph& character,
               int left, int top, int clipLeft, int clipRight, uint16_t color) {
    const auto* bitmap = face.coverage + character.offset;
    for (int row = 0; row < character.height; ++row) {
        const int y = top + row;
        if (y < 0 || y >= panel::height) continue;
        for (int x = std::max(left, clipLeft); x < std::min(left + character.width, clipRight); ++x) {
            const unsigned index = row * character.width + x - left;
            const unsigned coverage = (bitmap[index / 2] >> (index % 2 ? 0 : 4)) & 15;
            auto& pixel = frame[y * panel::width + x];
            if (coverage) pixel = coverage == 15 ? color : blendText(pixel, color, coverage);
        }
    }
}

const char* text(const char* label, int x, int y, int width, uint16_t color, int size = 1) {
    const auto& face = typeface(size);
    const int left = std::max(0, x);
    const int right = std::min(x + width, panel::width);
    while (*label) {
        const char* next = label;
        const auto& character = glyph(face, codepoint(next));
        if (x + character.advance > right) break;
        drawGlyph(face, character, x + character.left, y + face.baseline + character.top, left, right, color);
        x += character.advance;
        label = next;
    }
    return label;
}

int textWidth(const char* label, int size) {
    const auto& face = typeface(size);
    int width = 0;
    for (const char* p = label; *p;) width += glyph(face, codepoint(p)).advance;
    return width;
}

void center(const char* label, Rect r, uint16_t color, int size = 1) {
    const int inset = std::max(0, (r.w - textWidth(label, size)) / 2);
    text(label, r.x + inset, r.y + (r.h - typeface(size).height) / 2, r.w - inset, color, size);
}

void paragraph(const char* label, int x, int y, int width, int lines, uint16_t color, int size = 2) {
    const auto& face = typeface(size);
    const int lineStep = size == 1 ? face.height : face.height - 2;
    for (int i = 0; i < lines && *label; ++i, y += lineStep) {
        char line[96];
        const char* end = label;
        const char* space = nullptr;
        int used = 0;
        while (*end && end - label < int(sizeof(line) - 4)) {
            const char* next = end;
            used += glyph(face, codepoint(next)).advance;
            if (*end == ' ') space = end;
            if (used > width) break;
            end = next;
        }
        if (*end && space && space > label) end = space;
        const size_t bytes = end - label;
        memcpy(line, label, bytes);
        line[bytes] = '\0';
        text(line, x, y, width, color, size);
        label = end;
        while (*label == ' ') ++label;
    }
}

void target(Rect r, Action action, unsigned value = 0) {
    if (targetCount < sizeof(targets) / sizeof(targets[0])) targets[targetCount++] = {r, action, value};
}

void button(Rect r, const char* label, Action action, bool primary = false) {
    const bool enabled = action != Action::None;
    const bool emphasized = enabled && (primary || action == Action::Stop);
    surface(r, emphasized ? (action == Action::Stop ? DANGER : ACCENT) : enabled ? SELECT : PANEL);
    center(label, r, emphasized ? BG : enabled ? INK : MUTED, textWidth(label, 2) <= r.w - 12 ? 2 : 1);
    if (action != Action::None) target(r, action);
}

void photo(unsigned record, Rect r) {
    if (!assetRead(record, reinterpret_cast<uint8_t*>(imagePixels), sizeof(imagePixels))) {
        fill(r, PANEL);
        center(r.w >= 96 ? "No image" : "?", r, MUTED);
        return;
    }
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
            frame[(r.y + y) * panel::width + r.x + x] = imagePixels[(y * 32 / r.h) * 32 + x * 32 / r.w];
}

void itemPreview(const CatalogItem& item) {
    surface({12, 56, 176, 176}, PANEL);
    photo(item.image, {20, 64, 160, 160});
}

void trafficIndicators(uint32_t now) {
    fill({440, 0, 96, headerHeight - 1}, PANEL);
    const bool tx = uartTxBytes.load() && uint32_t(now - lastTxMs.load()) < 800;
    const bool rx = uartRxBytes.load() && uint32_t(now - lastRxMs.load()) < 800;
    fill({444, 21, 6, 6}, tx ? GREEN : LINE);
    text("TX", 455, 17, 24, tx ? GREEN : MUTED);
    fill({491, 21, 6, 6}, rx ? ORANGE : LINE);
    text("RX", 502, 17, 24, rx ? ORANGE : MUTED);
}

void header(const char* title, uint32_t now) {
    fill({0, 0, panel::width, headerHeight}, PANEL);
    fill({0, headerHeight - 1, panel::width, 1}, LINE);
    constexpr int titleX = 14;
    constexpr int titleWidth = 239 - titleX;
    const int titleSize = textWidth(title, 3) <= titleWidth ? 3 : 2;
    text(title, titleX, 9, titleWidth, INK, titleSize);
    center(product::hardwareVersion, versionArea, MUTED);
    trafficIndicators(now);
}

Rect tile(unsigned i) {
    const int width = depth ? (pagedContentRight - contentLeft - 2 * columnGap) / 3 : 168;
    return {contentLeft + int(i % 3) * (width + columnGap), bodyTop + int(i / 3) * (rowHeight + rowGap), width, rowHeight};
}

Rect catalogTile(unsigned i) {
    const int width = (pagedContentRight - contentLeft - columnGap) / 2;
    return {contentLeft + int(i % 2) * (width + columnGap), bodyTop + int(i / 2) * (rowHeight + rowGap), width, rowHeight};
}

void paging(unsigned count, unsigned perPage) {
    pageCount = count ? (count - 1) / perPage + 1 : 1;
    page.number = std::min(page.number, pageCount - 1);
}

void drawPageArrow(Rect r, bool up, bool enabled) {
    surface(r, enabled || up ? SELECT : PANEL);
    const uint16_t color = up && holdStep ? ACCENT : enabled ? INK : up ? MUTED : LINE;
    const int centerX = r.x + r.w / 2;
    const int centerY = r.y + r.h / 2 - (up ? 5 : 0);
    const int slope = up ? 1 : -1;
    for (int offset = -14; offset <= 14; ++offset)
        fill({centerX + offset, centerY + slope * (abs(offset) - 7) - 2, 1, 4}, color);
    if (up) {
        const Rect meter{r.x + 24, r.y + r.h - 17, r.w - 48, 3};
        fill(meter, LINE);
        fill({meter.x, meter.y, int(meter.w * holdStep / 5), meter.h}, ACCENT);
    }
}

void pageControls() {
    if (!depth || page.scene == Scene::Confirm || (page.scene == Scene::Result && status.busy)) return;
    drawPageArrow(previousPage, true, page.number > 0);
    target({previousPage.x, previousPage.y, previousPage.w + rightInset, previousPage.h}, Action::Previous);
    if (pageCount > 1) {
        const bool hasNext = page.number + 1 < pageCount;
        drawPageArrow(nextPage, false, hasNext);
        if (hasNext) target({nextPage.x, nextPage.y, nextPage.w + rightInset, nextPage.h}, Action::Next);
    }
}

template <size_t count>
void choices(const char* const (&options)[count], bool home = false) {
    paging(count, choicePageSize);
    const unsigned start = page.number * choicePageSize;
    for (unsigned i = 0; i < choicePageSize && start + i < count; ++i) {
        const Rect r = tile(i);
        surface(r, PANEL);
        if (home) {
            surface({r.x + 10, r.y + 12, 36, 36}, SELECT);
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 8; ++col)
                    if (homeIcons[i][row] & (0x80 >> col))
                        fill({r.x + 16 + col * 3, r.y + 18 + row * 3, 3, 3}, homeColors[i]);
            text(options[start + i], r.x + 53, r.y + 16, r.w - 59, INK, 2);
            text(homeHints[i], r.x + 12, r.y + 57, r.w - 24, MUTED);
        } else {
            center(options[start + i], r, INK, textWidth(options[start + i], 2) <= r.w - 16 ? 2 : 1);
        }
        target(r, Action::Open, start + i);
    }
}

const char* catalogTitle() {
    switch (page.kind) {
        case 0: return giftChoices[page.filter];
        case 1: return "Lab Items";
        case 2: return worldChoices[page.filter];
        case 3: return "Lab Codes";
        default: return "Favorites";
    }
}

void listItems() {
    paging(items.size(), catalogPageSize);
    if (items.empty()) {
        surface({12, bodyTop, pagedContentRight - 12, rowHeight}, PANEL);
        text("No saved items", 24, 68, 390, INK, 2);
        text("Open an item and tap Favorite to save it here.", 24, 107, 400, MUTED);
    }
    const unsigned start = page.number * catalogPageSize;
    for (unsigned i = 0; i < catalogPageSize && start + i < items.size(); ++i) {
        const unsigned id = items[start + i];
        const auto& item = catalogItems[id];
        const Rect r = catalogTile(i);
        surface(r, PANEL);
        photo(item.image, {r.x + 4, r.y + 4, 80, 80});
        const int nameX = r.x + 89;
        const int nameWidth = r.x + r.w - nameX - 8;
        const char* name = names::catalogNames[id][itemLanguage];
        if (textWidth(name, 2) <= nameWidth) {
            text(name, nameX, r.y + 11, nameWidth, INK, 2);
        } else {
            paragraph(name, nameX, r.y + 8, nameWidth, 3, INK, 1);
        }
        const bool saved = catalog::favorite(favorites, id);
        char label[24];
        snprintf(label, sizeof(label), "#%u%s", item.id, saved ? "  *" : "");
        text(label, nameX, r.y + 65, nameWidth, saved ? ACCENT : MUTED);
        target(r, Action::Open, id);
    }
}

const char* tamaMode(const CatalogItem& item) {
    return item.kind == 0 ? "Connect > Gift" : item.kind == 2 ? "Connect > Playdate" : "Connect > Download";
}

void detail() {
    const auto& item = catalogItems[page.item];
    itemPreview(item);
    paragraph(names::catalogNames[page.item][itemLanguage], 200, 66, 204, 3, INK);
    char label[48];
    snprintf(label, sizeof(label), "Item #%u", item.id);
    if (item.kind == 3) {
        text(item.code, 200, 155, 204, YELLOW, 2);
        text("Enter in Research Records", 200, 201, 204, MUTED);
    } else {
        if (item.kind == 1 || item.kind == 4) text("Buy 0 G / Sell 9999 G", 200, 159, 204, YELLOW);
        else if (item.kind == 0 && item.code[0]) {
            snprintf(label, sizeof(label), "Shop Code %s", item.code);
            text(label, 200, 159, 204, YELLOW);
        }
        else text(item.kind == 2 ? worldChoices[item.field] : label, 200, 159, 204, CYAN);
        text(tamaMode(item), 200, 201, 204, MUTED);
        button(detailContinue, "Continue", status.busy || !status.assetOK ? Action::None : Action::Confirm, true);
    }
    button(detailFavorite, catalog::favorite(favorites, page.item) ? "Unfavorite" : "Favorite", Action::Favorite);
}

void confirmation() {
    const char* label = page.job == Job::Echo ? "Run Test" : "Replay";
    if (page.job == Job::Item) {
        const auto& item = catalogItems[page.item];
        itemPreview(item);
        paragraph(names::catalogNames[page.item][itemLanguage], 200, 64, 204, 3, INK);
        text(tamaMode(item), 200, 155, 204, CYAN);
        const bool gift = item.kind == 0, visit = item.kind == 2;
        label = gift ? "Send Gift" : visit ? "Invite" : "Send Item";
        paragraph(gift ? "Tap Send Gift, then press B in Gift on Tama."
                       : visit ? "Tap Invite, then start Playdate on Tama."
                               : "Open Download on Tama first, then tap Send Item.",
                  200, 181, 204, 2, MUTED, 1);
    } else if (page.job == Job::Echo) {
        paragraph("Put Tama in connection mode to run the UART link test.", 18, 69, 384, 3, INK);
    } else {
        char description[48];
        snprintf(description, sizeof(description), "Replay the last %u captured bytes.", status.replayBytes);
        paragraph(description, 18, 69, 384, 2, INK);
        text("The bytes will be sent to Tama.", 18, 139, 384, MUTED, 2);
    }
    button(secondaryAction, "Cancel", Action::Back);
    const bool enabled = !status.busy && (page.job != Job::ReplayBytes || status.replayBytes);
    button(primaryAction, label, enabled ? Action::Submit : Action::None, true);
}

void result() {
    constexpr int progressWidth = 364;
    const uint16_t color = status.busy ? ACCENT : status.success ? GREEN : DANGER;
    surface({12, 56, 392, 176}, PANEL);
    fill({24, 76, 4, 44}, color);
    paragraph(status.message, 38, 75, 354, 2, INK);
    const unsigned progress = std::min(100u, status.progress);
    if (status.busy || progress) {
        fill({26, 151, progressWidth, 8}, LINE);
        fill({26, 151, int(progressWidth * progress / 100), 8}, color);
        char label[24];
        snprintf(label, sizeof(label), "%u%%", progress);
        text(label, 26, 180, 80, MUTED, 2);
    }
    if (status.busy) button(primaryAction, cancelled.load() ? "Stopping" : "Stop", Action::Stop);
    else button(secondaryAction, status.success ? "Done" : "Close", Action::Back, status.success);
}

void counter(const char* label, uint32_t bytes, int x) {
    surface({x, 90, 128, 86}, PANEL);
    text(label, x + 10, 102, 108, CYAN);
    char value[24];
    snprintf(value, sizeof(value), "%lu B", static_cast<unsigned long>(bytes));
    text(value, x + 10, 132, 108, INK, textWidth(value, 2) <= 108 ? 2 : 1);
}

void bridge() {
    text(status.mode == Mode::Bridge ? "Bridge active" : "Local operation", 12, 52, 390, INK, 2);
    counter("USB in", usbInBytes.load(), 12);
    counter("UART TX", uartTxBytes.load(), 144);
    counter("UART RX", uartRxBytes.load(), 276);
    char label[40];
    snprintf(label, sizeof(label), "Captured for replay: %u B", status.replayBytes);
    text(label, 12, 194, 392, MUTED);
    button(secondaryAction, "Replay", status.replayBytes && !status.busy ? Action::Confirm : Action::None);
}

void settingRow(int y, const char* label, const char* value, Action action) {
    constexpr int valueWidth = 164, valueX = pagedContentRight - valueWidth;
    const Rect row{contentLeft, y, pagedContentRight - contentLeft, 42};
    surface(row, PANEL);
    text(label, 24, y + 5, valueX - 32, INK, 2);
    const Rect control{valueX, y + 2, valueWidth - 2, 38};
    surface(control, SELECT);
    center(value, control, INK, textWidth(value, 2) <= control.w - 12 ? 2 : 1);
    target(row, action);
}

void settings() {
    const char* levels[] = {"Low", "Medium", "High"};
    settingRow(54, "Brightness", levels[brightness], Action::Brightness);
    settingRow(100, "Orientation", rotated ? "180 deg" : "0 deg", Action::Rotate);
    settingRow(146, "Item names", names::languages[itemLanguage].label, Action::ItemLanguage);
    settingRow(192, "Device information", "About", Action::Open);
}

void itemLanguageSettings() {
    paging(names::languageCount, choicePageSize);
    const unsigned start = page.number * choicePageSize;
    for (unsigned i = 0; i < choicePageSize && start + i < names::languageCount; ++i) {
        const unsigned choice = start + i;
        const bool selected = choice == itemLanguage;
        const Rect r = tile(i);
        surface(r, selected ? ACCENT : PANEL);
        center(names::languages[choice].label, r, selected ? BG : INK, 2);
        target(r, Action::ItemLanguage, choice);
    }
}

void render(uint32_t now) {
    targetCount = 0;
    pageCount = 1;
    fill({0, 0, panel::width, panel::height}, BG);
    switch (page.scene) {
        case Scene::Home:
            header(product::title, now);
            choices(homeChoices, true);
            text(status.assetOK ? "USB bridge" : "Asset error",
                 208, 17, 105, status.assetOK ? ACCENT : YELLOW);
            break;
        case Scene::Gifts: header("Gifts", now); choices(giftChoices); break;
        case Scene::Worlds: header("Playdates", now); choices(worldChoices); break;
        case Scene::More: header("More", now); choices(moreChoices); break;
        case Scene::Catalog: header(catalogTitle(), now); listItems(); break;
        case Scene::Detail: header("Item Details", now); detail(); break;
        case Scene::Confirm:
            header(page.job == Job::Item ? "Confirm Transfer" : page.job == Job::Echo ? "Link Test" : "USB Replay", now);
            confirmation();
            break;
        case Scene::Result: header(status.busy ? "Transfer" : status.success ? "Complete" : "Transfer result", now); result(); break;
        case Scene::Bridge: header("USB Bridge", now); bridge(); break;
        case Scene::Guide:
            paging(sizeof(guideTitles) / sizeof(guideTitles[0]), 1);
            header("Guide", now);
            surface({12, 54, pagedContentRight - 12, 180}, PANEL);
            text(guideTitles[page.number], 24, 64, 400, ACCENT, 2);
            paragraph(guideCopy[page.number], 24, 103, 404, 4, INK, 1);
            text("Hold the up arrow to go back", 24, 207, 400, MUTED);
            break;
        case Scene::Settings: header("Settings", now); settings(); break;
        case Scene::ItemLanguage: header("Item Language", now); itemLanguageSettings(); break;
        case Scene::About: {
            header("About", now);
            surface({12, 54, pagedContentRight - 12, 180}, PANEL);
            char label[64];
            text(product::name, 24, 71, 400, INK, 2);
            snprintf(label, sizeof(label), "%u catalog entries", catalogCount);
            text(label, 24, 117, 400, MUTED, 2);
            text("AMOLED 536 x 240 / UART 460800", 24, 173, 400, CYAN);
            break;
        }
        case Scene::LastItem:
            header("Last Item", now);
            surface({12, 54, pagedContentRight - 12, rowHeight}, PANEL);
            text("No recent transfer", 24, 68, 390, INK, 2);
            text("Your last successful transfer appears here.", 24, 107, 390, MUTED);
            break;
    }
    if (pageCount > 1) {
        char position[24];
        snprintf(position, sizeof(position), "%u / %u", page.number + 1, pageCount);
        const Rect pageLabel{240, 10, 76, 30};
        surface(pageLabel, SELECT);
        center(position, pageLabel, INK);
    }
    pageControls();
    panel::present(frame, rotated);
}

void open(Scene scene) {
    if (depth >= sizeof(history) / sizeof(history[0])) return;
    history[depth++] = page;
    page = {};
    page.scene = scene;
}

void updateCatalog() { items = catalog::select(page.kind, page.filter, favorites); }

void catalogPage(int kind, int filter = -1) {
    open(Scene::Catalog);
    page.kind = kind;
    page.filter = filter;
    updateCatalog();
}

void itemPage(unsigned id) {
    open(Scene::Detail);
    page.item = id;
}

void confirm(Job job) {
    const unsigned item = page.item;
    open(Scene::Confirm);
    page.job = job;
    page.item = item;
}

void back() {
    if (!depth) return;
    page = history[--depth];
    if (page.scene == Scene::Catalog) updateCatalog();
}

void select(unsigned value) {
    if (page.scene == Scene::Home) {
        switch (value) {
            case 0: open(Scene::Gifts); break;
            case 1: catalogPage(1); break;
            case 2: open(Scene::Worlds); break;
            case 3: catalogPage(3); break;
            case 4: itemPage(moneyItemIndex); break;
            case 5: open(Scene::More); break;
        }
    } else if (page.scene == Scene::More) {
        switch (value) {
            case Favorites: catalogPage(catalog::allFavorites); break;
            case LastItem: if (status.lastItem >= 0) itemPage(status.lastItem); else open(Scene::LastItem); break;
            case Settings: open(Scene::Settings); break;
            case UsbBridge: open(Scene::Bridge); break;
            case LinkTest: confirm(Job::Echo); break;
            case Guide: open(Scene::Guide); break;
        }
    } else if (page.scene == Scene::Gifts) catalogPage(0, value);
    else if (page.scene == Scene::Worlds) catalogPage(2, value);
    else if (page.scene == Scene::Catalog) itemPage(value);
    else if (page.scene == Scene::Settings) open(Scene::About);
}

void activate(Action action, unsigned value = 0) {
    status = statusSnapshot();
    if (page.scene == Scene::Result && status.busy && action != Action::Stop) return;
    switch (action) {
        case Action::Back: back(); break;
        case Action::Home: page = {}; depth = 0; break;
        case Action::Open: select(value); break;
        case Action::Previous: if (page.number) --page.number; break;
        case Action::Next: if (page.number + 1 < pageCount) ++page.number; break;
        case Action::Favorite:
            favorites[page.item / 8] ^= 1 << (page.item % 8);
            prefs.putBytes("favorites", favorites, sizeof(favorites));
            break;
        case Action::Confirm: confirm(page.scene == Scene::Bridge ? Job::ReplayBytes : Job::Item); break;
        case Action::Submit:
            if ((page.job != Job::ReplayBytes || status.replayBytes) && submit(page.job, page.item))
                page.scene = Scene::Result;
            break;
        case Action::Stop: if (status.busy) cancelled = true; break;
        case Action::Rotate:
            rotated = !rotated;
            prefs.putBool("rotated", rotated);
            break;
        case Action::Brightness:
            brightness = (brightness + 1) % 3;
            panel::setBrightness(brightness);
            prefs.putUInt("brightness", brightness);
            break;
        case Action::ItemLanguage:
            if (page.scene == Scene::Settings) {
                open(Scene::ItemLanguage);
                page.number = itemLanguage / choicePageSize;
            } else if (value < names::languageCount) {
                itemLanguage = value;
                prefs.putString("item-language", names::languages[itemLanguage].code);
                back();
            }
            break;
        case Action::None: return;
    }
    dirty = true;
}

Target hit(int x, int y) {
    for (unsigned i = 0; i < targetCount; ++i)
        if (targets[i].bounds.contains(x, y)) return targets[i];
    return {};
}

void readTouch(const panel::Touch& sample, uint32_t now) {
    if (ignoreUntilRelease) {
        if (sample.event == panel::Touch::Release) ignoreUntilRelease = false;
        return;
    }
    switch (sample.event) {
        case panel::Touch::None: return;
        case panel::Touch::Invalid:
            touching = false;
            return;
        case panel::Touch::Down:
            if (!touching) {
                startX = sample.x;
                startY = sample.y;
                pressed = hit(startX, startY);
                touchStart = now;
                touching = true;
                dragged = false;
            }
            break;
        case panel::Touch::Move:
        case panel::Touch::Release: break;
    }
    if (!touching) return;
    if (sample.x >= 0 && sample.y >= 0) {
        lastX = sample.x;
        lastY = sample.y;
    }
    const int dx = lastX - startX, dy = lastY - startY;
    dragged = dragged || abs(dx) > 18 || abs(dy) > 18;
    const uint32_t held = now - touchStart;
    if (!dragged && pressed.action == Action::Previous && held >= backHoldMs &&
        pressed.bounds.contains(lastX, lastY)) {
        touching = false;
        ignoreUntilRelease = sample.event != panel::Touch::Release;
        activate(Action::Back);
        return;
    }
    if (sample.event != panel::Touch::Release) return;
    touching = false;
    if (held > 1000) return;
    if (pageCount > 1 && startY >= bodyTop && startX < previousPage.x) {
        const int distance = abs(dy) > abs(dx) ? dy : dx;
        if (abs(distance) >= 48) {
            activate(distance < 0 ? Action::Next : Action::Previous);
            return;
        }
    }
    if (!dragged) {
        const auto released = hit(lastX, lastY);
        if (pressed.action == released.action && pressed.value == released.value)
            activate(pressed.action, pressed.value);
    }
}

}

void uiBegin() {
    prefs.begin("paradise-ui", false);
    const size_t saved = prefs.getBytesLength("favorites");
    if (saved && saved <= sizeof(favorites)) prefs.getBytes("favorites", favorites, saved);
    rotated = prefs.getBool("rotated", false);
    brightness = std::min(2u, unsigned(prefs.getUInt("brightness", 1)));
    const auto savedLanguage = prefs.getString("item-language", "en");
    itemLanguage = names::english;
    for (unsigned i = 0; i < names::languageCount; ++i)
        if (savedLanguage == names::languages[i].code) {
            itemLanguage = i;
            break;
        }
    if (!panel::begin()) return;
    frame = static_cast<uint16_t*>(heap_caps_malloc(panel::width * panel::height * sizeof(uint16_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!frame) return;
    pinMode(bootPin, INPUT_PULLUP);
    bootButton.begin(digitalRead(bootPin), millis());
    status = statusSnapshot();
    render(millis());
    panel::setBrightness(brightness);
    screenOn = true;
    lastActivityMs = millis();
}

void uiTick() {
    if (!frame) return;
    const uint32_t now = millis();
    static uint32_t lastUpdatePaint;
    static bool updateVisible;
    const auto update = statusSnapshot();
    if (update.usbUpdate) {
        lastActivityMs = now;
        if (!screenOn) screenOn = panel::setDisplayOn(true, brightness);
        if (!updateVisible || now - lastUpdatePaint >= 90) {
            updateVisible = true;
            lastUpdatePaint = now;
            fill({0, 0, panel::width, panel::height}, BG);
            header("Firmware Update", now);
            char version[48];
            snprintf(version, sizeof(version), "v%s  >  %s", PARALINK_VERSION,
                     *update.updateVersion ? update.updateVersion : "Waiting for version");
            text(version, 20, 70, 496, CYAN, 2);
            text(update.message, 20, 118, 496, INK, 2);
            fill({20, 171, 414, 10}, LINE);
            fill({20, 171, int(414 * update.progress / 100), 10}, ACCENT);
            char percent[10];
            snprintf(percent, sizeof(percent), "%u%%", update.progress);
            text(percent, 448, 157, 68, ACCENT, 2);
            panel::present(frame, rotated);
        }
        return;
    }
    if (updateVisible) {
        updateVisible = false;
        ignoreUntilRelease = true;
        dirty = true;
    }
    const bool touchInterrupt = panel::takeTouchInterrupt();
    const auto sample = panel::touch(rotated);
    const bool contact = sample.event == panel::Touch::Down || sample.event == panel::Touch::Move;
    const bool released = sample.event == panel::Touch::Release && (touching || ignoreUntilRelease);
    const bool bootPressed = digitalRead(bootPin) == LOW;
    const bool traffic = takeCommunicationActivity();
    if (touchInterrupt || contact || released || bootPressed || traffic) lastActivityMs = now;

    if (!screenOn) {
        if ((touchInterrupt || contact) && sample.event != panel::Touch::Release) {
            wakeTouchGuard = true;
            lastWakeContact = now;
        }
        if (sample.event == panel::Touch::Release) wakeTouchGuard = false;
        if (bootPressed) bootButton.begin(false, now);
        if (uint32_t(now - lastActivityMs) >= displayIdleMs || !panel::setDisplayOn(true, brightness)) return;
        screenOn = true;
        dirty = true;
    } else {
        if (wakeTouchGuard) {
            // A short tap may only leave an interrupt and no readable release report.
            if (contact) lastWakeContact = now;
            else if (sample.event == panel::Touch::Release || uint32_t(now - lastWakeContact) >= 120)
                wakeTouchGuard = false;
        }
        if (!wakeTouchGuard) readTouch(sample, now);
        if (uint32_t(now - lastActivityMs) >= displayIdleMs) {
            // Keep the controller responsive and make all OLED pixels black.
            panel::setDisplayOn(false, brightness);
            fill({0, 0, panel::width, panel::height}, 0);
            panel::present(frame, rotated);
            screenOn = false;
            touching = false;
            ignoreUntilRelease = false;
            wakeTouchGuard = false;
            holdStep = 0;
            return;
        }
    }
    const auto boot = bootButton.update(!bootPressed, now);
    if (boot != ButtonState::Event::None) {
        ignoreUntilRelease = ignoreUntilRelease || touching;
        touching = false;
        const auto current = statusSnapshot();
        activate(page.scene == Scene::Result && current.busy ? Action::Stop :
                 boot == ButtonState::Event::Long ? Action::Home : Action::Back);
    }

    static uint32_t lastPoll;
    static bool lastTx, lastRx;
    static uint32_t lastBytesIn, lastBytesTx, lastBytesRx;
    const unsigned currentHold = touching && !dragged && pressed.action == Action::Previous &&
        pressed.bounds.contains(lastX, lastY) ? std::min(5u, unsigned((now - touchStart) * 6 / backHoldMs)) : 0;
    const bool holdChanged = holdStep != currentHold;
    holdStep = currentHold;
    if (dirty || uint32_t(now - lastPoll) >= 100) {
        lastPoll = now;
        const auto current = statusSnapshot();
        const bool tx = uartTxBytes.load() && uint32_t(now - lastTxMs.load()) < 800;
        const bool rx = uartRxBytes.load() && uint32_t(now - lastRxMs.load()) < 800;
        const uint32_t in = usbInBytes.load(), sent = uartTxBytes.load(), received = uartRxBytes.load();
        const bool bridgeChanged = page.scene == Scene::Bridge &&
            (in != lastBytesIn || sent != lastBytesTx || received != lastBytesRx || status.mode != current.mode ||
             status.replayBytes != current.replayBytes || status.busy != current.busy);
        const bool replayChanged = page.scene == Scene::Confirm && page.job == Job::ReplayBytes &&
            status.replayBytes != current.replayBytes;
        if (dirty || bridgeChanged || replayChanged ||
            (page.scene == Scene::Result && (status.busy != current.busy || status.progress != current.progress ||
             status.success != current.success || strcmp(status.message, current.message)))) {
            lastBytesIn = in;
            lastBytesTx = sent;
            lastBytesRx = received;
            lastTx = tx;
            lastRx = rx;
            status = current;
            dirty = false;
            render(now);
            return;
        }
        if (tx != lastTx || rx != lastRx) {
            lastTx = tx;
            lastRx = rx;
            trafficIndicators(now);
            panel::presentRows(frame, rotated, 0, headerHeight);
        }
    }
    if (holdChanged) {
        drawPageArrow(previousPage, true, page.number > 0);
        panel::presentRows(frame, rotated, previousPage.y, previousPage.y + previousPage.h);
    }
}

void uiPreparingUpdate() {
    if (!frame) return;
    panel::setDisplayOn(true, brightness);
    fill({0, 0, panel::width, panel::height}, BG);
    header("USB update", millis());
    text("Opening download mode", 18, 76, 500, INK, 3);
    text("Follow the progress on your computer.", 18, 136, 500, MUTED, 2);
    panel::present(frame, rotated);
}
