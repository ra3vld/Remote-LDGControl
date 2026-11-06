#pragma once
#include <stdint.h>
#include <vector>
#define TFT_BLACK 0
#define TFT_MADCTL 0x36
#define TFT_MAD_BGR 0x08
class TFT_eSPI {
public:
    struct Flush { int x,y,w,h; };
    static TFT_eSPI* instance;
    std::vector<uint16_t> pixels = std::vector<uint16_t>(480*320);
    std::vector<Flush> flushes;
    Flush window{};
    uint8_t rotation=1, madctl=0x28;
    TFT_eSPI() { instance=this; }
    void init() {}
    void setRotation(int value) { rotation=value; }
    uint8_t getRotation() { return rotation; }
    int width() { return rotation&1 ? 480 : 320; }
    int height() { return rotation&1 ? 320 : 480; }
    void writecommand(uint8_t) {}
    void writedata(uint8_t value) { madctl=value; }
    void fillScreen(uint16_t color) { for(auto& pixel:pixels) pixel=color; }
    void startWrite() {}
    void setAddrWindow(int x,int y,int w,int h) { window={x,y,w,h}; }
    void pushColors(uint16_t* colors,uint32_t length,bool) {
        flushes.push_back(window);
        for(uint32_t i=0;i<length;++i) {
            int x=window.x+i%window.w, y=window.y+i/window.w;
            if(x>=0&&x<width()&&y>=0&&y<height()) {
                int column=(madctl&0x20) ? y : x, row=(madctl&0x20) ? x : y;
                if(madctl&0x40) column=319-column;
                if(madctl&0x80) row=479-row;
                pixels[column*480+row]=colors[i];
            }
        }
    }
    void endWrite() {}
};
