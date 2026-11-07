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
#include "HTTPConnection.hpp"

/**
 * Timeout
 */
bool HTTPConnection::isTimeoutExceeded() {
  return _lastTransmissionTS + HTTPS_CONNECTION_TIMEOUT < millis();
}

void HTTPConnection::complete(Response& res) {
            std::string hConnection = res.getHeader("Connection");
            if (hConnection == "close") {
              _isKeepAlive = false;
            }
            if (!_isKeepAlive) {
              // No KeepAlive -> We are done. Transition to next state.
              if (!isClosed()) {
                res.finalize();
                _connectionState = STATE_BODY_FINISHED;
              }
            } else {
              if (res.isResponseBuffered()) {
                // If the response could be buffered:
                res.setHeader("Connection", "keep-alive");
                res.finalize();
                if (_clientState != CSTATE_CLOSED) {
                  // Refresh the timeout for the new request
                  refreshTimeout();
                  // Reset headers for the new connection
                  _httpHeaders->clearAll();
                  // Go back to initial state
                  _connectionState = STATE_INITIAL;
                }
              }
              // The response could not be buffered or the client has closed:
              if (!isClosed() && _connectionState!=STATE_INITIAL) {
                _connectionState = STATE_BODY_FINISHED;
              }
            }
}
