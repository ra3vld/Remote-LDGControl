/*
Source excerpts from esp32_idf5_https_server (HTTPConnection.cpp).

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

HTTPConnection::~HTTPConnection() {
  // Close the socket
  closeConnection();
}

void HTTPConnection::closeConnection() {
  // TODO: Call an event handler here, maybe?

  if (_connectionState != STATE_ERROR && _connectionState != STATE_CLOSED) {

    // First call to closeConnection - set the timestamp to calculate the timeout later on
    if (_connectionState != STATE_CLOSING) {
      _shutdownTS = millis();
    }

    // Set the connection state to closing. We stay in closing as long as SSL has not been shutdown
    // correctly
    _connectionState = STATE_CLOSING;
  }

  // Tear down the socket
  if (_socket >= 0) {
    HTTPS_LOGI("Connection closed. Socket FID=%d", _socket);
    close(_socket);
    _socket = -1;
    _addrLen = 0;
  }

  if (_connectionState != STATE_ERROR) {
    _connectionState = STATE_CLOSED;
  }

  if (_httpHeaders != NULL) {
    HTTPS_LOGD("Free headers");
    delete _httpHeaders;
    _httpHeaders = NULL;
  }

  if (_wsHandler != nullptr) {
    HTTPS_LOGD("Free WS Handler");
    delete _wsHandler;
    _wsHandler = NULL;
  }
}

void HTTPConnection::loop() {
  switch (_connectionState) {
    case STATE_CLOSING: // As long as we are in closing state, we call closeConnection() again and wait for it to finish or timeout
      closeConnection();
      break;
    case STATE_WEBSOCKET: // Do handling of the websocket
      refreshTimeout();  // don't timeout websocket connection
      if(pendingBufferSize() > 0) {
        HTTPS_LOGD("Calling WS handler, FID=%d", _socket);
        _wsHandler->loop();
      }

      // If the client closed the connection unexpectedly
      if (_clientState == CSTATE_CLOSED) {
        HTTPS_LOGI("WS lost client, calling onClose, FID=%d", _socket);
        _wsHandler->onClose();
      }

      // If the handler has terminated the connection, clean up and close the socket too
      if (_wsHandler->closed() || _clientState == CSTATE_CLOSED) {
        HTTPS_LOGI("WS closed, freeing Handler, FID=%d", _socket);
        delete _wsHandler;
        _wsHandler = nullptr;
        _connectionState = STATE_CLOSING;
      }
      break;
    default:;
  }
}
