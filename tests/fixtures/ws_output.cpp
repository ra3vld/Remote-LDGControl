void WebsocketHandler::send(std::string data, uint8_t sendType) {
  HTTPS_LOGD(">> Websocket.send(): length=%d", data.length());
  WebsocketFrame frame;
  frame.fin    = 1;
  frame.rsv1   = 0;
  frame.rsv2   = 0;
  frame.rsv3   = 0;
  frame.opCode = sendType==SEND_TYPE_TEXT?OPCODE_TEXT:OPCODE_BINARY;
  frame.mask   = 0;
  if (data.length() < 126) {
    frame.len = data.length();
    _con->writeBuffer((uint8_t *)&frame, sizeof(frame));
  } else {
    frame.len = 126;
    _con->writeBuffer((uint8_t *)&frame, sizeof(frame));
    uint16_t net_len = htons((uint16_t)data.length());
    _con->writeBuffer((uint8_t *)&net_len, sizeof(uint16_t));  // Convert to network byte order from host byte order
  }
  _con->writeBuffer((uint8_t*)data.data(), data.length());
  HTTPS_LOGD("<< Websocket.send()");
} // Websocket::send
