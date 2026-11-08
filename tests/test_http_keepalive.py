"""Reuse exact-length streams without caching, preserve EOF framing and expiry."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('hook',ROOT/'scripts/apply_patches.py');hook=importlib.util.module_from_spec(spec);spec.loader.exec_module(hook)
HARNESS=r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <initializer_list>
#define HTTPS_CONNECTION_TIMEOUT 20000
uint32_t now=0;uint32_t millis(){return now;}
enum{STATE_INITIAL,STATE_BODY_FINISHED,CSTATE_CLOSED=3};
struct Headers{void clearAll(){++cleared;}int cleared=0;};
struct Response{bool buffered=false,framed=false,close=false;int finalized=0;
 std::string getHeader(const std::string& name){return name=="Connection"?(close?"close":""):framed?"3":"";}
 bool isResponseBuffered(){return buffered;}
 void setHeader(const char*,const char*){}void finalize(){++finalized;}};
struct HTTPConnection{bool _isKeepAlive=true,closed=false;int _clientState=0,_connectionState=7;
 uint32_t _lastTransmissionTS=0;Headers headers;Headers* _httpHeaders=&headers;
 void refreshTimeout(){_lastTransmissionTS=millis();}bool isClosed(){return closed;}
 void complete(Response&);bool isTimeoutExceeded();};
#include "src/HTTPConnection.cpp"
int main(){
 for(bool requested:{false,true})for(bool buffered:{false,true})for(bool framed:{false,true})for(bool closing:{false,true}){
  HTTPConnection c;c._isKeepAlive=requested;Response r;r.buffered=buffered;r.framed=framed;r.close=closing;
  c.complete(r);bool reused=requested && !closing && (buffered || framed);
  assert(c._connectionState==(reused?STATE_INITIAL:STATE_BODY_FINISHED));
  assert(c.headers.cleared==reused); // Only a reusable response resets request parser state.
 }
 for(auto start:{0u,0xfffffff0u}){
  HTTPConnection c;c._lastTransmissionTS=start;c._connectionState=STATE_INITIAL;
  now=start+3200;assert(!c.isTimeoutExceeded()); // 3s poll plus dispatch/network jitter.
  now=start+10000;assert(!c.isTimeoutExceeded());now=start+10001;assert(c.isTimeoutExceeded());
  c._connectionState=7;assert(!c.isTimeoutExceeded());now=start+20001;assert(c.isTimeoutExceeded());
 }
 for(int client:{0,int(CSTATE_CLOSED)}){
  HTTPConnection c;c.closed=true;c._clientState=client;Response r;r.framed=true;
  c.complete(r);assert(c._connectionState!=STATE_INITIAL && c.headers.cleared==0);
 }
}
'''
@unittest.skipUnless(shutil.which('c++'),'host C++ compiler required')
class StreamKeepaliveTests(unittest.TestCase):
 def test_stream_reuse_eof_framing_closed_socket_and_wrapping_idle_timeout(self):
  with tempfile.TemporaryDirectory(prefix='ldg-keepalive-') as directory:
   root=Path(directory);(root/'src').mkdir();(root/'src/HTTPConnection.hpp').write_text('')
   shutil.copyfile(ROOT/'tests/fixtures/http_keepalive.cpp',root/'src/HTTPConnection.cpp')
   (root/'check.cpp').write_text(HARNESS)
   def run():
    r=subprocess.run(['c++','-std=c++11',str(root/'check.cpp'),'-o',str(root/'check')],capture_output=True);self.assertEqual(r.returncode,0,r.stderr.decode())
    return subprocess.run([str(root/'check')],capture_output=True)
   self.assertNotEqual(run().returncode,0)
   p=ROOT/'patches/esp32_idf5_https_server-stream-keepalive.patch';hook.apply_patch(p,root);hook.apply_patch(p,root)
   result=run();self.assertEqual(result.returncode,0,result.stderr.decode())

 def test_cached_three_second_timeout_reproduces_poll_expiry_then_upgrades(self):
  with tempfile.TemporaryDirectory(prefix='ldg-keepalive-old-') as directory:
   root=Path(directory);(root/'src').mkdir();(root/'src/HTTPConnection.hpp').write_text('')
   shutil.copyfile(ROOT/'tests/fixtures/http_keepalive.cpp',root/'src/HTTPConnection.cpp')
   (root/'check.cpp').write_text(HARNESS)
   def run():
    subprocess.run(['c++','-std=c++11',str(root/'check.cpp'),'-o',str(root/'check')],check=True,capture_output=True)
    return subprocess.run([str(root/'check')],capture_output=True)
   previous=ROOT/'patches/compat/esp32_idf5_https_server-stream-keepalive.patch'
   current=ROOT/'patches/esp32_idf5_https_server-stream-keepalive.patch'
   hook.apply_patch(previous,root)
   self.assertNotEqual(run().returncode,0,'3s idle must reproduce expiry during a 3s poll with jitter')
   hook.apply_patch(current,root,previous);hook.apply_patch(current,root,previous)
   result=run();self.assertEqual(result.returncode,0,result.stderr.decode())
