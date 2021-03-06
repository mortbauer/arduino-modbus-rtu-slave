/*
  ModbusRtuSlave.cpp - Modbus RTU Slave library for Arduino

    Copyright (C) 2018 Sfera Labs S.r.l. - All rights reserved.

  This code is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  See file LICENSE.txt for further informations on licensing terms.
*/

#include "ModbusRtuSlave.h"
#include "CRC.h"

byte ModbusRtuSlaveClass::_unitAddr;
Stream *ModbusRtuSlaveClass::_port;
int ModbusRtuSlaveClass::_txEnPin;
uint16_t ModbusRtuSlaveClass::_last_available;
unsigned long ModbusRtuSlaveClass::_last_available_ts;
unsigned long ModbusRtuSlaveClass::_t35chars;
byte ModbusRtuSlaveClass::_inBuff[MODBUS_BUFFER_SIZE];
byte ModbusRtuSlaveClass::_outBuff[MODBUS_BUFFER_SIZE];
Callback *ModbusRtuSlaveClass::_callback;
int ModbusRtuSlaveClass::_respOffset;

void ModbusRtuSlaveClass::begin(byte unitAddr, Stream *serial, unsigned long baud, int txEnPin) {
  _unitAddr = unitAddr;
  _port = serial;
  _txEnPin = txEnPin;

  if (_txEnPin > 0) {
      pinMode(_txEnPin, OUTPUT);
  }

  _port->setTimeout(0);

  if (baud <= 19200) {
    _t35chars = 3500000 * 11 / baud;
  } else {
    _t35chars = 1750;
  }

  _last_available = 0;
}

void ModbusRtuSlaveClass::setCallback(Callback *callback) {
  _callback = callback;
}

void ModbusRtuSlaveClass::process() {
  uint16_t available;
  unsigned long now;
  size_t inLen = 0;
  size_t pduLen;

  available = _port->available();
  if (available > 0) {
    now = micros();
    if (available != _last_available) {
      _last_available = available;
      _last_available_ts = now;

    } else if (now - _last_available_ts >= _t35chars) {
      inLen = _port->readBytes(_inBuff, available);
      _last_available = 0;
    }
  }

  if (inLen < 8) {
    return;
  }

  byte unitAddr = _inBuff[0];

  if (_unitAddr > 0 && _unitAddr != unitAddr) {
    return;
  }

  byte crc[2];
  CRC.crc16(_inBuff, inLen - 2, crc);
  if (_inBuff[inLen - 2] != crc[0] || _inBuff[inLen - 1] != crc[1]) {
    return;
  }

  byte function = _inBuff[1];
  word regAddr = word(_inBuff[2], _inBuff[3]);
  word qty;
  byte exCode = MB_RESP_OK;
  byte *data;

  switch (function) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
      qty = word(_inBuff[4], _inBuff[5]);
      data = NULL;
      if (qty < 0x0001 || qty > 0x07D0) {
        exCode = MB_EX_ILLEGAL_DATA_VALUE;
      } else {
        _outBuff[2] = (qty - 1) / 8 + 1;
        pduLen = _outBuff[2] + 2;
      }
      break;

    case MB_FC_READ_HOLDING_REGISTERS:
    case MB_FC_READ_INPUT_REGISTER:
      qty = word(_inBuff[4], _inBuff[5]);
      data = NULL;
      if (qty < 0x0001 || qty > 0x007D) {
        exCode = MB_EX_ILLEGAL_DATA_VALUE;
      } else {
        _outBuff[2] = qty * 2;
        pduLen = _outBuff[2] + 2;
      }
      break;

    case MB_FC_WRITE_SINGLE_COIL:
      qty = 1;
      data = _inBuff + 4;
      if (data[1] != 0x00 || (data[0] != 0x00 && data[0] != 0xFF)) {
        exCode = MB_EX_ILLEGAL_DATA_VALUE;
      } else {
        pduLen = 5;
        memcpy(_outBuff + 2, _inBuff + 2, 4);
      }
      break;

    case MB_FC_WRITE_SINGLE_REGISTER:
      qty = 1;
      data = _inBuff + 4;
      pduLen = 5;
      memcpy(_outBuff + 2, _inBuff + 2, 4);
      break;

    case MB_FC_WRITE_MULTIPLE_COILS:
      qty = word(_inBuff[4], _inBuff[5]);
      data = _inBuff + 7;
      if (qty < 0x0001 || qty > 0x07D0 || inLen - 9 != ((qty - 1) / 8) + 1) {
        exCode = MB_EX_ILLEGAL_DATA_VALUE;
      } else {
        pduLen = 5;
        memcpy(_outBuff + 2, _inBuff + 2, 4);
      }
      break;

    case MB_FC_WRITE_MULTIPLE_REGISTERS:
      qty = word(_inBuff[4], _inBuff[5]);
      data = _inBuff + 7;
      if (qty < 0x0001 || qty > 0x007B || inLen - 9 != qty * 2) {
        exCode = MB_EX_ILLEGAL_DATA_VALUE;
      } else {
        pduLen = 5;
        memcpy(_outBuff + 2, _inBuff + 2, 4);
      }
      break;
    case MB_FC_TRANSFER_RAW:
      qty = _inBuff[2];
      data = _inBuff + 3;
      if (qty < 0x0001 || qty > 0x007B) {
        exCode = MB_EX_ILLEGAL_DATA_VALUE;
      } else {
        pduLen = 5;
        _outBuff[2] = qty;
      }
      break;

    default:
      exCode = MB_EX_ILLEGAL_FUNCTION;
  }

  if (exCode == MB_RESP_OK) {
    if (_callback != NULL) {
      _respOffset = 0;
      exCode = _callback(unitAddr, function, regAddr, qty, data);
    } else {
      exCode = MB_EX_SERVER_DEVICE_FAILURE;
    }
  }

  if (exCode == MB_RESP_IGNORE) {
    return;
  }

  _outBuff[0] = unitAddr;
  _outBuff[1] = function;

  if (exCode != MB_RESP_OK) {
    _outBuff[1] |= 0x80;
    _outBuff[2] = exCode;
    pduLen = 2;
  }

  CRC.crc16(_outBuff, pduLen + 1, _outBuff + pduLen + 1);

  if (_txEnPin > 0) {
    digitalWrite(_txEnPin, HIGH);
  }

  _port->write(_outBuff, pduLen + 3);
  _port->flush();

  if (_txEnPin > 0) {
    digitalWrite(_txEnPin, LOW);
  }
}

void ModbusRtuSlaveClass::responseAddBit(bool on) {
  int idx = 3 + _respOffset / 8;
  int bit = _respOffset % 8;

  if (on) {
      bitSet(_outBuff[idx], bit);
  } else {
      bitClear(_outBuff[idx], bit);
  }

  _respOffset++;
}

void ModbusRtuSlaveClass::responseAddRegister(word value) {
  _outBuff[3 + _respOffset] = highByte(value);
  _outBuff[4 + _respOffset] = lowByte(value);
  _respOffset += 2;
}

bool ModbusRtuSlaveClass::getDataCoil(byte function, byte* data, unsigned int idx) {
  if (function == MB_FC_WRITE_SINGLE_COIL) {
    return data[0] == 0xFF;
  } else if (function == MB_FC_WRITE_MULTIPLE_COILS) {
    return bitRead(data[idx / 8], idx % 8) == 1;
  }
}

word ModbusRtuSlaveClass::getDataRegister(byte function, byte* data, unsigned int idx) {
  if (function == MB_FC_WRITE_SINGLE_REGISTER) {
    return word(data[0], data[1]);
  } else if (function == MB_FC_WRITE_MULTIPLE_REGISTERS) {
    return word(data[idx * 2], data[idx * 2 + 1]);
  }
}
