/*************************************************************************
Title:    DCC Reverser
Authors:  Michael Petersen <railfan@drgw.net>
File:     $Id: $
License:  GNU General Public License v3

LICENSE:
    Copyright (C) 2016 Nathan D. Holmes & Michael D. Petersen

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

*************************************************************************/

#include "avr/wdt.h"
#include <LiquidCrystal.h>
#include <EEPROM.h>
#include <DCCPacket.h>
#include <DCCPacketQueue.h>
#include <DCCPacketScheduler.h>

#define BACKLIGHT_PIN        10
#define DETECTOR_INPUT_PIN_1 A4
#define DETECTOR_INPUT_PIN_2 A5

#define EE_START   0x100

#define btnRIGHT  0
#define btnUP     1
#define btnDOWN   2
#define btnLEFT   3
#define btnSELECT 4
#define btnNONE   5

#define STATE_NORMAL    0
#define STATE_RAMPDOWN  1
#define STATE_RAMPUP    2
#define STATE_SETUP     3
#define STATE_CONFIG    4
#define STATE_SAVE      5
#define STATE_LEARN    10
#define STATE_DET1     11
#define STATE_DET2     12

#define KEYDELAY_DEFAULT   250
#define CONFIG_TIMEOUT     10000
#define LONGPRESS          5000

LiquidCrystal lcd(8, 9, 4, 5, 6, 7);

DCCPacketScheduler dps;
unsigned long previousMillisDCC = 0;
unsigned int dccCurrent = 0;
unsigned long dccPowerRetry = 0;

uint16_t dccAddr[16] = {0};
uint8_t dccAddrIndex = 0;

uint8_t dccSpeed = 0;  // 0 to 120, in steps of 10.  Shifted by +1 when sent to the setSpeed function
uint8_t dccSpeedOrig = 0;
int8_t dccDirection = 1;
uint8_t backlightState = 1;
uint8_t state = STATE_NORMAL;
uint8_t dirstate = STATE_LEARN;
uint8_t updateDisplay = 0;
uint8_t rampRate = 50;
uint8_t currentLimit = 0;

char str[17];

unsigned long previousMillis = 0;
unsigned long pressTime = 0;
uint16_t keyDelay = KEYDELAY_DEFAULT;
uint8_t previousButton = btnNONE;

void dccPowerInit()
{
	pinMode(11, OUTPUT);
}

void dccPowerOn()
{
	digitalWrite(11, HIGH);
}

void dccPowerOff()
{
	digitalWrite(11, LOW);
}

uint8_t dccPowerState()
{
	return(digitalRead(11)?1:0);
}

uint8_t oldButton = btnNONE;

int lcdReadButtons()
{
	int adc = analogRead(0);
	uint8_t button = btnNONE;

/*	sprintf(str, "%4d", adc);*/
/*	lcd.setCursor(8,1);*/
/*	lcd.print(str);*/

	if (adc > 1000) button=btnNONE;
	else if (adc < 50)   button=btnRIGHT;
	else if (adc < 195)  button=btnUP;
	else if (adc < 380)  button=btnDOWN;
	else if (adc < 555)  button=btnLEFT;
	else if (adc < 790)
	{
		// SELECT
		// Only look for toggles
		if(oldButton != btnSELECT)
		{
			oldButton = btnSELECT;
			pressTime = millis();
			return btnSELECT;
		}
		else
		{
			// Check for long press
			if((millis() - pressTime) > LONGPRESS)
			{
				pressTime = millis();
				state = STATE_SETUP;
			}
			return btnNONE;
		}
	}
	
	oldButton = button;
	return button;
}

void printStats(void)
{
	if(currentLimit)
	{
		lcd.setCursor(5,0);
		lcd.print("CURRENT");
		lcd.setCursor(5,1);
		lcd.print("LIMIT!");
	}
	else
	{
		sprintf(str, "ADR:%4d", dccAddr[dccAddrIndex]);
		lcd.setCursor(0,0);
		lcd.print(str);

		sprintf(str, "SPD:%3d", dccSpeed);
		lcd.setCursor(9,0);
		lcd.print(str);

		sprintf(str, "RMP:%4d", rampRate);
		lcd.setCursor(0,1);
		lcd.print(str);
	
		sprintf(str, "%c", dccDirection>0?'F':'R');
		lcd.setCursor(15,1);
		lcd.print(str);
	
		lcd.setCursor(10,1);
		lcd.print(digitalRead(DETECTOR_INPUT_PIN_1)?'-':'*');
		lcd.setCursor(12,1);
		lcd.print(digitalRead(DETECTOR_INPUT_PIN_2)?'-':'*');
	//	lcd.setCursor(11,1);
	//	lcd.print(state);
	}
}

void setup()
{
	Serial.begin(115200);

	pinMode(DETECTOR_INPUT_PIN_1, INPUT_PULLUP);
	pinMode(DETECTOR_INPUT_PIN_2, INPUT_PULLUP);
	
	lcd.begin(16, 2);
	pinMode(BACKLIGHT_PIN, OUTPUT);     // Backlight

	for(int i=0; i<(sizeof(dccAddr)/sizeof(dccAddr[0])); i++)
	{
		dccAddr[i] = ((uint16_t)EEPROM.read(EE_START + 2*i + 1) << 8) | EEPROM.read(EE_START + 2*i + 0);
		if(dccAddr[i] > 9999)
			dccAddr[i] = 0;
	}

	// Enable watchdog
	wdt_reset();
	wdt_enable(WDTO_1S);

	dps.setup();
	dccPowerInit();
	dccPowerOn();
}

void loop() 
{
	unsigned long currentMillis = millis();

	uint8_t buttonAvailable = 0;
	uint8_t button;

	// LCD Section
	digitalWrite(BACKLIGHT_PIN, backlightState);  // Enable
	if((currentMillis - previousMillis) > keyDelay)
	{
		previousMillis = currentMillis;

		button = lcdReadButtons();

		if((btnNONE != button) && (button == previousButton))
		{
			if(keyDelay >= 10)
				keyDelay -= 10;
			else
				keyDelay = 0;
		}
		else
		{
			keyDelay = KEYDELAY_DEFAULT;
		}

		previousButton = button;
		buttonAvailable = 1;
	}

	switch(state)
	{
		case STATE_NORMAL:
			if(buttonAvailable)
			{
				buttonAvailable = 0;
				switch(button)
				{
					case btnRIGHT:
						// Stop the current address
						dps.setSpeed128(dccAddr[dccAddrIndex], DCC_LONG_ADDRESS, 1);
						dps.update();

						if(((sizeof(dccAddr)/sizeof(dccAddr[0])) - 1) == dccAddrIndex)
							dccAddrIndex = 0;
						else
							dccAddrIndex++;
						dccSpeed = 0;
						break;
					case btnLEFT:
						// Stop the current address
						dps.setSpeed128(dccAddr[dccAddrIndex], DCC_LONG_ADDRESS, 1);
						dps.update();

						if(0 == dccAddrIndex)
							dccAddrIndex = ((sizeof(dccAddr)/sizeof(dccAddr[0])) - 1);
						else
							dccAddrIndex--;
						dccSpeed = 0;
						break;
					case btnUP:
						if(dccSpeed <= 120)
							dccSpeed += 5;
						else
							dccSpeed = 125;
						break;
					case btnDOWN:
						if(dccSpeed >= 5)
							dccSpeed -= 5;
						else
							dccSpeed = 0;
						break;
					case btnSELECT:
/*						backlightState ^= 1;*/
						if(rampRate < 250)
						{
							rampRate += 50;
						}
						else
						{
							rampRate = 0;
						}
						break;
					case btnNONE:
						break;
				}
			}
			printStats();
			break;
		case STATE_RAMPDOWN:
			printStats();
			if(dccSpeed)
			{
				dccSpeed--;
			}
			else
			{
				dccDirection *= -1;
				state = STATE_RAMPUP;
			}
			delay(rampRate);
			break;
		case STATE_RAMPUP:
			printStats();
			if(dccSpeed < dccSpeedOrig)
			{
				dccSpeed++;
			}
			else
			{
				state = STATE_NORMAL;
			}
			delay(rampRate);
			break;
		case STATE_SETUP:
			dccSpeed = 0;
			backlightState = 1;
			dccAddrIndex = 0;
			lcd.clear();
			updateDisplay = 1;
			state = STATE_CONFIG;
			break;
		case STATE_CONFIG:
			if(buttonAvailable)
			{
				buttonAvailable = 0;
				switch(button)
				{
					case btnRIGHT:
						if(((sizeof(dccAddr)/sizeof(dccAddr[0])) - 1) == dccAddrIndex)
							dccAddrIndex = 0;
						else
							dccAddrIndex++;
						updateDisplay = 1;
						break;
					case btnLEFT:
						if(0 == dccAddrIndex)
							dccAddrIndex = ((sizeof(dccAddr)/sizeof(dccAddr[0])) - 1);
						else
							dccAddrIndex--;
						updateDisplay = 1;
						break;
					case btnUP:
						if(dccAddr[dccAddrIndex] < 9999)
							dccAddr[dccAddrIndex]++;
						updateDisplay = 1;
						break;
					case btnDOWN:
						if(dccAddr[dccAddrIndex] > 0)
							dccAddr[dccAddrIndex]--;
						updateDisplay = 1;
						break;
					case btnSELECT:
						// Finish
						state = STATE_SAVE;
						break;
					case btnNONE:
						break;
				}
			}

			if(updateDisplay)
			{
				updateDisplay = 0;
				sprintf(str, "Config Addr %2d", dccAddrIndex+1);
				lcd.setCursor(0,0);
				lcd.print(str);
				sprintf(str, "%4d", dccAddr[dccAddrIndex]);
				lcd.setCursor(6,1);
				lcd.print(str);
			}

			break;
		case STATE_SAVE:
			lcd.clear();
			lcd.setCursor(0,0);
			lcd.print("Saving...");
			// Write address table to EEPROM
			for(int i=0; i<(sizeof(dccAddr)/sizeof(dccAddr[0])); i++)
			{
				EEPROM.update(EE_START + 2*i + 1, dccAddr[i] >> 8);
				EEPROM.update(EE_START + 2*i + 0, dccAddr[i] & 0xFF);
			}
			delay(1000);
			lcd.clear();
			state = STATE_NORMAL;
			break;
	}

	if((STATE_NORMAL == state) && (0 == dccSpeed))
	{
		// Reset if speed = 0, except when ramping
		dirstate = STATE_LEARN;
	}

	switch(dirstate)
	{
		case STATE_LEARN:
			dccDirection = 1;
			if(!digitalRead(DETECTOR_INPUT_PIN_1))
			{
				dirstate = STATE_DET1;
				dccSpeedOrig = dccSpeed;
				if(dccSpeed)  // Only go through ramping if there is a non-zero speed.  Keeps it from getting stuck in RAMPDOWN/RAMPUP loop if starting with sensor covered.
					state = STATE_RAMPDOWN;
			}
			else if(!digitalRead(DETECTOR_INPUT_PIN_2))
			{
				dirstate = STATE_DET2;
				dccSpeedOrig = dccSpeed;
				if(dccSpeed)
					state = STATE_RAMPDOWN;
			}
			break;
		case STATE_DET1:
			if(!digitalRead(DETECTOR_INPUT_PIN_2))
			{
				dirstate = STATE_DET2;
				dccSpeedOrig = dccSpeed;
				if(dccSpeed)
					state = STATE_RAMPDOWN;
			}
			break;
		case STATE_DET2:
			if(!digitalRead(DETECTOR_INPUT_PIN_1))
			{
				dirstate = STATE_DET1;
				dccSpeedOrig = dccSpeed;
				if(dccSpeed)
					state = STATE_RAMPDOWN;
			}
			break;
	}

	// DCC Section
	if (previousMillisDCC + 1 <= currentMillis)
	{
		// Runs 100= times / second
		unsigned long analog_value = analogRead(A1);
		// Analog in is 1.65V per amp
		// 5V = 1023
		// Thus, 337 counts per amp
		// Let's turn that into milliamps
		analog_value = (analog_value * 1000) / 337;
		dccCurrent = (dccCurrent * 63 + analog_value) / 64;
		previousMillisDCC = currentMillis;
	}

	if (dccCurrent > 1000)
	{
		dccPowerOff();
		dccPowerRetry = currentMillis + 5000;
		currentLimit = 1;
		lcd.clear();
	}

	if (0 == dccPowerState() && dccPowerRetry <= currentMillis)
	{
		dccPowerOn();
		currentLimit = 0;
		lcd.clear();
	}

	
	wdt_reset();
	// Turn on the headlight
	dps.setFunctions0to4(dccAddr[dccAddrIndex], DCC_LONG_ADDRESS, 1);
	// Send speed
	dps.setSpeed128(dccAddr[dccAddrIndex], DCC_LONG_ADDRESS, dccDirection*(dccSpeed + 1));  // +1 so that minimum is stop (1), not estop (0)
	dps.update();
}

