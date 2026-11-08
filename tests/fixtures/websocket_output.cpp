/*
Source excerpt from esp32_idf5_https_server (WebsocketHandler.cpp).
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
void WebsocketHandler::close(uint16_t status, std::string message) {
  HTTPS_LOGD("Websocket close()");

  _sentClose = true;              // Flag that we have sent a close request.

  WebsocketFrame frame;           // Build the web socket frame indicating a close request.
  frame.fin    = 1;
  frame.rsv1   = 0;
  frame.rsv2   = 0;
  frame.rsv3   = 0;
  frame.opCode = OPCODE_CLOSE;
  frame.mask   = 0;
  frame.len    = message.length() + 2;
  int rc = _con->writeBuffer((uint8_t *)&frame, sizeof(frame));

  if (rc > 0) {
    rc = _con->writeBuffer((byte *) &status, 2);
  }

  if (rc > 0) {
    _con->writeBuffer((byte *) message.data(), message.length());
  }
} // Websocket::close

/**
 * @brief Send data down the web socket
 * See the WebSocket spec (RFC6455) section "6.1 Sending Data".
 * We build a WebSocket frame, send the frame followed by the data.
 * @param [in] data The data to send down the WebSocket.
 * @param [in] sendType The type of payload.  Either SEND_TYPE_TEXT or SEND_TYPE_BINARY.
 */
void WebsocketHandler::send(std::string data, uint8_t sendType) {
  // One transport write per frame. Separate 2-byte header/length writes
  // each reserve a full TCP pbuf and can leave a frame split across ACK waits.
  WebsocketFrame frame{};
  frame.fin = 1;
  frame.opCode = sendType == SEND_TYPE_TEXT ? OPCODE_TEXT : OPCODE_BINARY;
  frame.len = data.length() < 126 ? data.length() : 126;
  if (data.length() >= 126) {
    const uint16_t length = htons((uint16_t)data.length());
    data.insert(0, (const char*)&length, sizeof(length));
  }
  data.insert(0, (const char*)&frame, sizeof(frame));
  _con->writeBuffer((uint8_t*)data.data(), data.length());
} // Websocket::send


/**
 * @brief Send data down the web socket
 * See the WebSocket spec (RFC6455) section "6.1 Sending Data".
 * We build a WebSocket frame, send the frame followed by the data.
 * @param [in] data The data to send down the WebSocket.
 * @param [in] sendType The type of payload.  Either SEND_TYPE_TEXT or SEND_TYPE_BINARY.
 */
void WebsocketHandler::send(uint8_t* data, uint16_t length, uint8_t sendType) {
  HTTPS_LOGD(">> Websocket.send(): length=%d", length);
  WebsocketFrame frame;
  frame.fin    = 1;
  frame.rsv1   = 0;
  frame.rsv2   = 0;
  frame.rsv3   = 0;
  frame.opCode = sendType==SEND_TYPE_TEXT?OPCODE_TEXT:OPCODE_BINARY;
  frame.mask   = 0;
  if (length < 126) {
    frame.len = length;
    _con->writeBuffer((uint8_t *)&frame, sizeof(frame));
  } else {
    frame.len = 126;
    _con->writeBuffer((uint8_t *)&frame, sizeof(frame));
    uint16_t net_len = htons(length);
    _con->writeBuffer((uint8_t *)&net_len, sizeof(uint16_t));  // Convert to network byte order from host byte order
  }
  _con->writeBuffer(data, length);
  HTTPS_LOGD("<< Websocket.send()");
}  // Websocket::send

/**
 * Returns true if the connection has been closed, either by client or server
 */
bool WebsocketHandler::closed() {
  return _receivedClose || _sentClose;
}
