"""Execute the real root handler against a filesystem/HTTP harness."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include "web_asset_request.h"
namespace fs {
struct File {
 std::string data; size_t offset=0; bool valid=false;
 explicit operator bool() const {return valid;}
 size_t size() const {return data.size();}
 int read(uint8_t* out,size_t length) {length=std::min(length,data.size()-offset);
  memcpy(out,data.data()+offset,length);offset+=length;return length;}
 void close() {}
};
}
struct Files {
 std::map<std::string,std::string> files; int opens=0;
 bool exists(const char* name) {return files.count(name);}
 fs::File open(const char* name,const char*) {++opens;return {files[name],0,true};}
} LittleFS;
namespace hsv {
struct Params {std::string asset; bool has=false;
 bool getQueryParameter(const char*,std::string& out) {out=asset;return has;}};
struct HTTPRequest {Params params;std::string encoding;bool authenticated=true;
 Params* getParams() {return &params;} std::string getHeader(const char*) {return encoding;}};
struct HTTPResponse {
 std::map<std::string,std::string> headers;std::string body;int status=200;size_t largestWrite=0;
 void setHeader(const char* name,const std::string& value) {headers[name]=value;}
 void write(const uint8_t* data,size_t count) {largestWrite=std::max(largestWrite,count);body.append((const char*)data,count);}
 void print(const char* value) {body+=value;}
};
}
bool authenticate(hsv::HTTPRequest* req,hsv::HTTPResponse* res) {
 if(!req->authenticated)res->status=401;return req->authenticated;
}
void sendJson(hsv::HTTPResponse* res,int status,const char* body) {res->status=status;res->body=body;}
HANDLER
int main() {
 const std::string name="widgets.0123456789ab.js";
 LittleFS.files={{"/index.html",std::string(2000,'H')},{"/index.html.gz",std::string(1300,'G')},
  {"/"+name,"script"},{"/"+name+".gz","packed"}};
 for(bool asset:{false,true})for(bool gzip:{false,true}) {
  hsv::HTTPRequest request;request.params={name,asset};request.encoding=gzip?"br,gzip":"";
  hsv::HTTPResponse response;h_root(&request,&response);
  auto expected=LittleFS.files[(asset?"/"+name:"/index.html")+(gzip?".gz":"")];
  assert(response.status==200 && response.body==expected);
  assert(response.headers["Content-Length"]==std::to_string(expected.size()));
  assert(response.headers["Vary"]=="Accept-Encoding");
  assert(response.headers["X-Content-Type-Options"]=="nosniff");
  assert(response.headers["Content-Encoding"]==(gzip?"gzip":""));
  assert(response.headers["Cache-Control"]==(asset?"private, max-age=31536000, immutable":"no-store"));
  assert(response.headers["Content-Type"]==(asset?"text/javascript; charset=utf-8":"text/html; charset=utf-8"));
  assert(response.largestWrite<=512);
 }
 for(auto name:{"../cert.pem","settings.0123456789ab.js"}) {
  hsv::HTTPRequest request;request.params={name,true};hsv::HTTPResponse response;h_root(&request,&response);
  assert(response.status==404);
 }
 hsv::HTTPRequest request;hsv::HTTPResponse denied;
 request.authenticated=false;int opens=LittleFS.opens;h_root(&request,&denied);
 assert(denied.status==401 && LittleFS.opens==opens);
 request.authenticated=true;request.encoding="gzip;q=0,identity;q=0";
 hsv::HTTPResponse unacceptable;h_root(&request,&unacceptable);assert(unacceptable.status==406);
 request.encoding="gzip";LittleFS.files.erase("/index.html.gz");
 hsv::HTTPResponse fallback;h_root(&request,&fallback);
 assert(fallback.body==LittleFS.files["/index.html"] && !fallback.headers.count("Content-Encoding"));
}
'''

class WebAssetServingTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'host C++ compiler required')
    def test_production_handler_streams_negotiated_files_and_protects_paths(self):
        source = (ROOT / 'src/web_server.cpp').read_text()
        handler = source[source.index('static void h_root('):source.index('static void h_status(')]
        with tempfile.TemporaryDirectory(prefix='ldg-web-serving-') as directory:
            root = Path(directory)
            (root / 'main.cpp').write_text(HARNESS.replace('HANDLER', handler))
            subprocess.run(['c++', '-std=c++17', '-I', str(ROOT / 'include'), str(root / 'main.cpp'),
                            '-o', str(root / 'check')], check=True, capture_output=True)
            result = subprocess.run([str(root / 'check')], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))

if __name__ == '__main__':
    unittest.main()
