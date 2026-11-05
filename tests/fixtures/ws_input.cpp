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
int WebsocketHandler::read() {
  WebsocketFrame frame;
  int length = _con->readBuffer((uint8_t*)&frame, sizeof(frame));
  HTTPS_LOGD("Websocket: Read %d bytes", length);
  if(length == 0) 
    return 0;
  else if (length != sizeof(frame)) {
    HTTPS_LOGE("Websocket read error");
    //_con->closeConnection();
    return -1;
  }
  dumpFrame(frame);

  // The following section parses the WebSocket frame.
  uint32_t payloadLen = 0;
  uint8_t  mask[4];
  if (frame.len < 126) {
    payloadLen = frame.len;
  } else if (frame.len == 126) {
    uint16_t tempLen;
    _con->readBuffer((uint8_t*)&tempLen, sizeof(tempLen));
    payloadLen = ntohs(tempLen);
  } else if (frame.len == 127) {
    uint64_t tempLen;
    _con->readBuffer((uint8_t*)&tempLen, sizeof(tempLen));
    payloadLen = ntohl((uint32_t)tempLen);
  }
  if (frame.mask == 1) {
    _con->readBuffer(mask, sizeof(mask));
  }

  if (payloadLen == 0) {
    HTTPS_LOGW("WS payload not present");
  } else {
    HTTPS_LOGI("WS payload: length=%d", payloadLen);
  }

  switch(frame.opCode) {
    case OPCODE_TEXT:
    case OPCODE_BINARY: {
      HTTPS_LOGD("Creating Streambuf");
      WebsocketInputStreambuf streambuf(_con, payloadLen, frame.mask==1?mask:nullptr);
      HTTPS_LOGD("Calling onMessage");
      onMessage(&streambuf);
      HTTPS_LOGD("Discarding Streambuf");
      streambuf.discard();
      break;
    }

    case OPCODE_CLOSE: {  // If the WebSocket operation code is close then we are closing the connection.
      _receivedClose = true;
      onClose();
      //close(); // Close the websocket.
      return -1;
      break;
    }

    case OPCODE_CONTINUE: {
      break;
    }

    case OPCODE_PING: {
      break;
    }

    case OPCODE_PONG: {
      break;
    }

    default: {
        HTTPS_LOGW("WebSocketReader: Unknown opcode: %d", frame.opCode);
      break;
    }
  } // Switch opCode
  return 0;
}  // Websocket::read
