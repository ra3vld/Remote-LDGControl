/*
Source excerpt from esp32_idf5_https_server (HTTPResponse.cpp).
MIT License

Copyright (c) 2017 Frank Hessel

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include "HTTPResponse.hpp"

void HTTPResponse::printHeader() {
  if (!_headerWritten) {
    HTTPS_LOGD("Printing headers");

    // Status line, like: "HTTP/1.1 200 OK\r\n"
    std::string statusLine = "HTTP/1.1 " + intToString(_statusCode) + " " + _statusText + "\r\n";
    printInternal(statusLine, true);

    // Each header, like: "Host: myEsp32\r\n"
    std::vector<HTTPHeader *> * headers = _headers.getAll();
    for(std::vector<HTTPHeader*>::iterator header = headers->begin(); header != headers->end(); ++header) {
      printInternal((*header)->print()+"\r\n", true);
    }
    printInternal("\r\n", true);

    _headerWritten=true;
  }
}
