"""Check the real connection dispatch preamble declares EOF before any output."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("patch_hook", ROOT / "scripts/apply_patches.py")
hook = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hook)
HARNESS = r'''
#include <cassert>
#include <map>
#include <string>
#include <vector>
struct HTTPHeader { std::string _name, _value; };
struct HTTPHeaders {
    std::vector<HTTPHeader*> values;
    std::vector<HTTPHeader*>* getAll() { return &values; }
};
struct HTTPResponse {
    explicit HTTPResponse(void*) {}
    std::map<std::string,std::string> headers;
    void setHeader(const std::string& name,const std::string& value) { headers[name]=value; }
    std::string getHeader(const std::string& name) { return headers[name]; }
};
using HTTPSCallbackFunction=int;
struct HTTPConnection {
    bool _isKeepAlive=false;
    HTTPHeaders defaults;
    HTTPHeaders* _defaultHeaders=&defaults;
    std::string observed;
    void dispatch(bool);
};
#include "src/HTTPConnection.cpp"
int main() {
    for(bool keep:{false,true}) for(bool websocket:{false,true})
        for(std::string configured:{"","keep-alive","close"}) {
            HTTPConnection connection; connection._isKeepAlive=keep;
            HTTPHeader header{"Connection",configured};
            if(!configured.empty()) connection.defaults.values.push_back(&header);
            connection.dispatch(websocket);
            assert(connection.observed==(websocket?"Upgrade":keep?configured:"close"));
        }
}
'''


@unittest.skipUnless(shutil.which("c++"), "host C++ compiler required")
class ConnectionCloseTests(unittest.TestCase):
    def test_close_header_precedes_handler_and_preserves_keepalive_and_upgrade(self):
        with tempfile.TemporaryDirectory(prefix="ldg-http-close-") as directory:
            root = Path(directory)
            (root / "src").mkdir()
            shutil.copyfile(ROOT / "tests/fixtures/http_connection_close.cpp",
                            root / "src/HTTPConnection.cpp")
            (root / "check.cpp").write_text(HARNESS)

            def run():
                subprocess.run(["c++", "-std=c++11", str(root / "check.cpp"),
                                "-o", str(root / "check")], check=True, capture_output=True)
                return subprocess.run([str(root / "check")], capture_output=True)

            self.assertNotEqual(run().returncode, 0, "Before must reproduce the missing close header")
            patch = ROOT / "patches/esp32_idf5_https_server-declare-close.patch"
            hook.apply_patch(patch, root)
            hook.apply_patch(patch, root)
            result = run()
            self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == "__main__":
    unittest.main()
