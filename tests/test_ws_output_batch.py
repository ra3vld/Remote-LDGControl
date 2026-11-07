"""Exercise the production response-header path and exact HTTP/WS framing."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('patch_hook', ROOT/'scripts/apply_patches.py')
hook = importlib.util.module_from_spec(spec); spec.loader.exec_module(hook)
HARNESS = r'''#include <cassert>
#include <string>
#include <cstdint>
#include <arpa/inet.h>
#define HTTPS_LOGD(...) do{}while(0)
struct WebsocketFrame {uint8_t opCode:4,rsv3:1,rsv2:1,rsv1:1,fin:1;uint8_t len:7,mask:1;};
struct Connection {unsigned calls=0;std::string wire;
 size_t writeBuffer(uint8_t* p,size_t n){++calls;wire.append((const char*)p,n);return n;}};
struct WebsocketHandler {Connection* _con;enum{SEND_TYPE_TEXT=1,OPCODE_TEXT=1,OPCODE_BINARY=2};
 void send(std::string,uint8_t);};
#include "src/WebsocketHandler.cpp"
int main(){for(auto n:{0u,1u,125u,126u,1024u,65535u})for(auto type:{1,2}){
 Connection c;WebsocketHandler h{&c};std::string payload(n,'x');h.send(payload,type);
 assert(c.calls==1);unsigned header=n<126?2:4;assert(c.wire.size()==header+n);
 assert((uint8_t)c.wire[0]==(0x80|type));assert((uint8_t)c.wire[1]==(n<126?n:126));
 if(n>=126)assert((uint8_t)c.wire[2]==(n>>8) && (uint8_t)c.wire[3]==(n&255));
 assert(c.wire.substr(header)==payload);
}}
'''

@unittest.skipUnless(shutil.which('c++'),'host C++ compiler required')
class WsOutputBatchTests(unittest.TestCase):
 def test_complete_frames_use_one_write(self):
  with tempfile.TemporaryDirectory(prefix='ldg-headers-') as directory:
   root=Path(directory);(root/'src').mkdir()
   shutil.copyfile(ROOT/'tests/fixtures/ws_output.cpp',root/'src/WebsocketHandler.cpp')
   (root/'src/WebsocketHandler.hpp').write_text('');(root/'check.cpp').write_text(HARNESS)
   def run():
    subprocess.run(['c++','-std=c++11',str(root/'check.cpp'),'-o',str(root/'check')],check=True,capture_output=True)
    return subprocess.run([str(root/'check')],capture_output=True)
   self.assertNotEqual(run().returncode,0,'Original must reproduce fragmented writes')
   patch=ROOT/'patches/esp32_idf5_https_server-batched-ws-output.patch'
   hook.apply_patch(patch,root);hook.apply_patch(patch,root)
   result=run();self.assertEqual(result.returncode,0,result.stderr.decode())

if __name__=='__main__':unittest.main()
