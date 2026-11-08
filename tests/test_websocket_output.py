"""Production frame queue and SDK send path under bounded TCP backpressure."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('patch_hook', ROOT/'scripts/apply_patches.py')
hook = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hook)
QUEUE = r'''
#include <cassert>
#include "websocket_output.h"
int main() {
 for(auto start:{0u,0xfffffff0u}) {
  ldg::WebsocketOutput q; std::string wire;
  assert(q.submit("old",start));
  assert(q.flush(start,[](const char*,size_t){return 0;}));
  assert(q.submit("abcdef",start+1)); // Replace only a wholly unsent snapshot.
  auto part=[&](const char* p,size_t n){size_t k=n<2?n:2;wire.append(p,k);return int(k);};
  assert(q.flush(start+2,part));
  assert(q.submit("must not interleave",start+3));
  assert(q.flush(start+3,[](const char*,size_t){return 0;}));
  assert(q.flush(start+4,part) && q.flush(start+5,part));
  assert(!q.pending() && wire=="abcdef" && q.skipped()==2);
  assert(!q.submit(std::string(1541,'x'),start));
  assert(q.submit("new",start));
  assert(q.flush(start+9999,[](const char*,size_t){return 0;}));
  assert(q.submit("latest",start+9999)); // Replacement cannot extend stall forever.
  bool called=false;
  assert(!q.flush(start+10000,[&](const char*,size_t){called=true;return 0;}));
  assert(!called);
 }
 for(int result:{-1,100}) {
  ldg::WebsocketOutput q;assert(q.submit("x",0));
  assert(!q.flush(1,[&](const char*,size_t){return result;}));
 }
}
'''
SDK = r'''
#include <cassert>
#include <algorithm>
#include "src/WebsocketHandler.hpp"
uint32_t now=0;uint32_t millis(){return now;}
namespace httpsserver {
WebsocketHandler::WebsocketHandler():_con(nullptr),_receivedClose(false),_sentClose(false){}
WebsocketHandler::~WebsocketHandler(){}
void WebsocketHandler::initialize(ConnectionContext* c){_con=c;}
void WebsocketHandler::onClose(){}
void WebsocketHandler::onError(std::string){}
void WebsocketHandler::onMessage(WebsocketInputStreambuf*){}
#include "src/WebsocketHandler.cpp"
}
int main(){
 using namespace httpsserver;
 ConnectionContext c;WebsocketHandler h;h.initialize(&c);
#ifdef AFTER
 h.sendLatest("abcdef",WebsocketHandler::SEND_TYPE_TEXT);
#else
 h.send("abcdef",WebsocketHandler::SEND_TYPE_TEXT);
#endif
 assert(c.legacy==0); // Before: the shared worker enters its blocking writer.
#ifdef AFTER
 assert(h.flushOutput());assert(c.wire.size()==2);
 c.blocked=true;assert(h.flushOutput());
 h.sendLatest("discard",WebsocketHandler::SEND_TYPE_TEXT); // Partial frame must finish.
 c.blocked=false;
 for(int i=0;i<4;i++){++now;assert(h.flushOutput());}
 assert(c.wire==std::string("\x81\x06" "abcdef",8));
 assert(!h.closed() && !c.legacy);
 h.sendLatest("next",WebsocketHandler::SEND_TYPE_TEXT);
 h.close();assert(c.closing && c.legacy==0); // No close frame inside pending data.
 ConnectionContext binary;WebsocketHandler bytes;bytes.initialize(&binary);
 uint8_t payload[]={0,1,2};bytes.sendLatest(std::string((char*)payload,3));
 for(int i=0;i<3;i++){++now;assert(bytes.flushOutput());}
 assert(binary.legacy==0 && binary.wire==std::string("\x82\x03\x00\x01\x02",5));
 ConnectionContext stalled;WebsocketHandler dead;dead.initialize(&stalled);
 stalled.blocked=true;dead.sendLatest("x");now+=10000;
 assert(!dead.flushOutput() && dead.closed());
#endif
}
'''


@unittest.skipUnless(shutil.which('c++'), 'host C++ compiler required')
class WebsocketOutputTests(unittest.TestCase):
 def compile_run(self, root, source, *flags):
  (root/'check.cpp').write_text(source)
  result=subprocess.run(['c++','-std=c++11','-I',str(root),'-I',str(ROOT/'include'),*flags,str(root/'check.cpp'),'-o',str(root/'check')],capture_output=True)
  self.assertEqual(result.returncode,0,result.stderr.decode())
  return subprocess.run([str(root/'check')],capture_output=True)

 def test_queue_preserves_partial_bytes_deadline_and_wrapping_clock(self):
  with tempfile.TemporaryDirectory(prefix='ldg-ws-output-') as directory:
   result=self.compile_run(Path(directory),QUEUE)
   self.assertEqual(result.returncode,0,result.stderr.decode())

 def test_real_plain_transport_retries_without_waiting_and_preserves_tls(self):
  patch=ROOT/'patches/esp32_idf5_https_server-ws-resumable-output.patch'
  methods=[]
  for relative,hunks in hook.parse_patch(patch):
   if relative.name!='HTTPConnection.cpp':continue
   for before,after in hunks:
    body=b''.join(after).decode()
    if 'int HTTPConnection::writeSome(' in body:
     methods.append(body[body.index('int HTTPConnection::writeSome('):body.index('\n}\n',body.index('int HTTPConnection::writeSome('))+3])
  self.assertEqual(len(methods),1)
  harness=r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <initializer_list>
using byte=uint8_t;
constexpr int MSG_DONTWAIT=64,MALLOC_CAP_INTERNAL=0x800,MALLOC_CAP_8BIT=4;
int scenario=0,calls=0,tlsCalls=0;size_t allowance=2;
size_t heap_caps_get_free_size(int caps){assert(caps==0x804);return scenario==1?8000:40000;}
size_t heap_caps_get_largest_free_block(int caps){return scenario==2?2000:30000;}
size_t httpSendAllowance(int){return allowance;}
int send(int,byte*,size_t n,int flags){++calls;assert(n<=allowance && flags==MSG_DONTWAIT);
 if(scenario>=10){errno=scenario==10?EAGAIN:scenario==11?ENOMEM:scenario==12?ENOBUFS:ECONNRESET;return -1;}
 return int(n);
}
struct HTTPConnection {
 int _socket=7;bool closed=false,secure=false;
 bool isClosed(){return closed;}bool isSecure(){return secure;}
 size_t writeBuffer(byte*,size_t n){++tlsCalls;return n;}
 int writeSome(byte*,size_t);
};
''' + methods[0] + r'''
int main(){
 byte data[5]{};HTTPConnection c;
 assert(c.writeSome(data,5)==2 && calls==1 && !tlsCalls);
 for(scenario=1;scenario<=2;++scenario){calls=0;assert(c.writeSome(data,5)==0 && !calls);}
 scenario=0;allowance=0;assert(c.writeSome(data,5)==0 && !calls);allowance=2;
 for(scenario=10;scenario<=13;++scenario){calls=0;assert(c.writeSome(data,5)==(scenario==13?-1:0) && calls==1);}
 calls=0;c.closed=true;assert(c.writeSome(data,5)==-1 && !calls);c.closed=false;
 c.secure=true;assert(c.writeSome(data,5)==5 && tlsCalls==1 && !calls);
}
'''
  with tempfile.TemporaryDirectory(prefix='ldg-ws-transport-') as directory:
   result=self.compile_run(Path(directory),harness)
   self.assertEqual(result.returncode,0,result.stderr.decode())

 def test_real_sdk_send_uses_queue_instead_of_blocking_writer(self):
  with tempfile.TemporaryDirectory(prefix='ldg-ws-sdk-') as directory:
   root=Path(directory);(root/'src').mkdir();(root/'lwip').mkdir()
   (root/'Arduino.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\nusing byte=uint8_t;\nuint32_t millis();\n#define HTTPS_LOGD(...) do{}while(0)\n')
   (root/'lwip/def.h').write_text('#include <arpa/inet.h>\n')
   (root/'src/HTTPSServerConstants.hpp').write_text('')
   (root/'src/WebsocketInputStreambuf.hpp').write_text('#pragma once\nnamespace httpsserver{class WebsocketInputStreambuf{};}\n')
   (root/'src/ConnectionContext.hpp').write_text('''#pragma once
#include <string>
namespace httpsserver {struct ConnectionContext {
 std::string wire;unsigned legacy=0;bool blocked=false,closing=false;
 size_t writeBuffer(byte*,size_t){++legacy;return 0;}
 int writeSome(byte* p,size_t n){if(blocked)return 0;size_t k=n<2?n:2;wire.append((char*)p,k);return int(k);}
 void signalClientClose(){closing=true;}
};}
''')
   for suffix in ('cpp','hpp'):
    shutil.copyfile(ROOT/('tests/fixtures/websocket_output.'+suffix),root/('src/WebsocketHandler.'+suffix))
   self.assertNotEqual(self.compile_run(root,SDK).returncode,0)
   patch=ROOT/'patches/esp32_idf5_https_server-ws-resumable-output.patch'
   for relative,hunks in hook.parse_patch(patch):
    if relative.name not in ('WebsocketHandler.cpp','WebsocketHandler.hpp'):continue
    target=root.joinpath(*relative.parts)
    target.write_bytes(hook.patched_content(target.read_bytes(),hunks))
   result=self.compile_run(root,SDK,'-DAFTER')
   self.assertEqual(result.returncode,0,result.stderr.decode())


if __name__=='__main__':unittest.main()
