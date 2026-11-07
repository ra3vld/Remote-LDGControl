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
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#define HTTPS_LOGD(...) do{}while(0)
std::string intToString(int n){return std::to_string(n);}
struct HTTPHeader {
 std::string name,value;
 std::string print(){return name+": "+value;}
};
struct Headers {
 std::vector<HTTPHeader*> values;
 std::vector<HTTPHeader*>* getAll(){return &values;}
};
struct HTTPResponse {
 int _statusCode=200;std::string _statusText="OK";
 Headers _headers;bool _headerWritten=false,_isError=false,shortWrite=false;
 unsigned calls=0;std::string wire;bool buffered=false;HTTPHeader extra;
 bool isResponseBuffered(){return buffered;}
 std::string getHeader(const std::string& name){for(auto* h:_headers.values)if(h->name==name)return h->value;return "";}
 void setHeader(const std::string& name,const std::string& value){for(auto* h:_headers.values)if(h->name==name){h->value=value;return;}extra={name,value};_headers.values.push_back(&extra);}
 void printHeader();
 size_t writeBytesInternal(const void* bytes,int count,bool bypass){
  assert(bypass);++calls;size_t n=shortWrite?count-1:count;
  wire.append(static_cast<const char*>(bytes),n);return n;
 }
 void printInternal(const std::string& value,bool bypass){writeBytesInternal(value.data(),value.size(),bypass);}
};
#include "src/HTTPResponse.cpp"
int main(){
 for(bool buffered:{false,true})for(bool framed:{false,true}){
  HTTPHeader connection{"Connection","keep-alive"};HTTPHeader length{"Content-Length","3"};
  HTTPResponse r;r.buffered=buffered;r._headers.values={&connection};if(framed)r._headers.values.push_back(&length);
  r.printHeader();assert(r.getHeader("Connection")==(!buffered && !framed?"close":"keep-alive"));
 }
 for(bool ws:{false,true})for(bool truncated:{false,true}){
  HTTPHeader one{ws?"Upgrade":"Content-Type",ws?"websocket":"application/json"};
  HTTPHeader two{"Connection",ws?"Upgrade":"close"};
  HTTPResponse response;response._headers.values={&one,&two};response.shortWrite=truncated;
  if(ws){response._statusCode=101;response._statusText="Switching Protocols";}
  response.printHeader();
  const auto expected=std::string("HTTP/1.1 ")+(ws?"101 Switching Protocols":"200 OK")+"\r\n"+one.print()+"\r\n"+two.print()+"\r\n\r\n";
  assert(response.calls==1); // A header block must use one transport write.
  assert(response.wire==(truncated?expected.substr(0,expected.size()-1):expected));
  assert(response._headerWritten && response._isError==truncated);
  response.printHeader();assert(response.calls==1); // Never duplicate a header.
 }
}
'''

@unittest.skipUnless(shutil.which('c++'),'host C++ compiler required')
class HeaderBatchTests(unittest.TestCase):
 def test_headers_and_upgrade_are_batched_without_changing_wire_bytes(self):
  with tempfile.TemporaryDirectory(prefix='ldg-headers-') as directory:
   root=Path(directory);(root/'src').mkdir()
   shutil.copyfile(ROOT/'tests/fixtures/http_response_headers.cpp',root/'src/HTTPResponse.cpp')
   (root/'src/HTTPResponse.hpp').write_text('');(root/'check.cpp').write_text(HARNESS)
   def run():
    subprocess.run(['c++','-std=c++11',str(root/'check.cpp'),'-o',str(root/'check')],check=True,capture_output=True)
    return subprocess.run([str(root/'check')],capture_output=True)
   self.assertNotEqual(run().returncode,0,'Original must reproduce fragmented writes')
   patch=ROOT/'patches/esp32_idf5_https_server-batched-headers.patch'
   hook.apply_patch(patch,root);hook.apply_patch(patch,root)
   result=run();self.assertEqual(result.returncode,0,result.stderr.decode())

if __name__=='__main__':unittest.main()
