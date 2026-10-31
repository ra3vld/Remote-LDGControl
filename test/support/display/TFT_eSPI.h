#pragma once
#include <stdint.h>
#include <vector>
#define TFT_BLACK 0
class TFT_eSPI {
public:
    struct Flush { int x,y,w,h; };
    static TFT_eSPI* instance;
    std::vector<uint16_t> pixels = std::vector<uint16_t>(480*320);
    std::vector<Flush> flushes;
    Flush window{};
    TFT_eSPI() { instance=this; }
    void init() {}
    void setRotation(int) {}
    void fillScreen(uint16_t color) { for(auto& pixel:pixels) pixel=color; }
    void startWrite() {}
    void setAddrWindow(int x,int y,int w,int h) { window={x,y,w,h}; }
    void pushColors(uint16_t* colors,uint32_t length,bool) {
        flushes.push_back(window);
        for(uint32_t i=0;i<length;++i) {
            int x=window.x+i%window.w, y=window.y+i/window.w;
            if(x>=0&&x<480&&y>=0&&y<320) pixels[y*480+x]=colors[i];
        }
    }
    void endWrite() {}
};
