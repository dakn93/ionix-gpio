/*
Copyright 2012 Kasper Skårhøj, SKAARHOJ, kasperskaarhoj@gmail.com

This file is part of the ATEM library for Arduino

The ATEM library is free software: you can redistribute it and/or modify 
it under the terms of the GNU General Public License as published by the 
Free Software Foundation, either version 3 of the License, or (at your 
option) any later version.

The ATEM library is distributed in the hope that it will be useful, but 
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY 
or FITNESS FOR A PARTICULAR PURPOSE. 
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along 
with the ATEM library. If not, see http://www.gnu.org/licenses/.

*/

#if defined(ARDUINO) && ARDUINO >= 100
  #include "Arduino.h"
#else
  #include "WProgram.h"
#endif

#include "ATEM.h"
#include <string.h>

/**
 * Constructor, setting up IP address for the switcher (and local port to send packets from)
 */
ATEM::ATEM(IPAddress ip, uint16_t localPort)
    : _localPort(localPort), _switcherIP(ip), _serialOutput(false), _sessionID(0), _lastRemotePacketID(0),
      _localPacketIdCounter(1), _hasInitialized(false), _ATEM_tallySlotCount(8),
      _lastRxMs(0),
      _ATEM_tlFlCount(0), _inPrCount(0), _ATEM_inPrMaxNormalId(0),
      _ATEM_TrPr(false), _ATEM_TrSS_KeyersOnNextTransition(0), _ATEM_TrSS_TransitionStyle(0), _ATEM_TrPs_frameCount(0),
      _ATEM_TrPs_position(0), _ATEM_FtbS_frameCount(0) {
	memset(_ATEM_PrgI, 0, sizeof(_ATEM_PrgI));
	memset(_ATEM_PrvI, 0, sizeof(_ATEM_PrvI));
	memset(_ATEM_TlIn, 0, sizeof(_ATEM_TlIn));
	memset(_ATEM_TlFl, 0, sizeof(_ATEM_TlFl));
	memset(_inPrTab, 0, sizeof(_inPrTab));
	memset(_ATEM_KeOn, 0, sizeof(_ATEM_KeOn));
	memset(_ATEM_DskOn, 0, sizeof(_ATEM_DskOn));
	memset(_ATEM_DskTie, 0, sizeof(_ATEM_DskTie));
	memset(_ATEM_AuxS, 0, sizeof(_ATEM_AuxS));
	memset(_ATEM_MPType, 0, sizeof(_ATEM_MPType));
	memset(_ATEM_MPStill, 0, sizeof(_ATEM_MPStill));
	memset(_ATEM_MPClip, 0, sizeof(_ATEM_MPClip));
}

ATEM::~ATEM() { _Udp.stop(); }

/**
 * Initiating connection handshake to the ATEM switcher
 */
void ATEM::connect() {

	_localPacketIdCounter = 1;	// Init localPacketIDCounter to 1;
	_hasInitialized = false;
	_sessionID = 0;
	_lastRxMs = millis();
	_Udp.stop();
	_Udp.begin(_localPort);

	// Send connectString to ATEM:
	// TODO: Describe packet contents according to rev.eng. API
	byte connectHello[] = {  
		0x10, 0x14, 0x53, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3A, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	_Udp.beginPacket(_switcherIP,  9910);
	_Udp.write(connectHello,20);
	_Udp.endPacket();   

	// Waiting for the ATEM to answer back with a packet 20 bytes long.
	// According to packet analysis with WireShark, this feedback from ATEM
	// comes within a few microseconds!
	uint16_t packetSize = 0;
	uint32_t tWait = millis();
	while (packetSize != 20) {
		packetSize = _Udp.parsePacket();
		if (millis() - tWait > 4000) {
			_Udp.stop();
			return;
		}
		yield();
	}

	// Read the response packet. We will only subtract the session ID
	tWait = millis();
	while (!_Udp.available()) {
		if (millis() - tWait > 3000) {
			_Udp.stop();
			return;
		}
		yield();
	}

	_Udp.read(_packetBuffer, 20);
	_sessionID = _packetBuffer[15];


	// Send connectAnswerString to ATEM:
	_Udp.beginPacket(_switcherIP,  9910);
	
	// TODO: Describe packet contents according to rev.eng. API
	byte connectHelloAnswerString[] = {  
	  0x80, 0x0c, 0x53, 0xab, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00 };
	_Udp.write(connectHelloAnswerString,12);
	_Udp.endPacket();
}

/**
 * Keeps connection to the switcher alive - basically, this means answering back to ping packages.
 * Therefore: Call this in the Arduino loop() function and make sure it gets call at least 2 times a second
 * Other recommendations might come up in the future.
 */
void ATEM::runLoop() {

  // WARNING:
  // It can cause severe timing problems using "slow" functions such as Serial.print*() 
  // in the runloop, in particular during "boot" where the ATEM delivers some 10-20 kbytes of system status info which
  // must exit the RX-buffer quite fast. Therefore, using Serial.print for debugging in this 
  // critical phase will in it self affect program execution!

  // Limit of the RX buffer of the Ethernet interface is another general issue.
  // When ATEM sends the initial system status packets (10-20 kbytes), they are sent with a few microseconds in between
  // The RX buffer of the Ethernet interface on Arduino simply does not have the kapacity to take more than 2k at a time.
  // This means, that we only receive the first packet, the others seems to be discarded. Luckily most information we like to 
  // know about is in the first packet (and some in the second, the rest is probably thumbnails for the media player).
  // It may be possible to bump up this buffer to 4 or 8 k by simply re-configuring the amount of allowed sockets on the interface.
  // For some more information from a guy seemingly having a similar issue, look here:
  // http://www.arduino.cc/cgi-bin/yabb2/YaBB.pl?num=1282170842



  // If there's data available, read a packet
  uint16_t packetSize = _Udp.parsePacket();
  if (_Udp.available() && packetSize !=0)   {  

    // Track last receive time for connection-alive detection.
    _lastRxMs = millis();

    // Read packet header of 12 bytes:
    _Udp.read(_packetBuffer, 12);

    // Read out packet length (first word), remote packet ID number and "command":
    uint16_t packetLength = word(_packetBuffer[0] & B00000111, _packetBuffer[1]);
    _lastRemotePacketID = word(_packetBuffer[10],_packetBuffer[11]);
    uint8_t command = _packetBuffer[0] & B11111000;
    boolean command_ACK = command & B00001000 ? true : false;	// If true, ATEM expects an acknowledgement answer back!
		// The five bits in "command" (from LSB to MSB):
		// 1 = ACK, "Please respond to this packet" (using the _lastRemotePacketID). Exception: The initial 10-20 kbytes of Switcher status
		// 2 = ?. Set during initialization? (first hand-shake packets contains that)
		// 3 = "This is a retransmission". You will see this bit set if the ATEM switcher did not get a timely response to a packet.
		// 4 = ? ("hello packet" according to "ratte", forum at atemuser.com)
		// 5 = "This is a response on your request". So set this when answering...

    if (packetSize==packetLength) {  // Just to make sure these are equal, they should be!

      // If a packet is 12 bytes long it indicates that all the initial information 
      // has been delivered from the ATEM and we can begin to answer back on every request
	  // Currently we don't know any other way to decide if an answer should be sent back...
      if(!_hasInitialized && packetSize == 12) {
        _hasInitialized = true;
		if (_serialOutput) Serial.println("_hasInitialized=TRUE");
      } 
	
		if (packetLength > 12)	{
			_parsePacket(packetLength);
		}

      // If we are initialized, lets answer back no matter what:
		// TODO: "_hasInitialized && " should be inserted back before "command_ACK" but 
		// with Arduino 1.0 UDP library it has proven MORE likely that the initial
		// connection is made if we ALWAYS answer the switcher back.
		// Apparently the initial "chaos" of keeping up with the incoming data confuses 
		// the UDP library so that we might never get initialized - and thus never get connected
		// So... for now this is how we do it:
      if (command_ACK) {
        if (_serialOutput) {
			Serial.print("ACK, rpID: ");
        	Serial.println(_lastRemotePacketID, DEC);
		}

        _sendAnswerPacket(_lastRemotePacketID);
      }

    } else {
		if (_serialOutput) 	{
      		Serial.print("ERROR: Packet size mismatch: ");
		    Serial.print(packetSize, DEC);
		    Serial.print(" != ");
		    Serial.println(packetLength, DEC);
		}
		// Flushing the buffer:
		// TODO: Other way? _Udp.flush() ??
          while(_Udp.available()) {
              _Udp.read(_packetBuffer, ATEM_PACKET_BUFFER);
          }
    }
  }
}

/**
 * If a package longer than a normal acknowledgement is received from the ATEM Switcher we must read through the contents.
 * Usually such a package contains updated state information about the mixer
 * Selected information is extracted in this function and transferred to internal variables in this library.
 */
void ATEM::_parsePacket(uint16_t packetLength)	{
		uint8_t idx;	// General reusable index usable for keyers, mediaplayer etc below.
	
 		// If packet is more than an ACK packet (= if its longer than 12 bytes header), lets parse it:
      uint16_t indexPointer = 12;
      while (indexPointer < packetLength)  {

        // Read the length of segment (first word, big-endian):
        _Udp.read(_packetBuffer, 2);
        uint16_t cmdLength = ((uint16_t)(uint8_t)_packetBuffer[0] << 8) | (uint8_t)_packetBuffer[1];
        if (cmdLength <= 2) {
          indexPointer += cmdLength;
          continue;
        }
        const uint16_t body = cmdLength - 2;
        if (body > ATEM_PACKET_BUFFER) {
          uint16_t skip = body;
          uint8_t sink[256];
          while (skip > 0) {
            uint16_t n = skip > sizeof(sink) ? sizeof(sink) : skip;
            _Udp.read(sink, n);
            skip -= n;
            yield();
          }
          indexPointer += cmdLength;
          continue;
        }
        _Udp.read(_packetBuffer, body);
        if (body < 8) {
          indexPointer += cmdLength;
          continue;
        }

        char cmdStr[] = {_packetBuffer[2], _packetBuffer[3], _packetBuffer[4], _packetBuffer[5], '\0'};

          // Extract the specific state information we like to know about:
          if(strcmp(cmdStr, "PrgI") == 0) {  // Program Bus status (per M/E)
            {
              const uint8_t me = (uint8_t)_packetBuffer[6];
              // Source ID: little-endian uint16 at bytes 7-8 (falls back to byte 7 only)
              const uint16_t src = (body >= 10)
                ? ((uint16_t)(uint8_t)_packetBuffer[7] | ((uint16_t)(uint8_t)_packetBuffer[8] << 8))
                : (uint16_t)(uint8_t)_packetBuffer[7];
              if (me < kMaxME) _ATEM_PrgI[me] = src;
              if (_serialOutput) { Serial.print("PrgI ME"); Serial.print(me+1); Serial.print(": "); Serial.println(src); }
            }
          } else
          if(strcmp(cmdStr, "PrvI") == 0) {  // Preview Bus status (per M/E)
            {
              const uint8_t me = (uint8_t)_packetBuffer[6];
              const uint16_t src = (body >= 10)
                ? ((uint16_t)(uint8_t)_packetBuffer[7] | ((uint16_t)(uint8_t)_packetBuffer[8] << 8))
                : (uint16_t)(uint8_t)_packetBuffer[7];
              if (me < kMaxME) _ATEM_PrvI[me] = src;
              if (_serialOutput) { Serial.print("PrvI ME"); Serial.print(me+1); Serial.print(": "); Serial.println(src); }
            }
          } else
          if(strcmp(cmdStr, "TlIn") == 0) {
            const int base = 8;
            int n = (int)body - base;
            if (n < 1) n = 0;
            if (n > ATEM_MAX_TALLY) n = ATEM_MAX_TALLY;
            if (n > 0) {
              memcpy(_ATEM_TlIn, &_packetBuffer[base], (size_t)n);
              if (n < ATEM_MAX_TALLY)
                memset(_ATEM_TlIn + n, 0, (size_t)(ATEM_MAX_TALLY - n));
              _ATEM_tallySlotCount = (uint8_t)n;
            }
            if (_serialOutput) Serial.println("TlIn tally updated");
          } else
          // TlSr — Tally by Source (modern ATEMs, including Constellation)
          // Per OpenSwitcher: u16 count, then array of {u16 srcId, u8 flags(bit0=PGM,bit1=PRV)}
          if(strcmp(cmdStr, "TlSr") == 0 && body >= 8) {
            // Count: little-endian uint16
            const uint16_t count = (uint16_t)(uint8_t)_packetBuffer[6] | ((uint16_t)(uint8_t)_packetBuffer[7] << 8);
            uint16_t stored = 0;
            memset(_ATEM_TlFl, 0, sizeof(_ATEM_TlFl));
            for (uint16_t e = 0; e < count; e++) {
              const int off = 8 + (int)e * 3;
              if (off + 2 >= (int)body) break;
              // Source ID little-endian (consistent with InPr and PrgI)
              const uint16_t srcId = (uint16_t)(uint8_t)_packetBuffer[off] | ((uint16_t)(uint8_t)_packetBuffer[off + 1] << 8);
              const uint8_t  flags = (uint8_t)_packetBuffer[off + 2];
              if (stored < ATEM_MAX_TLFL) {
                _ATEM_TlFl[stored].id    = srcId;
                _ATEM_TlFl[stored].flags = flags;
                stored++;
              }
            }
            // Print to serial only when content actually changed, to avoid flooding.
            static uint32_t sLastTlFlSig = 0;
            uint32_t sig = stored;
            for (uint16_t i = 0; i < stored; i++) {
              sig = sig * 2654435761u + _ATEM_TlFl[i].id * 31u + _ATEM_TlFl[i].flags;
            }
            _ATEM_tlFlCount = stored;
            if (sig != sLastTlFlSig) {
              sLastTlFlSig = sig;
              Serial.printf("[ATEM TlFl] %u entries:", (unsigned)stored);
              for (uint16_t i = 0; i < stored && i < 32; i++) {
                Serial.printf(" %u%s%s",
                              _ATEM_TlFl[i].id,
                              (_ATEM_TlFl[i].flags & 0x01) ? "R" : "",
                              (_ATEM_TlFl[i].flags & 0x02) ? "G" : "");
              }
              Serial.println();
            }
          } else
          if (strcmp(cmdStr, "InPr") == 0 && body >= 12) {
            // Source ID: try little-endian first (byte6 = low, byte7 = high)
            const uint16_t srcId = (uint16_t)_packetBuffer[6] | ((uint16_t)_packetBuffer[7] << 8);
            // Long name: plain null-terminated ASCII starting at byte 8, up to 20 chars
            char nm[20];
            memset(nm, 0, sizeof(nm));
            int o = 0;
            for (int i = 8; i < (int)body && o < (int)sizeof(nm) - 1; i++) {
              if (_packetBuffer[i] == 0) break;
              if ((uint8_t)_packetBuffer[i] >= 32 && (uint8_t)_packetBuffer[i] < 127)
                nm[o++] = (char)_packetBuffer[i];
            }
            // Always track camera-range source IDs (IDs 1-40), name or not
            if (srcId >= 1 && srcId <= 40 && srcId > _ATEM_inPrMaxNormalId)
              _ATEM_inPrMaxNormalId = srcId;
            if (srcId > 0 && o > 0)
              _upsertInPr(srcId, nm);
          } else
          if(strcmp(cmdStr, "Time") == 0) {  // Time. What is this anyway?
	      } else 
	      if(strcmp(cmdStr, "TrPr") == 0) {  // Transition Preview
			_ATEM_TrPr = _packetBuffer[6+1] > 0 ? true : false;
            if (_serialOutput) Serial.print("Transition Preview: ");
            if (_serialOutput) Serial.println(_ATEM_TrPr, BIN);
          } else
	      if(strcmp(cmdStr, "TrPs") == 0) {  // Transition Position
			_ATEM_TrPs_frameCount = _packetBuffer[6+2];	// Frames count down
			_ATEM_TrPs_position = _packetBuffer[6+4]*256 + _packetBuffer[6+5];	// Position 0-1000
          } else
	      if(strcmp(cmdStr, "TrSS") == 0) {  // Transition Style and Keyer on next transition
			_ATEM_TrSS_KeyersOnNextTransition = _packetBuffer[6+2] & B11111;	// Bit 0: Background; Bit 1-4: Key 1-4
            if (_serialOutput) Serial.print("Keyers on Next Transition: ");
            if (_serialOutput) Serial.println(_ATEM_TrSS_KeyersOnNextTransition, BIN);

			_ATEM_TrSS_TransitionStyle = _packetBuffer[6+1];
            if (_serialOutput) Serial.print("Transition Style: ");	// 0=MIX, 1=DIP, 2=WIPE, 3=DVE, 4=STING
            if (_serialOutput) Serial.println(_ATEM_TrSS_TransitionStyle, DEC);
          } else
	      if(strcmp(cmdStr, "FtbS") == 0) {  // Fade To Black State
			_ATEM_FtbS_frameCount = _packetBuffer[6+2];	// Frames count down
          } else
	      if(strcmp(cmdStr, "DskS") == 0) {  // Downstream Keyer state. Also contains information about the frame count in case of "Auto"
			idx = _packetBuffer[6+0];
			if (idx >=0 && idx <=1)	{
				_ATEM_DskOn[idx] = _packetBuffer[6+1] > 0 ? true : false;
	            if (_serialOutput) Serial.print("Dsk Keyer ");
	            if (_serialOutput) Serial.print(idx+1);
	            if (_serialOutput) Serial.print(": ");
	            if (_serialOutput) Serial.println(_ATEM_DskOn[idx], BIN);
			}
          } else
	      if(strcmp(cmdStr, "DskP") == 0) {  // Downstream Keyer Tie
			idx = _packetBuffer[6+0];
			if (idx >=0 && idx <=1)	{
				_ATEM_DskTie[idx] = _packetBuffer[6+1] > 0 ? true : false;
	            if (_serialOutput) Serial.print("Dsk Keyer");
	            if (_serialOutput) Serial.print(idx+1);
	            if (_serialOutput) Serial.print(" Tie: ");
	            if (_serialOutput) Serial.println(_ATEM_DskTie[idx], BIN);
			}
          } else
		  if(strcmp(cmdStr, "KeOn") == 0) {  // Upstead Keyer on
				idx = _packetBuffer[6+1];
				if (idx >=0 && idx <=3)	{
					_ATEM_KeOn[idx] = _packetBuffer[6+2] > 0 ? true : false;
		            if (_serialOutput) Serial.print("Upstream Keyer ");
		            if (_serialOutput) Serial.print(idx+1);
		            if (_serialOutput) Serial.print(": ");
		            if (_serialOutput) Serial.println(_ATEM_KeOn[idx], BIN);
				}
		    } else 
		  if(strcmp(cmdStr, "ColV") == 0) {  // Color Generator Change
				// Todo: Relatively easy: 8 bytes, first is the color generator, the last 6 is hsl words
		    } else 
		  if(strcmp(cmdStr, "MPCE") == 0) {  // Media Player Clip Enable
				idx = _packetBuffer[6+0];
				if (idx >=0 && idx <=1)	{
					_ATEM_MPType[idx] = _packetBuffer[6+1];
					_ATEM_MPStill[idx] = _packetBuffer[6+2];
					_ATEM_MPClip[idx] = _packetBuffer[6+3];
				}
		    } else 
		  if(strcmp(cmdStr, "AuxS") == 0) {  // Aux Output Source
				uint8_t auxInput = _packetBuffer[6+0];
				if (auxInput >=0 && auxInput <=2)	{
					_ATEM_AuxS[auxInput] = _packetBuffer[6+1];
		            if (_serialOutput) Serial.print("Aux ");
		            if (_serialOutput) Serial.print(auxInput+1);
		            if (_serialOutput) Serial.print(" Output: ");
		            if (_serialOutput) Serial.println(_ATEM_AuxS[auxInput], DEC);
				}

		    } else 
			if (_hasInitialized){	// All the rest...
	            if (_serialOutput) {
					Serial.print("???? Unknown token: ");
					Serial.print(cmdStr);
					Serial.print(" : ");
				}
				for (uint16_t a = 6; a < body; a++) {
	            	if (_serialOutput) Serial.print((uint8_t)_packetBuffer[a], HEX);
	            	if (_serialOutput) Serial.print(" ");
				}
				if (_serialOutput) Serial.println("");
	          }

          indexPointer+=cmdLength;
      }
}

/**
 * Sending a regular answer packet back (tell the switcher that "we heard you, thanks.")
 */
void ATEM::_sendAnswerPacket(uint16_t remotePacketID)  {

  //Answer packet:
  memset(_answer, 0, 12);			// Using 12 bytes of answer buffer, setting to zeros.
  _answer[2] = 0x80;  // ??? API
  _answer[3] = _sessionID;  // Session ID
  _answer[4] = remotePacketID/256;  // Remote Packet ID, MSB
  _answer[5] = remotePacketID%256;  // Remote Packet ID, LSB
  _answer[9] = 0x41;  // ??? API
  // The rest is zeros.

  // Create header:
  uint16_t returnPacketLength = 10+2;
  _answer[0] = returnPacketLength/256;
  _answer[1] = returnPacketLength%256;
  _answer[0] |= B10000000;

  // Send connectAnswerString to ATEM:
  _Udp.beginPacket(_switcherIP,  9910);
  _Udp.write(_answer,returnPacketLength);
  _Udp.endPacket();  
}

/**
 * Sending a command packet back (ask the switcher to do something)
 */
void ATEM::_sendCommandPacket(char cmd[4], uint8_t commandBytes[16], uint8_t cmdBytes)  {

  if (cmdBytes <= 16)	{	// Currently, only a lenght up to 16 - can be extended, but then the _answer buffer must be prolonged as well (to more than 36)
	  //Answer packet preparations:
	  memset(_answer, 0, 36);
	  _answer[2] = 0x80;  // ??? API
	  _answer[3] = _sessionID;  // Session ID
	  _answer[10] = _localPacketIdCounter/256;  // Remote Packet ID, MSB
	  _answer[11] = _localPacketIdCounter%256;  // Remote Packet ID, LSB

	  // The rest is zeros.

	  // Command identifier (4 bytes, after header (12 bytes) and local segment length (4 bytes)):
	  int i;
	  for (i=0; i<4; i++)  {
	    _answer[12+4+i] = cmd[i];
	  }

  		// Command value (after command):
	  for (i=0; i<cmdBytes; i++)  {
	    _answer[12+4+4+i] = commandBytes[i];
	  }

	  // Command length:
	  _answer[12] = (4+4+cmdBytes)/256;
	  _answer[12+1] = (4+4+cmdBytes)%256;

	  // Create header:
	  uint16_t returnPacketLength = 10+2+(4+4+cmdBytes);
	  _answer[0] = returnPacketLength/256;
	  _answer[1] = returnPacketLength%256;
	  _answer[0] |= B00001000;

	  // Send connectAnswerString to ATEM:
	  _Udp.beginPacket(_switcherIP,  9910);
	  _Udp.write(_answer,returnPacketLength);
	  _Udp.endPacket();  

	  _localPacketIdCounter++;
	}
}






/********************************
 *
 * General Getter/Setter methods
 *
 ********************************/


/**
 * Setter method: If _serialOutput is set, the library may use Serial.print() to give away information about its operation - mostly for debugging.
 */
void ATEM::serialOutput(boolean serialOutput) {
	_serialOutput = serialOutput;
}

/**
 * Getter method: If true, the initial handshake and "stressful" information exchange has occured and now the switcher connection should be ready for operation. 
 */
bool ATEM::hasInitialized()	{
	return _hasInitialized;
}

/**
 * Returns last Remote Packet ID
 */
uint16_t ATEM::getATEM_lastRemotePacketId()	{
	return _lastRemotePacketID;
}








/********************************
 *
 * ATEM Switcher state methods
 * Returns the most recent information we've 
 * got about the switchers state
 *
 ********************************/

uint8_t ATEM::getProgramInput() {
	return (uint8_t)_ATEM_PrgI[0];
}
uint8_t ATEM::getPreviewInput() {
	return (uint8_t)_ATEM_PrvI[0];
}
boolean ATEM::getProgramTallyMe(uint16_t inputNumber, uint8_t me) const {
	if (me >= kMaxME || inputNumber == 0) return false;
	return _ATEM_PrgI[me] == inputNumber;
}
boolean ATEM::getPreviewTallyMe(uint16_t inputNumber, uint8_t me) const {
	if (me >= kMaxME || inputNumber == 0) return false;
	return _ATEM_PrvI[me] == inputNumber;
}
boolean ATEM::getProgramTally(uint16_t inputNumber) {
	if (inputNumber < 1) return false;
	// Check TlFl first (Constellation / newer ATEMs)
	if (_ATEM_tlFlCount > 0) {
		for (uint16_t i = 0; i < _ATEM_tlFlCount; i++) {
			if (_ATEM_TlFl[i].id == inputNumber)
				return (_ATEM_TlFl[i].flags & 0x01) != 0;
		}
		return false;
	}
	// Fall back to TlIn (older ATEMs, only supports IDs 1-8/40)
	const uint8_t n = _ATEM_tallySlotCount ? _ATEM_tallySlotCount : 8;
	if (inputNumber > n) return false;
	return (_ATEM_TlIn[inputNumber - 1] & 1) != 0;
}
boolean ATEM::getPreviewTally(uint16_t inputNumber) {
	if (inputNumber < 1) return false;
	// Check TlFl first (Constellation / newer ATEMs)
	if (_ATEM_tlFlCount > 0) {
		for (uint16_t i = 0; i < _ATEM_tlFlCount; i++) {
			if (_ATEM_TlFl[i].id == inputNumber)
				return (_ATEM_TlFl[i].flags & 0x02) != 0;
		}
		return false;
	}
	// Fall back to TlIn (older ATEMs)
	const uint8_t n = _ATEM_tallySlotCount ? _ATEM_tallySlotCount : 8;
	if (inputNumber > n) return false;
	return (_ATEM_TlIn[inputNumber - 1] & 2) != 0;
}

void ATEM::_upsertInPr(uint16_t srcId, const char *nm) {
	int slot = -1;
	int firstFree = -1;
	for (int i = 0; i < ATEM_INPR_TAB; i++) {
		if (_inPrTab[i].id == srcId) { slot = i; break; }
		if (firstFree < 0 && _inPrTab[i].id == 0) firstFree = i;
	}
	if (slot < 0) {
		// New entry: only insert if there is a free slot — never overwrite an existing entry.
		if (firstFree < 0) return;
		slot = firstFree;
	}
	const bool isNew = (_inPrTab[slot].id == 0);
	_inPrTab[slot].id = srcId;
	strncpy(_inPrTab[slot].name, nm, sizeof(_inPrTab[slot].name) - 1);
	_inPrTab[slot].name[sizeof(_inPrTab[slot].name) - 1] = '\0';
	if (isNew && _inPrCount < ATEM_INPR_TAB) _inPrCount++;
}

int ATEM::getInPrCount() const {
	return _inPrCount;
}

bool ATEM::getInPrEntry(int idx, uint16_t &outId, const char *&outName) const {
	if (idx < 0) return false;
	int found = 0;
	for (int i = 0; i < ATEM_INPR_TAB; i++) {
		if (_inPrTab[i].id == 0) continue;
		if (found == idx) { outId = _inPrTab[i].id; outName = _inPrTab[i].name; return true; }
		found++;
	}
	return false;
}

const char *ATEM::getInPrName(uint16_t sourceId) const {
	for (int i = 0; i < ATEM_INPR_TAB; i++) {
		if (_inPrTab[i].id == sourceId && _inPrTab[i].name[0])
			return _inPrTab[i].name;
	}
	return "";
}






/********************************
 *
 * ATEM Switcher Change methods
 * Asks the switcher to changes something
 *
 ********************************/



void ATEM::changeProgramInput(uint8_t inputNumber)  {
  // TODO: Validate that input number exists on current model!
	// On ATEM 1M/E: Black (0), 1 (1), 2 (2), 3 (3), 4 (4), 5 (5), 6 (6), 7 (7), 8 (8), Bars (9), Color1 (10), Color 2 (11), Media 1 (12), Media 2 (14)

  uint8_t commandBytes[4] = {0, inputNumber, 0, 0};
  _sendCommandPacket("CPgI", commandBytes, 4);
}
void ATEM::changePreviewInput(uint8_t inputNumber)  {
  // TODO: Validate that input number exists on current model!

  uint8_t commandBytes[4] = {0, inputNumber, 0, 0};
  _sendCommandPacket("CPvI", commandBytes, 4);
}
void ATEM::doCut()	{
  uint8_t commandBytes[4] = {0, 0xef, 0xbf, 0x5f};	// I don't know what that actually means...
  _sendCommandPacket("DCut", commandBytes, 4);
}
void ATEM::doAuto()	{
  uint8_t commandBytes[4] = {0, 0x32, 0x16, 0x02};	// I don't know what that actually means...
  _sendCommandPacket("DAut", commandBytes, 4);
}
void ATEM::fadeToBlackActivate()	{
	uint8_t commandBytes[4] = {0x00, 0x02, 0x58, 0x99};
	_sendCommandPacket("FtbA", commandBytes, 4);	// Reflected back from ATEM in "FtbS"
}
void ATEM::changeTransitionPosition(word value)	{
	if (value>0 && value<=1000)	{
		uint8_t commandBytes[4] = {0, 0xe4, value/256, value%256};
		_sendCommandPacket("CTPs", commandBytes, 4);  // Change Transition Position (CTPs)
	}
}
void ATEM::changeTransitionPositionDone()	{	// When the last value of the transition is sent (1000), send this one too (we are done, change tally lights and preview bus!)
	uint8_t commandBytes[4] = {0, 0xf6, 0, 0};  	// Done
	_sendCommandPacket("CTPs", commandBytes, 4);  // Change Transition Position (CTPs)
}
void ATEM::changeTransitionPreview(bool state)	{
	uint8_t commandBytes[4] = {0x00, state ? 0x01 : 0x00, 0x00, 0x00};
	_sendCommandPacket("CTPr", commandBytes, 4);	// Reflected back from ATEM in "TrPr"
}
void ATEM::changeTransitionType(uint8_t type)	{
	if (type>=0 && type<=4)	{	// 0=MIX, 1=DIP, 2=WIPE, 3=DVE, 4=STING
		uint8_t commandBytes[4] = {0x01, 0x00, type, 0x02};
		_sendCommandPacket("CTTp", commandBytes, 4);	// Reflected back from ATEM in "TrSS"
	}
}
void ATEM::changeUpstreamKeyOn(uint8_t keyer, bool state)	{
	if (keyer>=1 && keyer<=4)	{	// Todo: Should match available keyers depending on model?
		uint8_t commandBytes[4] = {0x00, keyer-1, state ? 0x01 : 0x00, 0x90};
		_sendCommandPacket("CKOn", commandBytes, 4);	// Reflected back from ATEM in "KeOn"
	}
}
void ATEM::changeUpstreamKeyNextTransition(uint8_t keyer, bool state)	{	// Not supporting "Background"
	if (keyer>=1 && keyer<=4)	{	// Todo: Should match available keyers depending on model?
		uint8_t stateValue = _ATEM_TrSS_KeyersOnNextTransition;
		if (state)	{
			stateValue = stateValue | (B10 << (keyer-1));
		} else {
			stateValue = stateValue & (~(B10 << (keyer-1)));
		}
				// TODO: Requires internal storage of state here so we can preserve all other states when changing the one we want to change.
		uint8_t commandBytes[4] = {0x02, 0x00, 0x6a, stateValue & B11111};
		_sendCommandPacket("CTTp", commandBytes, 4);	// Reflected back from ATEM in "TrSS"
	}
}
void ATEM::changeDownstreamKeyOn(uint8_t keyer, bool state)	{
	if (keyer>=1 && keyer<=2)	{	// Todo: Should match available keyers depending on model?
		uint8_t commandBytes[4] = {keyer-1, state ? 0x01 : 0x00, 0xff, 0xff};
		_sendCommandPacket("CDsL", commandBytes, 4);	// Reflected back from ATEM in "DskP" and "DskS"
	}
}
void ATEM::changeDownstreamKeyTie(uint8_t keyer, bool state)	{
	if (keyer>=1 && keyer<=2)	{	// Todo: Should match available keyers depending on model?
		uint8_t commandBytes[4] = {keyer-1, state ? 0x01 : 0x00, 0xff, 0xff};
		_sendCommandPacket("CDsT", commandBytes, 4);
	}
}
void ATEM::doAutoDownstreamKeyer(uint8_t keyer)	{
	if (keyer>=1 && keyer<=2)	{	// Todo: Should match available keyers depending on model?
  		uint8_t commandBytes[4] = {keyer-1, 0x32, 0x16, 0x02};	// I don't know what that actually means...
  		_sendCommandPacket("DDsA", commandBytes, 4);
	}
}
void ATEM::changeAuxState(uint8_t auxOutput, uint8_t inputNumber)  {
  // TODO: Validate that input number exists on current model!
	// On ATEM 1M/E: Black (0), 1 (1), 2 (2), 3 (3), 4 (4), 5 (5), 6 (6), 7 (7), 8 (8), Bars (9), Color1 (10), Color 2 (11), Media 1 (12), Media 1 Key (13), Media 2 (14), Media 2 Key (15), Program (16), Preview (17), Clean1 (18), Clean 2 (19)

	if (auxOutput>=1 && auxOutput<=3)	{	// Todo: Should match available aux outputs
  		uint8_t commandBytes[4] = {auxOutput-1, inputNumber, 0, 0};
  		_sendCommandPacket("CAuS", commandBytes, 4);
    }
}
void ATEM::settingsMemorySave()	{
	uint8_t commandBytes[4] = {0, 0, 0, 0};
	_sendCommandPacket("SRsv", commandBytes, 4);
}
void ATEM::settingsMemoryClear()	{
	uint8_t commandBytes[4] = {0, 0, 0, 0};
	_sendCommandPacket("SRcl", commandBytes, 4);
}
void ATEM::changeColorValue(uint8_t colorGenerator, uint16_t hue, uint16_t saturation, uint16_t lightness)  {
	if (colorGenerator>=1 && colorGenerator<=2
			&& hue>=0 && hue<=3600 
			&& saturation >=0 && saturation <=1000 
			&& lightness >=0 && lightness <= 1000
		)	{	// Todo: Should match available aux outputs
  		uint8_t commandBytes[8] = {0x07, colorGenerator-1, 
			highByte(hue), lowByte(hue),
			highByte(saturation), lowByte(saturation),
			highByte(lightness), lowByte(lightness)
							};
  		_sendCommandPacket("CClV", commandBytes, 8);
    }
}
void ATEM::mediaPlayerSelectSource(uint8_t mediaPlayer, boolean movieclip, uint8_t sourceIndex)  {
	if (mediaPlayer>=1 && mediaPlayer<=2)	{	// TODO: Adjust to particular ATEM model... (here 1M/E)
		uint8_t commandBytes[12];
		memset(commandBytes, 0, 12);
  		commandBytes[1] = mediaPlayer-1;
		if (movieclip)	{
			commandBytes[0] = 4;
			if (sourceIndex>=1 && sourceIndex<=2)	{
				commandBytes[4] = sourceIndex-1;
			}
		} else {
			commandBytes[0] = 2;
			if (sourceIndex>=1 && sourceIndex<=32)	{
				commandBytes[3] = sourceIndex-1;
			}
		}
		commandBytes[9] = 0x10;
		_sendCommandPacket("MPSS", commandBytes, 12);
			
			// For some reason you have to send this command immediate after (or in fact it could be in the same packet)
			// If not done, the clip will not change if there is a shift from stills to clips or vice versa.
		uint8_t commandBytes2[8] = {0x01, mediaPlayer-1, movieclip?2:1, 0xbf, movieclip?0x96:0xd5, 0xb6, 0x04, 0};
		_sendCommandPacket("MPSS", commandBytes2, 8);
	}
}