"""Run production dependency I/O paths against short reads and slow clients."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('patch_hook', ROOT / 'scripts/apply_patches.py')
hook = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hook)

WRITE = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <vector>
using byte=uint8_t;
#define MSG_DONTWAIT 64
#define STATE_ERROR 1
uint32_t ticks=0; unsigned calls=0; int scenario=0;
const uint32_t MALLOC_CAP_INTERNAL=0x800,MALLOC_CAP_8BIT=4;
struct Esp {size_t getFreeHeap(){return 73768;}}ESP; // Includes unusable IRAM.
size_t heap_caps_get_free_size(uint32_t caps){assert(caps==0x804);return scenario==7 || (scenario==6 && ticks<50)?8192:40000;}
std::vector<byte> wire;
uint32_t millis(){return ticks;}
void delay(unsigned n){ticks+=n;}
int send(int,byte* data,size_t n,int){
 ++calls;
 if(scenario==1){errno=EAGAIN;return -1;}
 if(scenario==2){errno=ECONNRESET;return -1;}
 if(scenario==5){errno=ENOMEM;return -1;}
 if((scenario==3 || scenario==4) && calls%2){errno=scenario==4?ENOBUFS:EWOULDBLOCK;return -1;}
 size_t count=n>2?2:n;wire.insert(wire.end(),data,data+count);return count;
}
struct HTTPConnection {
 int _socket=1,_connectionState=0;bool stopped=false;
 bool isClosed(){return stopped;}
 void closeConnection(){stopped=true;}
 size_t writeBuffer(byte*,size_t);
};
#include "src/HTTPConnection.cpp"
int main(){
 byte data[]={1,2,3,4,5};
 for(auto clock:{0u,0xfffffff0u})for(scenario=0;scenario<8;++scenario){
  ticks=clock;calls=0;wire.clear();HTTPConnection c;
  auto result=c.writeBuffer(data,5);
  if(scenario==1 || scenario==2 || scenario==5 || scenario==7){
   assert(result==0 && c.stopped && c._connectionState==STATE_ERROR);
   assert(uint32_t(ticks-clock)<=1000);unsigned done=calls;
   assert(c.writeBuffer(data,5)==0 && calls==done);
  }else{assert(result==5 && !c.stopped);assert(wire==std::vector<byte>(data,data+5));}
 }
}
'''
INPUT = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <arpa/inet.h>
#define HTTPS_LOGD(...) do{}while(0)
#define HTTPS_LOGE(...) do{}while(0)
#define HTTPS_LOGI(...) do{}while(0)
#define HTTPS_LOGW(...) do{}while(0)
#define HTTPS_CONNECTION_DATA_CHUNK_SIZE 2048
struct WebsocketFrame { uint8_t opCode:4,rsv3:1,rsv2:1,rsv1:1,fin:1;uint8_t len:7,mask:1; };
void dumpFrame(WebsocketFrame){}
struct Connection {std::vector<uint8_t> data;size_t pos=0;
 size_t readBuffer(uint8_t* out,size_t n){size_t count=data.size()-pos;if(count>n)count=n;
 memcpy(out,data.data()+pos,count);pos+=count;return count;}
};
struct WebsocketInputStreambuf {WebsocketInputStreambuf(Connection*,uint32_t,uint8_t*){} void discard(){}};
struct WebsocketHandler {
 enum{OPCODE_TEXT=1,OPCODE_BINARY=2,OPCODE_CLOSE=8,OPCODE_CONTINUE=0,OPCODE_PING=9,OPCODE_PONG=10};
 Connection* _con;bool _receivedClose=false;int messages=0;
 int read();void onMessage(WebsocketInputStreambuf*){++messages;}void onClose(){}
};
#include "src/WebsocketHandler.cpp"
int run(std::vector<uint8_t> data){Connection c;c.data=data;WebsocketHandler h;h._con=&c;return h.read();}
int main(){
 assert(run({0x81,0xfe,1})==-1); // incomplete 16-bit length
 assert(run({0x81,0x81,0,0,0})==-1); // incomplete mask
 assert(run({0x81,0xff,0,0,0,0,0,1,0,0})==-1); // oversized 64-bit length
 assert(run({0x81,0xfe,8,1,0,0,0,0})==-1); // >2048 bytes
 assert(run({0x81,0x81,0,0,0,0,'a'})==0);
 assert(run({0x88,0x82,0,0,0,0,3,0xe8})==-1);
 assert(run({0x81})==-1);
}
'''
UPGRADE = r'''
#include <cassert>
#include <initializer_list>
#define STATE_BODY_FINISHED 1
#define STATE_WEBSOCKET 2
struct SerialMock{void println(const char*){}}Serial;
struct Response{int status;int getStatusCode(){return status;}};
struct HTTPConnection;
struct Handler{void initialize(HTTPConnection*){}};
int created=0;
struct WebsocketNode{Handler* newHandler(){++created;return new Handler;}};
struct Resolved{WebsocketNode node;WebsocketNode* getMatchingNode(){return &node;}};
struct HTTPConnection{Handler* _wsHandler=nullptr;int _connectionState=0;bool stopped=false;
 bool isClosed(){return stopped;}
 void upgrade(Response& res,Resolved& resolvedResource);
 ~HTTPConnection(){delete _wsHandler;}
};
#include "src/HTTPConnection.cpp"
int main(){for(int code:{401,503,101})for(bool closed:{false,true}){
 created=0;HTTPConnection c;c.stopped=closed;Response r{code};Resolved node;c.upgrade(r,node);
 assert(created==(code==101 && !closed?1:0));
 assert(c._connectionState==(closed?0:code==101?STATE_WEBSOCKET:STATE_BODY_FINISHED));
}}
'''

MEMORY = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <new>
bool allocationFails=false;int allocated=0,rejected=0,closed=0;
const uint32_t MALLOC_CAP_INTERNAL=0x800,MALLOC_CAP_8BIT=4;
struct Esp {size_t heap=40000,block=30000;size_t getFreeHeap(){return heap+32768;}size_t getMaxAllocHeap(){return 32768;}}ESP;
size_t heap_caps_get_free_size(uint32_t caps){assert(caps==0x804);return ESP.heap;}
size_t heap_caps_get_largest_free_block(uint32_t caps){assert(caps==0x804);return ESP.block;}
int accept(int,void*,void*){++rejected;return 7;}
void close(int fd){assert(fd==7);++closed;}
namespace httpsserver {
struct HTTPConnection {
 HTTPConnection(void*){++allocated;}
 static void* operator new(size_t size){return std::malloc(size);}
 static void* operator new(size_t size,const std::nothrow_t&)noexcept{return allocationFails?nullptr:std::malloc(size);}
 static void operator delete(void* p){std::free(p);}
 int initialize(int,void*){return 9;}
};
struct HTTPServer{HTTPConnection* _connections[1]{};int _socket=1,_defaultHeaders=0;int createConnection(int);};
}
#include "src/HTTPServer.cpp"
int main(){
 using namespace httpsserver;
 for(int scenario=0;scenario<4;++scenario){
  ESP.heap=scenario==1?16000:40000;ESP.block=scenario==2?8000:30000;allocationFails=scenario==3;
  allocated=rejected=closed=0;HTTPServer server;
  int result=server.createConnection(0);
  if(scenario==0){assert(result==9 && allocated==1 && !rejected);delete server._connections[0];}
  else assert(result==-1 && !allocated && rejected==1 && closed==1 && !server._connections[0]);
 }
}
'''

RESPONSE_MEMORY = r'''
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
bool allocationFails=false;
void* operator new[](size_t size){if(allocationFails)throw std::bad_alloc();return std::malloc(size);}
void* operator new[](size_t size,const std::nothrow_t&)noexcept{return allocationFails?nullptr:std::malloc(size);}
void operator delete[](void* p)noexcept{std::free(p);}
using byte=unsigned char;
#define HTTPS_LOGD(...) do{}while(0)
struct ConnectionContext{size_t getCacheSize(){return 1400;}};
struct HTTPResponse{ConnectionContext* _con;int _statusCode;std::string _statusText;bool _headerWritten,_isError;
 size_t _responseCacheSize,_responseCachePointer;byte* _responseCache;
 HTTPResponse(ConnectionContext*);~HTTPResponse(){delete[] _responseCache;}
};
#include "src/HTTPResponse.cpp"
int main(){for(bool fail:{false,true}){allocationFails=fail;ConnectionContext c;HTTPResponse r(&c);
 assert((r._responseCache==nullptr)==fail);assert(r._responseCachePointer==0);}}
'''

@unittest.skipUnless(shutil.which('c++'), 'host C++ compiler required')
class TransportBoundsTests(unittest.TestCase):
    def execute(self, fixture, target, patch, harness):
        with tempfile.TemporaryDirectory(prefix='ldg-transport-') as directory:
            root=Path(directory);(root/'src').mkdir()
            shutil.copyfile(ROOT/'tests/fixtures'/fixture,root/'src'/target)
            (root/'src/HTTPConnection.hpp').write_text('')
            (root/'src/HTTPServer.hpp').write_text('')
            (root/'src/HTTPResponse.hpp').write_text('')
            (root/'src/esp_heap_caps.h').write_text('')
            (root/'src/mbedtls').mkdir()
            for name in ['base64.h','sha1.h']:(root/'src/mbedtls'/name).write_text('')
            (root/'main.cpp').write_text(harness)
            def run():
                compiled=subprocess.run(['c++','-std=c++11','-I',str(root/'src'),str(root/'main.cpp'),'-o',str(root/'check')],capture_output=True)
                self.assertEqual(compiled.returncode,0,compiled.stderr.decode(errors='replace'))
                return subprocess.run([str(root/'check')],cwd=root,capture_output=True,timeout=3)
            self.assertNotEqual(run().returncode,0,'Original source must reproduce the defect')
            path=ROOT/'patches'/patch
            hook.apply_patch(path,root);hook.apply_patch(path,root)
            fixed=run();self.assertEqual(fixed.returncode,0,fixed.stderr.decode(errors='replace'))
    def test_write_timeout_partial_writes_and_clock_wrap(self):
        self.execute('http_write.cpp','HTTPConnection.cpp','esp32_idf5_https_server-bounded-write.patch',WRITE)
    def test_short_and_oversized_ws_headers(self):
        self.execute('ws_input.cpp','WebsocketHandler.cpp','esp32_idf5_https_server-ws-bounded-input.patch',INPUT)
    def test_rejected_upgrade_does_not_allocate_handler(self):
        self.execute('http_upgrade.cpp','HTTPConnection.cpp','esp32_idf5_https_server-ws-upgrade-result.patch',UPGRADE)
    def test_low_memory_rejects_connections_without_reboot(self):
        self.execute('http_connection_memory.cpp','HTTPServer.cpp','esp32_idf5_https_server-connection-memory.patch',MEMORY)
    def test_failed_optional_response_cache_uses_unbuffered_output(self):
        self.execute('http_response_memory.cpp','HTTPResponse.cpp','esp32_idf5_https_server-response-memory.patch',RESPONSE_MEMORY)
