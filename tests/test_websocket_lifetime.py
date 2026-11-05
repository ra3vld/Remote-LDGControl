"""Execute the patched production WebSocket close paths with reentrant I/O."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("patch_hook", ROOT / "scripts/apply_patches.py")
hook = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hook)

HARNESS = r'''
#include <cassert>
#include <cstddef>
#define HTTPS_LOGI(...) do {} while (0)
#define HTTPS_LOGD(...) do {} while (0)
unsigned millis() { return 1; }
int close(int) { return 0; }
enum { STATE_WEBSOCKET, STATE_CLOSING, STATE_CLOSED, STATE_ERROR };
enum { CSTATE_ACTIVE, CSTATE_CLOSED };
int destroyedHeaders=0;
struct Header {
    bool borrowed=false;
    ~Header() { assert(!borrowed && "headers destroyed during request"); ++destroyedHeaders; }
    void clearAll() {}
};
struct Handler;
struct HTTPConnection {
    int _connectionState=STATE_WEBSOCKET, _clientState=CSTATE_ACTIVE;
    int _socket=-1, _addrLen=0, _shutdownTS=0, pending=2;
    Header* _httpHeaders=nullptr;
    Handler* _wsHandler=nullptr;
    ~HTTPConnection();
    void closeConnection();
    void loop();
    bool isClosed() { return _connectionState==STATE_CLOSED || _connectionState==STATE_ERROR; }
    void refreshTimeout() {}
    int pendingBufferSize() { return pending; }
};
int destroyed=0, notified=0;
struct Handler {
    HTTPConnection& owner;
    int action;
    bool active=false, done=false;
    Handler(HTTPConnection& c,int a):owner(c),action(a){}
    ~Handler() { assert(!active && "handler destroyed inside callback"); ++destroyed; }
    void loop() {
        active=true;
        if(action==1) owner.closeConnection(); // recv/reset while read() is running
        if(action==2) { owner._connectionState=STATE_ERROR; owner.closeConnection(); }
        if(action==3) done=true; // clean WebSocket CLOSE frame
        active=false;
    }
    void onClose() {
        active=true; ++notified;
        if(action==4) owner.closeConnection(); // reentrant application callback
        active=false;
    }
    bool closed() { return done; }
};
#include "src/HTTPConnection.cpp"
void checkRequestHeaders() {
    destroyedHeaders=0;
    { HTTPConnection c; c._httpHeaders=new Header;
      Header* requestHeaders=c._httpHeaders; requestHeaders->borrowed=true;
      c.closeConnection(); assert(destroyedHeaders==0);
      requestHeaders->clearAll(); // HTTPRequest::~HTTPRequest uses these after I/O
      requestHeaders->borrowed=false; }
    assert(destroyedHeaders==1);
}
int main(int argc,char**) {
    if(argc>1) { checkRequestHeaders(); return 0; }
    for(int action=1;action<=3;++action) {
        destroyed=notified=0;
        { HTTPConnection c; c._wsHandler=new Handler(c,action); c.loop();
          assert(c._wsHandler==nullptr); assert(destroyed==1); }
        assert(destroyed==1);
    }
    destroyed=notified=0;
    { HTTPConnection c; c.pending=0; c._clientState=CSTATE_CLOSED;
      c._wsHandler=new Handler(c,4); c.loop();
      assert(destroyed==1 && notified==1 && c._wsHandler==nullptr); }
    assert(destroyed==1);
    { HTTPConnection c; c.loop(); assert(c.isClosed()); } // absent handler
    destroyed=0;
    { HTTPConnection c; c._wsHandler=new Handler(c,0);
      c.closeConnection(); assert(destroyed==0); } // transport closes before dispatch
    assert(destroyed==1);
    destroyed=0;
    { HTTPConnection c; c._wsHandler=new Handler(c,0); c.loop(); assert(destroyed==0); }
    assert(destroyed==1);
    checkRequestHeaders();
}
'''


class WebSocketLifetimeTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler required")
    def test_transport_close_preserves_handler_until_callback_returns(self):
        with tempfile.TemporaryDirectory(prefix="ldg-ws-lifetime-") as directory:
            root = Path(directory)
            (root / "src").mkdir()
            source = root / "src/HTTPConnection.cpp"
            shutil.copyfile(ROOT / "tests/fixtures/http_ws_connection_lifetime.cpp", source)
            (root / "main.cpp").write_text(HARNESS)

            def compile_and_run(*args):
                subprocess.run(["c++", "-std=c++11", "-o", str(root / "check"),
                                str(root / "main.cpp")], check=True, capture_output=True)
                return subprocess.run([str(root / "check"), *args], capture_output=True, cwd=root)

            original = compile_and_run()
            self.assertNotEqual(original.returncode, 0, "Original code must reproduce the lifetime bug")
            self.assertIn(b"handler destroyed inside callback", original.stderr)
            headers = compile_and_run("headers")
            self.assertNotEqual(headers.returncode, 0)
            self.assertIn(b"headers destroyed during request", headers.stderr)
            patch = ROOT / "patches/esp32_idf5_https_server-ws-handler-lifetime.patch"
            hook.apply_patch(patch, root)
            hook.apply_patch(patch, root)  # also check repeated PlatformIO builds
            fixed = compile_and_run()
            self.assertEqual(fixed.returncode, 0, fixed.stderr.decode(errors="replace"))


if __name__ == "__main__":
    unittest.main()
