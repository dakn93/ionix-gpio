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


/**
  Version 1 beta
**/


#ifndef ATEM_h
#define ATEM_h

#if defined(ARDUINO) && ARDUINO >= 100
  #include "Arduino.h"
#else
  #include "WProgram.h"
#endif


#include <WiFiUdp.h>

#ifndef ATEM_PACKET_BUFFER
#define ATEM_PACKET_BUFFER 1536
#endif
#ifndef ATEM_MAX_TALLY
#define ATEM_MAX_TALLY 128
#endif
#ifndef ATEM_MAX_TLFL
#define ATEM_MAX_TLFL  128
#endif
#ifndef ATEM_INPR_TAB
#define ATEM_INPR_TAB 144
#endif

class ATEM
{
  private:
	WiFiUDP _Udp;			// Udp Object for communication, see constructor.
	uint16_t _localPort; 		// local port to send from
	IPAddress _switcherIP;		// IP address of the switcher
	boolean _serialOutput;		// If set, the library will print status/debug information to the Serial object

	uint8_t _sessionID;					// Used internally for storing packet size during communication
	uint16_t _lastRemotePacketID;		// The most recent Remote Packet Id from switcher
	char _packetBuffer[ATEM_PACKET_BUFFER];   			// Buffer for storing segments of the packets from ATEM.

	uint16_t _localPacketIdCounter;  	// This is our counter for the command packages we might like to send to ATEM
	boolean _hasInitialized;  			// If true, the initial reception of the ATEM memory has passed and we can begin to respond during the runLoop()
	uint32_t _lastRxMs;                // millis() when last packet was received from ATEM
	uint8_t _answer[36]; 				// Little buffer for creating answers back to the ATEM

		// Selected ATEM State values. Naming attempts to match the switchers own protocol names
		// Set through _parsePacket() when the switcher sends state information
		// Accessed through getter methods
	// Per-M/E program and preview inputs (index 0 = M/E 1, ..., 3 = M/E 4)
	static constexpr uint8_t kMaxME = 4;
	uint16_t _ATEM_PrgI[kMaxME];
	uint16_t _ATEM_PrvI[kMaxME];
	uint8_t _ATEM_TlIn[ATEM_MAX_TALLY];	// bit 0 = Prg tally, bit 1 = Prv tally
	uint8_t _ATEM_tallySlotCount;		// valid entries in _ATEM_TlIn (1..ATEM_MAX_TALLY)

	// TlFl — full tally by source (used by Constellation and newer ATEMs)
	struct TlFlEntry { uint16_t id; uint8_t flags; };
	TlFlEntry _ATEM_TlFl[ATEM_MAX_TLFL];
	uint16_t  _ATEM_tlFlCount;          // valid entries in _ATEM_TlFl

	struct InPrEntry {
		uint16_t id;
		char name[20];
	};
	InPrEntry _inPrTab[ATEM_INPR_TAB];
	int       _inPrCount;               // number of filled InPr slots
	uint16_t  _ATEM_inPrMaxNormalId;    // highest InPr source ID 1-40 (camera inputs)
	boolean _ATEM_TrPr;		// Transition Preview: Is it on or not?
	uint8_t _ATEM_TrSS_KeyersOnNextTransition;	// Bit 0: Background; Bit 1-4: Key 1-4
	uint8_t _ATEM_TrSS_TransitionStyle;			// 0=MIX, 1=DIP, 2=WIPE, 3=DVE, 4=STING
	boolean _ATEM_KeOn[4];	// Upstream Keyer 1-4 On state
	boolean _ATEM_DskOn[2];	// Downstream Keyer 1-2 On state
	boolean _ATEM_DskTie[2];	// Downstream Keyer Tie 1-2 On state
	uint8_t _ATEM_TrPs_frameCount;	// Count down of frames in case of a transition (manual or auto)
	uint16_t _ATEM_TrPs_position;	// Position from 0-1000 of the current transition in progress
	uint8_t _ATEM_FtbS_frameCount;	// Count down of frames in case of fade-to-black
	uint8_t _ATEM_AuxS[3];	// Aux Outputs 1-3 source
	uint8_t _ATEM_MPType[2];	// Media Player 1/2: Type (1=Clip, 2=Still)
	uint8_t _ATEM_MPStill[2];	// Still number (if MPType==2)
	uint8_t _ATEM_MPClip[2];	// Clip number (if MPType==1)
	
	
  public:
    ATEM(IPAddress ip, uint16_t localPort);
    ~ATEM();
    void connect();
    void runLoop();

  private:
	void _parsePacket(uint16_t packetLength);
	void _upsertInPr(uint16_t srcId, const char *nm);
	void _sendAnswerPacket(uint16_t remotePacketID);
	void _sendCommandPacket(char cmd[4], uint8_t commandBytes[16], uint8_t cmdBytes);


  public:

/********************************
 * General Getter/Setter methods
 ********************************/
  	void serialOutput(boolean serialOutput);
	bool hasInitialized();
	uint8_t sessionId() const { return _sessionID; }
	uint16_t getATEM_lastRemotePacketId();

/********************************
* ATEM Switcher state methods
* Returns the most recent information we've 
* got about the switchers state
 ********************************/
	uint8_t getProgramInput();   // M/E 1, backward compat
	uint8_t getPreviewInput();   // M/E 1, backward compat
	boolean getProgramTally(uint16_t inputNumber);
	boolean getPreviewTally(uint16_t inputNumber);
	/** Per-M/E tally: me=0..3. Returns true if inputNumber is on program/preview of that M/E. */
	boolean getProgramTallyMe(uint16_t inputNumber, uint8_t me) const;
	boolean getPreviewTallyMe(uint16_t inputNumber, uint8_t me) const;
	/** Number of camera inputs — prefers InPr count (covers Constellation), falls back to TlIn count. */
	uint8_t getTallySlotCount() const {
		if (_ATEM_inPrMaxNormalId > 0)
			return (uint8_t)(_ATEM_inPrMaxNormalId <= 255 ? _ATEM_inPrMaxNormalId : 255);
		return _ATEM_tallySlotCount ? _ATEM_tallySlotCount : 8;
	}
	/** Label from InPr if known, else empty string. */
	const char *getInPrName(uint16_t sourceId) const;
	/** Total number of stored InPr entries. */
	int getInPrCount() const;
	/** Returns id and name for InPr entry at index idx (0-based). Returns false if idx out of range. */
	bool getInPrEntry(int idx, uint16_t &outId, const char *&outName) const;
	/** Milliseconds since last packet was received from the switcher. */
	uint32_t msSinceLastRx() const { return (uint32_t)((int32_t)millis() - (int32_t)_lastRxMs); }

/********************************
 * ATEM Switcher Change methods
 * Asks the switcher to changes something
 ********************************/
	void changeProgramInput(uint8_t inputNumber);
	void changePreviewInput(uint8_t inputNumber);
	void doCut();
	void doAuto();
	void fadeToBlackActivate();
	void changeTransitionPosition(word value);
	void changeTransitionPositionDone();
	void changeTransitionPreview(bool state);
	void changeTransitionType(uint8_t type);
	void changeUpstreamKeyOn(uint8_t keyer, bool state);
	void changeUpstreamKeyNextTransition(uint8_t keyer, bool state);
	void changeDownstreamKeyOn(uint8_t keyer, bool state);
	void changeDownstreamKeyTie(uint8_t keyer, bool state);	
	void doAutoDownstreamKeyer(uint8_t keyer);
	void changeAuxState(uint8_t auxOutput, uint8_t inputNumber);
	void settingsMemorySave();
	void settingsMemoryClear();
	void changeColorValue(uint8_t colorGenerator, uint16_t hue, uint16_t saturation, uint16_t lightness);
	void mediaPlayerSelectSource(uint8_t mediaPlayer, boolean movieclip, uint8_t sourceIndex);
};

#endif

