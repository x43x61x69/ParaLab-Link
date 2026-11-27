#include "core.h"
#include "button_state.h"
#include "catalog_filter.h"
#include "item_names.h"
#include "link_version.h"
#include "ui_fonts.h"
#include "ui_theme.h"
#include <Preferences.h>
#include <TFT_eSPI.h>

namespace {
constexpr unsigned rows = 4;
constexpr int rowTop = 33, rowPitch = 23;
constexpr uint32_t idleMs = 30000;
using Options = const char* const;
Options home[]{"Gifts", "Lab Items", "Playdates", "Lab Codes", "Coins", "More"};
Options gifts[]{"Food & Snacks", "Toys & Decor", "Accessories", "Neon Planets", "Favorites"};
Options worlds[]{"Lab", "Land", "Water", "Sky", "Jade Forest", "Tropical", "Glacier", "Neon Planets"};
Options tools[]{"Favorites", "Last Item", "Settings", "USB Bridge", "Link Test", "Guide"};
Options settings[]{"Brightness", "Orientation", "Item names", "About"};
Options guides[]{"Wiring", "Buttons", "Send a gift", "Send an item", "Playdate"};
Options instructions[]{
    "JH: IO43 to Tama RX, IO44 to Tama TX, GND to GND. Leave 3V3 open.",
    "Tap A to move, B to select. Hold A to go back. Hold B to scroll lists or favorite an item.",
    "Select Send. On Tama, open Gift and press B.",
    "Open Download on Tama before selecting Send.",
    "Select Invite, then start Playdate on Tama."
};

enum class Scene { Home, Gifts, Worlds, Items, Detail, Confirm, Result, More, Settings, Languages, Bridge, Guide, About, Empty };
struct Page {
    Scene scene=Scene::Home;
    unsigned focus=0, item=0;
    int kind=0, filter=-1;
    Job job=Job::Item;
};
struct Key {
    int pin;
    ButtonState button;
};
TFT_eSPI lcd;
TFT_eSprite screen(&lcd);
Preferences prefs;
Page page, history[8];
unsigned depth, brightness, language;
bool rotated, ready, lit;
uint32_t lastActivity, lastPaint, lastRepeat;
bool fastScroll;
Key keys[]{{0,{}},{47,{}}};
std::vector<unsigned> items;
uint8_t favorites[(catalogCount+7)/8]{};
uint16_t thumbnail[1024];

void backlight(bool on) {
    constexpr uint8_t levels[]{195,105,0}; // Active-low backlight.
    ledcWrite(0,on?levels[brightness]:255);
}
uint32_t nextCode(const char*& p) {
    const uint8_t lead=*p++;
    if(lead<128)return lead;
    int remaining=lead<224?1:lead<240?2:3;
    uint32_t value=lead&((1u<<(6-remaining))-1);
    while(remaining--&&*p)value=(value<<6)|(uint8_t(*p++)&63);
    return value;
}
const uifont::Glyph& glyph(uint32_t code) {
    const auto& face=uifont::Compact;
    auto* end=face.glyphs+face.count;
    auto* hit=std::lower_bound(face.glyphs,end,code,[](const uifont::Glyph& g,uint32_t c){return g.code<c;});
    return hit!=end&&hit->code==code?*hit:face.glyphs['?'-' '];
}
void text(const char* value,int x,int y,int width,uint16_t ink=INK,unsigned lines=1) {
    const int left=x;
    for(;*value;){
        const char* next=value;
        const auto& g=glyph(nextCode(next));
        if(x+g.advance>left+width){
            if(--lines==0)return;
            x=left;y+=16;
            if(*value==' '){value=next;continue;}
        }
        for(int row=0;row<g.height;++row){
            for(int col=0;col<g.width;++col){
                const unsigned i=row*g.width+col;
                const auto packed=uifont::Compact.coverage[g.offset+i/2];
                if(((packed>>(i%2?0:4))&15)>=6)
                    screen.drawPixel(x+g.left+col,y+uifont::Compact.baseline+g.top+row,ink);
            }
        }
        x+=g.advance;value=next;
    }
}
void small(const char* value,int x,int y,uint16_t color=MUTED) {
    screen.setTextColor(color);
    screen.drawString(value,x,y,1);
}
void picture(unsigned record,int x,int y,int size) {
    if(!assetRead(record,reinterpret_cast<uint8_t*>(thumbnail),sizeof(thumbnail)))return;
    for(int row=0;row<size;++row)
        for(int col=0;col<size;++col)
            screen.drawPixel(x+col,y+row,thumbnail[row*32/size*32+col*32/size]);
}
const char* itemName(unsigned index) { return names::catalogNames[index][language]; }
const char* instructionsFor(const CatalogItem& item) {
    if(item.kind==0)return "Send here, then press B in Gift on Tama.";
    if(item.kind==2)return "Invite here, then start Playdate on Tama.";
    return "Open Download on Tama, then send here.";
}
const char* itemTitle() {
    if(page.kind==0)return gifts[page.filter];
    if(page.kind==2)return worlds[page.filter];
    if(page.kind==1)return "Lab Items";
    if(page.kind==3)return "Lab Codes";
    return "Favorites";
}
void refreshItems() {items=catalog::select(page.kind,page.filter,favorites);}
void open(Scene scene) {
    history[depth++]=page;
    page={};page.scene=scene;
}
void back() {
    if(!depth)return;
    page=history[--depth];
    if(page.scene==Scene::Items){
        refreshItems();
        page.focus=items.empty()?0:std::min(page.focus,unsigned(items.size()-1));
    }
}
void catalogPage(int kind,int filter=-1) {
    open(Scene::Items);page.kind=kind;page.filter=filter;refreshItems();
}
void detail(unsigned id) {open(Scene::Detail);page.item=id;}
void confirm(Job job,unsigned item=0) {open(Scene::Confirm);page.job=job;page.item=item;}
template<size_t count>
void choices(const char* const (&options)[count]) {
    const unsigned first=page.focus/rows*rows;
    for(unsigned i=first;i<count&&i<first+rows;++i){
        const int y=rowTop+int(i-first)*rowPitch;
        screen.fillRoundRect(3,y,122,22,4,i==page.focus?SELECT:PANEL);
        if(i==page.focus)screen.fillRect(3,y+4,2,14,ACCENT);
        text(options[i],8,y+3,114,i==page.focus?ACCENT:INK);
    }
}
void title(const char* name,uint32_t now) {
    screen.fillRect(0,0,128,14,PANEL);
    small(product::hardwareVersion,3,3,INK);
    small("TX",98,3,uartTxBytes.load()&&now-lastTxMs.load()<800?GREEN:MUTED);
    small("RX",114,3,uartRxBytes.load()&&now-lastRxMs.load()<800?ORANGE:MUTED);
    text(name,4,15,120,ACCENT);
}
void render(uint32_t now) {
    const auto status=statusSnapshot();
    screen.fillSprite(BG);
    switch(page.scene){
        case Scene::Home: title(product::title,now);choices(home);break;
        case Scene::Gifts: title("Gifts",now);choices(gifts);break;
        case Scene::Worlds: title("Playdates",now);choices(worlds);break;
        case Scene::More: title("More",now);choices(tools);break;
        case Scene::Settings: {
            title("Settings",now);
            const char* levels[]{"Low","Medium","High"};
            char light[22];snprintf(light,sizeof(light),"Light: %s",levels[brightness]);
            const char* labels[]{light,rotated?"Rotate: 270":"Rotate: 90","Item names","About"};
            choices(labels);break;
        }
        case Scene::Languages: {
            title("Item names",now);
            const char* labels[names::languageCount];
            for(unsigned i=0;i<names::languageCount;++i)labels[i]=names::languages[i].label;
            choices(labels);break;
        }
        case Scene::Items: {
            title(itemTitle(),now);
            if(items.empty())text("No items",4,52,120,MUTED);
            const unsigned first=page.focus/rows*rows;
            for(unsigned i=first;i<items.size()&&i<first+rows;++i){
                const auto id=items[i];
                const int y=rowTop+int(i-first)*rowPitch;
                screen.fillRoundRect(3,y,122,22,4,i==page.focus?SELECT:PANEL);
                picture(catalogItems[id].image,5,y+1,20);
                text(itemName(id),29,y+1,94,catalog::favorite(favorites,id)?ACCENT:INK);
                char number[14];snprintf(number,sizeof(number),"#%u",catalogItems[id].id);
                small(number,30,y+14);
            }
            break;
        }
        case Scene::Detail: {
            title("Details",now);
            const auto& item=catalogItems[page.item];
            picture(item.image,4,34,40);
            text(itemName(page.item),49,31,76,INK,3);
            if(catalog::favorite(favorites,page.item))small("* Saved",4,78,ACCENT);
            if(item.kind==3)text(item.code,4,91,120,YELLOW);
            else if(item.kind==0&&item.code[0]){
                char code[20];snprintf(code,sizeof(code),"Shop Code %s",item.code);
                small(code,4,98,YELLOW);
            }
            else if(item.kind==1||item.kind==4)small("Buy 0 / Sell 9999 G",4,98,YELLOW);
            else small(item.kind==2?worlds[item.field]:"Gift",4,98);
            break;
        }
        case Scene::Confirm:
            title(page.job==Job::Echo?"Link Test":page.job==Job::ReplayBytes?"USB Replay":"Confirm",now);
            text(page.job==Job::Echo?"Put Tama in connection mode.":page.job==Job::ReplayBytes?"Replay captured data to Tama.":instructionsFor(catalogItems[page.item]),4,38,120,INK,4);
            break;
        case Scene::Result: {
            title(status.busy?"Transfer":status.success?"Complete":"Result",now);
            text(status.message,4,31,120,status.success?GREEN:INK,4);
            const unsigned progress=std::min(100u,status.progress);
            screen.fillRect(4,98,90,4,LINE);
            screen.fillRect(4,98,90*progress/100,4,ACCENT);
            char label[8];snprintf(label,sizeof(label),"%u%%",progress);small(label,100,96);
            break;
        }
        case Scene::Bridge: {
            title("USB Bridge",now);
            char line[24];
            snprintf(line,sizeof(line),"USB %lu B",(unsigned long)usbInBytes.load());text(line,4,36,120);
            snprintf(line,sizeof(line),"TX  %lu B",(unsigned long)uartTxBytes.load());text(line,4,57,120);
            snprintf(line,sizeof(line),"RX  %lu B",(unsigned long)uartRxBytes.load());text(line,4,78,120);
            break;
        }
        case Scene::Guide: {
            title(guides[page.focus],now);
            text(instructions[page.focus],4,32,120,INK,5);
            break;
        }
        case Scene::About:
            title("About",now);text(product::name,4,39,120);
            small("128 x 128 LCD",4,70);small("UART 460800",4,92);break;
        case Scene::Empty:
            title("Last Item",now);text("No recent transfer",4,39,120,INK,2);break;
    }
    screen.pushSprite(0,0);
}
unsigned optionCount() {
    switch(page.scene){
        case Scene::Home:return 6;
        case Scene::Gifts:return 5;
        case Scene::Worlds:return 8;
        case Scene::More:return 6;
        case Scene::Settings:return 4;
        case Scene::Languages:return names::languageCount;
        case Scene::Items:return items.size();
        case Scene::Guide:return 5;
        default:return 0;
    }
}
void advance(unsigned count=1) {
    if(unsigned options=optionCount())page.focus=(page.focus+count)%options;
}
void select(const Status& status) {
    switch(page.scene){
        case Scene::Home:
            switch(page.focus){
                case 0:open(Scene::Gifts);break;
                case 1:catalogPage(1);break;
                case 2:open(Scene::Worlds);break;
                case 3:catalogPage(3);break;
                case 4:detail(moneyItemIndex);break;
                case 5:open(Scene::More);break;
            }
            break;
        case Scene::Gifts: catalogPage(0,page.focus);break;
        case Scene::Worlds: catalogPage(2,page.focus);break;
        case Scene::Items: if(!items.empty())detail(items[page.focus]);break;
        case Scene::More:
            switch(page.focus){
                case 0:catalogPage(catalog::allFavorites);break;
                case 1:if(status.lastItem>=0)detail(status.lastItem);else open(Scene::Empty);break;
                case 2:open(Scene::Settings);break;
                case 3:open(Scene::Bridge);break;
                case 4:confirm(Job::Echo);break;
                case 5:open(Scene::Guide);break;
            }
            break;
        case Scene::Settings:
            switch(page.focus){
                case 0:brightness=(brightness+1)%3;backlight(true);prefs.putUInt("brightness",brightness);break;
                case 1:rotated=!rotated;lcd.setRotation(rotated?1:3);prefs.putBool("rotated",rotated);break;
                case 2:open(Scene::Languages);page.focus=language;break;
                case 3:open(Scene::About);break;
            }
            break;
        case Scene::Languages:language=page.focus;prefs.putString("item-language",names::languages[language].code);back();break;
        case Scene::Detail:
            if(catalogItems[page.item].kind==3)back();
            else if(status.assetOK&&!status.busy)confirm(Job::Item,page.item);
            break;
        case Scene::Confirm:
            if((page.job!=Job::ReplayBytes||status.replayBytes)&&submit(page.job,page.item))page.scene=Scene::Result;
            break;
        case Scene::Bridge:if(status.replayBytes&&!status.busy)confirm(Job::ReplayBytes);break;
        case Scene::Result:if(!status.busy)back();break;
        case Scene::Guide:
        case Scene::About:
        case Scene::Empty:back();break;
    }
}
void input(unsigned key,ButtonState::Event event,uint32_t now) {
    const auto status=statusSnapshot();
    if(page.scene==Scene::Result&&status.busy){if(key==0)cancelled=true;return;}
    if(key==0){
        if(event==ButtonState::Event::Long||!optionCount())back();
        else advance();
    }else if(event==ButtonState::Event::Short){
        select(status);
    }else if(page.scene==Scene::Detail){
        favorites[page.item/8]^=1<<(page.item%8);
        prefs.putBytes("favorites",favorites,sizeof(favorites));
    }else if(optionCount()>rows){
        advance(rows);lastRepeat=now;fastScroll=true;
    }
}
}

void uiBegin() {
    prefs.begin("paradise-ui",false);
    const size_t saved=prefs.getBytesLength("favorites");
    if(saved&&saved<=sizeof(favorites))prefs.getBytes("favorites",favorites,saved);
    brightness=std::min(2u,unsigned(prefs.getUInt("brightness",prefs.getInt("brightness",1))));
    rotated=prefs.getBool("rotated",prefs.getInt("rotation",2)==0);
    const auto savedLanguage=prefs.getString("item-language","en");
    language=names::english;
    for(unsigned i=0;i<names::languageCount;++i)
        if(savedLanguage==names::languages[i].code){language=i;break;}
    lcd.init();lcd.setRotation(rotated?1:3);
    screen.setColorDepth(16);
    ready=screen.createSprite(128,128)!=nullptr;
    ledcSetup(0,5000,8);ledcAttachPin(10,0);backlight(true);
    lastActivity=millis();lit=true;
    for(auto& key:keys){pinMode(key.pin,INPUT_PULLUP);key.button.begin(digitalRead(key.pin),lastActivity);}
    if(ready)render(lastActivity);
}
void uiTick() {
    if(!ready)return;
    const uint32_t now=millis();
    static bool updateVisible;
    const bool a=digitalRead(keys[0].pin),b=digitalRead(keys[1].pin);
    const auto update=statusSnapshot();
    if(update.usbUpdate){
        lastActivity=now;
        if(!lit){backlight(true);lit=true;}
        if(!updateVisible||now-lastPaint>=90){
            updateVisible=true;lastPaint=now;
            screen.fillSprite(BG);title("Firmware Update",now);
            char version[48];snprintf(version,sizeof(version),"To: %s",*update.updateVersion?update.updateVersion:"Waiting");
            small(version,4,36,CYAN);
            text(update.message,4,52,120,INK,2);
            screen.fillRect(4,100,90,4,LINE);screen.fillRect(4,100,90*update.progress/100,4,ACCENT);
            char percent[10];snprintf(percent,sizeof(percent),"%u%%",update.progress);small(percent,100,98);
            screen.pushSprite(0,0);
        }
        return;
    }
    if(updateVisible){
        updateVisible=false;
        for(auto& key:keys)key.button.begin(digitalRead(key.pin),now);
        render(now);
    }
    const bool traffic=takeCommunicationActivity();
    if(!a||!b||traffic)lastActivity=now;
    if(now-lastActivity>=idleMs){
        if(lit){backlight(false);lit=false;}
        return;
    }
    if(!lit){
        for(auto& key:keys)key.button.begin(digitalRead(key.pin),now);
        backlight(true);lit=true;render(now);return;
    }
    bool redraw=false;
    const bool released[]{a,b};
    for(unsigned i=0;i<2;++i){
        const auto event=keys[i].button.update(released[i],now);
        if(event!=ButtonState::Event::None){
            fastScroll=false;input(i,event,now);redraw=true;
        }
    }
    if(b)fastScroll=false;
    if(fastScroll&&now-lastRepeat>=250){advance(rows);lastRepeat=now;redraw=true;}
    if(redraw||now-lastPaint>=200){lastPaint=now;render(now);}
}
void uiPreparingUpdate() {
    if(!ready)return;
    backlight(true);screen.fillSprite(BG);title("USB update",millis());
    text("Opening download mode",4,38,120,INK,3);
    small("Track progress on PC",4,101);
    screen.pushSprite(0,0);
}
