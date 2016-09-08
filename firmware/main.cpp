#include "mbed.h"
#include "SDFileSystem.h"
#include <string.h>
#include "atoh.h"
#include "USBSerial.h"
#include "ethernet_manager.hpp"
#include "settings_handler.hpp"
#include "test_manager.hpp"

#include "sids.h"
#include "pids.h"
#include "shared_defines.h"
#include "information_types.h"
#include "sd_helper.hpp"
#include "mitm_helper.hpp"
#include "replay_helper.hpp"

//Create an SDFileSystem object
SDFileSystem sd(p5, p6, p7, p8, "sd");

FileHandle* file;
FileHandle* file2;

/*
TODO:
-TP Scan data by ID is fucking up by not sending ACK
-Continue with SecurityAccess (add more algos)
-Add Car Discovery function to scan all protocols and modules and attempt to recover all information from them (create a service and status map).
-Add file dumping for files transfered via UDS and TP 2.0 while logging - Working on it
-use fputs(buffer, filename); for sd instead of gipsy byte by byte writing
-Add ECU emulation by fuzzing all SIDs and their replies and dumping them to an emulation file on SD
-Data Transfer dumpers should also look for Read/Write MemByAddr
-When you get an "Message Length or Format incorrect" error message on a request, it means that you need to add the tester ID (0xF1)
-Create method to probe an ECU to see if it requires the tester ID for proper message handling
-Add the keyboard action upon input (press f12 when detect securityaccess request for example)
-to properly run a scan, we must toggle power on the ECU after each 0x100 IDS scanned, or it can go in a state where it will not reply to requests anymore
*/

/*
Protocol notes:

TP 2.0
-ACK should be +1 from the header requesting it. f.ex, if server sends 11, you should reply b2. if you ack from a previous point, it will repeat the frames from that one.


UDS
-UDSRead returns the payload WITHOUT the positive SID byte
*/
/*

-P17 controls ECU power. PCB has been redesigned to use p-channel mosfet for both
-reportError on handlers has 0 for no error reported, 1 for all errors reported, and 2 for reporting errors that are not SERVICE_NOT_SUPPORTED
-P18 is used as analog in to measure voltage (12v line)
*/

/*****LEDS***/
DigitalOut led1(LED1);
DigitalOut led2(LED2);
DigitalOut led3(LED3);
DigitalOut led4(LED4);

/****ECU Control***/
DigitalOut ECU_CTRL(p17);

/****Voltage Measurement***/
AnalogIn ain(p18);

/***LED stuff****/
DigitalOut GLED(p22);//green led
DigitalOut RLED(p21);//red led
#define LED_OFF 0
#define LED_RED 1
#define LED_GREEN 2
#define LED_ORANGE 3


/***Push button***/
class Counter {
public:
    Counter(PinName pin) : _interrupt(pin) {        // create the InterruptIn on the pin specified to Counter
        _interrupt.rise(this, &Counter::increment); // attach increment function of this counter instance
    }

    void increment() {
    	wait(0.2);
        _count++;
    }

    int read() {
        return _count;
    }
    void reset() {
        _count = 0;
    }

private:
    InterruptIn _interrupt;
    volatile int _count;
};

Counter counter(p15);

/***CAN***/
CAN can1(p9, p10);
CAN can2(p30, p29);

/****SPI RAM***/
SPI ram(p11, p12, p13); // mosi, miso, sclk
DigitalOut ramCS1(p14);

/***PC Serial***/
USBSerial device(0x1f00, 0x2012, 0x0001, false);

/***Timer***/
Timer timer;

Ticker tick; //used to schedule TesterPresent
uint32_t ownID=0;//our CAN ID
uint32_t rID=0; //remote CAN ID
uint8_t inSession=0; //Used to determine if we are in a session
volatile uint8_t frameCounter=0;//used for frames
uint8_t protocol=0; //0 for TP 2.0, 1 for UDS
uint8_t timeoutCounter=0;//timeout counter for TP 2.0 testerpresent
uint8_t rejectReason;
uint8_t BSBuffer[2048]={0}; //Buffer to store bullshit
volatile uint32_t BSCounter1=0; //Counter for random bullshit
volatile uint32_t BSCounter2=0; //Counter for random bullshit
uint8_t bridge = 0;
uint8_t testerAddress=0;//used to indicate if the tester address should be used, and what address to use. 0 means not used
uint8_t testerMode=0;//to indicate how the tester address should be included

EthernetManager *ethernetManager;
SettingsHandler *settingsHandler;
CanbadgerSettings *settings;
// this queue will be filled by the ethernet manager and will contain ACTION ethernetMessages to be executed
Mail<EthernetMessage, 16> *commandQueue;
bool uartMode;

/****SPI RAM stuff***/
#define CMD_READ    0x03
#define CMD_WRITE   0x02
#define CMD_RDMR    0x05
#define CMD_WRMR    0x01

uint32_t getUInt32FromData(uint32_t offset, char *data) {
	return (data[offset+3] << 24) | (data[offset+2] << 16) | (data[offset+1] << 8) | (data[offset+0]);
}

uint32_t ram_write (uint32_t addr, uint8_t *buf, uint32_t len) {
	uint32_t i;
    ramCS1 = 0;
    ram.write(CMD_WRITE);
    ram.write((addr >> 16) & 0xff);
    ram.write((addr >> 8) & 0xff);
    ram.write(addr & 0xff);

    for (i = 0; i < len; i ++) {
    	ram.write(buf[i]);
    }
    ramCS1 = 1;
    return i;
}

uint32_t ram_read (uint32_t addr, uint8_t *buf, uint32_t len) {
	uint32_t i;

    ramCS1 = 0;
    ram.write(CMD_READ);
    ram.write((addr >> 16) & 0xff);
    ram.write((addr >> 8) & 0xff);
    ram.write(addr & 0xff);

    for (i = 0; i < len; i ++) {
        buf[i] = ram.write(0);
    }
    ramCS1 = 1;
    return i;
}

void clearRAM()
{
	uint32_t cnt=0;
	memset(BSBuffer,0xFF,2048);
	while(cnt < 0x20000)
	{
		ram_write(cnt, BSBuffer, 2048);
		cnt = (cnt + 2048);
	}
}

void showRAM(uint32_t offset, uint32_t length)
{
	uint32_t ct = offset;
	while(ct < (offset + length))
	{
		uint8_t a[16]={0};
		uint8_t b=0;
		if(((offset + length) - ct) >= 16)
		{
			ram_read(ct,a,16);
			b=16;
		}
		else
		{
			ram_read(ct,a,((offset + length) - ct));
			b=((offset + length) - ct);
		}
		for(uint8_t c=0;c<b;c++)
		{
			device.printf("0x%x ",a[c]);
		}
		device.printf("\n");
		ct = ct + 16;
	}
}

int getLine(char *buff, uint8_t whereFrom)//get a line from file2
{
    uint32_t count=0;
    char tmp[2]={'\n',0};
    while(tmp[0] == '\n' || tmp[0] == '\r' || tmp[0] == '\t' || tmp[0] == '\\')
    {
		if(whereFrom == 2)
		{
			if(file2->read(tmp, 1) != 1)
			{
				return -1;
			}
		}
		else
		{
			if(file->read(tmp, 1) != 1)
			{
				return -1;
			}
		}
    }
    while(tmp[0] != '\n' && tmp[0] != '\r' && tmp[0] != '\t' && tmp[0] != '\\')
    {
    	buff[count]=tmp[0];
    	count++;
    	if(whereFrom == 2)
    	{
			if(file2->read(tmp, 1) != 1)
			{
				return -1;
			}
    	}
    	else
    	{
			if(file->read(tmp, 1) != 1)
			{
				return -1;
			}
    	}
    }
    buff[count]=0;
    return count;
}

void strngcat(char *output, char *input)//quick alternative for strcat
{
	uint32_t cnt1=0;
	uint32_t cnt2 =0;
	while(output[cnt1] !='\0')//first, search for the end of the first string
	{
		cnt1++;
	}
	while(input[cnt2] !='\0')//now, copy until the end of the string
	{
		output[cnt1]=input[cnt2];
		cnt1++;
		cnt2++;
	}
	output[cnt1]='\0';//terminate the string

}

void itox(unsigned int i, char *s)//interger to ascii
{
   unsigned char n;
   s += 4;
   *s = '\0';
   for (n = 4; n != 0; --n)
   {
       *--s = "0123456789ABCDEF"[i & 0x0F];
        i >>= 4;
   }
}

uint32_t getArrayLength(char *input)
{
		uint32_t count=0;
	    while(input[count] != 0)
	    {
	    	count++;
	    }
	    return count;
}

void setLED(uint8_t color)
{
	if(color == LED_OFF )//turn it off
	{
		GLED=0;
		RLED=0;
	}
	else if(color == LED_RED)//Red
	{
		GLED=0;
		RLED=1;
	}
	else if(color == LED_GREEN)//Green
	{
		GLED=1;
		RLED=0;
	}
	else if(color == LED_ORANGE)//Orange
	{
		GLED=1;
		RLED=1;
	}
}

void blinkLED(uint8_t times, uint8_t color)
{
	for(uint8_t a=0;a<times;a++)
	{
		setLED(LED_OFF);
		wait(0.2);
		setLED(color);
		wait(0.2);
	}
}

/*----------------------------------------------------------------------------
  setup acceptance filter.  CAN controller (1..2)
 *----------------------------------------------------------------------------*/
/*bool CAN_wrFilter (uint32_t ctrl, uint32_t id, uint8_t format)
{
  static int CAN_std_cnt = 0;
  static int CAN_ext_cnt = 0;
         uint32_t buf0, buf1;
         int cnt1, cnt2, bound1;


  if ((((CAN_std_cnt + 1) >> 1) + CAN_ext_cnt) >= 512)
    return 0;


  LPC_CANAF->AFMR = 0x00000001;

  if (format == CANStandard)  {
    id |= (ctrl-1) << 13;
    id &= 0x0000F7FF;


    if ((CAN_std_cnt & 0x0001) == 0 && CAN_ext_cnt != 0) {
      cnt1   = (CAN_std_cnt >> 1);
      bound1 = CAN_ext_cnt;
      buf0   = LPC_CANAF_RAM->mask[cnt1];
      while (bound1--)  {
        cnt1++;
        buf1 = LPC_CANAF_RAM->mask[cnt1];
        LPC_CANAF_RAM->mask[cnt1] = buf0;
        buf0 = buf1;
      }        
    }

    if (CAN_std_cnt == 0)  {
      LPC_CANAF_RAM->mask[0] = 0x0000FFFF | (id << 16);
    }  else if (CAN_std_cnt == 1)  {
      if ((LPC_CANAF_RAM->mask[0] >> 16) > id)
        LPC_CANAF_RAM->mask[0] = (LPC_CANAF_RAM->mask[0] >> 16) | (id << 16);
      else
        LPC_CANAF_RAM->mask[0] = (LPC_CANAF_RAM->mask[0] & 0xFFFF0000) | id;
    }  else  {

      cnt1 = 0;
      cnt2 = CAN_std_cnt;
      bound1 = (CAN_std_cnt - 1) >> 1;
      while (cnt1 <= bound1)  {
        if ((LPC_CANAF_RAM->mask[cnt1] >> 16) > id)  {
          cnt2 = cnt1 * 2;
          break;
        }
        if ((LPC_CANAF_RAM->mask[cnt1] & 0x0000FFFF) > id)  {
          cnt2 = cnt1 * 2 + 1;
          break;
        }
        cnt1++;
      }

      if (cnt1 > bound1)  {
        if ((CAN_std_cnt & 0x0001) == 0)
          LPC_CANAF_RAM->mask[cnt1]  = 0x0000FFFF | (id << 16);
        else
          LPC_CANAF_RAM->mask[cnt1]  = (LPC_CANAF_RAM->mask[cnt1] & 0xFFFF0000) | id;
      }  else  {
        buf0 = LPC_CANAF_RAM->mask[cnt1];
        if ((cnt2 & 0x0001) == 0)
          buf1 = (id << 16) | (buf0 >> 16);
        else
          buf1 = (buf0 & 0xFFFF0000) | id;
     
        LPC_CANAF_RAM->mask[cnt1] = buf1;

        bound1 = CAN_std_cnt >> 1;

        while (cnt1 < bound1)  {
          cnt1++;
          buf1  = LPC_CANAF_RAM->mask[cnt1];
          LPC_CANAF_RAM->mask[cnt1] = (buf1 >> 16) | (buf0 << 16);
          buf0  = buf1;
        }

        if ((CAN_std_cnt & 0x0001) == 0)
          LPC_CANAF_RAM->mask[cnt1] = (LPC_CANAF_RAM->mask[cnt1] & 0xFFFF0000) | (0x0000FFFF);
      }
    }
    CAN_std_cnt++;
  }  else  {
    id |= (ctrl-1) << 29;

    cnt1 = ((CAN_std_cnt + 1) >> 1);
    cnt2 = 0;
    while (cnt2 < CAN_ext_cnt)  {
      if (LPC_CANAF_RAM->mask[cnt1] > id)
        break;
      cnt1++;
      cnt2++;
    }

    buf0 = LPC_CANAF_RAM->mask[cnt1];
    LPC_CANAF_RAM->mask[cnt1] = id;

    CAN_ext_cnt++;

    bound1 = CAN_ext_cnt - 1;

    while (cnt2 < bound1)  {
      cnt1++;
      cnt2++;
      buf1 = LPC_CANAF_RAM->mask[cnt1];
      LPC_CANAF_RAM->mask[cnt1] = buf0;
      buf0 = buf1;
    }        
  }
  

  buf0 = ((CAN_std_cnt + 1) >> 1) << 2;
  buf1 = buf0 + (CAN_ext_cnt << 2);


  LPC_CANAF->SFF_sa     = 0;
  LPC_CANAF->SFF_GRP_sa = buf0;
  LPC_CANAF->EFF_sa     = buf0;
  LPC_CANAF->EFF_GRP_sa = buf1;
  LPC_CANAF->ENDofTable = buf1;

  LPC_CANAF->AFMR = 0x00000000;
  return 1;
}*/

void dumpSerial()
{
    wait(0.001);
    while(device.readable())
    {
        uint8_t trash=device.getc();
        wait(0.001);//wait for next char
    }//get the first char, trash the rest
}

void CANreceive1() {
    CANMessage msg1;
    if(can1.read(msg1))//should always be true since we use this with an interrupt, but who knows!
    {
    	if(msg1.id != 0)
		{
			if(ownID == 0 || rID == 0)//used to indicate that all frames should be logged
			{
				uint32_t us=timer.read_ms();
				uint8_t tmpbuf[15]={0};
				tmpbuf[0]=(us >> 24);
				tmpbuf[1]=(us >> 16);
				tmpbuf[2]=(us >> 8);
				tmpbuf[3]=us;
				tmpbuf[4]=(msg1.id >> 8);
				tmpbuf[5]=(msg1.id & 0xFF);
				tmpbuf[6]=msg1.len;
				for(uint8_t a=0; a<tmpbuf[6];a++)
				{
					tmpbuf[(a+7)]=msg1.data[a];
				}
				uint8_t wtf=0; //because we can
				for(uint8_t a=0;a<15;a++)
				{
					if(a+BSCounter1 > 2010)//reset the buffer pointer to prevent overflow
					{
						BSCounter1=0;
						wtf = a;
					}
					BSBuffer[((a + BSCounter1) - wtf)]=tmpbuf[a];
				}
				BSCounter1=(BSCounter1+15);//set the new pointer
				BSCounter2++;//increase the buffered frame count
			}
    		else if (msg1.id == ownID || msg1.id == rID)
			{
				uint8_t tmpbuf[14]={0};
				uint32_t us=timer.read_ms();
				tmpbuf[0]=(us >> 24);
				tmpbuf[1]=(us >> 16);
				tmpbuf[2]=(us >> 8);
				tmpbuf[3]=us;
				if(msg1.id == ownID)
				{
					tmpbuf[4]=(ownID >> 8);
					tmpbuf[5]=ownID;
				}
				else
				{
					tmpbuf[4]=(rID >> 8);
					tmpbuf[5]=rID;
				}
				tmpbuf[6]=msg1.len;
				for(uint8_t a=0; a<tmpbuf[6];a++)
				{
					tmpbuf[(a+7)]=msg1.data[a];
				}
				uint8_t wtf=0; //because we can
				for(uint8_t a=0;a<15;a++)
				{
					if(a+BSCounter1 > 2010)//reset the buffer pointer to prevent overflow
					{
						BSCounter1=0;
						wtf = a;
					}
					BSBuffer[((a + BSCounter1) - wtf)]=tmpbuf[a];
				}
				BSCounter1=(BSCounter1+15);//set the new pointer
				BSCounter2++;//increase the buffered frame count
			}
		}
    	if(bridge == 1 && msg1.id != 0)
    	{
    		uint32_t cntt=0;
    		while(!can2.write(CANMessage(msg1.id, reinterpret_cast<char*>(&msg1.data), msg1.len)) && cntt < 10) //add a timeout
    		{
    			wait(0.0001);
    			cntt++;
    		}//make sure the msg goes out
    	}
    }
}

void CANreceive2() {
    CANMessage msg2;
    if(can2.read(msg2))//should always be true since we use this with an interrupt
    {
    	if(msg2.id != 0)
		{
			if(ownID == 0 || rID == 0)//used to indicate that all frames should be logged
			{
				uint8_t tmpbuf[15]={0};//here we will store the two bytes of the ID, so we need an extra byte
				uint32_t us=timer.read_ms();
				tmpbuf[0]=(us >> 24);
				tmpbuf[1]=(us >> 16);
				tmpbuf[2]=(us >> 8);
				tmpbuf[3]=us;
				tmpbuf[4]=(msg2.id >> 8);
				tmpbuf[5]=(msg2.id & 0xFF);
				tmpbuf[6]=msg2.len;
				for(uint8_t a=0; a<tmpbuf[6];a++)
				{
					tmpbuf[(a+7)]=msg2.data[a];
				}
				uint8_t wtf=0; //because we can
				for(uint8_t a=0;a<15;a++)
				{
					if(a+BSCounter1 > 2010)//reset the buffer pointer to prevent overflow
					{
						BSCounter1=0;
						wtf = a;
					}
					BSBuffer[((a + BSCounter1) - wtf)]=tmpbuf[a];
				}
				BSCounter1=(BSCounter1+15);//set the new pointer
				BSCounter2++;//increase the buffered frame count
			}
			else if (msg2.id == ownID || msg2.id == rID)
			{
				uint8_t tmpbuf[15]={0};
				uint32_t us=timer.read_ms();
				tmpbuf[0]=(us >> 24);
				tmpbuf[1]=(us >> 16);
				tmpbuf[2]=(us >> 8);
				tmpbuf[3]=us;
				if(msg2.id == ownID)
				{
				   tmpbuf[4]=(ownID >> 8);
				   tmpbuf[5]=ownID;
				}
				else
				{
					tmpbuf[4]=(rID >> 8);
					tmpbuf[5]=rID;
				}
				tmpbuf[6]=msg2.len;
				for(uint8_t a=0; a<tmpbuf[6];a++)
				{
					tmpbuf[(a+7)]=msg2.data[a];
				}
				uint8_t wtf=0; //because we can
				for(uint8_t a=0;a<15;a++)
				{
					if(a+BSCounter1 > 2010)//reset the buffer pointer to prevent overflow
					{
						BSCounter1=0;
						wtf = a;
					}
					BSBuffer[((a + BSCounter1) - wtf)]=tmpbuf[a];
				}
				BSCounter1=(BSCounter1+15);//set the new pointer
				BSCounter2++;//increase the buffered frame count
			}
			if(bridge == 1 && msg2.id != 0)
			{
	    		uint32_t cntt=0;
	    		while(!can1.write(CANMessage(msg2.id, reinterpret_cast<char*>(&msg2.data), msg2.len)) && cntt<10) //add a timeout
	    		{
	    			cntt++;
	    		}//make sure the msg goes out
	    	}
		}
    }
}

bool writeToLog(char *stuff) //just to make it easier
{
	if (file->write(stuff,getArrayLength(stuff)) != getArrayLength(stuff))
	{
		return 0;
	}
	return 1;
}

/*void removeCANFilter()
{
	for(uint16_t a=0;a<512;a++)
	{
		filtered[a]=0;
	}
}*/

/*void filterCANIDs()
{
    device.printf("Waiting for the first frame to begin building filter list...\n");
    CANMessage can1_msg(0,CANExtended);
    uint32_t cnt=0;
    while(!can1.read(can1_msg) && cnt<1000)
    {
    	cnt++;
    	wait(0.001);
    }
    if (cnt==1000)
    {
    	device.printf("No CAN traffic was detected, filter list was not created\n");
    	return;
    }
    device.printf("Got the first frame, creating filtering list...\n");
    uint32_t timr=0;
    while(f_counter<512 && timr<5000)
    {
      uint32_t cid=can1_msg.id;
      uint8_t doit=1;
      for(uint32_t cnt=0;cnt<f_counter;cnt++)
      {
        if(filtered[cnt]==cid)
        {
            doit=0;
            break;
        }
      }
      if(doit==1 || f_counter==0)//need to also save the first ID!
      {
          filtered[f_counter]=cid;
          f_counter++;
      }
      while(!can1.read(can1_msg) && timr<5000)
      {
        wait(0.001);
        timr++;
      }
    }
}*/

double grabFloatValue()
{
	uint8_t cnt=0;
	char ch[3]={0};
	char got[12]={0};
	uint8_t a = file2->read(ch, 1);
	while((ch[0] == '*' || ch[0] == 0xA || ch[0] == 0xD) && a == 1)//discard the bs
	{
		a = file2->read(ch, 1);
	}
	if(a != 1)
	{
		return 0xFFFFFFFF;//end of file
	}
	while(ch[0] != ',' && ch[0] != 0xA && ch[0] != 0xD)
	{
		got[cnt]=ch[0];
		cnt++;
		file2->read(ch, 1);
	}
	return strtod (got, NULL);
}

uint32_t grabHexValue()//returns hex value read from file2
{
	uint8_t cnt=0;
	char ch[3]={0};
	char got[9]={0};
	uint8_t a = file2->read(ch, 1);
	while((ch[0] == '*' || ch[0] == 0xA || ch[0] == 0xD) && a == 1)//discard the bs
	{
		a = file2->read(ch, 1);
	}
	if(a != 1)
	{
		return 0xFFFFFFFF;//end of file
	}
	while(ch[0] != ',' && ch[0] != 0xA && ch[0] != 0xD)
	{
		got[cnt]=ch[0];
		cnt++;
		file2->read(ch, 1);
	}
	return strtol(got,NULL,0);
}

bool logToRawFrame(uint32_t *tim, uint32_t *tID, uint8_t *tlen, uint8_t *tdata)
{
	uint32_t tmpval = grabHexValue();
	if (tmpval == 0xFFFFFFFF)//EOF
	{
		return 0;
	}
	*tim=tmpval;
	*tID=grabHexValue();
	*tlen = grabHexValue();
	for (uint8_t ccnt = 0; ccnt< *tlen; ccnt++)
	{
		tdata[ccnt]=grabHexValue();
	}
	return 1;

}

bool fileToRawFrame(uint32_t *tim, uint32_t *tID, uint8_t *tlen, uint8_t *tdata)
{
	uint8_t ch[3]={0};
	uint8_t a = file2->read(ch, 1);
	if(a != 1)
	{
		return 0;//end of file
	}
	*tim=ch[0];
	file2->read(ch, 1);
	*tim = ((*tim << 8) + ch[0]);
	file2->read(ch, 1);
	*tim = ((*tim << 8) + ch[0]);
	file2->read(ch, 1);
	*tim = ((*tim << 8) + ch[0]);
	file2->read(ch, 1);
	*tID=ch[0];
	file2->read(ch, 1);
	*tID = ((*tID << 8) + ch[0]);
	file2->read(ch, 1);
	*tlen =  ch[0];
	for(uint8_t a = 0; a < 8; a++)
	{
		file2->read(ch, 1);
		tdata[a]=ch[0];
	}
	return 1;
}

bool fileToFrame(uint32_t *tim, uint32_t *tID, uint8_t *tlen, uint8_t *tdata)
{
	uint8_t ch[2]="*";
	while(ch[0] == '*')//just to skip the header
	{
		uint8_t a = file2->read(ch, 1);
		if(a != 1)
		{
			return 0;//end of file
		}
	}
	*tim=ch[0];
	file2->read(ch, 1);
	*tim = ((*tim << 8) + ch[0]);
	file2->read(ch, 1);
	*tim = ((*tim << 8) + ch[0]);
	file2->read(ch, 1);
	*tim = ((*tim << 8) + ch[0]);
	file2->read(ch, 1);
	*tID=ch[0];
	file2->read(ch, 1);
	*tID = ((*tID << 8) + ch[0]);
	file2->read(ch, 1);
	*tlen =  ch[0];
	for(uint8_t a = 0; a< 8; a++)
	{
		file2->read(ch, 1);
		tdata[a]=ch[0];
	}
	return 1;
}

bool lineToHex(char *tline, uint32_t *tID, uint8_t *tlen, uint8_t *tdata)//remember that ID and len must be defered!
{
	if(tline[0]== '*')
	{
		return 0;
	}
	char buf[8]={0};
	uint8_t cnt=0;//to keep track of line
	uint8_t cnt2=0;//to keep track of buf
	char tmp=0;
	while(tline[cnt] !=',')
	{
		buf[cnt2]=tline[cnt];
		cnt++;
		cnt2++;
	}
	sscanf(buf, "%x", tID);//store the ID
	cnt2=0;
	cnt++;//point the array to the next char
	for(uint8_t a=0;a<6;a++)//reset the buf
	{
		buf[a]=0;
	}
	while(tline[cnt] !=',')
	{
		buf[cnt2]=tline[cnt];
		cnt++;
		cnt2++;
	}
	sscanf(buf, "%x", tlen);//store the length
	cnt++;//point the array to the next char
	cnt2=0;
	while(tline[cnt] !='\n')//read until the end of the line
	{
		while(tline[cnt] !=',' && tline[cnt] !='\n')
		{
			buf[cnt2]=tline[cnt];
			cnt++;
			cnt2++;
		}
		sscanf(buf, "%x", &tdata[tmp]);//store the length
		if(tline[cnt] =='\n')
		{
			return 1;
		}
		tmp++;
		cnt++;
		cnt2=0;
		buf[0]=0;
		buf[1]=0;
	}
	return 1;
}

bool doesFileExist(const char *fileName)
{
	FileHandle* file123;
	file123 = sd.open(fileName, O_RDONLY);
	if (file123 == NULL)
    {
        return 0;
    }
    file123->close();
    return 1;
}

bool doesDirExist(const char *dirName)
{
	 DIR* dir;
	 dir=opendir(dirName);
	 if(dir)
	 {
		 closedir(dir);
		 return 1;
	 }
    return 0;
}

bool toggleLogging(const char *filename, uint8_t loging)//opens or closes a file to write to.
{
    if(loging==1)
    {
        device.printf("\n");
        if(!doesFileExist(filename))
        {
        	file = sd.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
        	if (file == NULL)
            {
                device.printf("Could not create file for writing\n");
                return 0;
            }
        }
        else
        {
        	file = sd.open(filename, O_APPEND);
        	if (file == NULL)
            {
                device.printf("Could not open file for writing\n");
                return 0;
            }
        }
        char tmp[]="*********************************************************\n";
        file->write(tmp,(sizeof(tmp) - 1));
        device.printf("Please dont remove the SD or data will be lost!\n");
    }
    else
    {
    	char tmp[]="*********************************************************\n";
    	file->write(tmp,(sizeof(tmp) - 1));
        file->close();
    }
    return 1;
}

bool toggleRawLogging(const char *filename, uint8_t loging)//opens or closes a file to write to.
{
    if(loging==1)
    {
        device.printf("\n");
        if(!doesFileExist(filename))
        {
        	file = sd.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
        	if (file == NULL)
            {
                device.printf("Could not create file for writing\n");
                return 0;
            }
        }
        else
        {
    		char fn[64]="/sd";
    		strcat(fn,filename);
    		remove(fn);
    		if(doesFileExist(filename))
    		{
    			device.printf("\nCould not remove existing file for creating new one\n");
    			return 0;
    		}
    		file = sd.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    		if(file == NULL)
    		{
    		   device.printf("\nCould not open file for writing\n");
    		   return 0;
    		}
        }
    }
    else
    {
        file->close();
    }
    return 1;
}

bool toggleDump(const char *filename, uint8_t loging)//opens or closes a file to write to. loging 0 closes the file, 1 opens it with append if exists, 2 overwrites if exists
{
    if(loging==1)
    {
        if(!doesFileExist(filename))
        {
        	file = sd.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
            if(file == NULL)
            {
                device.printf("\nCould not create file for writing\n");
                return 0;
            }
        }
        else
        {
        	file = sd.open(filename, O_APPEND);
            if(file == NULL)
            {
                device.printf("\nCould not open file for writing\n");
                return 0;
            }
        }
    }
    else if(loging==2)
    {
    	if(!doesFileExist(filename))
    	{
    	   file = sd.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    	   if(file == NULL)
    	   {
    	      device.printf("\nCould not create file for writing\n");
    	      return 0;
    	   }
    	}
    	else
    	{
    		char fn[64]="/sd";
    		strcat(fn,filename);
    		remove(fn);
    		if(doesFileExist(filename))
    		{
    			device.printf("\nCould not remove existing file for creating new one\n");
    			return 0;
    		}
    		file = sd.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    		if(file == NULL)
    		{
    		   device.printf("\nCould not open file for writing\n");
    		   return 0;
    		}
    	}
    }
    else
    {
        file->close();
    }
    return 1;
}

uint32_t inputToHex()
{
  char param[17]={0};
  uint32_t xx=0;
  uint8_t cnt=0;
  char inc=0xFF;
  while(!device.readable());//wait for incoming
  wait(0.1);//give time for all chars to arrive
  while(device.readable() && cnt < 16)
  {
    inc=device.getc();
    if(inc != 0xA && inc != 0xD)//to sanitize input
    {
		param[cnt]=inc;
		cnt++;
    }
    else
    {
    	break;
    }
    //device.printf(reinterpret_cast<char*>(&param[cnt]));
  }
  if(cnt == 0)
  {
	  dumpSerial();
	  return 0;
  }
  sscanf(param, "%x", &xx);
  dumpSerial();
  return xx;
}

uint32_t inputToDec()
{
  char param[17]={0};
  uint32_t xx=0;
  uint8_t cnt=0;
  char inc=0xFF;
  while(!device.readable());//wait for incoming
  wait(0.1);//give time for all chars to arrive
  while(device.readable() && cnt < 16)
  {
	  inc=device.getc();
	  if(inc >=0x30 && inc <= 0x39)
	    {
			param[cnt]=inc;
			cnt++;
	    }
	    else
	    {
	    	break;
	    }
    //device.printf(reinterpret_cast<char*>(&param[cnt]));
  }
  if(cnt == 0)
  {
	  dumpSerial();
	  return 0;
  }
  sscanf(param, "%d", &xx);
  dumpSerial();
  return xx;
}

uint32_t arrayToHex(char *input, uint8_t len)
{
  char param[len];
  uint32_t xx=0;
  for (uint8_t cnt=0;cnt<len;cnt++)
  {
    param[cnt]=input[cnt];
    //device.printf(reinterpret_cast<char*>(&param[cnt]));
  }
  sscanf(param, "%x", &xx);
  return xx;
}


/*uint32_t getLineNumber(const char *str, uint32_t *buff)
{
    file->lseek(0,SEEK_SET);//rewind the file
    uint32_t line_num = 0;
    uint32_t find_result = 0;
    char temp [512];
    while(sd.gets(temp, 512, file) != NULL)
    {
        if((strstr(temp, str)) != NULL) 
        {
            buff[find_result]=line_num;
            find_result++;
        }
        line_num++;
    }
    return find_result;
}*/

double getVoltage()
{
	double readv=(ain.read()*100.0);
	return((16.225*readv)/100.0);
}

void resetCAN()
{
    can1.reset();
    can2.reset();
    can1.frequency(500000);
    can2.frequency(500000);
}

bool getBit(uint8_t b, uint8_t bitNumber) //checks if a bit is set in a byte
{
	if((b & (1 << bitNumber)))
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

uint32_t getFrameCount(char *input)
{
	file2 = sd.open(input, O_RDONLY);//open source file
	if (file2 == NULL)
	{
		device.printf("Error opening source file file\n");
		return 0;
	}
	uint32_t counted=0;
	char ch[3]={0};
	uint8_t a= file2->read(ch, 1);
	while(a == 1)
	{
		a = file2->read(ch, 1);
		if(ch[0] ==  0xA)
		{
			counted++;
		}
	}
	file2->close();
	return (counted - 2);//we need to remove the header and the last ***** stuff
}

void sendFrame(uint8_t *payloadd, uint8_t lnn, uint8_t bus)
{
	if(bus == 1)
	{
		uint8_t timeout=0;
		while (!can1.write(CANMessage(ownID, reinterpret_cast<char*>(payloadd),lnn)) && !device.readable() && timeout < 10)
		{
			wait(0.001);
			timeout++;
		}//make sure the msg goes out
	}
	else
	{
		uint8_t timeout=0;
		while (!can2.write(CANMessage(ownID, reinterpret_cast<char*>(payloadd),lnn)) && !device.readable() && timeout < 10)
		{
			wait(0.001);
			timeout++;
		}//make sure the msg goes out
	}

}

void TPChannelAlive()
{
	CANMessage can_msg3(0,CANStandard);
	if(can1.read(can_msg3))
	{
		if (can_msg3.id == rID && can_msg3.data[0] == CHANNEL_TEST)
		{
			//led1 = !led1;
			uint8_t data[8]={PARAMETER_REQUEST_OK,0xF,0xCA,0xFF,0x05,0xFF};
			sendFrame(data,6,1);
			//led1 = !led1;
			timer.reset();
		}
		if(timer.read_ms() > 3000)
		{
			timer.stop();
			timer.reset();
			inSession=0;
		}
	}
}

void checkError(uint8_t &errorCode)//to have some verbosity on the error codes
{
	device.printf("Target reported error due to: ");
    switch (errorCode)
    {
    	case RESPONSE_TOO_LONG:
    	{
    		device.printf("Response too long\n");
    		break;
    	}
    	case REQUEST_SEQUENCE_ERROR:
    	{
    		device.printf("Request Sequence error\n");
    		break;
    	}
    	case NR_FROM_SUBNET_COMPONENT:
    	{
    		device.printf("No response from sub-net component\n");
    		break;
    	}
    	case FAILURE_PREVENTS_EXECUTION:
    	{
    		device.printf("Failure prevents execution of requested action\n");
    		break;
    	}
    	case UPLOAD_DOWNLOAD_NOT_ACCEPTED:
    	{
    		device.printf("Upload/Download not accepted\n");
    		break;
    	}
    	case WRONG_BLOCK_SEQUENCE_COUNTER:
    	{
    		device.printf("Wrong Block Sequence counter\n");
    		break;
    	}
    	case SUBFUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION:
    	{
    		device.printf("Sub-function not supported in active session\n");
    		break;
    	}
    	case SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION:
    	{
    		device.printf("Service not supported in active session\n");
    		break;
    	}
    	case SERVICE_NOT_SUPPORTED:
        {
            device.printf("Service not supported\n");
            break;
        }
        case INVALID_FORMAT:
        {
            device.printf("Sub-Function not supported or Invalid Format\n");
            break;
        }
        case CONDITIONS_NOT_MET:
        {
            device.printf("Conditions not met\n");
            break;
        }
        case REQUEST_OUT_OF_RANGE:
        {
            device.printf("Request out of range\n");
            break;
        }
        case INVALID_KEY:
        {
            device.printf("Invalid Key\n");
            break;
        }
        case EXCEEDED_NUMBER_OF_ATTEMPTS:
        {
            device.printf("Exceeded number of attempts\n");
            break;
        }
        case TIME_DELAY_NOT_EXPIRED:
        {
            device.printf("Time delay not expired\n");
            break;
        }
        case SECURITY_ACCESS_REQUIRED:
        {
            device.printf("Security Access required\n");
            break;
        }
        case BUSY_REPEAT_REQUEST:
        {
            device.printf("Busy, repeat request\n");
            break;
        }
        case ROUTINE_NOT_COMPLETE:
        {
            device.printf("Routine not complete\n");
            break;
        }
        case MESSAGE_LENGTH_OR_FORMAT_INCORRECT:
        {
            device.printf("Message Length or Format incorrect\n");
            break;
        }
        case GENERAL_ERROR:
        {
            device.printf("General Error\n");
            break;
        }
        case DOWNLOAD_NOT_ACCEPTED:
        {
            device.printf("Download not Accepted\n");
            break;
        }
        case IMPROPER_DOWNLOAD_TIME:
        {
            device.printf("Improper download time\n");
            break;
        }
        case CANT_DOWNLOAD_TO_SPECIFIED_ADDRESS:
        {
            device.printf("Can't Download to specified address\n");
            break;
        }
        case CANT_DOWNLOAD_NUMBER_OF_BYTES_REQUESTED:
        {
            device.printf("Can't Download number of bytes requested\n");
            break;
        }
        case UPLOAD_NOT_ACCEPTED:
        {
            device.printf("Upload not accepted\n");
            break;
        }
        case IMPROPER_UPLOAD_TYPE:
        {
            device.printf("Improper Upload type\n");
            break;
        }
        case CANT_UPLOAD_FROM_SPECIFIED_ADDRESS:
        {
            device.printf("Can't Upload from specified address\n");
            break;
        }
        case CANT_UPLOAD_NUMBER_OF_BYTES_REQUESTED:
        {
            device.printf("Can't Upload number of bytes requested\n");
            break;
        }
        case TRANSFER_SUSPENDED:
        {
            device.printf("Transfer suspended\n");
            break;
        }
        case TRANSFER_ABORTED:
        {
            device.printf("Transfer Aborted\n");
            break;
        }
        case ILLEGAL_ADDRESS_IN_BLOCK_TRANSFER:
        {
            device.printf("Illegal address in Block Transfer\n");
            break;
        }
        case ILLEGAL_BYTE_COUNT_IN_BLOCK_TRANSFER:
        {
            device.printf("Illegal byte count in block transfer\n");
            break;
        }
        case ILLEGAL_BLOCK_TRANSFER_TYPE:
        {
            device.printf("Illegal Block Transfer type\n");
            break;
        }
        case BLOCK_TRANSFER_DATA_CHECKSUM_ERROR:
        {
            device.printf("Block Transfer data checksumm error\n");
            break;
        }
        case INCORRECT_BYTE_COUNT_DURING_BLOCK_TRANSFER:
        {
            device.printf("Incorrect byte count during block transfer\n");
            break;
        }
        case SERVICE_NOT_SUPPORTED_IN_ACTIVE_DIAGNOSTIC_MODE:
        {
            device.printf("Service not supported in active Diagnostics Mode\n");
            break;
        }
        default:
        {
            device.printf("Unknown error: 0x%x\n",errorCode);
            break;
        }
    }
}

void sendTesterPresent()
{
    CANMessage can_msg3(0,CANStandard);
    uint32_t cnta=0;
    if(protocol==1)
    {
        uint8_t data[8]={0x02,TESTER_PRESENT,0x00,0x00,0x00,0x00,0x00,0x00};
        uint8_t a=0;//used by tester mode 2
        if(testerMode == 2)
        {
        	data[0] = (rID & 0xFF);
        	data[1] = 0x02;
        	data[2] = TESTER_PRESENT;
        	a = 1;
        }
        sendFrame(data,8,1);
        while(!can1.read(can_msg3) || (can_msg3.id != rID && cnta<10000))
        {
            wait(0.0001);
            cnta++;
        }
        led1 = !led1;
        if(can_msg3.data[(1 + a)] != (TESTER_PRESENT + 0x40) || cnta == 10000)
        {
           /*if(cnta<10000)
           {
        	   checkError(can_msg3.data[3]);
           }*/
           inSession=0;
           tick.detach();
        }
        led1 = !led1;
    }
}

bool parsePIDData(uint8_t *data)
{
    char tmpoo[1024]={0};
	switch(data[0])
	{
		case PID_ENGINE_LOAD:
		{
			sprintf(tmpoo,"%f % \n",((100.0/255.0) * data[1]));
			break;
		}
		case PID_COOLANT_TEMP:
		{
			sprintf(tmpoo,"%d°C \n",(data[1] - 40));
			break;
		}
		case PID_SHORT_TERM_FUEL_TRIM_1:
		{
			sprintf(tmpoo,"%f % \n",(((100.0/128.0) * data[1]) - 100.0));
			break;
		}
		case PID_LONG_TERM_FUEL_TRIM_1:
		{
			sprintf(tmpoo,"%f % \n",(((100.0/128.0) * data[1]) - 100.0));
			break;
		}
		case PID_SHORT_TERM_FUEL_TRIM_2:
		{
			sprintf(tmpoo,"%f % \n",(((100.0/128.0) * data[1]) - 100.0));
			break;
		}
		case PID_LONG_TERM_FUEL_TRIM_2:
		{
			sprintf(tmpoo,"%f % \n",(((100.0/128.0) * data[1]) - 100.0));
			break;
		}
		case PID_FUEL_PRESSURE:
		{
			sprintf(tmpoo,"%d kPa \n",(data[1] * 3));
			break;
		}
		case PID_INTAKE_MAP:
		{
			sprintf(tmpoo,"%d kPa \n",data[1]);
			break;
		}
		case PID_RPM:
		{
			sprintf(tmpoo,"%d RPM \n",(((256 * data[1]) + data[2]) / 4));
			break;
		}
		case PID_SPEED:
		{
			sprintf(tmpoo,"%d KM/H \n",data[1]);
			break;
		}
		case PID_TIMING_ADVANCE:
		{
			sprintf(tmpoo,"%f ° before TDC \n", ((data[1] / 2.0) - 64.0));
			break;
		}
		case PID_INTAKE_TEMP:
		{
			sprintf(tmpoo,"%d°C \n", (data[1] - 40));
			break;
		}
		case PID_MAF_FLOW:
		{
			sprintf(tmpoo,"%f grams/sec \n",(((256.0 * data[1]) + data[2]) / 100.0));
			break;
		}
		case PID_THROTTLE:
		{
			sprintf(tmpoo,"%f % \n",((100.0/255.0) * data[1]));
			break;
		}
		case PID_AUX_INPUT:
		{
			if(data[1] == 0)
			{
				sprintf(tmpoo,"Power Take Off \n");
			}
			else
			{
				sprintf(tmpoo,"Active \n");
			}
			break;
		}
		case PID_RUNTIME:
		{
			sprintf(tmpoo,"%d Seconds \n",((256 * data[1]) + data[2]));
			break;
		}
		case PID_DISTANCE_WITH_MIL:
		{
			sprintf(tmpoo,"%d KM \n",((256 * data[1]) + data[2]));
			break;
		}
		case PID_FUEL_LEVEL:
		{
			sprintf(tmpoo,"%f % \n",((100.0/255.0) * data[1]));
			break;
		}
		case PID_DISTANCE:
		{
			sprintf(tmpoo,"%d KM \n",((256 * data[1]) + data[2]));
			break;
		}
		case PID_BAROMETRIC:
		{
			sprintf(tmpoo,"%d kPa \n",data[1]);
			break;
		}
		case PID_CONTROL_MODULE_VOLTAGE:
		{
			sprintf(tmpoo,"%f V \n",(((256.0 * data[1]) + data[2]) / 1000.0));
			break;
		}
		case PID_TIME_WITH_MIL:
		{
			sprintf(tmpoo,"%d Minutes \n",((256 * data[1]) + data[2]));
			break;
		}
		case PID_TIME_SINCE_CODES_CLEARED:
		{
			sprintf(tmpoo,"%d Minutes  \n",((256 * data[1]) + data[2]));
			break;
		}
		case PID_ETHANOL_FUEL:
		{
			sprintf(tmpoo,"%f % \n", ((100.0/255.0) * data[1]));
			break;
		}
		case PID_ENGINE_OIL_TEMP:
		{
			sprintf(tmpoo,"%d°C \n", (data[1] - 40));
			break;
		}
		case PID_ENGINE_FUEL_RATE:
		{
			sprintf(tmpoo,"%f L/h \n", (((256.0 * data[1]) + data[2]) / 20.0));
			break;
		}
		case PID_MONITOR_STATUS:
		{
			uint8_t dtc_cnt= (data[1] & 0x7F);
			uint8_t MIL = (data[1] & 0x80);
			uint8_t monitorType = (data[2] & 8);
			uint8_t MisfireAv = (data[2] & 1);
			uint8_t MisfireCp = (data[2] & 10);
			uint8_t FuelAv = (data[2] & 2);
			uint8_t FuelCp = (data[2] & 20);
			uint8_t CompAv = (data[2] & 4);
			uint8_t CompCp = (data[2] & 40);
			if(MIL == 0)
			{
				sprintf(tmpoo,"\n-MIL Status: Off \n");
			}
			else
			{
				sprintf(tmpoo,"\n-MIL Status: On \n");
			}
			writeToLog(tmpoo);
			sprintf(tmpoo,"-Number of confirmed emissions-related DTCs available: %d \n", dtc_cnt);
			writeToLog(tmpoo);
			sprintf(tmpoo,"-Misfire test is ");
			writeToLog(tmpoo);
			if(MisfireAv == 0)
			{
				sprintf(tmpoo,"Not Available \n");
			}
			else
			{
				sprintf(tmpoo,"Available with status: ");
				writeToLog(tmpoo);
				if(MisfireCp != 0)
				{
					sprintf(tmpoo,"Incomplete \n");
				}
				else
				{
					sprintf(tmpoo,"Complete \n");
				}
			}
			writeToLog(tmpoo);
			sprintf(tmpoo,"-Fuel System test is ");
			writeToLog(tmpoo);
			if(FuelAv == 0)
			{
				sprintf(tmpoo,"Not Available \n");
			}
			else
			{
				sprintf(tmpoo,"Available with status: ");
				writeToLog(tmpoo);
				if(FuelCp != 0)
				{
					sprintf(tmpoo,"Incomplete \n");
				}
				else
				{
					sprintf(tmpoo,"Complete \n");
				}
			}
			writeToLog(tmpoo);
			sprintf(tmpoo,"-Components test is ");
			writeToLog(tmpoo);
			if(CompAv == 0)
			{
				sprintf(tmpoo,"Not Available \n");
			}
			else
			{
				sprintf(tmpoo,"Available with status: ");
				writeToLog(tmpoo);
				if(CompCp != 0)
				{
					sprintf(tmpoo,"Incomplete \n");
				}
				else
				{
					sprintf(tmpoo,"Complete \n");
				}
			}
			writeToLog(tmpoo);
			if(monitorType == 0)//ad tests for spark ignition shit
			{
				uint8_t CatAv = (data[3] & 1);
				uint8_t CatCp = (data[4] & 1);
				uint8_t HCatAv = (data[3] & 2);
				uint8_t HCatCp = (data[4] & 2);
				uint8_t EvapAv = (data[3] & 4);
				uint8_t EvapCp = (data[4] & 4);
				uint8_t SASAv = (data[3] & 8);
				uint8_t SASCp = (data[4] & 8);
				uint8_t ACAv = (data[3] & 16);
				uint8_t ACCp = (data[4] & 16);
				uint8_t OxSenAv = (data[3] & 32);
				uint8_t OxSenCp = (data[4] & 32);
				uint8_t OxHeatAv = (data[3] & 64);
				uint8_t OxHeatCp = (data[4] & 64);
				uint8_t EGRAv = (data[3] & 128);
				uint8_t EGRCp = (data[4] & 128);
				sprintf(tmpoo,"-Catalyst test is ");
				writeToLog(tmpoo);
				if(CatAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(CatCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-Heated Catalyst test is ");
				writeToLog(tmpoo);
				if(HCatAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(HCatCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-Evaporative System test is ");
				writeToLog(tmpoo);
				if(EvapAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(EvapCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-Secondary Air System test is ");
				writeToLog(tmpoo);
				if(SASAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(SASCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-A/C Refrigerant test is ");
				writeToLog(tmpoo);
				if(ACAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(ACCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-Oxygen Sensor test is ");
				writeToLog(tmpoo);
				if(OxSenAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(OxSenCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-Oxygen Sensor Heater test is ");
				writeToLog(tmpoo);
				if(OxHeatAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(OxHeatCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-EGR System test is ");
				writeToLog(tmpoo);
				if(EGRAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(EGRCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
			}
			else //sorry to use same var names, but way too lazy to rename them
			{
				uint8_t CatAv = (data[3] & 1);
				uint8_t CatCp = (data[4] & 1);
				uint8_t HCatAv = (data[3] & 2);
				uint8_t HCatCp = (data[4] & 2);
				uint8_t SASAv = (data[3] & 8);
				uint8_t SASCp = (data[4] & 8);
				uint8_t OxSenAv = (data[3] & 32);
				uint8_t OxSenCp = (data[4] & 32);
				uint8_t OxHeatAv = (data[3] & 64);
				uint8_t OxHeatCp = (data[4] & 64);
				uint8_t EGRAv = (data[3] & 128);
				uint8_t EGRCp = (data[4] & 128);
				sprintf(tmpoo,"-NMHC Catalyst test is ");
				writeToLog(tmpoo);
				if(CatAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(CatCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-NOx/SCR Monitor test is ");
				writeToLog(tmpoo);
				if(HCatAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(HCatCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-Boost Pressure test is ");
				writeToLog(tmpoo);
				if(SASAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(SASCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-Exhaust Gas Sensor test is ");
				writeToLog(tmpoo);
				if(OxSenAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(OxSenCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-PM Filter Monitoring test is ");
				writeToLog(tmpoo);
				if(OxHeatAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(OxHeatCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
				sprintf(tmpoo,"-EGR and/or VVT System test is ");
				writeToLog(tmpoo);
				if(EGRAv == 0)
				{
					sprintf(tmpoo,"Not Available \n");
				}
				else
				{
					sprintf(tmpoo,"Available with status: ");
					writeToLog(tmpoo);
					if(EGRCp != 0)
					{
						sprintf(tmpoo,"Incomplete \n");
					}
					else
					{
						sprintf(tmpoo,"Complete \n");
					}
				}
				writeToLog(tmpoo);
			}
			return 1;
		}
		case PID_FUEL_TYPE:
		{
			switch(data[1])
			{
				case 0:
				{
					sprintf(tmpoo,"Not Available \n");
					break;
				}
				case 1:
				{
					sprintf(tmpoo,"Gasoline \n");
					break;
				}
				case 2:
				{
					sprintf(tmpoo,"Methanol \n");
					break;
				}
				case 3:
				{
					sprintf(tmpoo,"Ethanol \n");
					break;
				}
				case 4:
				{
					sprintf(tmpoo,"Diesel \n");
					break;
				}
				case 8:
				{
					sprintf(tmpoo,"Electric \n");
					break;
				}
				case 17:
				{
					sprintf(tmpoo,"Hybrid Gasoline \n");
					break;
				}
				case 18:
				{
					sprintf(tmpoo,"Hybrid Ethanol \n");
					break;
				}
				case 19:
				{
					sprintf(tmpoo,"Hybrid Diesel \n");
					break;
				}
				case 20:
				{
					sprintf(tmpoo,"Hybrid Electric \n");
					break;
				}
				case 21:
				{
					sprintf(tmpoo,"Hybrid running electric and combustion engine \n");
					break;
				}
				default:
				{
					sprintf(tmpoo,"Type 0x%x \n", data[1]);
					break;
				}
			}
			break;
		}
		case PID_ENGINE_COOLANT_TEMP:
		{
			sprintf(tmpoo,"%d°C \n", (data[1] - 40));
			break;
		}
		default:
		{
			return 0;
		}
	}
	writeToLog(tmpoo);
	return 1;
}

void parseVehicleInfoRequest(uint8_t &request)
{
	char tmp[128]={0};
	switch (request)
	{
		case 0x0:
		{
				sprintf(tmp,"Supported PIDs (0x01 - 0x20) ");
				break;
		}
		case 0x1:
		{
				sprintf(tmp,"VIN Message Count in PID 0x02 ");
				break;
		}
		case 0x2:
		{
				sprintf(tmp,"Vehicle Identification Number (VIN) ");
				break;
		}
		case 0x3:
		{
				sprintf(tmp,"Calibration ID message count for PID 04 ");
				break;
		}
		case 0x4:
		{
				sprintf(tmp,"Calibration ID ");
				break;
		}
		case 0x5:
		{
				sprintf(tmp,"Calibration verification numbers (CVN) message count for PID 06 ");
				break;
		}
		case 0x6:
		{
				sprintf(tmp,"Calibration Verification Numbers (CVN) ");
				break;
		}
		case 0x7:
		{
				sprintf(tmp,"In-use performance tracking message count for PID 08 and 0B ");
				break;
		}
		case 0x8:
		{
				sprintf(tmp,"In-use performance tracking for spark ignition vehicles ");
				break;
		}
		case 0x9:
		{
				sprintf(tmp,"ECU name message count for PID 0A ");
				break;
		}
		case 0xA:
		{
				sprintf(tmp,"ECU name ");
				break;
		}
		case 0xB:
		{
				sprintf(tmp,"In-use performance tracking for compression ignition vehicles ");
				break;
		}
		default:
		{
			sprintf(tmp,"Unknown request code: 0x%x ",request);
			break;
		}
	}
	writeToLog(tmp);
}

void printError(uint8_t &errorCode)//Same as above, but to SD
{
   char tmp[128]={0};
	switch (errorCode)
    {
    	case RESPONSE_TOO_LONG:
        {
        		sprintf(tmp,"Response too long\n");
        		break;
        }
        case REQUEST_SEQUENCE_ERROR:
        {
        		sprintf(tmp,"Request Sequence error\n");
        		break;
        }
        case NR_FROM_SUBNET_COMPONENT:
        {
        		sprintf(tmp,"No response from sub-net component\n");
        		break;
        }
        case FAILURE_PREVENTS_EXECUTION:
        {
        		sprintf(tmp,"Failure prevents execution of requested action\n");
        		break;
        }
        case UPLOAD_DOWNLOAD_NOT_ACCEPTED:
        {
        		sprintf(tmp,"Upload/Download not accepted\n");
        		break;
        }
        case WRONG_BLOCK_SEQUENCE_COUNTER:
        {
        		sprintf(tmp,"Wrong Block Sequence counter\n");
        		break;
        }
        case SUBFUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION:
        {
        		sprintf(tmp,"Sub-function not supported in active session\n");
        		break;
        }
        case SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION:
        {
        		sprintf(tmp,"Service not supported in active session\n");
        		break;
        }
    	case SERVICE_NOT_SUPPORTED:
        {
        	sprintf(tmp,"Service not supported ");
            break;
        }
        case INVALID_FORMAT:
        {
        	sprintf(tmp,"Sub-Function not supported or Invalid Format ");
            break;
        }
        case CONDITIONS_NOT_MET:
        {
        	sprintf(tmp,"Conditions not met ");
            break;
        }
        case REQUEST_OUT_OF_RANGE:
        {
        	sprintf(tmp,"Request out of range ");
            break;
        }
        case INVALID_KEY:
        {
        	sprintf(tmp,"Invalid Key ");
            break;
        }
        case EXCEEDED_NUMBER_OF_ATTEMPTS:
        {
        	sprintf(tmp,"Exceeded number of attempts ");
            break;
        }
        case TIME_DELAY_NOT_EXPIRED:
        {
        	sprintf(tmp,"Time delay not expired ");
            break;
        }
        case SECURITY_ACCESS_REQUIRED:
        {
        	sprintf(tmp,"Security Access required ");
            break;
        }
        case BUSY_REPEAT_REQUEST:
        {
        	sprintf(tmp,"Busy, repeat request ");
            break;
        }
        case ROUTINE_NOT_COMPLETE:
        {
        	sprintf(tmp,"Routine not complete ");
            break;
        }
        case MESSAGE_LENGTH_OR_FORMAT_INCORRECT:
        {
        	sprintf(tmp,"Message Length or Format incorrect ");
            break;
        }
        case GENERAL_ERROR:
        {
        	sprintf(tmp,"General Error ");
            break;
        }
        case DOWNLOAD_NOT_ACCEPTED:
        {
        	sprintf(tmp,"Download not Accepted ");
            break;
        }
        case IMPROPER_DOWNLOAD_TIME:
        {
        	sprintf(tmp,"Improper download time ");
            break;
        }
        case CANT_DOWNLOAD_TO_SPECIFIED_ADDRESS:
        {
        	sprintf(tmp,"Can't Download to specified address ");
            break;
        }
        case CANT_DOWNLOAD_NUMBER_OF_BYTES_REQUESTED:
        {
        	sprintf(tmp,"Can't Download number of bytes requested ");
            break;
        }
        case UPLOAD_NOT_ACCEPTED:
        {
        	sprintf(tmp,"Upload not accepted ");
            break;
        }
        case IMPROPER_UPLOAD_TYPE:
        {
        	sprintf(tmp,"Improper Upload type ");
            break;
        }
        case CANT_UPLOAD_FROM_SPECIFIED_ADDRESS:
        {
        	sprintf(tmp,"Can't Upload from specified address ");
            break;
        }
        case CANT_UPLOAD_NUMBER_OF_BYTES_REQUESTED:
        {
        	sprintf(tmp,"Can't Upload number of bytes requested ");
            break;
        }
        case TRANSFER_SUSPENDED:
        {
        	sprintf(tmp,"Transfer suspended ");
            break;
        }
        case TRANSFER_ABORTED:
        {
        	sprintf(tmp,"Transfer Aborted ");
            break;
        }
        case ILLEGAL_ADDRESS_IN_BLOCK_TRANSFER:
        {
        	sprintf(tmp,"Illegal address in Block Transfer ");
            break;
        }
        case ILLEGAL_BYTE_COUNT_IN_BLOCK_TRANSFER:
        {
        	sprintf(tmp,"Illegal byte count in block transfer ");
            break;
        }
        case ILLEGAL_BLOCK_TRANSFER_TYPE:
        {
        	sprintf(tmp,"Illegal Block Transfer type ");
            break;
        }
        case BLOCK_TRANSFER_DATA_CHECKSUM_ERROR:
        {
        	sprintf(tmp,"Block Transfer data checksumm error ");
            break;
        }
        case INCORRECT_BYTE_COUNT_DURING_BLOCK_TRANSFER:
        {
        	sprintf(tmp,"Incorrect byte count during block transfer ");
            break;
        }
        case SERVICE_NOT_SUPPORTED_IN_ACTIVE_DIAGNOSTIC_MODE:
        {
        	sprintf(tmp,"Service not supported in active Diagnostics Mode ");
            break;
        }
        case RESPONSE_PENDING:
        {
        	sprintf(tmp,"Response pending ");
            break;
        }
        default:
        {
        	sprintf(tmp,"Unknown error: 0x%x ",errorCode);
            break;
        }
    }
	writeToLog(tmp);
}

void TesterAddressMenu()
{
    device.printf("Tester Address is currently ");
    if(testerMode ==1)
    {
    	device.printf("enabled with mode %d and address 0x%x\n",testerMode,testerAddress);
    }
    else if(testerMode == 2)
    {
    	device.printf("enabled with mode %d\n",testerMode);
    }
    else
    {
    	device.printf("disabled\n");
    }
	device.printf("\nPlease select an option\n");
    if(testerAddress == 0)
    {
    	 device.printf("1)Enable Tester Address\n");
    }
    else
    {
    	device.printf("1)Disable/Change Tester Mode or address\n");
    }
    device.printf("\n");
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
		case '1':
		{
		    if(testerAddress == 0)
		    {
		    	device.printf("Please enter the Tester Mode to use (0 - Disable, 1 - Standard, 2 - Special) \n");
		    	testerMode=inputToHex();
		    	if(testerMode == 1)
		    	{
		    		device.printf("Please enter the Tester Address to use in HEX (typically F1)\n");
		    		testerAddress=inputToHex();
		    	}
		    	else if(testerMode >2)
		    	{
		    		device.printf("Unknown mode %d, tester address has been disabled\n",testerMode);
		    		testerMode=0;
		    		break;
		    	}
		    }
		    else
		    {
		    	device.printf("Please enter the new Tester Mode to use (0 - Disable, 1 - Standard, 2 - Special) \n");
		    	testerMode=inputToHex();
		    	if(testerMode == 1)
		    	{
		    		device.printf("Please enter the Tester Address to use in HEX (typically F1)\n");
		    		testerAddress=inputToHex();
		    	}
		    }
	    	device.printf("\n");
	        device.printf("Tester Address has been ");
	        if(testerMode ==1)
	        {
	        	device.printf("enabled with mode %d and address 0x%x\n",testerMode,testerAddress);
	        }
	        else if(testerMode == 2)
	        {
	        	device.printf("enabled with mode %d\n",testerMode);
	        }
	        else
	        {
	        	device.printf("disabled\n");
	        }
			break;
		}
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }

}

void parsePID(uint8_t &pid)
{
	char tmpoo[265]={0};
	switch (pid)
	{
		case PID_ENGINE_LOAD:
		{
			sprintf(tmpoo,"Engine Load ");
			break;
		}
		case PID_COOLANT_TEMP:
		{
			sprintf(tmpoo,"Coolant Temperature ");
			break;
		}
		case PID_SHORT_TERM_FUEL_TRIM_1:
		{
			sprintf(tmpoo,"Short Term Fuel Trim 1 ");
			break;
		}
		case PID_LONG_TERM_FUEL_TRIM_1:
		{
			sprintf(tmpoo,"Long Term Fuel Trim 1 ");
			break;
		}
		case PID_SHORT_TERM_FUEL_TRIM_2:
		{
			sprintf(tmpoo,"Short Term Fuel Trim 2 ");
			break;
		}
		case PID_LONG_TERM_FUEL_TRIM_2:
		{
			sprintf(tmpoo,"Long Term Fuel Trim 2 ");
			break;
		}
		case PID_FUEL_PRESSURE:
		{
			sprintf(tmpoo,"Fuel Pressure ");
			break;
		}
		case PID_INTAKE_MAP:
		{
			sprintf(tmpoo,"Intake Manifold Absolute Pressure ");
			break;
		}
		case PID_RPM:
		{
			sprintf(tmpoo,"RPM ");
			break;
		}
		case PID_SPEED:
		{
			sprintf(tmpoo,"Speed ");
			break;
		}
		case PID_TIMING_ADVANCE:
		{
			sprintf(tmpoo,"Timing Advance ");
			break;
		}
		case PID_INTAKE_TEMP:
		{
			sprintf(tmpoo,"Intake Temperature ");
			break;
		}
		case PID_MAF_FLOW:
		{
			sprintf(tmpoo,"MAF Flow ");
			break;
		}
		case PID_THROTTLE:
		{
			sprintf(tmpoo,"Throttle ");
			break;
		}
		case PID_AUX_INPUT:
		{
			sprintf(tmpoo,"Aux Input ");
			break;
		}
		case PID_RUNTIME:
		{
			sprintf(tmpoo,"RunTime ");
			break;
		}
		case PID_DISTANCE_WITH_MIL:
		{
			sprintf(tmpoo,"Distance with MIL ");
			break;
		}
		case PID_COMMANDED_EGR:
		{
			sprintf(tmpoo,"Commanded EGR ");
			break;
		}
		case PID_EGR_ERROR:
		{
			sprintf(tmpoo,"EGR Error status ");
			break;
		}
		case PID_COMMANDED_EVAPORATIVE_PURGE:
		{
			sprintf(tmpoo,"Commanded Evaporative Purge ");
			break;
		}
		case PID_FUEL_LEVEL:
		{
			sprintf(tmpoo,"Fuel Level ");
			break;
		}
		case PID_WARMS_UPS:
		{
			sprintf(tmpoo,"Warm Ups ");
			break;
		}
		case PID_DISTANCE:
		{
			sprintf(tmpoo,"Distance traveled since DTCs cleared ");
			break;
		}
		case PID_EVAP_SYS_VAPOR_PRESSURE:
		{
			sprintf(tmpoo,"Evaporative System Vapor Pressure ");
			break;
		}
		case PID_BAROMETRIC:
		{
			sprintf(tmpoo,"Barometric Data ");
			break;
		}
		case PID_CATALYST_TEMP_B1S1:
		{
			sprintf(tmpoo,"Catalyst Temperature B1S1 ");
			break;
		}
		case PID_CATALYST_TEMP_B2S1:
		{
			sprintf(tmpoo,"Catalyst Temperature B2S1 ");
			break;
		}
		case PID_CATALYST_TEMP_B1S2:
		{
			sprintf(tmpoo,"Catalyst Temperature B1S2 ");
			break;
		}
		case PID_CATALYST_TEMP_B2S2:
		{
			sprintf(tmpoo,"Catalyst Temperature B2S2 ");
			break;
		}
		case PID_CONTROL_MODULE_VOLTAGE:
		{
			sprintf(tmpoo,"Control Module Voltage ");
			break;
		}
		case PID_ABSOLUTE_ENGINE_LOAD:
		{
			sprintf(tmpoo,"Absolute Engine Load ");
			break;
		}
		case PID_AIR_FUEL_EQUIV_RATIO:
		{
			sprintf(tmpoo,"Air-Fuel Equivalence Ratio ");
			break;
		}
		case PID_RELATIVE_THROTTLE_POS:
		{
			sprintf(tmpoo,"Relative Throttle Position ");
			break;
		}
		case PID_AMBIENT_TEMP:
		{
			sprintf(tmpoo,"Ambient Temperature ");
			break;
		}
		case PID_ABSOLUTE_THROTTLE_POS_B:
		{
			sprintf(tmpoo,"Absolute Throttle Position B ");
			break;
		}
		case PID_ABSOLUTE_THROTTLE_POS_C:
		{
			sprintf(tmpoo,"Absolute Throttle Position C ");
			break;
		}
		case PID_ACC_PEDAL_POS_D:
		{
			sprintf(tmpoo,"Accelerator Pedal Position D ");
			break;
		}
		case PID_ACC_PEDAL_POS_E:
		{
			sprintf(tmpoo,"Accelerator Pedal Position E ");
			break;
		}
		case PID_ACC_PEDAL_POS_F:
		{
			sprintf(tmpoo,"Accelerator Pedal Position F ");
			break;
		}
		case PID_COMMANDED_THROTTLE_ACTUATOR:
		{
			sprintf(tmpoo,"Commanded Throttle Actuator ");
			break;
		}
		case PID_TIME_WITH_MIL:
		{
			sprintf(tmpoo,"Time With MIL ");
			break;
		}
		case PID_TIME_SINCE_CODES_CLEARED:
		{
			sprintf(tmpoo,"Time Since DTCs Cleared ");
			break;
		}
		case PID_ETHANOL_FUEL:
		{
			sprintf(tmpoo,"Ethanol Fuel Percentage ");
			break;
		}
		case PID_FUEL_RAIL_PRESSURE:
		{
			sprintf(tmpoo,"Fuel Rail Pressure ");
			break;
		}
		case PID_HYBRID_BATTERY_PERCENTAGE:
		{
			sprintf(tmpoo,"Hybrid Battery Percentage ");
			break;
		}
		case PID_ENGINE_OIL_TEMP:
		{
			sprintf(tmpoo,"Engine Oil Temperature ");
			break;
		}
		case PID_FUEL_INJECTION_TIMING:
		{
			sprintf(tmpoo,"Fuel Injection Timing ");
			break;
		}
		case PID_ENGINE_FUEL_RATE:
		{
			sprintf(tmpoo,"Engine Fuel Rate ");
			break;
		}
		case PID_ENGINE_TORQUE_DEMANDED:
		{
			sprintf(tmpoo,"Engine Torque Demanded ");
			break;
		}
		case PID_ENGINE_TORQUE_PERCENTAGE:
		{
			sprintf(tmpoo,"Engine Torque Percentage ");
			break;
		}
		case PID_ENGINE_REF_TORQUE:
		{
			sprintf(tmpoo,"Engine Reference Torque ");
			break;
		}
		case PID_SUPPORTED_PIDS_1_20:
		{
			sprintf(tmpoo,"Supported PIDs (0x01 - 0x20) ");
			break;
		}
		case PID_MONITOR_STATUS:
		{
			sprintf(tmpoo,"Monitor Status ");
			break;
		}
		case PID_FREEZE_DTC:
		{
			sprintf(tmpoo,"Freeze DTCs ");
			break;
		}
		case PID_FUEL_SYSTEM_STATUS:
		{
			sprintf(tmpoo,"Fuel System Status ");
			break;
		}
		case PID_COMMANDED_SECOND_AIR_STAT:
		{
			sprintf(tmpoo,"Commanded Secondary Air Status ");
			break;
		}
		case PID_OXYGEN_SENSORS_PRESENT:
		{
			sprintf(tmpoo,"Oxygen Sensors Present (2 banks) ");
			break;
		}
		case PID_OBD_STANDARD:
		{
			sprintf(tmpoo,"OBD Standards Conformed by vehicle ");
			break;
		}
		case PID_OXYGEN_SENSORS_PRESENT_4:
		{
			sprintf(tmpoo,"Oxygen Sensors Present (4 banks) ");
			break;
		}
		case PID_SUPPORTED_PIDS_21_40:
		{
			sprintf(tmpoo,"Supported PIDs in range 0x21 - 0x40 ");
			break;
		}
		case PID_FUEL_RAIL_PRESSURE2:
		{
			sprintf(tmpoo,"Fuel Rail Pressure ");
			break;
		}
		case PID_FUEL_RAIL_GAUGE_PRESSURE:
		{
			sprintf(tmpoo,"Fuel Rail Gauge Pressure ");
			break;
		}
		case PID_OXYGEN_SENSOR_FA_1:
		{
			sprintf(tmpoo,"Oxygen Sensor 1 Fuel-Air Ratio ");
			break;
		}
		case PID_OXYGEN_SENSOR_FA_2:
		{
			sprintf(tmpoo,"Oxygen Sensor 2 Fuel-Air Ratio ");
			break;
		}
		case PID_OXYGEN_SENSOR_FA_3:
		{
			sprintf(tmpoo,"Oxygen Sensor 3 Fuel-Air Ratio ");
			break;
		}
		case PID_OXYGEN_SENSOR_FA_4:
		{
			sprintf(tmpoo,"Oxygen Sensor 4 Fuel-Air Ratio ");
			break;
		}
		case PID_OXYGEN_SENSOR_FA_5:
		{
			sprintf(tmpoo,"Oxygen Sensor 5 Fuel-Air Ratio ");
			break;
		}
		case PID_OXYGEN_SENSOR_FA_6:
		{
			sprintf(tmpoo,"Oxygen Sensor 6 Fuel-Air Ratio ");
			break;
		}
		case PID_OXYGEN_SENSOR_FA_7:
		{
			sprintf(tmpoo,"Oxygen Sensor 7 Fuel-Air Ratio ");
			break;
		}
		case PID_OXYGEN_SENSOR_FA_8:
		{
			sprintf(tmpoo,"Oxygen Sensor 8 Fuel-Air Ratio ");
			break;
		}
		case PID_SUPPORTED_PIDS_41_60:
		{
			sprintf(tmpoo,"Supported PIDs in range 0x41 - 0x60 ");
			break;
		}
		case PID_MONITOR_STATUS_THIS_DRIVE_CYCLE:
		{
			sprintf(tmpoo,"Monitor Status in this drive cycle ");
			break;
		}
		case PID_FUEL_TYPE:
		{
			sprintf(tmpoo,"Fuel Type ");
			break;
		}
		case PID_RELATIVE_ACC_PEDAL_POSITION:
		{
			sprintf(tmpoo,"Relative Accelerator Pedal Position ");
			break;
		}
		case PID_EMMISION_REQUIREMENTS:
		{
			sprintf(tmpoo,"Emission Requirements ");
			break;
		}
		case PID_SUPPORTED_PIDS_61_80:
		{
			sprintf(tmpoo,"Supported PIDs in range 0x61 - 0x80 ");
			break;
		}
		case PID_ENGINE_PERCENT_TORQUE:
		{
			sprintf(tmpoo,"Engine percent torque data ");
			break;
		}
		case PID_AUXILIARY_IO_SUPPORTED:
		{
			sprintf(tmpoo,"Auxiliary Input/Output supported ");
			break;
		}
		case PID_MAF:
		{
			sprintf(tmpoo,"Mass Air Flow Sensor ");
			break;
		}
		case PID_ENGINE_COOLANT_TEMP:
		{
			sprintf(tmpoo,"Engine Coolant Temp ");
			break;
		}
		case PID_INTAKE_AIR_TEMP_SENSOR:
		{
			sprintf(tmpoo,"Intake Air Temperature Sensor Data ");
			break;
		}
		case PID_COMMANDED_EGR_ERROR:
		{
			sprintf(tmpoo,"Commanded EGR and EGR Errors status ");
			break;
		}
		case PID_EGR_TEMP:
		{
			sprintf(tmpoo,"EGR Temperature ");
			break;
		}
		case PID_TURBO_INLET_PRESSURE:
		{
			sprintf(tmpoo,"Turbo Inlet Pressure ");
			break;
		}
		case PID_BOOST_PRESSURE_CONTROL:
		{
			sprintf(tmpoo,"Boost Pressure Control status ");
			break;
		}
		case PID_VGT_CONTROL:
		{
			sprintf(tmpoo,"Variable Geometry Turbo Control status ");
			break;
		}
		case PID_WASTEGATE_CONTROL:
		{
			sprintf(tmpoo,"Wastegate Control Status ");
			break;
		}
		case PID_EXHAUST_PRESSURE:
		{
			sprintf(tmpoo,"Exhaust Pressure ");
			break;
		}
		case PID_TURBO_RPM:
		{
			sprintf(tmpoo,"Turbo RPM ");
			break;
		}
		case PID_TURBO_TEMP:
		{
			sprintf(tmpoo,"Turbo Temperature ");
			break;
		}
		case PID_DPF_1:
		{
			sprintf(tmpoo,"Diesel Particle Filter 1 Status ");
			break;
		}
		case PID_DPF_2:
		{
			sprintf(tmpoo,"Diesel Particle Filter 2 Status ");
			break;
		}
		case PID_ENGINE_RUNTIME:
		{
			sprintf(tmpoo,"Engine Run time ");
			break;
		}
		case PID_SUPPORTED_PIDS_A1_C0:
		{
			sprintf(tmpoo,"Supported PIDs in range 0xA1 - 0xC0 ");
			break;
		}
		case PID_SUPPORTED_PIDS_C1_E0:
		{
			sprintf(tmpoo,"Supported PIDs in range 0xC1 - 0xE0 ");
			break;
		}
		default:
		{
			sprintf(tmpoo,"unknown PID 0x%x ",pid);
			break;
		}
	}
	writeToLog(tmpoo);
}

void increaseFrameCounter()//just to keep the frame counter within range
{
    frameCounter++;
    if(frameCounter >0xF)
    {
        frameCounter=0;
    }
}

void TPSendCA(uint8_t wat)
{
	if(wat == 0)
	{
		can1.attach(0);
		wait_ms(20);
		timer.stop();
		timer.reset();

	}
	else
	{
		wait_ms(20);
		can1.attach(&TPChannelAlive);
	    timer.start();
	}
}

void TPSendACK(uint8_t ackCounter, uint8_t bus)//ackType depends on if final ack or beginning ack
{
  uint8_t dataa[8]={0};
  dataa[0]=(1 + ackCounter);
  if(dataa[0]>0xF)
  {
      dataa[0]=0;
  }
  dataa[0]=dataa[0] + tpACK;
  sendFrame(dataa,1,bus);
}


bool TPGetACK(uint8_t bus)
{
    uint32_t cnt2=0;
    CANMessage can_msg(0,CANStandard);
    while(1)//wait for the message one second
    {
 	   if(can1.read(can_msg) && can_msg.id == rID)
 	   {
 		   if (can_msg.data[0] == CHANNEL_TEST)//need to expect channel test while doing stuff
 		   {
 				uint8_t data[8]={PARAMETER_REQUEST_OK,0xF,0xCA,0xFF,0x05,0xFF};
 				sendFrame(data,6, bus);
 				cnt2 = 0; //wait until we get a proper reply;
 		   }
 		   else if((can_msg.data[0] & 0xF0) == tpACK)
 		   {
 			   return 1;
 		   }
 	   }
 	   else
 	   {
 		   wait(0.001);
 	   }
 	   cnt2++;
 	   if(cnt2 == 5000)
 	   {
            device.printf("ACK Timeout\n");
            return 0; //timeout
 	   }
    }
}


uint8_t TPRead(uint8_t *payload, uint8_t reportError, uint8_t bus) // returns the payload from a TP2.0 stream
{
   CANMessage can_msg(0,CANStandard);
   uint32_t cnt=0;
   uint32_t cnt2=0;
   rejectReason=0;
   while(1)//wait for the message one second
   {
	   if(can1.read(can_msg))
	   {
		   if (can_msg.id == rID && can_msg.data[0] == CHANNEL_TEST)//need to expect channel test while doing stuff
		   {
				uint8_t data[8]={PARAMETER_REQUEST_OK,0xF,0xCA,0xFF,0x05,0xFF};
				sendFrame(data,6,1);
				cnt2 = 0; //wait until we get a proper reply;
		   }
		   else if(can_msg.id != rID){}//ignore it
		   else if(can_msg.id == rID && can_msg.data[3]==0x7F && can_msg.data[5]==0x78)//if we get a "wait" request
		   {
			   uint8_t ackCounter = (can_msg.data[0]& 0x0F);//Store the counter for ACK
	            if((can_msg.data[0]& 0xF0) == ACK_LAST)//if they want an ack
	            {
	                TPSendACK(ackCounter, bus);
	            }
	            cnt2=0;
		   }
		   else//but if we got a frame from the rID
		   {
			   break;
		   }
	   }
	   wait(0.0001);
	   cnt2++;
	   if(cnt2 == 5000)
	   {
           device.printf("Read Timeout\n");
           return 0; //timeout
	   }
   }
   uint8_t ttype = (can_msg.data[0]& 0xF0);//check for the frame type
   uint8_t ackCounter = (can_msg.data[0]& 0x0F);//Store the counter for ACK
   if(ttype == ACK_LAST)
   {
   		TPSendACK(ackCounter, bus);
   		wait(0.1);//we need this to prevent the interrupt from not sending the ACK
   }
   if(can_msg.data[3]==0x7F)//check for the error
   {
	   if(reportError==1) //normal error reporting
       {
           device.printf("Access to SID %x rejected. ", can_msg.data[4]);
           checkError(can_msg.data[5]);
       }
       else if (reportError==2 && can_msg.data[5] != SERVICE_NOT_SUPPORTED && can_msg.data[5] !=INVALID_FORMAT)
       {
           device.printf("Access to SID %x rejected. ", can_msg.data[4]);
           checkError(can_msg.data[5]);
           return 1; //1 is used to indicate that the parameters that generated the error should be reported too
       }
       rejectReason=1;
       return 0;
   }
   if(ttype == ACK_LAST || ttype == NOACK_LAST) //if single frame
   {       
       for(cnt=0;cnt<can_msg.data[2];cnt++)//grab the data
       {
           payload[cnt]=can_msg.data[cnt+3];
       }
       return can_msg.data[2];//return the length
   }
   else if (ttype == ACK_FOLLOW || ttype == NOACK_FOLLOW)
   {
       uint8_t lnn=can_msg.data[2];//get the length for returning it later
       for (cnt=0;cnt<5;cnt++)//grab the last 5 bytes
       {
           payload[cnt]=can_msg.data[(cnt+3)];
       }
       if((can_msg.data[0] & 0xF0) == ACK_FOLLOW)
       {
           ackCounter = (can_msg.data[0]& 0x0F);//Store the counter for ACK
           TPSendACK(ackCounter, bus);
       }
       while((can_msg.data[0] & 0xF0) != ACK_LAST && (can_msg.data[0] & 0xF0) != NOACK_LAST)
       {
    	   cnt2=0;
    	   while(1)//wait for the message one second
    	   {
    		   if(can1.read(can_msg))
    		   {
    			   if (can_msg.id == rID && can_msg.data[0] == CHANNEL_TEST)//need to expect channel test while doing stuff
    			   {
    					uint8_t data[8]={PARAMETER_REQUEST_OK,0xF,0xCA,0xFF,0x05,0xFF};
    					sendFrame(data,6,1);
    					cnt2 = 0; //wait until we get a proper reply;
    			   }
    			   else if(can_msg.id != rID){}//ignore it
    			   else if(can_msg.id == rID && can_msg.data[3]==0x7F && can_msg.data[5]==0x78)//if we get a "wait" request
    			   {
    				   uint8_t ackCounter = (can_msg.data[0]& 0x0F);//Store the counter for ACK
    		            if((can_msg.data[0]& 0xF0) == ACK_LAST)//if they want an ack
    		            {
    		                TPSendACK(ackCounter, bus);
    		            }
    		            cnt2=0;
    			   }
    			   else//but if we got a frame from the rID
    			   {
    				   break;
    			   }
    		   }
    		   wait(0.0001);
    		   cnt2++;
    		   if(cnt2 == 5000)
    		   {
    	           device.printf("Transmission Timeout\n");
    	           return 0; //timeout
    		   }
    	   }
           if((can_msg.data[0] & 0xF0) == ACK_LAST || (can_msg.data[0] & 0xF0) == NOACK_LAST)
           {
               uint8_t finalcount=(lnn - cnt);
               for (uint8_t cnt1=0;cnt1<finalcount;cnt1++)//grab the last 7 bytes
               {
                   payload[cnt]=can_msg.data[(cnt1 + 1)];
                   cnt++; //dont forget to increase the array counter!
               }
               if((can_msg.data[0] & 0xF0) == ACK_LAST)//send an ACK if requested
               {
                  ackCounter = (can_msg.data[0]& 0x0F);//Store the counter for ACK
                  TPSendACK(ackCounter, bus);
                  wait(0.1);//we need this to prevent the interrupt from not sending the ACK
               }
               return lnn;
           }
           else
           {
               for (uint8_t cnt1=1;cnt1<8;cnt1++)//grab the last 7 bytes
               {
                  payload[cnt]=can_msg.data[cnt1];
                  cnt++; //dont forget to increase the array counter!
               }
               if((can_msg.data[0] & 0xF0) == ACK_FOLLOW)
               {
                  ackCounter = (can_msg.data[0]& 0x0F);//Store the counter for ACK
                  TPSendACK(ackCounter, bus);
               }
           }
       }
    }
    return 0; //should never get here
}
   
//Need to add tester address
bool TPWrite(uint8_t *payload, uint8_t lnn, uint8_t doack, uint8_t bus) //sends a payload. lnn is the length, and doack is 0 for no ack and 1 for ack
{
    uint8_t rqtype;
    if ((lnn < 6 && testerAddress == 0) || (lnn < 5 && testerAddress != 0))//if its a single frame
    {
        if(doack==0)
        {
           rqtype = NOACK_LAST;
        }
        else
        {
            rqtype = ACK_LAST;
        }
        uint8_t data[8]={(rqtype + frameCounter),0,0,0,0,0,0,0};
        if(testerAddress != 0)//if we need to use the tester address
        {
        	data[2] = (lnn + 1);
        	data[3] = payload[0]; //fill already the first byte
        	data[4] = testerAddress;//add the tester address
            for(uint8_t a=1;a<lnn;a++)//and fill in the rest
            {
            	data[(a + 4)]=payload[a];
            }
            sendFrame(data,(lnn+3),bus); //send the frame

        }
        else
        {
			data[2] = lnn;
        	for(uint8_t a=0;a<lnn;a++)
			{
				data[(a + 3)]=payload[a];
			}
			sendFrame(data,(lnn+3),bus); //send the frame
        }
        increaseFrameCounter(); //increase the counter
        if(doack==1)
        {
            if (!TPGetACK(bus))
            {
                return 0;
            }
            else
            {
                return 1;
            }
        }
        else
        {
            return 1;
        }
    }
    else
    {
        uint8_t cnt=0;
        uint8_t sendcnt=0; //used to store sent frames for expecting ack.
        rqtype=NOACK_FOLLOW;
        while(cnt<lnn)
        {
            if (cnt == 0)
            {
                uint8_t data[8]={(rqtype + frameCounter),0,0,0,0,0,0,0};
                if(testerAddress != 0)//if we need to use the tester address
                {
                	data[2] = (lnn + 1);
                	data[3] = payload[0]; //fill already the first byte
                	data[4] = testerAddress;//add the tester address
                    for(uint8_t a=1;a<lnn;a++)//and fill in the rest
                    {
                    	data[(a + 4)]=payload[a];
                    }
                    sendFrame(data,8,bus); //send the frame
                    cnt = cnt+4;

                }
                else
                {
        			data[2] = lnn;
                	for(uint8_t a=0;a<lnn;a++)
        			{
        				data[(a + 3)]=payload[a];
        			}
        			sendFrame(data,8,bus); //send the frame
        			cnt = cnt+5;
                }
                increaseFrameCounter(); //increase the counter
                sendcnt++;
            }
            else if((lnn-cnt) < 8) //last frame
            {
                if(doack==1)
                {
                    rqtype = ACK_LAST;
                }
                uint8_t data[8]={(rqtype + frameCounter),payload[cnt],payload[cnt+1],payload[cnt+2],payload[cnt+3],payload[cnt+4],payload[cnt+5],payload[cnt+6]}; //componse the single frame
                increaseFrameCounter(); //increase the counter
                sendFrame(data,(1+(lnn-cnt)),1); //send the frame
                if(doack==1)
                {
                    if (!TPGetACK(bus))
                    {
                        return 0;
                    }
                    else
                    {
                        return 1;
                    }
                }
                else
                {
                    return 1;
                }
                
            }
            else //middle frame
            {
                uint8_t data[8]={(rqtype + frameCounter),payload[cnt],payload[cnt+1],payload[cnt+2],payload[cnt+3],payload[cnt+4],payload[cnt+5],payload[cnt+6]}; //componse the single frame
                increaseFrameCounter(); //increase the counter
                sendFrame(data,8,1); //send the frame
                cnt = cnt+7;
                sendcnt++;
                if (sendcnt == 0xF && doack == 1)
                {
                    if (!TPGetACK(bus))
                    {
                        return 0;
                    }
                    sendcnt=0;
                }
            }
        }
    }
    return 0; //should never get here
}

uint8_t UDSRead(uint8_t *response, uint8_t reportError,uint8_t bus)
{
	   tick.detach();//remove the TesterPresent thingy
	   CANMessage can_msg(0,CANStandard);
	   uint32_t cnt=0;//counter for timeout
	   uint8_t ln=0xFF;//for storing the payload length
	   uint8_t cnt2=0; //to know how much data is left
	   rejectReason=0;
	   uint8_t skipTester=0;//used to indicate if the first byte should be skipped because tester
	   while(cnt < 10000 && ln>cnt2)//1 second timeout and wait until data is received
	   {
	       if(bus == 1)
	       {
			   while(!can1.read(can_msg)  || (can_msg.id != rID && cnt <10000))
				{
					wait(0.0001);
					cnt++;
				}
	       }
	       else
	       {
			   while(!can2.read(can_msg)  || (can_msg.id != rID && cnt <10000))
				{
					wait(0.0001);
					cnt++;
				}
	       }
	        if(cnt==10000)
	        {
	            if(reportError==1)
	            {
	            	device.printf("Read Timeout...\n");
	            }
	            rejectReason=0;
	            return 0;
	        }
	        cnt=0;//reset the counter
			if((can_msg.data[0] != 0x10 && can_msg.data[0] != 0x20 && can_msg.data[0] != 0x30 && can_msg.data[0] > 7) && (can_msg.data[1] == 0x10 || can_msg.data[1] == 0x20 || can_msg.data[1] == 0x30 || can_msg.data[1] <= 6) && can_msg.data[0] == (ownID & 0xFF))//if they are potentially using the first byte as the target address
			{
				skipTester=1;
			}
	        if((can_msg.data[(0 + skipTester)] & 0xF0) == 0)//Single frame event
	        {
	           if (can_msg.data[(1 + skipTester)] == 0x7F && can_msg.data[(3 + skipTester)] != 0x78)//if we get a rejection, we should know why!
	           {
	               if(reportError==1)
	               {
	                   device.printf("Access to SID %x rejected. ", can_msg.data[(2 + skipTester)]);
	                   checkError(can_msg.data[(3  + skipTester)]);
	               }
	               else if (reportError==2 && can_msg.data[(3 + skipTester)] != SERVICE_NOT_SUPPORTED && can_msg.data[(3 + skipTester)] !=INVALID_FORMAT && can_msg.data[(3 + skipTester)] != REQUEST_OUT_OF_RANGE)
	               //else if (reportError==2 && can_msg.data[3] != SERVICE_NOT_SUPPORTED && can_msg.data[3] != REQUEST_OUT_OF_RANGE)
	               {
	                   device.printf("Access to SID %x rejected. ", can_msg.data[(2 + skipTester)]);
	                   checkError(can_msg.data[(3 + skipTester)]);
	                   rejectReason=1;
	               }
	               else
	               {
	            	   rejectReason=1;
	               }
	               return 0;
	           }
	           else if (can_msg.data[(1 + skipTester)] == 0x7F && can_msg.data[(3 + skipTester)] == 0x78)
	           {
	               //do nothing, as we need to wait for response
	           }
	           else
	           {
	                for(uint8_t a=0;a<can_msg.data[(0 + skipTester)];a++)//this returns the payload without the positive SID byte!!
	                {
	                    if(testerMode == 0 && skipTester == 0)//if no tester address
	                    {
	                    	response[a]=can_msg.data[(a + 2)];
	                    }
	                    else//otherwise, we need to skip the tester address too
	                    {
	                    	response[a]=can_msg.data[(a + 3)];
	                    }
	                }
	                if(testerMode == 1 && can_msg.data[2] == testerAddress)
	                {
	                	return (can_msg.data[0] - 2);
	                }
	                else
	                {
	                	return (can_msg.data[(0 + skipTester)] - 1);
	                }
	           }
	        }
	        else if(can_msg.data[(0 + skipTester)] == 0x10)//Start of multiframe with ACK request. I like turtles btw.
	        {
	            if(testerMode == 0 || testerMode == 2)//if we are not using testerAddress OR we are using mode 2 for tester
	            {
	            	ln=(can_msg.data[(1 + skipTester)] - 1);//remove 1 byte from SID
		            for(uint8_t a=(3  + skipTester);a<8;a++)
		            {
		                response[cnt2]=can_msg.data[a];
		                cnt2++;
		            }
	            }
	            else
	            {
	            	ln=(can_msg.data[1] - 2);//we also need to remove the tester address part
		            for(uint8_t a=4;a<8;a++)
		            {
		                response[cnt2]=can_msg.data[a];
		                cnt2++;
		            }
	            }
	            uint8_t tmp[8]={0};//we send an ack
	            if(testerMode == 2)//if using mode 2, we add the target address on first byte
	            {
	            	tmp[0]=(rID & 0xFF);
	            	tmp[1]=0x30;
	            }
	            else
	            {
	            	tmp[0]=0x30;
	            }
	            sendFrame(tmp,8,1);
	        }
	        else if ((can_msg.data[(0 + skipTester)] & 0xF0) == 0x20)//Continuation of multiframe
	        {
	            if((ln-cnt2)>(6 - skipTester))
	            {
	                for(uint8_t a=(1  + skipTester);a<8;a++)
	                {
	                    response[cnt2]=can_msg.data[a];
	                    cnt2++;
	                }
	            }
	            else
	            {
	                uint8_t tmp=(ln-cnt2);
	                for(uint8_t a=(1 + skipTester);a<(tmp+1);a++)
	                {
	                    response[cnt2]=can_msg.data[a];
	                    cnt2++;
	                }
	                return ln;
	            }
	        }
	        else
	        {
	            return 0; //not a valid UDS frame
	        }
	    }
	    device.printf("\n\n");
	    return ln;
}

bool UDSWrite(uint8_t *rqst, uint8_t len,uint8_t bus)
{
	uint8_t data[8]={0};//need to initialize all to zero except length as per protocol specifications
	CANMessage can_msg(0,CANStandard);
	uint32_t cnt=0;//counter for array
	uint8_t UDSframeCounter=0;//counter for frames
	uint8_t skipTester=0;
	if(testerMode == 2)
	{
		skipTester=1;
	}
	if((len < 8 && testerMode == 0) || (len < 7 && testerMode != 0))
	{
		if(testerMode == 0)//if no tester address is being used
		{
			data[0]=len;//on single frame, the length is stored on the first byte
			for (uint8_t cnt2=0;cnt2<len;cnt2++)//then grab the rest of the payload straight away
			{
				data[(cnt2+1)]=rqst[cnt2];
			}
		}
		else
		{
			if(testerMode == 1)
			{
				data[0]=(len + 1);//Need to add the testerAddress byte to the payload
				data[1]=rqst[0];//and lets grab the first byte of the request
				data[2]=testerAddress;//and lets also grab the tester address part
				for (uint8_t cnt2=1;cnt2<len;cnt2++)//then grab the rest of the payload straight away
				{
					data[(cnt2+2)]=rqst[cnt2];
				}
			}
			else
			{
				data[0]=(rID & 0xFF);//ad the remote address on the first byte
				data[1]=len;//add the length
				for (uint8_t cnt2=0;cnt2<len;cnt2++)//then grab the rest of the payload straight away
				{
					data[(cnt2+2)]=rqst[cnt2];
				}
			}
		}
		if(bus == 1)
		{
			uint8_t timeout=0;
			while (!can1.write(CANMessage(ownID, reinterpret_cast<char*>(&data), 8)) && timeout < 10)
			{
				timeout++;
			}//make sure the msg goes out
		}
		else
		{
			uint8_t timeout=0;
			while (!can2.write(CANMessage(ownID, reinterpret_cast<char*>(&data), 8)) && timeout < 10)
			{
				timeout++;
			}//make sure the msg goes out
		}
	}
	else
	{
		if(testerMode == 0)
		{
			data[0]=0x10;//first byte is always 10 for multiframe, which is also an ACK request
			data[1]=len;//second byte should be the payload length
			for (cnt=0;cnt<6;cnt++)
			{
				data[(cnt+2)]=rqst[cnt];
			}
		}
		else
		{
			if(testerMode == 1)
			{
				data[0]=0x10;//first byte is always 10 for multiframe, which is also an ACK request
				data[1]=(len + 1);//second byte should be the payload length plus one for tester address
				data[2]=rqst[0];
				data[3]=testerAddress;
				for (cnt=1;cnt<5;cnt++)
				{
					data[(cnt+3)]=rqst[cnt];
				}
			}
			else
			{
				data[0]=(rID & 0xFF);
				data[1]=0x10;//first byte is always 10 for multiframe, which is also an ACK request
				data[2]=(len + 1);//second byte should be the payload length plus one for tester address
				for (cnt=0;cnt<5;cnt++)
				{
					data[(cnt+3)]=rqst[cnt];
				}
			}
		}
		UDSframeCounter++;//increase the frame counter
		if(bus == 1)
		{
			uint8_t timeout=0;
			while (!can1.write(CANMessage(ownID, reinterpret_cast<char*>(&data), 8)) && timeout < 10)
			{
				timeout++;
			}//make sure the msg goes out
		}
		else
		{
			uint8_t timeout=0;
			while (!can2.write(CANMessage(ownID, reinterpret_cast<char*>(&data), 8)) && timeout < 10)
			{
				timeout++;
			}//make sure the msg goes out
		}
		uint32_t cntt=0;
		if(bus == 1)
		{
			while(!can1.read(can_msg) || (can_msg.id != rID && cntt < 10000))
			{
				wait(0.0001);
				cntt++;
			}//wait for the ACK
		}
		else
		{
			while(!can2.read(can_msg) || (can_msg.id != rID && cntt < 10000))
			{
				wait(0.0001);
				cntt++;
			}//wait for the ACK
		}
		if(can_msg.data[(0 + skipTester)] != 0x30 || cntt == 10000)//if its not an ACK or timeout
		{
			return 0;
		}
		while (cnt<len)
		{
			memset(data,0,8);//clear the frame
			if(testerMode != 2)
			{
				data[0]=(0x20 + UDSframeCounter);
				if ((len-cnt)>6)//if its the full payload, just parse the whole thing
				{
					for(uint8_t a=1;a<8;a++)
					{
						data[a]=rqst[cnt];
						cnt++;
					}
				}
				else
				{
					uint8_t remainingLen=len-cnt;
					for(uint8_t a=0;a<remainingLen;a++)
					{
						data[a+1]=rqst[cnt];
						cnt++;
					}
				}
			}
			else
			{
				data[0] = (rID & 0xFF);
				data[1]=(0x20 + UDSframeCounter);
				if ((len-cnt)>5)//if its the full payload, just parse the whole thing
				{
					for(uint8_t a=2;a<8;a++)
					{
						data[a]=rqst[cnt];
						cnt++;
					}
				}
				else
				{
					uint8_t remainingLen=len-cnt;
					for(uint8_t a=0;a<remainingLen;a++)
					{
						data[a+2]=rqst[cnt];
						cnt++;
					}
				}
			}
			if(bus == 1)
			{
				uint8_t timeout=0;
				while (!can1.write(CANMessage(ownID, reinterpret_cast<char*>(&data), 8)) && timeout < 10)
				{
					timeout++;
				}//make sure the msg goes out
			}
			else
			{
				uint8_t timeout=0;
				while (!can2.write(CANMessage(ownID, reinterpret_cast<char*>(&data), 8)) && timeout < 10)
				{
					timeout++;
				}//make sure the msg goes out
			}
			UDSframeCounter++;
			if(UDSframeCounter>0x0F)//don't forget to bring a towel! (and reset the counter too)
			{
				UDSframeCounter=0;
			}
		}
	}
	return 1;
}
  
uint8_t UDSResponseHandler(uint8_t *rqst, uint8_t len, uint8_t *response, uint8_t reportError, uint8_t bus)//this shit is pretty complex. Used only for DOWNLOAD
{
   tick.detach();
   UDSWrite(rqst, len, bus);
   return UDSRead(response ,reportError, bus);
}


bool UDSChannelSetup(uint8_t SessionType)//the input here is the CAN ID of the module.
{
  device.printf("\nAttempting to establish a communication channel via UDS using ID 0x%x with remote ID 0x%x...\n\n",ownID,rID);
  uint8_t data[8]={0x10,SessionType,0,0,0,0};
  uint8_t buffer[256];
  uint8_t len=UDSResponseHandler(data,2,buffer,1,1);
  if (len !=0)
  {
      device.printf("UDS Session was established correctly for ID %x\n", SessionType);
      inSession=1;
      protocol=1;
      return 1;
  }
  device.printf("UDS session failed\n");
  return 0;
}

void UDSChannelSetupScan(uint32_t target_ID)//the input here is the CAN ID of the module.
{
  led1 = !led1;
  uint8_t data[8]={0x02,0x10,0x01,0,0,0,0,0};//standard diag session
  if(testerMode == 1)
  {
	  data[0] = 3;
	  data[2] = 0xF1;
	  data[3] = 0x01;
  }
  if(testerMode == 2)
  {
	  data[0]=0xF1;
	  data[1]=0x2;
	  data[2]=0x10;
	  data[3]=0x1;

  }
  uint8_t timeout=0;
  while (!can1.write(CANMessage(target_ID, reinterpret_cast<char*>(&data), 8)) && !device.readable() && timeout < 10)
  {
	  timeout++;
  }//make sure the msg goes out
  CANMessage can_msg(0,CANStandard);
  //below lines are gipsy, need to pass them to the handler
  uint32_t cnt=0;
  while(cnt<2000)//lets wait a bit for a reply before timeout, but not wait for too long to speed things up
  {
    while(!can1.read(can_msg) && cnt<2000)
    {
        wait(0.0001);
        cnt++;
    }
    if(can_msg.data[2] == 0x1 && can_msg.data[1]==0x50 && cnt<2000)
    {
        device.printf("UDS session established using ID 0x%x with reply from 0x%x\n",target_ID,can_msg.id);
        wait(6);//wait to timeout the session and go for the next one.
        led1 = !led1;
        return;
    }
    else if(can_msg.data[2] == 0x10 && can_msg.data[1]==0x7F && cnt<2000)
    {
    	device.printf("Got a reply using ID 0x%x from ID 0x%x:\n",target_ID,can_msg.id);
    	checkError(can_msg.data[3]);
    	device.printf("\n");
    }
    else
    {
    	cnt++;//count frames as well for timeout
    }
  }
  led1 = !led1;
  return;
}


bool TPChannelSetup(const uint32_t requestAddress, uint8_t target_ID) //the input here is the logical address of the module. the ECU would be 0x01, for example
{
  device.printf("Attempting to establish a communication channel via TP 2.0 with target 0x%x using Base Addres 0x%x...\n",target_ID, requestAddress);
  uint8_t data[7]={target_ID,SETUP_CHANNEL,0x00,0x7,0x00,0x03,0x01};
  uint8_t timeout=0;
  while (!can1.write(CANMessage(requestAddress, reinterpret_cast<char*>(&data), 7)) && timeout < 10)
  {
	  timeout++;
  }//make sure the msg goes out
  CANMessage can_msg(0,CANStandard);
  uint32_t cnt=0;
  rID=0; //reset the ID for the loop to work
  while(1)
  {
      if(can1.read(can_msg))
      {
    	  if(can_msg.id == (requestAddress + target_ID))
    	  {
    		  break;
    	  }
      }
      cnt++;
      if(cnt>5000)
      {
    	  device.printf("Channel Setup Timeout\n");
    	  resetCAN();
    	  return 0;
      }
      wait(0.0001);
  }
  if(can_msg.data[1] != 0xD0)
  {
    device.printf("Error 0x%x\n", can_msg.data[1]);
    resetCAN();
    return 0;
  }
  else
  {
      rID= can_msg.data[3];
      rID=rID<<8;
      rID=rID+can_msg.data[2];
      ownID=can_msg.data[5];
      ownID=ownID<<8;
      ownID=ownID+can_msg.data[4];
  }
  //now we exchange timing parameters
  uint8_t data1[6]={PARAMETERS_REQUEST,0xF,0xCA,0xFF,0x05,0xFF};
  timeout=0;
  while (!can1.write(CANMessage(ownID, reinterpret_cast<char*>(&data1), 6)) && timeout < 10)
  {
	  timeout++;
  }//make sure the msg goes out
  cnt=0;
  while(1)
  {
      if(can1.read(can_msg))
      {
    	  if(can_msg.id == rID)
    	  {
    		  break;
    	  }
      }
      cnt++;
      if(cnt>5000)
      {
    	  device.printf("Channel Parameters Timeout\n");
    	  return 0;
      }
      wait(0.0001);
  }
  if(can_msg.data[0] != 0xA1)
  {
    device.printf("Error 0x%x\n", can_msg.data[1]);
    resetCAN();
    return 0; 
  }
  inSession=1;
  protocol=0;
  frameCounter=0;
  device.printf("\nTP2.0 Channel established!\n");
  return 1;
}

void TPChannelSetupScan(const uint32_t requestAddress, uint8_t target_ID) //the input here is the logical address of the module. the ECU would be 0x01, for example
{
  led1 = !led1;
  uint8_t data[7]={target_ID,SETUP_CHANNEL,0x00,0x7,0x00,0x03,0x01};
  uint8_t timeout=0;
  while (!can1.write(CANMessage(requestAddress, reinterpret_cast<char*>(&data), 7)) && timeout < 10)
  {
	  timeout++;
  }//make sure the msg goes out
  CANMessage can_msg(0,CANStandard);
  uint32_t cnt=0;
  uint8_t done=0;
  while (cnt<200 && done==0)
  {
    while(!can1.read(can_msg) && cnt < 200)
    {
        wait(0.001);
        cnt++;    
    }
    if(can_msg.id==(requestAddress + target_ID))
    {
      if(can_msg.data[1] != 0xD0)
      {
        return;   
      }
      else
      {
        rID= can_msg.data[3];
        rID=rID<<8;
        rID=rID+can_msg.data[2];
        ownID=can_msg.data[5];
        ownID=ownID<<8;
        ownID=ownID+can_msg.data[4];
        done=1;
      }
      
    }
  }
  if(cnt>=200)
  {
      led1 = !led1;
      return;
  }
  //now we exchange timing parameters
  while(can1.read(can_msg)){}//lets empty the buffer first
  uint8_t data1[6]={PARAMETERS_REQUEST,0xF,0xCA,0xFF,0x05,0xFF};
  timeout=0;
  while (!can1.write(CANMessage(ownID, reinterpret_cast<char*>(&data1), 6)) && timeout < 10)
  {
	  timeout++;
  }//make sure the msg goes out
  cnt=0;
  while (can_msg.id != rID && cnt<200)
  { 
      while(!can1.read(can_msg))
      {
         wait(0.001);
         cnt++; 
      }//wait for reply
  }
  if(cnt>=200 || can_msg.data[0] != 0xA1)
  {
      led1 = !led1;
      return;
  }
  device.printf("TP2.0 session established with ID %x\n",target_ID);
  data1[0]=CHANNEL_DISCONNECT;
  timeout=0;
  while (!can1.write(CANMessage(ownID, reinterpret_cast<char*>(&data1), 1)) && timeout < 10)
  {
	  timeout++;
  }//make sure the msg goes out
  led1 = !led1;
  return;
}

uint8_t TPResponseHandler(uint8_t *input, uint8_t lnn, uint8_t *output, uint8_t reportErrors, uint8_t bus)//Handles all requests to TP
{
   TPSendCA(0); //remove the tick to properly handle replies
   if(TPWrite(input,lnn,1,bus))
   {
	   uint8_t poop = TPRead(output,reportErrors,bus);
	   TPSendCA(1); //send the TesterPresent packet every half second if we are idle but in a session
	   return poop;
   }
   else
   {
	   return 0;
   }
} 

void TPScan(const uint32_t requestAddress)
{
    device.printf("Starting the TP2.0 scanner...\n");
    for(uint32_t a=0;a<0x100;a++)
    {
        TPChannelSetupScan(requestAddress, a);
        if(device.readable())
        {
        	device.printf("Aborted by user\n");
        	dumpSerial();
        	return;
        }
    }
    device.printf("Done!\n");
}   

void UDSScan(uint32_t start, uint32_t finish)
{
    device.printf("Starting the UDS scanner for range 0x%x to 0x%x\n",start,finish);
    uint32_t sstart=start;
    uint32_t ffinish=finish;
    if(start ==0)
    {
    	device.printf("Start cannot be 0, setting it to 1\n");
    	sstart=1;
    }
    if(ffinish > 0x7FF)
    {
    	device.printf("Finish cannot be bigger than 0x7FF, setting it to 0x7FF\n");
    	ffinish=0x7FF;
    }
    device.printf("\n");
    uint32_t cnt=0x100;//Will be used to give some verbosity on the progress
    for(uint32_t a=sstart;a<ffinish;a++)
    {
        if(device.readable())
        {
        	device.printf("Scan aborted by user\n");
        	dumpSerial();
        	break;
        }
    	UDSChannelSetupScan(a);
        if(a==cnt)
        {
            cnt=cnt+0x100;
            device.printf("Now scanning ID %x\n",a);
        }
    }
    device.printf("Done!\n");
}

void detectCANSpeed(uint8_t busno)
{
    device.printf("Attempting to detect CAN speed...\n");
    uint8_t cnt=0;//to set CAN speed
    uint32_t cnt2=0;//for timeout
    for(uint8_t a=0;a<5;a++)
    {
        uint32_t speed;
    	switch (cnt)
        {
		   case 0:
		   {
			   speed=125000;
			   break;
		   }
		   case 1:
		   {
			   speed=250000;
			   break;
		   }
		   case 2:
		   {
			   speed=500000;
			   break;
		   }
		   case 3:
		   {
			   speed=800000;
			   break;
		   }
		   case 4:
		   {
			   speed=1000000;
				break;
		   }
        }
    	CANMessage can_msg(0,CANStandard);
    	if(busno == 1)
    	{
    		can1.frequency(speed);
    		while(can1.read(can_msg)){}//just to clear the buffer
            while(!can1.read(can_msg) || (can_msg.id == 0  && cnt2<100))
            {
                wait(0.01);
                cnt2++;
            }
    	}
    	else
    	{
    		can2.frequency(speed);
    		while(can2.read(can_msg)){}//just to clear the buffer
            while(!can2.read(can_msg) || (can_msg.id == 0  && cnt2<100))
            {
                wait(0.01);
                cnt2++;
            }
    	}
        if(cnt2<100)
        {
           device.printf("Detected CAN%d speed at ", busno);
           switch (cnt)
            {
                case 0:
                {
                    device.printf("125Kbps");
                    break;
                }
                case 1:
                {
                    device.printf("250Kbps");
                    break;
                }
                case 2:
                {
                    device.printf("500Kbps");
                    break;
                }
                case 3:
                {
                    device.printf("800Kbps");
                    break;
                }
                case 4:
                {
                    device.printf("1Mbps");
                    break;
                }
            }
            device.printf(", speed has been adjusted to this value\n");
            return;
        }
        cnt2=0;//reset the delay counter
        cnt++;
    }
    device.printf("Speed is not Standard or there is no traffic on this bus\nSpeed has been reset to 500Kbps\n");
	if(busno == 1)
	{
		can1.frequency(500000);
	}
	else
	{
		can2.frequency(500000);
	}
    return;
    
}   

    
void setCANSpeed()
{
    device.printf("Please, select a bus to set the speed (1 or 2)\n\n");
    uint8_t busno = inputToDec();
    if(busno != 1 && busno != 2)
    {
    	 device.printf("CAN%d is not a valid number for a bus. Seriously, it isnt.\n\n", busno);
    	 return;
    }
    device.printf("Please, select the speed to be set for CAN%d\n\n");
	device.printf("1)125KBPS\n");
    device.printf("2)250KBPS\n");
    device.printf("3)500KBPS\n");
    device.printf("4)800KBPS\n");
    device.printf("5)1MBPS\n");
    device.printf("6)Custom\n\n");
    while(!device.readable()){}
    char option=device.getc();
    uint32_t speed;
    dumpSerial();
    switch (option)
    {
        case '1':
        {
        	speed=125000;
            break;
        }
        case '2':
        {
        	speed=250000;
            break;
        }
        case '3':
        {
        	speed=500000;
            break;
        }
        case '4':
        {
        	speed=800000;
            break;
        }
        case '5':
        {
        	speed=1000000;
            break;
        }
        case '6':
        {
        	device.printf("Please enter the desired speed for CAN%d in bps (125000, 250000...)\n\n", busno);
        	speed=inputToDec();
        	break;
        }
        default:
        {
            device.printf("Unknown option!\n");
            return;
        }
    }  
	device.printf("Setting CAN%d speed to %d BPS...\n", busno, speed);
	bool worked=0;
	if(busno == 1)
	{
		worked = can1.frequency(speed);
	}
	else
	{
		worked = can2.frequency(speed);
	}
	if(worked)
	{
		device.printf("Done!\n\n");
	}
	else
	{
		device.printf("Failed setting speed\n\n");
	}
}

void UDSProtocolScanFromLog(char *filename)//more tricky, yet less complex
{
    setLED(LED_ORANGE);
	uint32_t IDs[256]={0};//dont expect to find more than 256 IDS in a standard environment
    uint32_t ID=0;
    uint8_t len=0;
    uint8_t data[8]={0};
    uint32_t IDCount=0;//to keep track of the stored IDs
    file2 = sd.open(filename, O_RDONLY);
    uint32_t time1=0;
    while(1)//we will go thru the whole log
    {
        if (!logToRawFrame(&time1,&ID,&len,data))
        {
        	break;
        }
        uint8_t filtered=0;
        for (uint32_t a=0;a<IDCount;a++)//make sure the ID we got is not filtered
        {
          if(ID == IDs[a])
          {
              filtered=1;
          }
        }
        if(filtered==0)//if its not a filtered ID. Should also add to "filtered IDS" the ones that are checked not to be UDS compliant
        {
            if(ID==0x7DF)//if we just grab a channel broadcast used in diag
            {
                IDs[IDCount]=ID;
                IDCount++;
            	uint8_t request[2]={data[1], data[2]};//most likely its gonna be an SID request, therefor no need to handle multiple frames here
                uint8_t cnt=0;
                uint8_t cnt2=0;//to know how many IDS were found replying to broadcast
                while(cnt<20)//lets check 20 frames
                {
                    if (!logToRawFrame(&time1,&ID,&len,data))
                    {
                    	break;
                    }
                    bool toCheck=1;
                    for (uint32_t a=0;a<IDCount;a++)//make sure the ID we got is not filtered
                    {
                      if(ID == IDs[a])
                      {
                          toCheck = 0;
                      }
                    }
                    if((data[0] & 0xF0) == 0x00 && data[1] == (request[0] + 0x40) && data[2] == request[1] && toCheck)
                    {
                    	IDs[IDCount]=ID;
                    	IDCount++;
                    	cnt2++;
                    }
                    else if(((data[0] & 0xF0) == 0x10 ||  (data[0] & 0xF0) == 0x20) && data[2] == (request[0] + 0x40) && data[3] == request[1] && toCheck)
                    {
                    	IDs[IDCount]=ID;
                    	IDCount++;
                    	cnt2++;
                    }
                    cnt++;
                }
                device.printf("Found %d UDS IDs replying to broadcast requests (0x7FD):\n",cnt2);
                for(uint8_t a=0;a<cnt2;a++)
                {
                	device.printf("-0x%x\n",IDs[(IDCount - (cnt2 - a))]);
                }
                device.printf("\n");
                file2->lseek(0,SEEK_SET);//rewind the file

            }
            else if ((data[0] & 0xF0) == 0 || (data[0] & 0xF0) == 0x10 || (data[0] & 0xF0) == 0x20 || (data[0] & 0xF0) == 0x30)//try to see if we get a valid UDS frame
            {
                bool valid=0;
            	if((data[0] & 0xF0) == 0)//if we get a single frame stream
                {
                	uint8_t howLong = (data[0] & 0x0F);
                	if(len == (howLong + 1) && data[1] != 0xC0)//avoid channel negotiations
                	{
                		valid=1;
                	}
                	else if(len > howLong && howLong < 8 && howLong > 0 )
                	{
                		uint8_t a;
                		for(a = howLong; a < len; a++)
                		{
                			if(data[a] != 0x0 || data[a] != 0xAA || data[a] != 0x55)//maybe length is more, but should be using one of these for extra bytes
                			{
                				break;
                			}
                		}
                		if(a == len)
                		{
                			valid = 1;
                		}
                	}
                }
            	else if((data[0] & 0xF0) == 0x10)//ACK Request
            	{
            		if(len == 8 && data[0] == 0x10)//length must be 8 for multiframe
            		{
            			uint32_t tmpID=ID;//save the original ID
            			for(uint8_t p=0;p<100;p++)//we should find an ACK close if it is really UDS
            			{
            		        if (!logToRawFrame(&time1,&ID,&len,data))
            		        {
            		        	break;
            		        }
            		        if((data[0] & 0xF0) == 0x30)//if we get an ACK
            		        {
                        		if(len == 1)
                        		{
                        			valid=1;
                        			break;
                        		}
                        		else
                        		{
                        			uint8_t a;
                        			for(a = 1; a < len; a++)
                            		{
                            			if(data[a] != 0x0 || data[a] != 0xAA || data[a] != 0x55)//maybe length is more, but should be using one of these for extra bytes
                            			{
                            				break;
                            			}
                            		}
                            		if(a == len)
                            		{
                            			valid = 1;
                            			break;
                            		}
                        		}
            		        }
            			}
            			if(valid)
            			{
            				device.printf("Found UDS channel established between IDs 0x%x and 0x%x\n",tmpID, ID);
                           	IDs[IDCount]=ID;
                           	IDs[(IDCount + 1)]=tmpID;
                            IDCount = (IDCount + 2);
                            file2->lseek(0,SEEK_SET);//rewind the file
                            valid=0;
            			}
            		}
            	}
            	else if((data[0] & 0xF0) == 0x20)//if we got a multiframe
            	{
            		uint8_t frcnt = (data[0] & 0x0F);//grab the frame counter
            		uint32_t tmpID = ID;//grab the target ID
            		for(uint8_t a=0;a<20;a++)//would never expect a following frame to be more than 20 frames away
            		{
        		        if (!logToRawFrame(&time1,&ID,&len,data))
        		        {
        		        	break;
        		        }
        		        if(ID == tmpID && ((data[0] & 0xF0) == (frcnt + 1) || (data[0] & 0xF0) == (frcnt + 2)))
        		        {
        		        	valid=1;
        		        	break;
        		        }
            		}

            	}
            	else if((data[0] & 0xF0) == 0x30)//if we got a potential ACK
            	{
            		if(len == 1)
            		{
            			valid=1;
            		}
            		else
            		{
            			uint8_t a;
            			for(a = 1; a < len; a++)
                		{
                			if(data[a] != 0x0 || data[a] != 0xAA || data[a] != 0x55)//maybe length is more, but should be using one of these for extra bytes
                			{
                				break;
                			}
                		}
                		if(a == len)
                		{
                			valid = 1;
                		}
            		}
            	}
            	if(valid)
            	{
            		device.printf("Found ID 0x%x using UDS protocol\n",ID);
            		IDs[IDCount]=ID;
            		IDCount++;
            		file2->lseek(0,SEEK_SET);//rewind the file
            	}

            }
            else //if it doesnt look like an UDS stream, add it to filtered list
            {
            	IDs[IDCount]=ID;
            	IDCount++;
            }
        }
    }
    if(IDCount ==0)
    {
    	device.printf("No UDS Sessions found\n\n");
    }
    file2->close();
    setLED(LED_GREEN);
}

void TPProtocolScanFromLog(char *filename)
{
    setLED(LED_ORANGE);
	uint32_t IDs[256]={0};//neither expect to find more than 256 IDS in a standard environment
	uint8_t fNegs[256]={0};
    uint32_t ID=0;
    uint8_t len=0;
    uint8_t data[8]={0};
    uint32_t cnt2=0;
    uint32_t IDCount=0;//to keep track of the stored IDs
    file2 = sd.open(filename, O_RDONLY);
    uint32_t time1=0;
    while(1)//we will go thru the whole log
    {
        if (!logToRawFrame(&time1,&ID,&len,data))
        {
        	break;
        }
    	cnt2++;
        uint8_t filtered=0;
        for (uint32_t a=0;a<IDCount;a++)//make sure the ID we got is not filtered
        {
          if(ID == IDs[a])
          {
              filtered=1;
          }
        }
        if(filtered==0)//if its not a filtered ID. Should also add to "filtered IDS" the ones that are checked not to be TP 2.0 compliant
        {
            if(ID==0x200)//if we just grab a potential channel negotiation
            {
                bool wasThisDoneBefore=0;
                uint8_t gotValue=0xFF;
                uint16_t cnt3=0;
                while(gotValue !=0 && cnt3 < 0x100)//now, lets make sure the Module ID we got was not used before
                {
                	gotValue= fNegs[cnt3];
                	if(gotValue == data[0])//if it was used
                	{
                		wasThisDoneBefore=1;
                		break;
                	}
                	else if(gotValue == 0)//or if we get to the end of the data in the array
                	{
                		fNegs[cnt3] = data[0];//add it
                		break;
                	}
                	cnt3++;
                }
            	if(data[1]==0xC0 && !wasThisDoneBefore)
                {

            		uint8_t target = data[0];//target module
                    uint8_t appType = data[6];//app type
                    uint32_t tID = 0;//target ID (server)
                    uint32_t lID = 0;//local ID (tester)
                    uint32_t cnt=0;
                    while(ID != (0x200 + target) && cnt <50)
                    {
                    	if(!logToRawFrame(&time1,&ID,&len,data))
                    	{
                    		break;
                    	}
                        if (ID == (0x200 + target) && data[6] == appType && data[1] == 0xD0)//make really sure it is the right one
                        {
                            tID=data[3];
                            tID=(tID<<8);
                            tID=tID + data[2];
                            lID=data[5];
                            lID=(lID<<8);
                            lID=lID + data[4];
                            device.printf("TP 2.0 channel setup captured with following params:\n-Server ID: 0x%x\n-Requested App: 0x%x\n-Server Channel ID: 0x%x\n-Tester Channel ID: 0x%x\n\n",target,appType,tID,lID);
                            IDs[IDCount]=tID;//Store the used IDs to exclude them from the scan.
                            IDs[(IDCount + 1)]= lID;
                            IDCount = (IDCount + 2);
                            if(IDCount > 254)
                            {
                            	device.printf("ID limit reached for TP2.0, returning\n");
                              	return;
                            }
                            file2->lseek(0,SEEK_SET);//rewind the file
                        }
                    }
                }
            }
            else if ((data[0] & 0xF0) == ACK && len == 1)//detecting an ACK is a straight forward way of detecting a protocol
            {
                uint32_t ID1=ID;//grab this ID
                uint8_t done=0;//to signal that the task is done
                uint32_t cnt=0;
                while(done==0 && cnt<100)
                {
                	if(!logToRawFrame(&time1,&ID,&len,data))
                	{
                		break;
                	}
                    filtered=0;//reset filter
                    for (uint32_t a=0;a<IDCount;a++)//make sure the ID we got is not filtered
                    {
                        if(ID == IDs[a])
                        {
                            filtered=1;
                        }
                    }
                    if(filtered ==0 && ID != ID1)//now make sure we are not grabbing the same ID
                    {
                        uint8_t toCheck=(data[0] & 0xF0);
                        if(toCheck == ACK_LAST || toCheck == ACK_FOLLOW)
                        {
                            if(toCheck == ACK_LAST)//if its an ACK request, we should expect an ACK soon from the first ID we grabbed
                            {
                                cnt=0;
                                while((data[0] & 0xF0) != ACK && cnt<100 && len != 1)//an ACK should not take more than 1 second
                                {
                                	if(!logToRawFrame(&time1,&ID,&len,data))
                                	{
                                		break;
                                	}
                                	cnt++;
                                    if ((data[0] & 0xF0) == ACK && cnt<100 && len == 1 && ID != ID1)//make extra sure it is the correct one to avoid false positives
                                    {
                                        file2->lseek(0,SEEK_SET);//rewind the file to determine who is Server
                                        uint32_t ID2 = ID;
                                        ID = 0;
                                        while(ID !=ID1 && ID !=ID2)
                                        {
                                        	if(!logToRawFrame(&time1,&ID,&len,data))
                                        	{
                                        		break;
                                        	}
                                        	if(ID == ID2)
                                        	{
                                                IDs[IDCount]=ID2;//Store the used IDs to exclude them from the scan.
                                                IDs[(IDCount + 1)]= ID1;
                                                IDCount = (IDCount + 2);
                                        	}
                                        	else if(ID == ID1)
                                        	{
                                                IDs[IDCount]=ID1;//Store the used IDs to exclude them from the scan.
                                                IDs[(IDCount + 1)]= ID2;
                                                IDCount = (IDCount + 2);

                                        	}
                                        }
                                        device.printf("Found already established TP 2.0 channel between CAN IDs 0x%x (Server) and 0x%x (Tool)\n\n",ID1, ID2);
                                        done=1;
                                        file2->lseek(0,SEEK_SET);//rewind the file
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if(IDCount ==0)
    {
    	device.printf("No TP2.0 Sessions found\n\n");
    }
    file2->close();
    setLED(LED_GREEN);
}

void TPProtocolScan()
{
    setLED(LED_ORANGE);
	uint32_t IDs[512];
    uint32_t cnt=0;
    uint32_t cnt2=0;
    uint32_t IDCount=0;//to keep track of the stored IDs
    CANMessage can_msg(0,CANStandard);
    while(cnt2<10000 && counter.read() == 0)//we will grab 10k frames
    {
        while(!can1.read(can_msg) && cnt<10000)
        {
            wait(0.0001); 
            cnt++;    
        }
        if (cnt==10000)//timeout
        {
            device.printf("No CAN traffic is currently detected\n");
            setLED(LED_GREEN);
            return;
        }
        cnt=0;
        cnt2++;
        uint8_t filtered=0;
        for (uint32_t a=0;a<IDCount;a++)//make sure the ID we got is not filtered
        {
          if(can_msg.id == IDs[a])
          {
              filtered=1;
          }  
        }
        if(filtered==0)//if its not a filtered ID. Should also add to "filtered IDS" the ones that are checked not to be TP 2.0 compliant
        {
            if(can_msg.id==0x200)
            {
                if(can_msg.data[1]==0xC0)//if we just grab a potential channel negotiation
                {
                    uint8_t target = can_msg.data[0];//target module
                    uint8_t appType = can_msg.data[6];//app type
                    uint32_t tID = 0;//target ID (server)
                    uint32_t lID = 0;//local ID (tester)
                    cnt=0;
                    while(can_msg.id != (0x200 + target) && cnt <10000)
                    {
                        while(!can1.read(can_msg) && cnt<10000)
                        {
                            wait(0.0001); 
                            cnt++;    
                        }
                        if (can_msg.id == (0x200 + target) && can_msg.data[6] == appType)//make really sure it is the right one
                        {
                            if (can_msg.data[1] == 0xD0)
                            {
                                filtered=0;
                                tID=can_msg.data[3];
                                tID=tID<<8;
                                tID=tID + can_msg.data[2];
                                lID=can_msg.data[5];
                                lID=lID<<8;
                                lID=lID + can_msg.data[4];
                                for (uint32_t a=0;a<IDCount;a++)//make sure the IDs we got are not filtered
                                {
                                  if(lID == IDs[a] || tID == IDs[a])//this is not correct since a same ID could establish different sessions during scann. need to fix it
                                  {
                                      filtered=1;
                                  }  
                                }
                                if(filtered==0)
                                {
                                    device.printf("TP 2.0 channel setup captured with following params:\n-Server ID: 0x%x\n-Requested App: 0x%x\n-Server Channel ID: 0x%x\n-Tester Channel ID: 0x%x\n\n",target,appType,tID,lID);
                                    IDs[IDCount]=tID;//Store the used IDs to exclude them from the scan.
                                    IDs[(IDCount + 1)]= lID;
                                    IDCount = (IDCount + 2);
                                }
                            }
                        }
                    }
                }   
            }
            else if ((can_msg.data[0] & 0xF0) == ACK && can_msg.len == 1)//detecting an ACK is a straight forward way of detecting a protocol
            {
                uint32_t ID1=can_msg.id;//grab this ID
                uint8_t done=0;//to signal that the task is done
                while(done==0 && cnt<10000)
                {
                    while(!can1.read(can_msg) && cnt<10000)
                    {
                        wait(0.0001); 
                        cnt++;    
                    }
                    filtered=0;//reset filter
                    for (uint32_t a=0;a<IDCount;a++)//make sure the ID we got is not filtered
                    {
                        if(can_msg.id == IDs[a])
                        {
                            filtered=1;
                        }  
                    }
                    if(filtered ==0 && can_msg.id != ID1)//now make sure we are not grabbing the same ID
                    {
                        uint8_t toCheck=(can_msg.data[0] & 0xF0);
                        if(toCheck == ACK_LAST || toCheck == ACK_FOLLOW || toCheck == NOACK_LAST || toCheck == NOACK_FOLLOW)
                        {
                            if(toCheck == ACK_LAST)//if its an ACK request, we should expect an ACK soon from the first ID we grabbed
                            {
                                cnt=0;
                                while((can_msg.data[0] & 0xF0) != ACK && cnt<10000 && can_msg.len != 1)//an ACK should not take more than 1 second
                                {
                                    while(!can1.read(can_msg) && cnt<10000)
                                    {
                                        wait(0.0001); 
                                        cnt++;    
                                    }
                                }
                                if ((can_msg.data[0] & 0xF0) == ACK && cnt<10000 && can_msg.len == 1 && can_msg.id != ID1)//make extra sure it is the correct one to avoid false positives
                                {
                                    device.printf("Found already established TP 2.0 channel between CAN IDs 0x%x and 0x%x\n\n",ID1, can_msg.id);
                                    IDs[IDCount]=ID1;//Store the used IDs to exclude them from the scan.
                                    IDs[(IDCount + 1)]= can_msg.id;
                                    IDCount = (IDCount + 2);
                                }
                                done=1;   
                            }
                        }
                    }
                }
            }
        }           
                 
    }
    if(IDCount ==0)
    {
    	device.printf("No TP 2.0 Sessions found\n\n");
    }
    setLED(LED_GREEN);
    if(counter.read() >0)
    {
    	counter.reset();
    }
}

void UDSProtocolScan()
{
    setLED(LED_ORANGE);
	uint32_t IDs[256]={0};//dont expect to find more than 256 IDS in a standard environment
    uint32_t IDCount=0;//to keep track of the stored IDs
    CANMessage can_msg(0,CANStandard);
    uint32_t frameCount=0;//we will only grab 1000 frames
    uint32_t cnt = 0;
    while(frameCount < 10000 && counter.read() == 0)
    {
        while(!can1.read(can_msg) && cnt<10000)
        {
            wait(0.0001);
            cnt++;
        }
        if (cnt==10000)//timeout
        {
            device.printf("No CAN traffic is currently detected\n");
            setLED(LED_GREEN);
            return;
        }
        cnt=0;
        frameCount++;
        uint8_t filtered=0;
        for (uint32_t a=0;a<IDCount;a++)//make sure the ID we got is not filtered
        {
          if(can_msg.id == IDs[a])
          {
              filtered=1;
          }
        }
        if(filtered==0)//if its not a filtered ID. Should also add to "filtered IDS" the ones that are checked not to be UDS compliant
        {
            if(can_msg.id==0x7DF)//if we just grab a channel broadcast used in diag
            {
                IDs[IDCount]=can_msg.id;
                IDCount++;
            	uint8_t request[2]={can_msg.data[1], can_msg.data[2]};//most likely its gonna be an SID request, therefor no need to handle multiple frames here
                uint8_t cnt=0;
                uint8_t cnt2=0;//to know how many IDS were found replying to broadcast
                while(cnt<20)//lets check 20 frames
                {
                    while(!can1.read(can_msg) && cnt<10000)
                    {
                        wait(0.0001);
                        cnt++;
                    }
                    if (cnt==10000)//timeout
                    {
                        device.printf("No CAN traffic is currently detected\n");
                        setLED(LED_GREEN);
                        return;
                    }
                    cnt=0;
                    bool toCheck=1;
                    for (uint32_t a=0;a<IDCount;a++)//make sure the ID we got is not filtered
                    {
                      if(can_msg.id == IDs[a])
                      {
                          toCheck = 0;
                      }
                    }
                    if((can_msg.data[0] & 0xF0) == 0x00 && can_msg.data[1] == (request[0] + 0x40) && can_msg.data[2] == request[1] && toCheck)
                    {
                    	IDs[IDCount]=can_msg.id;
                    	IDCount++;
                    	cnt2++;
                    }
                    else if(((can_msg.data[0] & 0xF0) == 0x10 ||  (can_msg.data[0] & 0xF0) == 0x20) && can_msg.data[2] == (request[0] + 0x40) && can_msg.data[3] == request[1] && toCheck)
                    {
                    	IDs[IDCount]=can_msg.id;
                    	IDCount++;
                    	cnt2++;
                    }
                    cnt++;
                }
                device.printf("Found %d UDS IDs replying to broadcast requests (0x7FD):\n",cnt2);
                for(uint8_t a=0;a<cnt2;a++)
                {
                	device.printf("-0x%x\n",IDs[(IDCount - (cnt2 - a))]);
                }
                device.printf("\n");

            }
            else if ((can_msg.data[0] & 0xF0) == 0 || (can_msg.data[0] & 0xF0) == 0x10 || (can_msg.data[0] & 0xF0) == 0x20 || (can_msg.data[0] & 0xF0) == 0x30)//try to see if we get a valid UDS frame
            {
                bool valid=0;
            	if((can_msg.data[0] & 0xF0) == 0)//if we get a single frame stream
                {
                	uint8_t howLong = (can_msg.data[0] & 0x0F);
                	if(can_msg.len == (howLong + 1) && can_msg.data[1] != 0xC0)//avoid channel negotiations
                	{
                		valid=1;
                	}
                	else if(can_msg.len > howLong && howLong < 8 && howLong > 0 )
                	{
                		uint8_t a;
                		for(a = howLong; a < can_msg.len; a++)
                		{
                			if(can_msg.data[a] != 0x0 || can_msg.data[a] != 0xAA || can_msg.data[a] != 0x55)//maybe length is more, but should be using one of these for extra bytes
                			{
                				break;
                			}
                		}
                		if(a == can_msg.len)
                		{
                			valid = 1;
                		}
                	}
                }
            	else if((can_msg.data[0] & 0xF0) == 0x10)//ACK Request
            	{
            		if(can_msg.len == 8 && can_msg.data[0] == 0x10)//length must be 8 for multiframe
            		{
            			uint32_t tmpID=can_msg.id;//save the original ID
            			for(uint8_t p=0;p<100;p++)//we should find an ACK close if it is really UDS
            			{
            		        while(!can1.read(can_msg) && cnt<10000)
            		        {
            		            wait(0.0001);
            		            cnt++;
            		        }
            		        if (cnt==10000)//timeout
            		        {
            		            device.printf("No more CAN traffic is currently detected\n");
            		            setLED(LED_GREEN);
            		            return;
            		        }
            		        cnt=0;
            		        if((can_msg.data[0] & 0xF0) == 0x30)//if we get an ACK
            		        {
                        		if(can_msg.len == 1)
                        		{
                        			valid=1;
                        			break;
                        		}
                        		else
                        		{
                        			uint8_t a;
                        			for(a = 1; a < can_msg.len; a++)
                            		{
                            			if(can_msg.data[a] != 0x0 || can_msg.data[a] != 0xAA || can_msg.data[a] != 0x55)//maybe length is more, but should be using one of these for extra bytes
                            			{
                            				break;
                            			}
                            		}
                            		if(a == can_msg.len)
                            		{
                            			valid = 1;
                            			break;
                            		}
                        		}
            		        }
            			}
            			if(valid)
            			{
            				device.printf("Found UDS channel established between IDs 0x%x and 0x%x\n",tmpID, can_msg.id);
                           	IDs[IDCount]=can_msg.id;
                           	IDs[(IDCount + 1)]=tmpID;
                            IDCount = (IDCount + 2);
                            valid=0;
            			}
            		}
            	}
            	else if((can_msg.data[0] & 0xF0) == 0x20)//if we got a multiframe
            	{
            		uint8_t frcnt = (can_msg.data[0] & 0x0F);//grab the frame counter
            		uint32_t tmpID = can_msg.id;//grab the target ID
            		for(uint8_t a=0;a<20;a++)//would never expect a following frame to be more than 20 frames away
            		{
            	        while(!can1.read(can_msg) && cnt<10000)
            	        {
            	            wait(0.0001);
            	            cnt++;
            	        }
            	        if (cnt==10000)//timeout
            	        {
            	            device.printf("No more CAN traffic is currently detected\n");
            	            setLED(LED_GREEN);
            	            return;
            	        }
            	        cnt=0;
        		        if(can_msg.id == tmpID && ((can_msg.data[0] & 0xF0) == (frcnt + 1) || (can_msg.data[0] & 0xF0) == (frcnt + 2)))
        		        {
        		        	valid=1;
        		        	break;
        		        }
            		}

            	}
            	else if((can_msg.data[0] & 0xF0) == 0x30)//if we got a potential ACK
            	{
            		if(can_msg.len == 1)
            		{
            			valid=1;
            		}
            		else
            		{
            			uint8_t a;
            			for(a = 1; a < can_msg.len; a++)
                		{
                			if(can_msg.data[a] != 0x0 || can_msg.data[a] != 0xAA || can_msg.data[a] != 0x55)//maybe length is more, but should be using one of these for extra bytes
                			{
                				break;
                			}
                		}
                		if(a == can_msg.len)
                		{
                			valid = 1;
                		}
            		}
            	}
            	if(valid)
            	{
            		device.printf("Found ID 0x%x MIGHT be using UDS protocol\n",can_msg.id);
            		IDs[IDCount]=can_msg.id;
            		IDCount++;
            	}

            }
            else //if it doesnt look like an UDS stream, add it to filtered list
            {
            	IDs[IDCount]=can_msg.id;
            	IDCount++;
            }
        }
    }
    if(IDCount ==0)
    {
    	device.printf("No UDS Sessions found\n\n");
    }
    setLED(LED_GREEN);
    if(counter.read() >0)
    {
    	counter.reset();
    }
}

void protocolScan()
{
  device.printf("\nScanning for TP 2.0..\n\n");
  TPProtocolScan();
  device.printf("\nScanning for UDS..\n\n");
  UDSProtocolScan();
  device.printf("\n");
  device.printf("Scan finished!\n\n");
}

void parseUDSSID(uint8_t *data)
{
    uint8_t iLikeTurtles=0;// used to know if multi or single frame
    uint8_t iUseTesterAddressWhenIDontNeedTo=0;// used to properly address the use of tester address
    char tmp[1024]={0};
    char tmpbuf[265]={0};
    if(((data[0] & 0xF0) == 0 && (data[2] == 0xF1 || data[2] == testerAddress)) || (((data[0] & 0xF0) == 0x10 || (data[0] & 0xF0) == 0x20) && (data[3]== 0xF1 || data[3] == testerAddress))) //so, if the tester address is present
    {
    	file->write("(Using Tester address) ", (sizeof("(Using Tester address) ") - 1));
    	iUseTesterAddressWhenIDontNeedTo=1;
    }
	if((data[0] & 0xF0) != 0)//need to check if its a single or multi frame
    {
    	iLikeTurtles=1;
    }
	switch(data[(1+iLikeTurtles)])
    {
        case ERROR:
        {
        	sprintf(tmp,"Error processing SID 0x%x. Cause: ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	writeToLog(tmp);
            printError(data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            return;
        }
        case READ_ECU_ID://readECUIdentification
        {
        	sprintf(tmp,"(KWP2000)ECU Information request for parameter 0x%x ",data[4]);
            break;
        }
        case REQUEST_FREEZE_FRAME:
        {
        	sprintf(tmp,"Request for Freeze Frame: ");
        	writeToLog(tmp);
        	parsePID(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	sprintf(tmp,"\nRequested Frame: 0x%x ",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	break;
        }
        case REQUEST_STORED_DTCS:
        {
        	sprintf(tmp,"Request for Stored DTCs ");
        	break;
        }
        case REQUEST_CLEAR_DTCS:
        {
        	sprintf(tmp,"Request to Clear Stored DTCs ");
        	break;
        }
        case REQUEST_PENDING_DTCS:
        {
        	sprintf(tmp,"Request for Pending DTCs ");
        	break;
        }
        case REQUEST_PERMANENT_DTCS:
        {
        	sprintf(tmp,"Request for Permanent DTCs ");
        	break;
        }
        case REQUEST_POWERTRAIN_DIAG_DATA:
        {
        	sprintf(tmp,"Request Powertrain Diagnostics data: ");
        	writeToLog(tmp);
        	parsePID(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	return;
        }
        case REQUEST_VEHICLE_INFORMATION:
        {
        	sprintf(tmp,"Request Vehicle Information: ");
        	writeToLog(tmp);
        	parseVehicleInfoRequest(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	return;
        }
        case COMMUNICATION_CONTROL:
        {
        	sprintf(tmpbuf,"Routine Control: ");
        	strngcat (tmp,tmpbuf);
        	sprintf(tmp,"Communication Control request for ");
        	switch (data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
        	{
        		case 0x01:
        		{
        			sprintf(tmpbuf,"Normal Communication messages: ");
        			break;
        		}
        		case 0x02:
        		{
        			sprintf(tmpbuf,"Network Management Communication messages: ");
        		    break;
        		}
        		case 0x03:
        		{
        			sprintf(tmpbuf,"Network Management and Normal Communication messages: ");
        		    break;
        		}
        		default:
        		{
        			sprintf(tmpbuf,"Unknown Communication Type 0x%x : ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        			break;
        		}
        	}
        	strngcat (tmp,tmpbuf);
        	switch (data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
        	{
        		case 0x01:
        		{
        			sprintf(tmpbuf,"Enable RX and TX ");
        			break;
        		}
        		case 0x02:
        		{
        			sprintf(tmpbuf,"Enable RX and disable TX ");
        		    break;
        		}
        		case 0x03:
        		{
        			sprintf(tmpbuf,"Disable RX and enable TX ");
        		    break;
        		}
        		case 0x04:
        		{
        			sprintf(tmpbuf,"Disable RX and TX ");
        		    break;
        		}
        		default:
        		{
        			sprintf(tmpbuf,"Unknown Control Type 0x%x ",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        			break;
        		}
        	}
        	strngcat (tmp,tmpbuf);
        	break;
        }
        case CONTROL_DTC_SETTINGS:
        {
        	sprintf(tmpbuf,"Routine Control: Control DTC Setting request to turn DTC ");
        	strngcat (tmp,tmpbuf);
        	switch (data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
        	{
        		case 0x01:
        		{
        			sprintf(tmpbuf,"On ");
        			break;
        		}
        		case 0x02:
        		{
        			sprintf(tmpbuf,"Off ");
        		    break;
        		}
        		default:
        		{
        			sprintf(tmpbuf,"to Unknown Status 0x%x : ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        			break;
        		}
        	}
        	strngcat (tmp,tmpbuf);
        	break;
        }
        case ACCESS_TIMING_PARAMETERS:
        {
        	sprintf(tmp,"Request to access timing parameters");
            break;
        }
        case READ_DTC_BY_STATUS:
        {
            uint32_t dtcGroup = data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
            dtcGroup= ((dtcGroup<<8) + data[(4+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            sprintf(tmp,"Read DTC by Status request for DTCs with status 0x%x on group 0x%x ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)],dtcGroup);
            break;
        }
        case DIAGNOSTIC_SESSION_CONTROL: //startDiagnosticsSession
        {
        	sprintf(tmpbuf,"Request to Start Diagnostics Session with parameter ");
        	strngcat (tmp,tmpbuf);
            switch(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
            {
                case UDS_DEFAULT_SESSION:
                {
                	sprintf(tmpbuf,"Default Session");
                    break;
                }
                case UDS_EXTENDED_SESSION:
                {
                	sprintf(tmpbuf,"Extended Session");
                    break;
                }
                case UDS_PROGRAMMING_SESSION:
                {
                	sprintf(tmpbuf,"Programming Session");
                    break;
                }
                default:
                {
                	sprintf(tmpbuf,"0x%x ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                    break;
                }
            }
            strngcat(tmp,tmpbuf);
            break;
        }
        case CLEAR_DTCS:
        {
            uint16_t tmp1=data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
            tmp1=(tmp1<<8)+data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
            sprintf(tmp,"Request to Clear Diagnostics Information for group 0x%x",tmp1);
            break;
        }
        case SECURITY_ACCESS:
        {
            if(data[0] < 5)
            {
            	sprintf(tmp,"Security Access request with parameters ");
                for(uint8_t a=0;a<(data[(0+iLikeTurtles)] - (1+iUseTesterAddressWhenIDontNeedTo));a++)
                {
                	sprintf(tmpbuf,"0x%x ",data[(2+iLikeTurtles+a+iUseTesterAddressWhenIDontNeedTo)]);
                	strngcat(tmp,tmpbuf);
                }
            }
            else
            {
            	sprintf(tmp,"Security Access calculated key for level 0x%x: ",(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] - 1));
                for(uint8_t a=0;a<(data[(0+iLikeTurtles)] - (2 + iUseTesterAddressWhenIDontNeedTo));a++)
                {
                	sprintf(tmpbuf,"0x%x ",data[(3+a+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                	strngcat(tmp,tmpbuf);
                }
            }
            break;
        }
        case TESTER_PRESENT:
        {
        	sprintf(tmp,"Tester Present ");
            break;
        }
        case READ_DATA_BY_ID://readECUIdentification
        {
        	sprintf(tmp,"ECU Data request for ID 0x%x ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            break;
        }
        case READ_DATA_BY_LOCAL_ID:
        {
        	sprintf(tmp,"Request to Read Data by ID 0x%x ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            break;
        }
        case WRITE_DATA_BY_ID:
        {
        	sprintf(tmp,"Request to Write Data by ID 0x%x and payload:\n -HEX: ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	for(uint8_t a=0;a<(data[(0+iLikeTurtles)] - (1+iUseTesterAddressWhenIDontNeedTo));a++)
        	{
        		sprintf(tmpbuf,"0x%x ",data[(3+iLikeTurtles+a+iUseTesterAddressWhenIDontNeedTo)]);
        		strngcat(tmp,tmpbuf);
        	}
            break;
        }
        case ECU_RESET:
        {
        	sprintf(tmp,"ECU Reset -> ");
        	switch(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
        	{
				case HARD_RESET:
				{
					sprintf(tmpbuf,"Hard Reset ");
					break;
				}
				case IGNITION_RESET:
				{
					sprintf(tmpbuf,"Ignition Reset ");
					break;
				}
				case SOFT_RESET:
				{
					sprintf(tmpbuf,"Soft Reset ");
					break;
				}
				case ENABLE_RAPID_POWER_SHUTDOWN:
				{
					sprintf(tmpbuf,"Enable Rapid Power Shutdown ");
					break;
				}
				case DISABLE_RAPID_POWER_SHUTDOWN:
				{
					sprintf(tmpbuf,"Disable Rapid Power Shutdown ");
					break;
				}
				default:
				{
					sprintf(tmpbuf,"Unknown reset type 0x%x",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
					break;
				}
        	}
        	strngcat(tmp,tmpbuf);
            break;
        }
        case READ_MEMORY_BY_ADDRESS: //Having issues here with tester mode 2, need to fix it.
        {
        	uint8_t a=data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
        	uint32_t offset=0;
        	uint8_t b=(a & 0xF);
        	for (uint8_t c=0;c<b;c++)
        	{
        		offset=(offset<<8);
        		offset=(offset + data[((3+c)+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	}
        	uint8_t d=((a & 0xF0) >> 4);
        	uint32_t ln=0;
        	for (uint8_t c=0;c<d;c++)
        	{
        		ln=(ln<<8);
        		ln=(ln + data[((3+c+b)+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	}
        	sprintf(tmp,"Read Memory by Address request for offset 0x%x and length 0x%x",offset,ln);
            break;
        }
        case WRITE_MEMORY_BY_ADDRESS:
        {

            break;
        }
        case IO_CONTROL_BY_LOCAL_ID:
        {

            break;
        }
        case ROUTINE_CONTROL:
        {
        	sprintf(tmp,"Routine Control: Start routine ");
            switch (data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
			{
            	case START_ROUTINE:
            	{
            		if(data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] == 2 && data[(4+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] == 3)
            		{
            			sprintf(tmpbuf,"Check Programming Preconditions\n");
            		}
            		else if(data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] == 2 && data[(4+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] == 2) //erase memory
            		{
            			sprintf(tmpbuf,"Check Memory\n");
            		}
            		else if(data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] == 0xFF && data[(4+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] == 0)
            		{
            			sprintf(tmpbuf,"Erase Memory\n");
            		}
            		else
            		{
						sprintf(tmpbuf," with following parameters:\n-Routine ID: 0x%x\n",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            		}
					strngcat (tmp,tmpbuf);
					sprintf(tmpbuf,"-Routine parameters: ");
					strngcat (tmp,tmpbuf);
					uint8_t cnt=(data[(0+iLikeTurtles)] - (3+iUseTesterAddressWhenIDontNeedTo));//remove the header
					for(uint8_t a=0;a<cnt;a++)
					{
						sprintf(tmpbuf,"0x%x ",data[(a+4+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
						strngcat (tmp,tmpbuf);
					}
            		break;
            	}
            	case STOP_ROUTINE:
            	{
            		sprintf(tmpbuf,"Stop routine with ID 0x%x",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            		strngcat (tmp,tmpbuf);
            		break;
            	}
            	case REQUEST_ROUTINE_RESULT:
            	{
            		sprintf(tmpbuf,"Request Routine results for ID 0x%x",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            		strngcat (tmp,tmpbuf);
            		break;
            	}
			}
            break;
        }
        case REQUEST_DOWNLOAD:
        {
        	uint8_t a=data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
        	uint32_t offset=0;
        	uint8_t b=(a & 0xF);
        	for (uint8_t c=0;c<b;c++)
        	{
        	    offset=(offset<<8);
        	    offset=(offset + data[((4+c)+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	}
        	uint8_t d=((a & 0xF0) >> 4);
        	uint32_t ln=0;
        	for (uint8_t c=0;c<d;c++)
        	{
        	    ln=(ln<<8);
        	    ln=(ln + data[((4+c+b)+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	}
        	sprintf(tmp,"Download Request to address 0x%x using Encryption method 0x%x, Compression method 0x%x and length of 0x%x bytes",offset,(data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] & 0x0F),((data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] & 0xF0)>>4) ,ln);
            break;
        }
        case REQUEST_UPLOAD:
        {
        	uint8_t a=data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
        	uint32_t offset=0;
        	uint8_t b=(a & 0xF);
        	for (uint8_t c=0;c<b;c++)
        	{
        	    offset=(offset<<8);
        	    offset=(offset + data[((4+c)+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	}
        	uint8_t d=((a & 0xF0) >> 4);
        	uint32_t ln=0;
        	for (uint8_t c=0;c<d;c++)
        	{
        	    ln=(ln<<8);
        	    ln=(ln + data[((4+c+b)+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	}
        	sprintf(tmp,"Upload Request from address 0x%x using Encryption method 0x%x, Compression method 0x%x and length of 0x%x bytes",offset,(data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] & 0x0F),((data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] & 0xF0)>>8) ,ln);
            break;
        }
        case TRANSFER_DATA:
        {
            if(data[0+iLikeTurtles]==(2+iUseTesterAddressWhenIDontNeedTo))
            {
            	sprintf(tmp,"Transfer Data OK for block 0x%x",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            }
            else
            {
            	sprintf(tmp,"Transfer Data request for block 0x%x with payload: \n",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                uint8_t cnt=(data[(0+iLikeTurtles)] - (2+iUseTesterAddressWhenIDontNeedTo));//remove the header
                uint8_t cnt2=0;//for new line
                for(uint16_t a=0;a<cnt;a++)
                {
                    sprintf(tmpbuf,"0x%x ",data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                    strngcat (tmp,tmpbuf);
                    cnt2++;
                    if(cnt2==16)
                    {
                        cnt2=0;
                        sprintf(tmpbuf,"\n");
                        strngcat (tmp,tmpbuf);
                    }
                }
            }
            break;
        }
        case REQUEST_TRANSFER_EXIT:
        {
        	sprintf(tmp,"Transfer Exit Request");
            break;
        }
        case REQUEST_FILE_TRANSFER:
        {

            break;
        }
        case DYNAMICALLY_DEFINE_DATA_ID:
        {

            break;
        }
        case (READ_ECU_ID + 0x40):
        {
            sprintf(tmp,"(KWP2000)ECU Information reply for parameter 0x%x with data:\n-HEX: ",data[4]);
            uint8_t cnt=(data[2] - 2);//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                sprintf(tmpbuf,"0x%x ",data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                strngcat (tmp,tmpbuf);
            }
            sprintf(tmpbuf,"\n-ASCII: ");
            strngcat (tmp,tmpbuf);
            cnt=(data[2] - 2);//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                 sprintf(tmpbuf,"%c",data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                 strngcat (tmp,tmpbuf);
            }
            break;
        }
        case (REQUEST_FREEZE_FRAME + 0x40):
        {
        	sprintf(tmp,"Request for Freeze Frame: ");
        	writeToLog(tmp);
        	parsePID(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	sprintf(tmp,"\nRequested Frame: 0x%x ",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	break;
        }
        case (REQUEST_STORED_DTCS + 0x40):
        {
        	sprintf(tmp,"Reply to Request for Stored DTCs \n-HEX: ");
        	uint8_t cnt=(data[(0+iLikeTurtles)] - (1+iUseTesterAddressWhenIDontNeedTo));//remove the header
        	for(uint8_t a=0;a<cnt;a++)
        	{
        	    sprintf(tmpbuf,"0x%x ",data[(a+2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	    strngcat (tmp,tmpbuf);
        	}
        	break;
        }
        case (REQUEST_CLEAR_DTCS + 0x40):
        {
        	sprintf(tmp,"Positive reply to Request to Clear Stored DTCs ");
        	break;
        }
        case (REQUEST_PENDING_DTCS + 0x40):
        {
        	sprintf(tmp,"Reply to Request for Pending DTCs \n-HEX: ");
        	uint8_t cnt=(data[(0+iLikeTurtles)] - (1+iUseTesterAddressWhenIDontNeedTo));//remove the header
        	for(uint8_t a=0;a<cnt;a++)
        	{
        	    sprintf(tmpbuf,"0x%x ",data[(a+2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	    strngcat (tmp,tmpbuf);
        	}
        	break;
        }
        case (REQUEST_PERMANENT_DTCS + 0x40):
        {
        	sprintf(tmp,"Reply to Request for Permanent DTCs \n-HEX:\n");
        	uint8_t cnt=(data[(0+iLikeTurtles)] - (1+iUseTesterAddressWhenIDontNeedTo));//remove the header
        	for(uint8_t a=0;a<cnt;a++)
        	{
        	    sprintf(tmpbuf,"0x%x ",data[(a+2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	    strngcat (tmp,tmpbuf);
        	}
        	break;
        }
        case (REQUEST_POWERTRAIN_DIAG_DATA + 0x40):
         {
         	sprintf(tmp,"Reply to Request Powertrain Diagnostics data: ");
         	writeToLog(tmp);
         	parsePID(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
         	uint8_t data2[256]={0};
        	uint8_t cnt=(data[(0+iLikeTurtles)] - (1+iUseTesterAddressWhenIDontNeedTo));//remove the header
        	for(uint8_t a=0;a<cnt;a++)
        	{
        	    data2[a] = data[(a+2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
        	}
         	if(!parsePIDData(data2))
         	{
         		sprintf(tmp,"\nNo parser found for this PID, providing raw data...\n-HEX: ");
         		uint8_t cnt=(data[(0+iLikeTurtles)] - (2+iUseTesterAddressWhenIDontNeedTo));//remove the header
         		for(uint8_t a=0;a<cnt;a++)
         		{
         			sprintf(tmpbuf,"0x%x ",data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
         			strngcat (tmp,tmpbuf);
         		}
         		writeToLog(tmp);
         	}
         	return;
         }
         case (REQUEST_VEHICLE_INFORMATION  + 0x40):
         {
         	sprintf(tmp,"Reply to Request Vehicle Information: ");
         	writeToLog(tmp);
         	parseVehicleInfoRequest(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
         	sprintf(tmp,"\n-HEX: ");
            uint8_t cnt=(data[(0+iLikeTurtles)] - (2+iUseTesterAddressWhenIDontNeedTo));//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                sprintf(tmpbuf,"0x%x ",data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                strngcat (tmp,tmpbuf);
            }
            sprintf(tmpbuf,"\n-ASCII: ");
            strngcat (tmp,tmpbuf);
            cnt=(data[(0+iLikeTurtles)] - (2+iUseTesterAddressWhenIDontNeedTo));//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                 sprintf(tmpbuf,"%c",data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                 strngcat (tmp,tmpbuf);
            }
         	break;
         }
        case (CONTROL_DTC_SETTINGS + 0x40):
        {
        	sprintf(tmp,"Positive response for Control DTC Setting request to turn DTC ");
        	switch (data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
        	{
        		case 0x01:
        		{
        			sprintf(tmpbuf,"On ");
        			break;
        		}
        		case 0x02:
        		{
        			sprintf(tmpbuf,"Off ");
        		    break;
        		}
        		default:
        		{
        			sprintf(tmpbuf,"to Unknown Status 0x%x : ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        			break;
        		}
        	}
        	strngcat (tmp,tmpbuf);
        	break;
        }
        case (ACCESS_TIMING_PARAMETERS + 0x40): //here are the positive responses
        {

            break;
        }
        case (START_DIAG_SESSION + 0x40):
        {
        	sprintf(tmp,"Positive response to Start Diagnostics Session with parameter ");
            switch(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
            {
            	case UDS_DEFAULT_SESSION:
                {
                	sprintf(tmpbuf,"Default Session");
                    break;
                }
                case UDS_EXTENDED_SESSION:
                {
                	sprintf(tmpbuf,"Extended Session");
                    break;
                }
                case UDS_PROGRAMMING_SESSION:
                {
                	sprintf(tmpbuf,"Programming Session");
                    break;
                }
                default:
                {
                	sprintf(tmpbuf,"0x%x",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                    break;
                }
            }
            strngcat (tmp,tmpbuf);
            break;
        }
        case (STOP_DIAGNOSTICS_SESSION + 0x40):
        {
        	sprintf(tmp,"Positive response to Stop Diagnostics Session");
            break;
        }
        case (CLEAR_DTCS + 0x40):
        {
            uint16_t tmp2=data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
            tmp2=(tmp2<<8)+data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
            sprintf(tmp,"Positive response to Clear Diagnostics Information for group 0x%x",tmp2);
            break;
        }
        case (SECURITY_ACCESS + 0x40):
        {
            if(data[(0+iLikeTurtles)]<5)
            {
            	sprintf(tmp,"Security Access granted for level 0x%x ",(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)] - 1));
            }
            else
            {
            	sprintf(tmp,"Security Access Random Seed for level 0x%x: ",(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]));
                for(uint8_t a=0;a<(data[(0+iLikeTurtles)] - (2+iUseTesterAddressWhenIDontNeedTo));a++)
                {
                	sprintf(tmpbuf,"0x%x ",data[(3+iLikeTurtles+a+iUseTesterAddressWhenIDontNeedTo)]);
                	strngcat (tmp,tmpbuf);
                }
            }
            break;
        }
        case (READ_DTC_BY_STATUS + 0x40):
        {
        	sprintf(tmp,"Read DTC by Status reply with %d DTCs stored in total:\n",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            uint8_t cnt=(data[(0+iLikeTurtles)] - (2+iUseTesterAddressWhenIDontNeedTo));//remove the header
            for(uint8_t a=0;a<cnt;a=(a+3))
            {
                uint16_t tmp2=data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
                tmp2=((tmp2<<8) + data[(a+4+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                sprintf(tmpbuf,"-DTC code 0x%x with status 0x%x\n",tmp2,data[(a+5+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                strngcat (tmp,tmpbuf);
            }
            break;
        }
        case (TESTER_PRESENT + 0x40):
        {
        	sprintf(tmp,"Tester Present OK ");
            break;
        }
        case (READ_DATA_BY_ID + 0x40):
        {
        	sprintf(tmp,"ECU Data reply for ID 0x%x with data:\n-HEX: ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            uint8_t cnt=(data[(0+iLikeTurtles)] - (2+iUseTesterAddressWhenIDontNeedTo));//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                sprintf(tmpbuf,"0x%x ",data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                strngcat (tmp,tmpbuf);
            }
            sprintf(tmpbuf,"\n-ASCII: ");
            strngcat (tmp,tmpbuf);
            cnt=(data[(0+iLikeTurtles)] - (2+iUseTesterAddressWhenIDontNeedTo));//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                 sprintf(tmpbuf,"%c",data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                 strngcat (tmp,tmpbuf);
            }
            break;
        }
        case (WRITE_DATA_BY_ID + 0x40):
        {
        	sprintf(tmp,"Positive response to Request to Read Data by ID 0x%x ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            break;
        }
        case (ECU_RESET + 0x40):
        {
        	sprintf(tmp,"Positive response to ECU reset -> Going into ");
        	switch(data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
        	{
				case HARD_RESET:
				{
					sprintf(tmpbuf,"Hard Reset ");
					break;
				}
				case IGNITION_RESET:
				{
					sprintf(tmpbuf,"Ignition Reset ");
					break;
				}
				case SOFT_RESET:
				{
					sprintf(tmpbuf,"Soft Reset ");
					break;
				}
				case ENABLE_RAPID_POWER_SHUTDOWN:
				{
					sprintf(tmpbuf,"Enable Rapid Power Shutdown ");
					break;
				}
				case DISABLE_RAPID_POWER_SHUTDOWN:
				{
					sprintf(tmpbuf,"Disable Rapid Power Shutdown ");
					break;
				}
				default:
				{
					sprintf(tmpbuf,"Unknown reset type 0x%x ",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
					break;
				}
        	}
        	strngcat (tmp,tmpbuf);
        	sprintf(tmpbuf,"in %d seconds ",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	strngcat (tmp,tmpbuf);
            break;
        }
        case (READ_MEMORY_BY_ADDRESS + 0x40):
        {
        	sprintf(tmp,"Positive reply to Read Memory by Address with data:\n-HEX: ");
        	uint8_t cnt=(data[(0+iLikeTurtles)] - (1+iUseTesterAddressWhenIDontNeedTo));//remove the header
        	uint8_t poo=0;
        	for(uint8_t a=0;a<cnt;a++)
        	{
        	    sprintf(tmpbuf,"0x%x ",data[(a+2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	    strngcat (tmp,tmpbuf);
        	    poo++;
        	    if(poo == 16)
        	    {
        	    	poo=0;
        	    	sprintf(tmpbuf,"\n");
        	    	strngcat (tmp,tmpbuf);
        	    }
        	}
        	break;
        }
        case (WRITE_MEMORY_BY_ADDRESS + 0x40):
        {
        	sprintf(tmp,"Positive reply to Write Memory by Address\n");
            break;
        }
        case (IO_CONTROL_BY_LOCAL_ID + 0x40):
        {

            break;
        }
        case (ROUTINE_CONTROL + 0x40):
        {
                    sprintf(tmp,"Routine Control: ");
                    switch (data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
        			{
                    	case START_ROUTINE:
                    	{
                    		sprintf(tmpbuf,"Start routine with ID 0x%x positive reply",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                    		strngcat (tmp,tmpbuf);
                    		break;
                    	}
                    	case STOP_ROUTINE:
                    	{
                    		sprintf(tmpbuf,"Stop routine with ID 0x%x positive reply",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                    		strngcat (tmp,tmpbuf);
                    		break;
                    	}
                    	case REQUEST_ROUTINE_RESULT:
                    	{
                    		char tmpbuf[265];
                    		sprintf(tmpbuf,"Request Routine Results for ID 0x%x positive reply with result:\n -",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                    		strngcat (tmp,tmpbuf);
                    		uint8_t cnt=(data[(0+iLikeTurtles)] - (3+iUseTesterAddressWhenIDontNeedTo));//remove the header
                    		for(uint8_t a=0;a<cnt;a++)
                    		{
                    		      sprintf(tmpbuf,"0x%x ",data[(a+4+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                    		      strngcat (tmp,tmpbuf);
                    		}
                    	}
        			}
                    break;
        }
        case (REQUEST_DOWNLOAD + 0x40):
        {
        	uint8_t a=data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
        	uint8_t b=((a & 0xF0) >> 4);
        	uint32_t ln=0;
        	for (uint8_t c=0;c<b;c++)
        	{
        	    ln=(ln<<8);
        	    ln=(ln + data[((3+c)+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	}
        	sprintf(tmp,"Acknowledge to Download request. Will be performed in blocks of 0x%x bytes ",ln);
            break;
        }
        case (REQUEST_UPLOAD + 0x40):
        {
        	uint8_t a=data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)];
        	uint8_t b=((a & 0xF0) >> 8);
        	uint32_t ln=0;
        	for (uint8_t c=0;c<b;c++)
        	{
        	     ln=(ln<<8);
        	     ln=(ln + data[((3+c)+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        	}
        	sprintf(tmp,"Acknowledge to Upload request. Will be performed in blocks of 0x%x bytes ",ln);
            break;
        }
        case (TRANSFER_DATA + 0x40):
        {
            if(data[0+iLikeTurtles]==(2+iUseTesterAddressWhenIDontNeedTo))
            {
            	sprintf(tmp,"Transfer Data OK for block 0x%x",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
            }
            else
            {
                sprintf(tmp,"Transfer Data OK for block 0x%x with payload: \n",data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                uint16_t cnt=(data[(0+iLikeTurtles)] - (2+iUseTesterAddressWhenIDontNeedTo));//remove the header
                uint16_t cnt2=0;//for new line
                for(uint16_t a=0;a<cnt;a++)
                {
                    sprintf(tmpbuf,"0x%x ",data[(a+3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
                    strngcat (tmp,tmpbuf);
                    cnt2++;
                    if(cnt2==16)
                    {
                        cnt2=0;
                        sprintf(tmpbuf,"\n");
                        strngcat (tmp,tmpbuf);
                    }

                }
            }
            break;
        }
        case (COMMUNICATION_CONTROL +0x40):
		{
        	sprintf(tmp,"Communication Control positive reply for ");
        	switch (data[(2+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)])
        	{
        		case 0x01:
        		{
        			sprintf(tmpbuf,"Enable RX and TX ");
        			break;
        		}
        		case 0x02:
        		{
        			sprintf(tmpbuf,"Enable RX and disable TX ");
        		    break;
        		}
        		case 0x03:
        		{
        			sprintf(tmpbuf,"Disable RX and enable TX ");
        		    break;
        		}
        		case 0x04:
        		{
        			sprintf(tmpbuf,"Disable RX and TX ");
        		    break;
        		}
        		default:
        		{
        			sprintf(tmpbuf,"Unknown Control Type 0x%x ",data[(3+iLikeTurtles+iUseTesterAddressWhenIDontNeedTo)]);
        			break;
        		}
        	}
        	strngcat (tmp,tmpbuf);
        	break;
		}
        case (REQUEST_TRANSFER_EXIT + 0x40):
        {
        	sprintf(tmp,"Transfer Exit OK");
            break;
        }
        case (REQUEST_FILE_TRANSFER + 0x40):
        {

            break;
        }
        case (DYNAMICALLY_DEFINE_DATA_ID + 0x40):
        {

            break;
        }
        default:
        {
            uint8_t ln;
            if((data[0] & 0xF0) == 0)
            {
            	ln=data[0];
            }
            else
            {
            	ln=data[1];
            }
            sprintf(tmp,"Sent an unknown request using payload ");
            strngcat (tmp,tmpbuf);
            for (uint8_t a=0;a<ln;a++)
            {
                sprintf(tmpbuf,"0x%x ",data[(a+1+iLikeTurtles)]);
                strngcat (tmp,tmpbuf);
            }
            break;
        }
    }
	writeToLog(tmp);
}

void parseTPSID(uint8_t *data)
{
	char tmp[1024]={0};
	char tmpbuf[265]={0};
	switch(data[3])
    {
        case ERROR:
        {
        	sprintf(tmp,"Error processing SID 0x%x. Cause: ",data[4]);
        	writeToLog(tmp);
            printError(data[5]);
            return;
        }
        case DISABLE_NORMAL_MSG_TRANSMISSION:
        {
        	sprintf(tmp,"Request to disable non-diagnostics messages transmission ");
        	break;
        }
        case ENABLE_NORMAL_MSG_TRANSMISSION:
        {
        	sprintf(tmp,"Request to enable non-diagnostics messages transmission ");
            break;
        }
        case ACCESS_TIMING_PARAMETERS:
        {
            
            break;
        }
        case READ_DTC_BY_STATUS:
        {
            uint32_t dtcGroup = data[5];
            dtcGroup= ((dtcGroup<<8) + data[6]);
            sprintf(tmp,"Read DTC by Status request for DTCs with status 0x%x on group 0x%x ",data[4],dtcGroup);
            break;
        }
        case START_DIAG_SESSION: //startDiagnosticsSession
        {
        	sprintf(tmp,"Request to Start Diagnostics Session with parameter ");
            switch(data[4])
            {             
                case KWP_DEFAULT_SESSION:
                {
                	sprintf(tmpbuf,"KWP2K Default Session");
                    break;
                }
                case KWP_EOL_SYSTEM_SUPPLIER:
                {
                	sprintf(tmpbuf,"KWP2K EOL System Supplier");
                    break;
                }
                case KWP_ECU_PROGRAMMING_MODE:
                {
                	sprintf(tmpbuf,"KWP2K ECU Programming Mode");
                    break;
                }
                case KWP_COMPONENT_STARTING:
                {
                	sprintf(tmpbuf,"KWP2K Component Starting Mode");
                    break;
                }
                default:
                {
                	sprintf(tmpbuf,"0x%x",data[4]);
                    break;
                }
            }
            strngcat (tmp,tmpbuf);
            break;
        }
        case STOP_DIAGNOSTICS_SESSION:
        {
        	sprintf(tmp,"Request to Stop Diagnostics Session");
            break;
        }
        case CLEAR_DTCS:
        {
            uint16_t tmp2=data[4];
            tmp2=(tmp2<<8)+data[5];
            sprintf(tmp,"Request to Clear Diagnostics Information for group 0x%x",tmp2);
            break;
        }
        case SECURITY_ACCESS:
        {
            if(data[2]<5)
            {
            	sprintf(tmp,"Security Access request with parameters ");
                for(uint8_t a=0;a<(data[2] - 1);a++)
                {
                	sprintf(tmpbuf,"0x%x ",data[(4+a)]);
                	strngcat (tmp,tmpbuf);
                }
            }
            else
            {
            	sprintf(tmp,"Security Access calculated key for level 0x%x: ",(data[4] - 1));
                for(uint8_t a=0;a<(data[2] - 2);a++)
                {
                	sprintf(tmpbuf,"0x%x ",data[(5+a)]);
                	strngcat (tmp,tmpbuf);
                }
            }
            break;
        }
        case TESTER_PRESENT:
        {
        	sprintf(tmp,"Tester Present ");
            break;
        }
        case READ_ECU_ID://readECUIdentification
        {
        	sprintf(tmp,"ECU Information request for parameter 0x%x ",data[4]);
            break;
        }
        case READ_DATA_BY_LOCAL_ID:
        {
        	sprintf(tmp,"Read Data by Local ID request for Local ID 0x%x ",data[4]);
            break;
        }
        case WRITE_DATA_BY_LOCAL_ID:
        {
            
            break;
        }
        case ECU_RESET:
        {
        	sprintf(tmp,"ECU Reset -> ");
        	switch(data[4])
        	{
				case HARD_RESET:
				{
					sprintf(tmpbuf,"Hard Reset ");
					break;
				}
				case IGNITION_RESET:
				{
					sprintf(tmpbuf,"Ignition Reset ");
					break;
				}
				case SOFT_RESET:
				{
					sprintf(tmpbuf,"Soft Reset ");
					break;
				}
				case ENABLE_RAPID_POWER_SHUTDOWN:
				{
					sprintf(tmpbuf,"Enable Rapid Power Shutdown ");
					break;
				}
				case DISABLE_RAPID_POWER_SHUTDOWN:
				{
					sprintf(tmpbuf,"Disable Rapid Power Shutdown ");
					break;
				}
				default:
				{
					sprintf(tmpbuf,"Unknown reset type 0x%x",data[4]);
					break;
				}
        	}
        	strngcat(tmp,tmpbuf);
            break;
        }
        case READ_MEMORY_BY_ADDRESS:
        {
        	uint8_t a=data[4];
        	uint32_t offset=0;
        	uint8_t b=(a & 0xF);
        	for (uint8_t c=0;c<b;c++)
        	{
        		offset=(offset<<8);
        		offset=(offset + data[(5+c)]);
        	}
        	uint8_t d=((a & 0xF0) >> 4);
        	uint32_t ln=0;
        	for (uint8_t c=0;c<d;c++)
        	{
        		ln=(ln<<8);
        		ln=(ln + data[(5+c+b)]);
        	}
        	sprintf(tmp,"Read Memory by Address request for offset 0x%x and length 0x%x",offset,ln);
            break;
        }
        case WRITE_MEMORY_BY_ADDRESS:
        {
            
            break;
        }
        case START_COMMUNICATION:
        {
            
            break;
        }
        case STOP_COMMUNICATION:
        {
        	sprintf(tmp,"Stop Communication Request ");
            break;
        }
        case IO_CONTROL_BY_LOCAL_ID:
        {
            
            break;
        }
        case START_ROUTINE_BY_LOCAL_ID:
        {
        	sprintf(tmp,"Start Routine by Local ID with following parameters:\n-Routine ID: 0x%x\n-Routine parameters: ",data[4]);
            uint8_t cnt=(data[2] - 2);//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                sprintf(tmpbuf,"0x%x ",data[(a+5)]);
                strngcat (tmp,tmpbuf);
            }
            break;
        }
        case STOP_ROUTINE_BY_LOCAL_ID:
        {
        	sprintf(tmp,"Stop Routine by Local ID request for Routine ID 0x%x ",data[4]);
            break;
        }
        case REQUEST_ROUTINE_RESULTS_BY_LOCAL_ID:
        {
        	sprintf(tmp,"Request Routine results by Local ID for 0x%x",data[4]);
            break;
        }
        case REQUEST_DOWNLOAD:
        {
            uint32_t addr=data[4];
            addr=(addr<<8)+data[5];
            addr=(addr<<8)+data[6];
            uint32_t lng=data[8];
            lng=(lng<<8)+data[9];
            lng=(lng<<8)+data[10];
            sprintf(tmp,"Download Request to address 0x%x using Data Format Identifier 0x%x and length of 0x%x bytes",addr,data[7],lng);
            break;
        }
        case REQUEST_UPLOAD:
        {
            uint32_t addr=data[4];
            addr=(addr<<8)+data[5];
            addr=(addr<<8)+data[6];
            uint32_t lng=data[8];
            lng=(lng<<8)+data[9];
            lng=(lng<<8)+data[10];
            sprintf(tmp,"Upload Request from address 0x%x using Data Format Identifier 0x%x and length of 0x%x bytes",addr,data[7],lng);
            break;
        }
        case TRANSFER_DATA:
        {
            if(data[2]==1)
            {
            	sprintf(tmp,"Transfer Data request");
            }
            else
            {
            	sprintf(tmp,"Transfer Data request with payload: \n");
                uint8_t cnt=(data[2] - 1);//remove the header
                uint8_t cnt2=0;//for new line
                for(uint16_t a=0;a<cnt;a++)
                {
                    sprintf(tmpbuf,"0x%x ",data[(a+4)]);
                    strngcat (tmp,tmpbuf);
                    cnt2++;
                    if(cnt2==16)
                    {
                        cnt2=0;
                        sprintf(tmpbuf,"\n");
                        strngcat (tmp,tmpbuf);
                    }
                }
            }    
            break;
        }
        case REQUEST_TRANSFER_EXIT:
        {
        	sprintf(tmp,"Transfer Exit Request");
            break;
        }
        case REQUEST_FILE_TRANSFER:
        {
            
            break;
        }
        case DYNAMICALLY_DEFINE_DATA_ID:
        {
            
            break;
        }
        case (ACCESS_TIMING_PARAMETERS + 0x40): //here are the positive responses
        {
            
            break;
        }
        case (DISABLE_NORMAL_MSG_TRANSMISSION + 0x40):
        {
        	sprintf(tmp,"Positive response to disable non-diagnostics messages transmission ");
            break;
        }
        case (ENABLE_NORMAL_MSG_TRANSMISSION + 0x40):
        {
        	sprintf(tmp,"Positive response to enable non-diagnostics messages transmission ");
            break;
        }
        case (START_DIAG_SESSION + 0x40):
        {
        	sprintf(tmp,"Positive response to Start Diagnostics Session with parameter ");
            switch(data[4])
            {             
                case KWP_DEFAULT_SESSION:
                {
                	sprintf(tmpbuf,"Default Session");
                    break;
                }
                case KWP_EOL_SYSTEM_SUPPLIER:
                {
                	sprintf(tmpbuf,"EOL System Supplier");
                    break;
                }
                case KWP_ECU_PROGRAMMING_MODE:
                {
                	sprintf(tmpbuf,"ECU Programming Mode");
                    break;
                }
                case KWP_COMPONENT_STARTING:
                {
                	sprintf(tmpbuf,"Component Starting Mode");
                    break;
                }
                default:
                {
                	sprintf(tmpbuf,"0x%x",data[4]);
                    break;
                }
            }
            strngcat (tmp,tmpbuf);
            break;
        }
        case (STOP_DIAGNOSTICS_SESSION + 0x40):
        {
        	sprintf(tmp,"Positive response to Stop Diagnostics Session");
            break;
        }
        case (CLEAR_DTCS + 0x40):
        {
            uint16_t tmp2=data[4];
            tmp2=(tmp2<<8)+data[5];
            sprintf(tmp,"Positive response to Clear Diagnostics Information for group 0x%x",tmp2);
            break;
        }
        case (SECURITY_ACCESS + 0x40):
        {
            if(data[2]<5)
            {
            	sprintf(tmp,"Security Access granted for level 0x%x ",(data[4] - 1));
            }
            else
            {
            	sprintf(tmp,"Security Access Random Seed for level 0x%x: ",(data[4]));
                for(uint8_t a=0;a<(data[2] - 2);a++)
                {
                	sprintf(tmpbuf,"0x%x ",data[(5+a)]);
                	strngcat (tmp,tmpbuf);
                }
            }
            break;
        }
        case (READ_DTC_BY_STATUS + 0x40):
        {
            sprintf(tmp,"Read DTC by Status reply with %d DTCs stored in total:\n",data[4]);
            uint8_t cnt=(data[2] - 2);//remove the header
            for(uint8_t a=0;a<cnt;a=(a+3))
            {
                uint16_t tmp2=data[(a+5)];
                tmp2=((tmp2<<8) + data[(a+6)]);
                sprintf(tmpbuf,"-DTC code 0x%x with status 0x%x\n",tmp2,data[(a+7)]);
                strngcat (tmp,tmpbuf);
            }
            break;
        }
        case (TESTER_PRESENT + 0x40):
        {
        	sprintf(tmp,"Tester Present OK ");
            break;
        }
        case (READ_ECU_ID + 0x40):
        {
            sprintf(tmp,"ECU Information reply for parameter 0x%x with data:\n-HEX: ",data[4]);
            uint8_t cnt=(data[2] - 2);//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                sprintf(tmpbuf,"0x%x ",data[(a+5)]);
                strngcat (tmp,tmpbuf);
            }
            sprintf(tmpbuf,"\n-ASCII: ");
            strngcat (tmp,tmpbuf);
            cnt=(data[2] - 2);//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                 sprintf(tmpbuf,"%c",data[(a+5)]);
                 strngcat (tmp,tmpbuf);
            }
            break;
        }
        case (READ_DATA_BY_LOCAL_ID + 0x40):
        {
        	sprintf(tmp,"Reply to Read Data by Local ID for Local ID 0x%x\n-HEX:",data[4]);
            uint8_t cnt=(data[2] - 2);//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                sprintf(tmpbuf,"0x%x ",data[(a+5)]);
                strngcat (tmp,tmpbuf);
            }
            sprintf(tmpbuf,"\n-ASCII: ");
            strngcat (tmp,tmpbuf);
            cnt=(data[2] - 2);//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                 sprintf(tmpbuf,"%c",data[(a+5)]);
                 strngcat (tmp,tmpbuf);
            }
            break;
        }
        case (WRITE_DATA_BY_LOCAL_ID + 0x40):
        {
            
            break;
        }
        case (ECU_RESET + 0x40):
        {
            
            break;
        }
        case (READ_MEMORY_BY_ADDRESS + 0x40):
        {
            
            break;
        }
        case (WRITE_MEMORY_BY_ADDRESS + 0x40):
        {
            
            break;
        }
        case (START_COMMUNICATION + 0x40):
        {
            
            break;
        }
        case (STOP_COMMUNICATION + 0x40):
        {
        	sprintf(tmp,"Positive response to Stop Communication request ");
            break;
        }
        case (IO_CONTROL_BY_LOCAL_ID + 0x40):
        {
            
            break;
        }
        case (START_ROUTINE_BY_LOCAL_ID + 0x40):
        {
        	sprintf(tmp,"Positive response for Start Routine by Local ID 0x%x",data[4]);
            break;
        }
        case (STOP_ROUTINE_BY_LOCAL_ID + 0x40):
        {
        	sprintf(tmp,"Positive response for Stop Routine by Local ID 0x%x",data[4]);
            break;
        }
        case (REQUEST_ROUTINE_RESULTS_BY_LOCAL_ID + 0x40):
        {
        	sprintf(tmp,"Request Routine Results by Local ID 0x%x positive reply with result:\n -",data[4]);
            uint8_t cnt=(data[2] - 2);//remove the header
            for(uint8_t a=0;a<cnt;a++)
            {
                sprintf(tmpbuf,"0x%x ",data[(a+5)]);
                strngcat (tmp,tmpbuf);
            }
            break;
        }
        case (REQUEST_DOWNLOAD + 0x40):
        {
        	sprintf(tmp,"Acknowledge to Download request. Will be performed in blocks of 0x%x bytes ",data[4]);
            break;
        }
        case (REQUEST_UPLOAD + 0x40):
        {
        	sprintf(tmp,"Acknowledge to Upload request. Will be performed in blocks of 0x%x bytes ",data[4]);
            break;
        }
        case (TRANSFER_DATA + 0x40):
        {
            if(data[2]==1)
            {
            	sprintf(tmp,"Transfer Data OK");
            }
            else
            {
            	sprintf(tmp,"Transfer Data OK with payload: \n");
                uint16_t cnt=(data[2] - 1);//remove the header
                uint16_t cnt2=0;//for new line
                for(uint16_t a=0;a<cnt;a++)
                {
                    sprintf(tmpbuf,"0x%x ",data[(a+4)]);
                    strngcat (tmp,tmpbuf);
                    cnt2++;
                    if(cnt2==16)
                    {
                        cnt2=0;
                        sprintf(tmpbuf,"\n");
                        strngcat (tmp,tmpbuf);
                    }
                        
                }
            } 
            break;
        }
        case (REQUEST_TRANSFER_EXIT + 0x40):
        {
        	sprintf(tmp,"Transfer Exit OK");
            break;
        }
        case (REQUEST_FILE_TRANSFER + 0x40):
        {
            
            break;
        }
        case (DYNAMICALLY_DEFINE_DATA_ID + 0x40):
        {
            
            break;
        }
        default:
        {
            uint8_t ln = data[2];
            sprintf(tmp,"Sent an unknown request using payload ");
            for (uint8_t a=0;a<ln;a++)
            {
                sprintf(tmpbuf,"0x%x ",data[(a+3)]);
                strngcat (tmp,tmpbuf);
            }
            break;
        }
    }
    writeToLog(tmp);
}

uint32_t parseCANStream(char *input, char *output, uint32_t frameCount)//we use two bytes for CAN ID here
{
	wait(1);//just in case we are switching too fast from logging onto parsing, which would cause a file error
	file2 = sd.open(input, O_RDONLY);//open source file
	if (file2 == NULL)
	{
		device.printf("Error opening source file file\n");
		return 0;
	}
	if (!toggleLogging(output,1))//open destination file
	{
	   device.printf("Error creating or opening log file\n");
	   return 0;
	}
	//file2->lseek(0x0,SEEK_SET);
	uint8_t data[8]={0};
	char bf[128]={0};
	uint32_t time2=0;
	uint8_t ln=0;
	uint32_t ID=0;
	uint32_t framesProc=0;//to know how many frames have been processed
	while (framesProc < frameCount) //while there are frames left
	{
		if(fileToRawFrame(&time2,&ID,&ln,data))//get the whole line
		{
			if(ln > 8)
			{
				//do nothing, there is a bug that returns a pointer instead of length, but still works. thats the reason for the missing frames
			}
			else
			{
				framesProc++;
				sprintf(bf,"%d,",time2);
				writeToLog(bf);
				sprintf(bf,"0x%x",ID);
				writeToLog(bf);
				sprintf(bf,",%d",ln);
				writeToLog(bf);
				for(uint8_t a = 0; a < ln; a++)
				{
					sprintf(bf,",0x%x",data[a]);
					writeToLog(bf);
				}
				sprintf(bf,"\n");
				writeToLog(bf);
			}
		}
		else
		{
			break; //if we reach the end of the file somehow, just get out of here
		}
	}
	toggleLogging(output,0);
	file2->close();
	return framesProc;
}
        

uint32_t parseUDSStream(char *input, char *output, uint32_t frameCount, uint8_t mode)//need to make use of ln to know if we are getting an already started stream!
{
	wait(1);//just in case we are switching too fast from logging onto parsing, which would cause a file error
	file2 = sd.open(input, O_RDONLY);//open source file
	if (file2 == NULL)
	{
		device.printf("Error opening source file file\n");
		return 0;
	}
	if (!toggleLogging(output,1))//open destination file
	{
	   device.printf("Error creating or opening log file\n");
	   return 0;
	}
	//file2->lseek(0x0,SEEK_SET);
	uint8_t data[265]={0};
	char bf[128]={0};
	uint8_t ln=0;
	uint32_t ID=0;
	uint32_t time1=0;
	uint32_t time2=0;
	uint32_t framesProc=0;//to know how many frames have been processed
	while (framesProc < frameCount) //while there are frames left
	{
		bool wazzap=0;
		if(mode == 0)
		{
			wazzap=fileToFrame(&time2,&ID,&ln,data);//get the whole line
		}
		else
		{
			wazzap=logToRawFrame(&time2,&ID,&ln,data);//get the whole line
		}
		if(wazzap)//get the whole line
		{
			framesProc++;
			if(mode == 0 || (mode == 1 && (ID ==  rID || ID ==  ownID)))
			{
				uint8_t streamType=0;
				uint8_t tunnelMode=0;
				if((data[0] != 0x10 && data[0] != 0x20 && data[0] != 0x30 && data[0] > 7) && (data[1] == 0x10 || data[1] == 0x20 || data[1] == 0x30 || data[1] <= 6) && (data[0] == (ownID & 0xFF) || data[0] == (rID & 0xFF)))//if they are potentially using the first byte as the target address
				{
					streamType = (data[1] & 0xF0);
					tunnelMode=1;
					for(uint8_t a = 0;a  <  7;a++)
					{
						data[a] = data[(a + 1)];//to get rid of bs tester address on first byte
					}
				}
				else
				{
					streamType = (data[0] & 0xF0);
				}
				uint8_t tmpdata[8]={0};
				uint32_t tmpID=0;
				if(time1 == 0)
				{
					sprintf(bf,"%dms - ",time2);
				}
				else
				{
					if(time2 < time1)//timer overflow
					{
						uint32_t tmptime=0;
						while(time2 < time1)
						{
							time1++;
							tmptime++;
						}
						sprintf(bf,"%dms - ",tmptime);
					}
					else
					{
						sprintf(bf,"%dms - ",(time2 - time1));
					}
				}
				writeToLog(bf);
				switch (streamType)
				{
					case 0x0://single frame
					{
						sprintf(bf,"0x%x ",ID);
						writeToLog(bf);
						parseUDSSID(data);
						sprintf(bf,"\n");
						writeToLog(bf);
						break;
					}
					case 0x10://With ACK request
					{
						if(ln==8)//make sure we got an UDS packet!
						{
							uint32_t cnt=0;
							uint32_t cnt2=0;
							if(data[0] != (ownID & 0xFF) && data[0] != (rID & 0xFF))
							{
								cnt=data[1];//to know the transmission length
								cnt2=8;//to keep track of the array. Set to the current array length
							}
							else
							{
								cnt=data[2];
								cnt2=7;
							}
							while(cnt>(cnt2-2))//run until we get the whole length
							{
								if(mode == 0)
								{
									wazzap=fileToFrame(&time2,&tmpID,&ln,tmpdata);//get the whole line
								}
								else
								{
									wazzap=logToRawFrame(&time2,&tmpID,&ln,tmpdata);//get the whole line
								}
								if(!wazzap)
								{
									break;
								}
								framesProc++;
								if(tmpID == ID)
								{
									if((cnt-(cnt2-2))>6 && tunnelMode == 0)//if 7 or more bytes are left, just grab the whole thing
									{
										for(uint8_t a=1;a<8;a++)
										{
											data[cnt2]=tmpdata[a];
											cnt2++;
										}
									}
									else if((cnt-(cnt2-2))>5 && tunnelMode == 1)//if they are using the bs tester address byte on the beginning
									{
										for(uint8_t a=2;a<8;a++)
										{
											data[cnt2]=tmpdata[a];
											cnt2++;
										}
									}
									else
									{
										uint8_t remaining=((cnt-(cnt2-2))+1);
										if(tunnelMode ==0)
										{
											for(uint8_t a=1;a<remaining;a++)
											{
												data[cnt2]=tmpdata[a];
												cnt2++;
											}
										}
										else
										{
											for(uint8_t a=2;a<remaining;a++)
											{
												data[cnt2]=tmpdata[a];
												cnt2++;
											}
										}
									}
								}

							}
							sprintf(bf,"0x%x with %d data bytes ->",ID,(cnt - 1));//we remove the 2 header bytes plus SID
							writeToLog(bf);
							parseUDSSID(data);
							sprintf(bf,"\n");
							writeToLog(bf);
						}
						break;
					}
					case 0x20://multiple frames stream without ACK. This is strange to see as a first frame, but who knows!
					{
						uint32_t cnt=0;
						uint32_t cnt2=0;
						if(data[0] != 0xF1)
						{
							cnt=data[1];//to know the transmission length
							cnt2=8;//to keep track of the array. Set to the current array length
						}
						else
						{
							cnt=data[2];
							cnt2=7;
						}
						while(cnt>(cnt2-2))//run until we get the whole length
						{
							if(mode == 0)
							{
								wazzap=fileToFrame(&time2,&tmpID,&ln,tmpdata);//get the whole line
							}
							else
							{
								wazzap=logToRawFrame(&time2,&tmpID,&ln,tmpdata);//get the whole line
							}
							if(!wazzap)
							{
								break;
							}
							framesProc++;
							if(tmpID == ID)
							{
								if((cnt-(cnt2-2))>6 && tunnelMode == 0)//if 7 or more bytes are left, just grab the whole thing
								{
									for(uint8_t a=1;a<8;a++)
									{
										data[cnt2]=tmpdata[a];
										cnt2++;
									}
								}
								else if((cnt-(cnt2-2))>5 && tunnelMode == 1)//if they are using the bs tester address byte on the beginning
								{
									for(uint8_t a=2;a<8;a++)
									{
										data[cnt2]=tmpdata[a];
										cnt2++;
									}
								}
								else
								{
									uint8_t remaining=((cnt-(cnt2-2))+1);
									if(tunnelMode ==0)
									{
										for(uint8_t a=1;a<remaining;a++)
										{
											data[cnt2]=tmpdata[a];
											cnt2++;
										}
									}
									else
									{
										for(uint8_t a=2;a<remaining;a++)
										{
											data[cnt2]=tmpdata[a];
											cnt2++;
										}
									}
								}
							}

						}
						sprintf(bf,"0x%x with %d data bytes ->",ID,(cnt -1));//we remove the 2 header bytes plus SID
						writeToLog(bf);
						parseUDSSID(data);
						sprintf(bf,"\n");
						writeToLog(bf);
						break;
					}
					case 0x30://ACK
					{
						sprintf(bf,"0x%x ACK \n",ID);
						writeToLog(bf);
						break;
					}

				}
				time1=time2;//sync times
			}
		}
		else
		{
			break; //if we reach the end of the file somehow, just get out of here
		}
	}
	toggleLogging(output,0);
	file2->close();
	return framesProc;
}
        

uint32_t parseTPStream(char *input, char *output, uint32_t frameCount, uint8_t mode)
{
	wait(1);//just in case we are switching too fast from logging onto parsing, which would cause a file error
	file2 = sd.open(input, O_RDONLY);//open source file
	if (file2 == NULL)
	{
		device.printf("Error opening source file file\n");
		return 0;
	}
	if (!toggleLogging(output,1))//open destination file
	{
	   device.printf("Error creating or opening log file\n");
	   return 0;
	}
	//file2->lseek(0x0,SEEK_SET);
	uint8_t data[280]={0};
	uint8_t ln=0;
	uint32_t ID=0;
	uint32_t framesProc=0;//to know how many frames have been processed
	uint32_t time1=0;
	uint32_t time2=0;
	while (framesProc < frameCount) //while there are frames left
	{
		bool wazzap=0;
		if(mode == 0)
		{
			wazzap=fileToFrame(&time2,&ID,&ln,data);//get the whole line
		}
		else
		{
			wazzap=logToRawFrame(&time2,&ID,&ln,data);//get the whole line
		}
		if(wazzap)
		{
			framesProc++;
			if(mode == 0 || (mode == 1 && (ID ==  rID || ID ==  ownID)))
			{
				uint8_t streamType = (data[0] & 0xF0);
				char bf[256]={0};
				uint8_t tmpdata[16]={0};
				uint32_t tmpID=0;
				if(time1 == 0)
				{
					sprintf(bf,"%dms - ",time2);
				}
				else
				{
					if(time2 < time1)//timer overflow
					{
						uint32_t tmptime=0;
						while(time2 < time1)
						{
							time1++;
							tmptime++;
						}
						sprintf(bf,"%dms - ",tmptime);
					}
					else
					{
						sprintf(bf,"%dms - ",(time2 - time1));
					}
				}
				writeToLog(bf);
				switch (streamType)
				{
					case 0xA0:
					{
						sprintf(bf,"0x%x ",ID);
						writeToLog(bf);
						switch (data[0])
						{
							case CHANNEL_DISCONNECT:
							{
								sprintf(bf,"Channel Disconnect\n");
								break;
							}
							case CHANNEL_TEST:
							{
								sprintf(bf,"Channel Test\n");
								break;
							}
							case PARAMETERS_REQUEST:
							{
								uint16_t t1=data[3];
								t1=(t1<<8)+data[2];
								uint16_t t2=data[5];
								t2=(t2<<8)+data[4];
								float f1=(t1/10000);
								float f2=(t2/10000);
								sprintf(bf,"Channel Parameters Request with Block size 0x%x, %f seconds for ACK timeout and %f seconds for packet timeout\n",data[1],f1,f2);
								break;
							}
							case PARAMETER_REQUEST_OK:
							{
								uint16_t t1=data[3];
								t1=(t1<<8)+data[2];
								uint16_t t2=data[5];
								t2=(t2<<8)+data[4];
								float f1=(t1/10000);
								float f2=(t2/10000);
								sprintf(bf,"Channel Parameters Response with Block size 0x%x, %f seconds for ACK timeout and %f seconds for packet timeout\n",data[1],f1,f2);
								break;
							}
							case CHANNEL_BREAK:
							{
								sprintf(bf,"Channel Break\n");
								break;
							}
							default:
							{
								sprintf(bf,"Unknown Channel request 0x%x\n",data[0]);
								break;
							}

						}
						writeToLog(bf);
						break;
					}
					case ACK_LAST:
					{
						if(ln > 3)//make sure we are not getting the last packet of a stream, which would fuck up the whole thing
						{
							sprintf(bf,"0x%x ",ID);
							writeToLog(bf);
							parseTPSID(data);
							sprintf(bf,"\n");
							writeToLog(bf);
						}
						break;
					}
					case NOACK_LAST:
					{
						if(ln > 3 && data[1]==0)
						{
							sprintf(bf,"0x%x ",ID);
							writeToLog(bf);
							parseTPSID(data);
							sprintf(bf,"\n");
							writeToLog(bf);
						}
						break;
					}
					case tpACK:
					{
						sprintf(bf,"0x%x ACK\n",ID);
						writeToLog(bf);
						break;
					}
					case tpNACK:
					{
						sprintf(bf,"0x%x NACK\n",ID);
						writeToLog(bf);
						break;
					}
					case ACK_FOLLOW:
					{
						uint8_t buf[265];//need to make extra space for TP2 header
						for(uint8_t a=0;a<8;a++)//should be a whole frame
						{
							buf[a]=data[a];
						}
						uint32_t cnt=8;//array counter, we set it to the current array position
						uint8_t a1=0x90;
						while((a1 & 0xF0) != ACK_LAST && (a1 & 0xF0) != NOACK_LAST)
						{
							if(mode == 0)
							{
								wazzap=fileToFrame(&time2,&tmpID,&ln,tmpdata);//get the whole line
							}
							else
							{
								wazzap=logToRawFrame(&time2,&tmpID,&ln,tmpdata);//get the whole line
							}
							if(!wazzap)
							{
								break;
							}
							framesProc++;
							if(tmpID == ID && (tmpdata[0] & 0xF0) != 0xA0)//make sure to ignore channel stuff
							{
								for(uint8_t a=0;a<(ln - 1);a++)//need to skip first byte of frame to grab only the payload
								{
									buf[(cnt+a)]=tmpdata[(a+1)];
								}
								cnt=(cnt+(ln - 1));//increase counter
								a1=tmpdata[0];
							}
						}
						sprintf(bf,"0x%x with %d data bytes ->",ID,(cnt - 4));//we remove the 4 header bytes
						writeToLog(bf);
						parseTPSID(buf);
						sprintf(bf,"\n");
						writeToLog(bf);
						break;
					}
					case NOACK_FOLLOW: //lets grab the whole thing
					{
						uint8_t buf[265];//need to make extra space for TP2 header
						memset(buf, 0, 265);
						for(uint8_t a=0;a<8;a++)//should be a whole frame
						{
							buf[a]=data[a];
						}
						CANMessage can_msg(0,CANStandard); //we will now grab the rest of the stream
						uint32_t cnt=8;//array counter, we set it to the current array position
						uint8_t a1=0x90;
						while((a1 & 0xF0) != ACK_LAST && (a1 & 0xF0) != NOACK_LAST)
						{
							if(mode == 0)
							{
								wazzap=fileToFrame(&time2,&tmpID,&ln,tmpdata);//get the whole line
							}
							else
							{
								wazzap=logToRawFrame(&time2,&tmpID,&ln,tmpdata);//get the whole line
							}
							if(!wazzap)
							{
								break;
							}
							framesProc++;
							if(tmpID == ID)//make sure we dont grab the channel test as data!
							{
								if((tmpdata[0] & 0xF0) == ACK_FOLLOW  || (tmpdata[0] & 0xF0) == ACK_LAST || (tmpdata[0] & 0xF0) == NOACK_FOLLOW  || (tmpdata[0] & 0xF0) == NOACK_LAST)
								{
									for(uint8_t a=0;a<(ln - 1);a++)//need to skip first byte of frame to grab only the payload
									{
										buf[(cnt+a)]=tmpdata[(a+1)];
									}
									cnt=(cnt+(ln - 1));//increase counter
									a1=tmpdata[0];
								}
							}
						}
						sprintf(bf,"0x%x with %d data bytes ->",ID,(cnt - 4));//we remove the 4 header bytes
						writeToLog(bf);
						parseTPSID(buf);
						sprintf(bf,"\n");
						writeToLog(bf);
						break;
					}
					case BROADCAST_REQUEST:
					{

						break;
					}
					case BROADCAST_REQUEST_OK:
					{

						break;
					}
					default:
					{
						sprintf(bf,"0x%x Unknown frame type with payload ",ID);
						writeToLog(bf);
						for (uint8_t a=0;a<ln;a++)
						{
							sprintf(bf,"0x%x ",data[a]);
							writeToLog(bf);
						}
						sprintf(bf,"\n");
						writeToLog(bf);
						break;
					}
				}
				sprintf(bf,"\n");
				writeToLog(bf);
				time1=time2;//sync times
			}
		}
		else
		{
			break; //if we reach the end of the file somehow, just get out of here
		}
	}
	toggleLogging(output,0);
	file2->close();
	return framesProc;
}

void UDSLogger(uint8_t mode)
{
     device.printf("Please enter the three digits for the first Target ID address in HEX (f.ex: 123, 205, 7E0):");
     ownID=inputToHex();
     device.printf("\n");
     device.printf("Please enter the three digits for the second Target ID address in HEX (f.ex: 123, 205, 7E0):");
     rID=inputToHex();
     device.printf("\n");
     char filename[]="/Logging/tmp.txt";//create a temp file to store raw CAN data
     remove("/sd/Logging/tmp.txt");//remove it before creating a new one, but not after so we can use it too
     if (!toggleRawLogging(filename,1))
     {
         device.printf("Error creating or opening log file\n");
         return;
     }
     //can1.monitor(1);
     device.printf("Logging UDS traffic for IDs 0x%x and 0x%x to SD\n",ownID,rID);
     device.printf("Press any key to stop...\n");
     setLED(LED_ORANGE);
     uint32_t bufPointer=0;//used to know the last checked position
     BSCounter1=0;//reset the data
     BSCounter2=0;
     uint32_t frameCount=0;
     timer.reset();
     timer.start();
     can1.attach(&CANreceive1);
     if(mode == 1)
     {
 		can2.attach(&CANreceive2);
 		bridge = 1;
     }
     while(!device.readable() && counter.read() == 0)
     {
         if(BSCounter2 >0)
         {
        	 uint8_t tmpbuf[15]={0};
        	 for(uint8_t a=0;a<15;a++)
        	 {
         		if(a+bufPointer > 2010)//reset the buffer pointer to prevent overflow
         		{
         			bufPointer=0;
         		}
        		 tmpbuf[a]=BSBuffer[a+bufPointer];
        	 }
        	 BSCounter2--;
        	 bufPointer=(bufPointer + 15);
        	 while(file->write(tmpbuf,15) != 15){}
        	 frameCount++;
         }
     }
     can1.attach(0);//detach the interrupt
     if(mode == 1)
     {
 		can2.attach(0);
 		bridge = 0;
     }
     timer.stop();
     toggleRawLogging(filename,0);//close the log to save the changes
     char filename2[]="/Logging/UDS.txt";
     device.printf("Parsing UDS stream to file, please wait...\n");
     uint32_t processedFrames = parseUDSStream(filename,filename2,frameCount,0);//now we parse the file
     device.printf("Successfully parsed %d of %d total frames grabbed.\nDone!\n",processedFrames,frameCount);
     device.printf("\nDone!\n");
     dumpSerial();
     //can1.monitor(0);
     resetCAN();
     if(counter.read() >0)
     {
    	 counter.reset();
     }
     remove("/sd/Logging/tmp.txt");
     setLED(LED_GREEN);
}

void TPLogger(uint8_t mode)
{
    device.printf("Please enter the three digits for the first Target ID byte in HEX (f.ex: 123, 205, 7E0):");
    ownID=inputToHex();
    device.printf("\n");
    device.printf("Please enter the three digits for the second Target ID byte in HEX (f.ex: 123, 205, 7E0):");
    rID=inputToHex();
    device.printf("\n");
    char filename[]="/Logging/tmp.txt";//create a temp file to store raw CAN data
    remove("/sd/Logging/tmp.txt");//remove it before creating a new one, but not after so we can use it too
    if (!toggleRawLogging(filename,1))
    {
        device.printf("Error creating or opening log file\n");
        return;
    }
    //can1.monitor(1);
    device.printf("Logging TP2.0 traffic for IDs 0x%x and 0x%x to SD\n",ownID,rID);
    device.printf("Press any key to stop...\n");
    setLED(LED_ORANGE);
    uint32_t bufPointer=0;//used to know the last checked position
    BSCounter1=0;//reset the data
    BSCounter2=0;
    uint32_t frameCount=0;
    timer.reset();
    timer.start();
    can1.attach(&CANreceive1);
    if(mode == 1)
    {
		can2.attach(&CANreceive2);
		bridge = 1;
    }
    while(!device.readable() && counter.read() == 0)
    {
        if(BSCounter2 >0)
        {
       	 uint8_t tmpbuf[15]={0};
       	 for(uint8_t a=0;a<15;a++)
       	 {
        		if(a+bufPointer > 2010)//reset the buffer pointer to prevent overflow
        		{
        			bufPointer=0;
        		}
       		 tmpbuf[a]=BSBuffer[a+bufPointer];
       	 }
       	 BSCounter2--;
       	 bufPointer=(bufPointer + 15);
       	 while(file->write(tmpbuf,15) != 15){}
       	 frameCount++;
        }
    }
    can1.attach(0);//detach the interrupt
    if(mode == 1)
    {
		can2.attach(0);
		bridge = 0;
    }
    timer.stop();
    toggleRawLogging(filename,0);//close the log to save the changes
    char filename2[]="/Logging/TP2.txt";
    device.printf("Parsing TP2.0 stream to file, please wait...\n");
    uint32_t processedFrames = parseTPStream(filename,filename2,frameCount,0);//now we parse the file
    device.printf("Successfully parsed %d of %d total frames grabbed.\nDone!\n",processedFrames,frameCount);
    if(counter.read() > 0)
    {
    	counter.reset();
    }
    dumpSerial();
    //can1.monitor(0);
    resetCAN();
    remove("/sd/Logging/tmp.txt");
    setLED(LED_GREEN);
}

void parseFile()//displays the contents of an opened file on the terminal
{
    device.printf("Press any key to stop...\n");
    char ch[2] = {0};
    while (!device.readable())
    {
    	if (file->read(ch, 1) == 1)
    	{
    		device.printf("%c",ch[0]);
    		//wait(0.005);//add delay to prevent flooding the serial console
    	}
    	else
        {
            break;
        }
    }
    if(device.readable())
    {
        dumpSerial();
    }
}

/*void extractFiles(const char *fileName) //we use a lot of ram on strings here, should use strcp instead
{
    file = sd.open(fileName, O_RDONLY);
    device.printf("Analyzing log for file transfers...\n");
    char temp[512];
    int find_Upload = 0;
    int find_Download = 0;
    char str[]="Acknowledge to Upload request";//we will first check for approved Upload requests
    while(fgets(temp, 512, fp) != NULL) 
    {
        if((strstr(temp, str)) != NULL) 
        {
            find_Upload++;
        }
    }
    char str2[]="Acknowledge to Download request";//then for approved Download requests
    while(fgets(temp, 512, fp) != NULL) 
    {
        if((strstr(temp, str2)) != NULL) 
        {
            find_Download++;
        }
    }
    if(find_Upload == 0 && find_Download == 0) 
    {
        device.printf("No file transfers found in log\n");
        return;
    }
    else
    {
        device.printf("Found %d Uploads and %d Downloads\n",find_Upload,find_Download);
    }
    if(find_Upload > 0)//if we have uploads, lets get the files
    {
        uint32_t lines[512];//to store the line numbers
        uint8_t numOfLines = getLineNumber("Acknowledge to Upload request", lines);//get an array with the line numbers to grab all upload data parameters
        for (uint8_t a=0;a<numOfLines;a++)//now we dump files individually
        {
            char tmpstr[512];
            uint32_t tmpline=(lines[a] - 4);//the line where the request should be made. would be a good idea to run it in a loop instead of fixed 4
            if(getLine(tmpstr,tmpline))
            {
                if(strstr(tmpstr,"using Data Format") != NULL)//if we got the right line
                {
                    char b=0;
                    uint32_t params[3];//to later show the parameters
                    uint8_t cnt=0;//to control array
                    uint8_t cnt3=2;//lets skip the first x from the address
                    while(b != '\n')//read until end of line
                    {
                        while(b != 'x' && b != '\n')
                        {
                            b=tmpstr[cnt3];
                            cnt3++;
                        }
                        if(b == 'x')
                        {
                            char tmp[12]={0,0,0,0,0,0,0,0,0,0,0,0};
                            b=tmpstr[cnt3];
                            cnt3++;
                            uint8_t cnt2=0;
                            while(b != ' ')
                            {
                                tmp[cnt2]=b;
                                cnt2++;
                                b=tmpstr[cnt3];
                                cnt3++;
                            }
                            device.printf(tmp);
                            device.printf("\n");
                            params[cnt] = atoh<uint32_t>(tmp);//now we store the hex value in the array
                            cnt++;
                        }
                    }
                    device.printf("Will dump file with start at 0x%x and length of 0x%x bytes\n",params[0],params[2]);
                        
                }
            }
            
            
        }
        
    }
    if(find_Download > 0)//if we have downloads, lets get the files too
    {
        
    }
    
} */

bool TPUpload(uint32_t tAddress, uint32_t tLength, uint8_t mode)
{
	if(mode == 1)
	{
		device.printf("Requesting Upload from address 0%x with length 0x%x\n", tAddress, tLength);
	}
	uint8_t data[256]={REQUEST_UPLOAD,(tAddress >> 16), (tAddress >> 8), tAddress, 0, (tLength >> 16), (tLength >> 8), tLength };
	uint8_t buffer[256];
	uint8_t len=TPResponseHandler(data,8,buffer,0,1);
	if (len == 0)
	{
	    return 0;
	}
	wait(0.01);
	if(mode == 0)
	{
		uint8_t blah[8] ={REQUEST_TRANSFER_EXIT};
		TPResponseHandler(blah,1,buffer,1,1);
		return 1;
	}
	device.printf("Upload Accepted, will be performed in chunks of 0x%x bytes\n", buffer[1]);
	char tmpbuf[32] = {0};
	char filename[64] = "/MemDumps/";
	sprintf(tmpbuf,"U_%x_%x_%x.bin",rID,tAddress,tLength);
	strngcat (filename,tmpbuf);
	if (!toggleRawLogging(filename,1))//we need to have the file ready or we will miss frames
	{
		return 0;
	}
	uint32_t cnt = 0;
	while(cnt<tLength)
	{
		uint8_t canIHaz[8] = {TRANSFER_DATA};
		uint8_t got = TPResponseHandler(canIHaz,1,buffer,1,1);
		wait(0.01);
		if(got == 0)//if something goes wrong
		{
			break;
		}
		cnt = (cnt + (got - 1));
		for (uint16_t a=0;a<(got - 1);a++)
		{
			buffer[a] = buffer[(a+1)]; //move everything one byte to the left to get the data without the SID reply
		}
		file->write(buffer, (got - 1));//write to the file
	}
	uint8_t blah[8] ={REQUEST_TRANSFER_EXIT};
	TPResponseHandler(blah,1,buffer,1,1);
	if(cnt == tLength)
	{
		device.printf("Done!\n\n");
	}
	else
	{
		device.printf("Upload partially completed due to error\n\n");
	}
	toggleRawLogging(filename,0);
	return 1;

}

void scanLogProtocols()
{
	device.printf("\nChecking if CAN log exists...\n");
    char filename[]="/Logging/CAN.txt";
	if(!doesFileExist(filename))
	{
		device.printf("Log does not exist, please log data to add to emulator\n");
		return;
	}
	device.printf("Log found, Scanning for protocols...\n\n");
	TPProtocolScanFromLog(filename);
	UDSProtocolScanFromLog(filename);
	device.printf("Finished Scanning for protocols\n\n");

}

void logManager()
{
    device.printf("\nPlease, select an action:\n");  
    device.printf("\n");
    device.printf("1)View log\n");
    device.printf("2)Generate TP2.0 log from CAN log\n");
    device.printf("3)Generate UDS log from CAN log\n");
    device.printf("4)Scan CAN log for protocols\n");
    device.printf("5)Delete log\n");
    device.printf("\n");
    while(!device.readable()){ }//Thread::wait(10); }
    char option=device.getc();
    char option2=0;
    dumpSerial();
    if(option == '1' || option == '5')
    {
		device.printf("\nPlease, select the log type:\n");
		device.printf("\n");
		device.printf("1)TP 2.0 log\n");
		device.printf("2)UDS log\n");
		device.printf("3)CAN log\n");
		device.printf("\n");
		while(!device.readable()){}//Thread::wait(10); }
		option2=device.getc();
		dumpSerial();
    }
    switch(option)//extremely dirty, need to remake code with pointers or strcp
    {
        case '1':
        {
            switch(option2)
            {
                case '1':
                {
                    char filename[]="/Logging/TP2.txt";
                    if(doesFileExist(filename))
                    {
                        file = sd.open(filename, O_RDONLY);
                        parseFile();
                        file->close();
                    }
                    else
                    {
                        device.printf("Log does not exist\n"); 
                    }
                    break;
                }
                case '2':
                {
                    char filename[]="/Logging/UDS.txt";
                    if(doesFileExist(filename))
                    {
                        file = sd.open(filename, O_RDONLY);
                        parseFile();
                        file->close();
                    }
                    else
                    {
                        device.printf("Log does not exist\n"); 
                    }
                    break;
                }
                case '3':
                {
                    char filename[]="/Logging/CAN.txt";
                    if(doesFileExist(filename))
                    {
                        file = sd.open(filename, O_RDONLY);
                        parseFile();
                        file->close();
                    }
                    else
                    {
                        device.printf("Log does not exist\n"); 
                    }
                    break;
                }
                default:
                {
                    device.printf("Unknown option\n");
                    break;
                }
            }
            device.printf("\nDone\n");
            break;
        }
        case '2':
        {
            device.printf("Please enter the three digits for the First TP2.0 ID (f.ex: 200, 300, 0A0):\n");
            rID=inputToHex();
            device.printf("Please enter the three digits for the Second TP2.0 ID (f.ex: 200, 300, 0A0):\n");
            ownID=inputToHex();
        	device.printf("Parsing CAN log for IDS 0x%x and 0x%x to TP2.0 stream log, please wait...\n",rID, ownID);
        	char filename[]="/Logging/CAN.txt";
        	char filename2[]="/Logging/TP2.txt";
        	uint32_t frameCount = getFrameCount(filename);
        	uint32_t processedFrames = parseTPStream(filename,filename2,frameCount,1);//now we parse the file
        	device.printf("Successfully parsed %d of %d total frames grabbed.\nDone!\n",processedFrames,frameCount);
        	break;
        }
        case '3':
        {
            device.printf("Please enter the three digits for the First UDS ID (f.ex: 200, 300, 0A0):\n");
            rID=inputToHex();
            device.printf("Please enter the three digits for the Second UDS ID (f.ex: 200, 300, 0A0):\n");
            ownID=inputToHex();
            device.printf("Checking CAN log, please wait...\n");
        	char filename[]="/Logging/CAN.txt";
        	char filename2[]="/Logging/UDS.txt";
        	uint32_t frameCount = getFrameCount(filename);
        	device.printf("Parsing CAN log with %d frames for IDS 0x%x and 0x%x to UDS stream log, please wait...\n", frameCount ,rID, ownID);
        	uint32_t processedFrames = parseUDSStream(filename,filename2,frameCount,1);//now we parse the file
        	device.printf("Successfully parsed %d of %d total frames grabbed.\nDone!\n",processedFrames,frameCount);
        	break;
        }
        case '4':
        {
        	scanLogProtocols();
        	break;
        }
        case '5':
        {
              switch(option2)
            {
                case '1':
                {
                    if(doesFileExist("/Logging/TP2.txt"))
                    {
                        remove("/sd/Logging/TP2.txt");
                        device.printf("TP2 log removed\n");
                    }
                    else
                    {
                        device.printf("Log does not exist\n"); 
                    }
                    break;
                }
                case '2':
                {
                    if(doesFileExist("/Logging/UDS.txt"))
                    {
                        remove("/sd/Logging/UDS.txt");
                        device.printf("UDS log removed\n");
                    }
                    else
                    {
                        device.printf("Log does not exist\n"); 
                    }
                    break;
                }
                case '3':
                {
                    if(doesFileExist("/Logging/CAN.txt"))
                    {
                        remove("/sd/Logging/CAN.txt");
                        remove("/sd/Logging/CANRAW.txt");
                        device.printf("CAN log removed\n");
                    }
                    else
                    {
                        device.printf("Log does not exist\n"); 
                    }
                    break;
                }
                default:
                {
                    device.printf("Unknown option\n");
                    break;
                }
            }
            break;
        }

        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }           
                 
    
    
}

void CANLogger(uint8_t mode, bool writeSdLog)
{
	if(mode != 2)
	{
		ownID=0;
	}
	rID=0;
	if(uartMode || writeSdLog) {
		char filename[]="/Logging/tmp.txt";//create a temp file to store raw CAN data
		remove("/sd/Logging/tmp.txt");//remove it before creating a new one, but not after so we can use it too
		if (!toggleRawLogging(filename,1))
		{
			blinkLED(5,LED_RED);
			device.printf("Error creating or opening log file\n");
			setLED(LED_GREEN);
		   return;
		}
		//can1.monitor(1);
		device.printf("Logging all CAN traffic to SD\n");
		device.printf("Press any key to stop...\n");
		setLED(LED_ORANGE);
		uint32_t bufPointer=0;//used to know the last checked position
		BSCounter1=0;//reset the data
		BSCounter2=0;
		uint32_t frameCount=0;
		timer.reset();
		timer.start();
		counter.reset();
		uint32_t ramPointer=0;//used in mode 2
		can1.attach(&CANreceive1);
		if(mode == 1)
		{
			can2.attach(&CANreceive2);
			bridge = 1;
		}
		while(!device.readable() && counter.read() == 0)
		{
			 if(BSCounter2 >0)//if we have frames stored in the buffer
			 {
				uint8_t tmpbuf[15]={0};
				for(uint8_t a=0;a<15;a++)
				{
					if((a+bufPointer) > 2010)//reset the buffer pointer to prevent overflow
					{
						bufPointer=0;
					}
					tmpbuf[a]=BSBuffer[(a+bufPointer)];
				}
				BSCounter2--;
				bufPointer=(bufPointer + 15);
				while(file->write(tmpbuf,15) != 15){}
				frameCount++;
			 }
			 if(mode ==  2)
			 {
				 uint8_t data[256]={0};
				 uint8_t len[2] = {0};
				 ram_read(ramPointer,len,1);
				 if(len[0] == 0)
				 {
					 ramPointer=0;
					 ram_read(ramPointer,len,1);
				 }
				 ramPointer++;
				 ram_read(ramPointer,data,len[0]);
				 UDSWrite(data,len[0],1);
				 wait_ms(20);
				 ramPointer = (ramPointer + len[0]);
			 }

		}
		can1.attach(0);//detach the interrupt
		if(mode == 1)
		{
			can2.attach(0);
			bridge = 0;
		}
		timer.stop();
		toggleRawLogging(filename,0);//close the log to save the changes
		char filename2[]="/Logging/CAN.txt";
		device.printf("Parsing CAN stream to file, please wait...\n");
		uint32_t processedFrames = parseCANStream(filename,filename2,frameCount);//now we parse the file
		dumpSerial();
		//can1.monitor(0);
		resetCAN();
		rename("/sd/Logging/tmp.txt","/sd/Logging/CANRAW.txt");
		device.printf("Successfully parsed %d of %d total frames grabbed.\nDone!\n",processedFrames,frameCount);
		if(counter.read() > 0)
		{
			counter.reset();
		}

	} else {
		clearRAM();
		settings->currentActionIsRunning = true;
		setLED(LED_ORANGE);
		uint32_t bufPointer=0;//used to know the last checked position
		BSCounter1=0;//reset the data
		BSCounter2=0;
		uint32_t frameCount=0;
		timer.reset();
		timer.start();
		can1.attach(&CANreceive1);
		if(mode == 1)
		{
			can2.attach(&CANreceive2);
			bridge = 1;
		}
		while(settings->currentActionIsRunning)
		{
			if(counter.read() != 0)
			{
				break;
			}
			if(BSCounter2 >0)//if we have frames stored in the buffer
			 {
				uint8_t tmpbuf[15]={0};
				for(uint8_t a=0;a<15;a++)
				{
					if((a+bufPointer) > 2010)//reset the buffer pointer to prevent overflow
					{
						bufPointer=0;
					}
					tmpbuf[a]=BSBuffer[(a+bufPointer)];
				}
				BSCounter2--;
				bufPointer=(bufPointer + 15);
				//while(file->write(tmpbuf,15) != 15){}
				ethernetManager->sendRamFrame(DATA, (char*)tmpbuf, (uint32_t) 15);
				frameCount++;
			 }
		}
		can1.attach(0);//detach the interrupt
		if(mode == 1)
		{
			can2.attach(0);
			bridge = 0;
		}
		timer.stop();
		resetCAN();
		if(counter.read() > 0)
		{
			counter.reset();
		}
	}
	setLED(LED_GREEN);
}

void TPTransferFileDump(uint32_t ID)
{
	uint8_t buf[260]={0};//to store each transfer
	uint8_t cnt=0;//to keep track of the array length
	uint8_t lng = 0;//to store the payload length
	uint32_t ID3=0;
	uint32_t time2=0;
	uint8_t len2=0;
	uint8_t data2[8];
	while(logToRawFrame(&time2,&ID3,&len2,data2))//while we still have frames
	{
		if(device.readable() ||(len2 ==4 && (data2[3] == 0x37 || data2[3] == 0x77) &&  data2[1] == 0x00 && data2[2] == 0x01) )//if we get a transfer exit
		{
			return;
		}//if we got something to interrupt it, do nothing so we get out of here
		else
		{
			if(ID3 == ID && (data2[3] == 0x36 || data2[3] == 0x76) && data2[0] != CHANNEL_TEST && data2[0] != PARAMETER_REQUEST_OK && data2[0] != PARAMETERS_REQUEST)//filter out all the unnecessary stuff
			{
				for (cnt=0;cnt<(len2 - 4);cnt++)//save the first part
				{
					buf[cnt]=data2[(cnt+4)];
				}
				lng = (data2[2] - 1);//store the real payload length
				while(cnt<lng)
				{
					if(!logToRawFrame(&time2,&ID3,&len2,data2))
					{
						return;
					}
					if(ID3 == ID && (data2[0] & 0xF0) != ACK && data2[0] != CHANNEL_TEST && data2[0] != PARAMETER_REQUEST_OK && data2[0] != PARAMETERS_REQUEST)
					{
						for (uint8_t a=0;a<(len2 - 1);a++)//move the payload into the buffer
						{
							buf[cnt]=data2[(a+1)];
							cnt++;
						}
					}
				}
				file->write(buf, lng);
				file->fsync();
			}
		}
	}

}

uint32_t LVL3AuthExtract(uint32_t seed, uint32_t key)//simple, right?
{
	return(key - seed);
}

bool UDSSetSession(uint8_t SessionType, uint8_t verbose)
{
  uint8_t data[8]={0x10,SessionType};
  uint8_t buffer[256];
  uint8_t len=UDSResponseHandler(data,2,buffer,1,1);
  if (len != 0 && verbose != 0)
  {
      device.printf("Session was stablished correctly with parameter 0x%x!\n",SessionType);
      return 1;
  }
  else if(len != 0 && verbose == 0)
  {
	  return 1;
  }
  resetCAN();
  return 0;

}

bool TPSetSession(uint8_t SessionType, uint8_t verbose)
{
  uint8_t data[256]={0x10,SessionType};
  uint8_t buffer[256];
  uint8_t len=TPResponseHandler(data,2,buffer,1,1);
  if (len !=0 && buffer[0]==0x50 && verbose !=0)
  {
      device.printf("Session was stablished correctly!\n");
      return 1;
  }
  else if (len !=0 && buffer[0]==0x50 && verbose ==0)
  {
	  return 1;
  }
  return 0;
}

void TPTransferDump()//need to fix after removing filters
{
		 device.printf("Please enter the first Target ID byte in HEX (f.ex: 123, 205, 7E0):");
	     uint32_t ID1=inputToHex();
	     device.printf("\n");
	     device.printf("Please enter the second Target ID byte in HEX (f.ex: 123, 205, 7E0):");
	     uint32_t ID2=inputToHex();
	     device.printf("\n");
	     char filename[]="/Logging/CAN.txt";
	 	if(!doesFileExist(filename))
	 	{
	 		device.printf(" CAN Log does not exist!\n");
	 		return;
	 	}
	 	file2 = sd.open(filename, O_RDONLY);//open source file
	     device.printf("Checking CAN log for TP 2.0 transfers for IDs 0x%x and 0x%x\n",ID1,ID2);
	     device.printf("Press any key to stop...\n");
	 	if (file2 == NULL)
	 	{
	 		device.printf("Error opening source file file\n");
	 		return;
	 	}
	     led4 = !led4;
	     uint8_t buf[32];
	     uint8_t requestType;
	     uint8_t len=0;
	     uint8_t data[8];
	     uint32_t ID;
	     uint32_t time1=0;//dummy
	     if (!toggleDump("/Transfers/TPDump.bin",1))//we need to have the file ready or we will miss frames. Well, not really since we now read from the log :D
	     {
	     	return;
	     }
	     while(logToRawFrame(&time1,&ID,&len,data))//while we still have frames
	     {
	    	 if(device.readable())
			 {
	    		 device.printf("Canceled by user\n");
	    		 dumpSerial();
	    		 break;
			 }
			 else
			 {
				if(ID == ID1 || ID == ID2)//if they are the target IDs. always gonna be positive if we set filters anyway
				{
					uint8_t streamType = (data[0] & 0xF0);
					if (streamType == NOACK_FOLLOW && data[1] == 0 && (data[3] == REQUEST_DOWNLOAD || data[3] == REQUEST_UPLOAD)) //lets grab the whole thing
					{
						requestType = data[3];
						uint32_t tmpID=ID;
						uint32_t tmpID2;
						if(tmpID == ID1)
						{
							tmpID2 = ID2;
						}
						else
						{
							tmpID2 = ID1;
						}
						for(uint8_t a=0;a<8;a++)//should be a whole frame
						{
							buf[a]=data[a];
						}
						if(!logToRawFrame(&time1,&ID,&len,data))
						{
							break;
						}
						bool finished=0;
						while(ID != tmpID)
						{
							if(!logToRawFrame(&time1,&ID,&len,data))
							{
								finished=1;
								break;
							}
						}
						if(finished)
						{
							break;
						}
						for(uint8_t a=0;a<(len - 1);a++)//need to skip first byte of frame to grab only the payload
						{
							buf[(8+a)]=data[(a+1)];
						}//now we have the request
						uint32_t addr=buf[4];
						addr=(addr<<8)+buf[5];
						addr=(addr<<8)+buf[6];
						uint32_t lng=buf[8];
						lng=(lng<<8)+buf[9];
						lng=(lng<<8)+buf[10];
						device.printf("Found a request for address 0x%x using Data Format Identifier 0x%x and length of 0x%x bytes\n",addr,buf[7],lng);

						while(ID != tmpID2 || (data[0] & 0xF0) == ACK)
						{
							if(!logToRawFrame(&time1,&ID,&len,data))
							{
								finished=1;
								break;
							}
						}
						if(finished)
						{
							break;
						}
						if(data[3] == (REQUEST_DOWNLOAD + 0x40) || data[3] == (REQUEST_UPLOAD + 0x40))//if upload/download accepted
						{
							char filename[32]={0};
							char tmpbuf[32]={0};
							if(requestType == REQUEST_DOWNLOAD)
							{
								sprintf(tmpbuf,"D_%x_%x_%x",ID,addr,lng);
							}
							else
							{
								sprintf(tmpbuf,"U_%x_%x_%x",ID,addr,lng);
							}
							strngcat (filename,tmpbuf);
							file->write(filename, 32);//write header to file (32 bytes)
							if(requestType == REQUEST_DOWNLOAD)
							{
								TPTransferFileDump(ID);
							}
							else
							{
								device.printf("Now dumping to file %s\n",tmpbuf);
								if(ID==ID1)
								{
									TPTransferFileDump(ID2);
								}
								else
								{
									TPTransferFileDump(ID1);
								}
							}
						}
					}
				}
					 //end of if
			}

	     }
	     led4 = !led4;
	     toggleDump("/Transfers/TPDump.bin",0);
	     resetCAN();
}

void UDSTransferDump()//need to fix after removing filters
{
		 device.printf("Please enter the first UDS Target ID byte in HEX (f.ex: 123, 205, 7E0):");
	     uint32_t ID1=inputToHex();
	     device.printf("\n");
	     device.printf("Please enter the second UDS Target ID byte in HEX (f.ex: 123, 205, 7E0):");
	     uint32_t ID2=inputToHex();
	     device.printf("\n");
	     char filename[]="/Logging/CAN.txt";
	 	if(!doesFileExist(filename))
	 	{
	 		device.printf(" CAN Log does not exist!\n");
	 		return;
	 	}
	 	file2 = sd.open(filename, O_RDONLY);//open source file
	     device.printf("Checking CAN log for UDS transfers for IDs 0x%x and 0x%x\n",ID1,ID2);
	     device.printf("Press any key to stop...\n");
	 	if (file2 == NULL)
	 	{
	 		device.printf("Error opening source file file\n");
	 		return;
	 	}
	     led4 = !led4;
	     uint8_t buf[32];
	     uint8_t requestType;
	     uint8_t len=0;
	     uint8_t data[8];
	     uint32_t ID;
	     uint32_t time1=0;//dummy
	     if (!toggleDump("/Transfers/UDSDump.bin",1))//we need to have the file ready or we will miss frames. Well, not really since we now read from the log :D
	     {
	     	return;
	     }
	     while(logToRawFrame(&time1,&ID,&len,data))//while we still have frames
	     {
	    	 if(device.readable())
			 {
	    		 device.printf("Canceled by user\n");
	    		 dumpSerial();
	    		 break;
			 }
			 else
			 {
				if(ID == ID1 || ID == ID2)//if they are the target IDs.
				{
					uint8_t streamType = (data[0] & 0xF0);

					if (streamType == 0x00 && (data[1] == READ_MEMORY_BY_ADDRESS || data[1] == WRITE_MEMORY_BY_ADDRESS)) //lets grab the whole thing
					{
						requestType = data[1];
						uint32_t tmpID=ID;
						uint32_t tmpID2;
						if(tmpID == ID1)
						{
							tmpID2 = ID2;
						}
						else
						{
							tmpID2 = ID1;
						}
						for(uint8_t a=0;a<8;a++)//should be a whole frame
						{
							buf[a]=data[a];
						}
						if(!logToRawFrame(&time1,&ID,&len,data))
						{
							break;
						}
						bool finished=0;
						while(ID != tmpID2)
						{
							if(!logToRawFrame(&time1,&ID,&len,data))
							{
								finished=1;
								break;
							}
						}
						if(finished)
						{
							break;
						}
						uint8_t addrlength= (buf[2] & 0x0F);//grab the address length
						uint32_t addr=0;
						for(uint8_t a=0;a<addrlength;a++)//get the address!
						{
							addr=(addr<<8)+buf[(3 + a)];
						}
						uint8_t blockSize=buf[(4 + addrlength)];
						device.printf("Found a request for address 0x%x using blocks of 0x%x bytes\n",addr,blockSize);
						while(ID == tmpID)
						{
							if(!logToRawFrame(&time1,&ID,&len,data))
							{
								finished=1;
								break;
							}
						}
						if(finished)
						{
							break;
						}
						if(data[2] == (READ_MEMORY_BY_ADDRESS + 0x40) || data[2] == (WRITE_MEMORY_BY_ADDRESS + 0x40))//if upload/download accepted
						{
							led3 = !led3;
							char filename[32]={0};
							char tmpbuf[32]={0};
							if(requestType == READ_MEMORY_BY_ADDRESS)
							{
								sprintf(tmpbuf,"R_%x_%x",ID,addr);
							}
							else
							{
								sprintf(tmpbuf,"W_%x_%x",ID,addr);
							}
							strngcat (filename,tmpbuf);
							file->write(filename, 32);//write header to file (32 bytes)
							if(requestType == REQUEST_DOWNLOAD)
							{
								TPTransferFileDump(ID);
							}
							else
							{
								device.printf("Now dumping to file %s\n",tmpbuf);
								if(ID==ID1)
								{
									TPTransferFileDump(ID2);
								}
								else
								{
									TPTransferFileDump(ID1);
								}
							}
							led3 = !led3;
						}
					}
				}
					 //end of if
			}

	     }
	     led4 = !led4;
	     toggleDump("/Transfers/UDSDump.bin",0);
	     resetCAN();
}

uint8_t UDSSecurityHijack(uint8_t level, uint8_t sessionType)
{
	if(level == 0)
	{
		device.printf("\nWaiting for Security Access on any level for IDS 0x%x and 0x%x... Press any key to abort\n",ownID,rID);
	}
	else
	{
		device.printf("\nWaiting for Security Access on level 0x%x for IDS 0x%x and 0x%x... Press any key to abort\n",level,ownID,rID);
	}
	CANMessage can1_msg(0,CANStandard);
	CANMessage can2_msg(0,CANStandard);
	uint8_t pwn=0;//used as a counter for actions
	uint8_t lvl = 0;//to grab the security level
	uint8_t cnnt=0;//to count frames and discard a false channel negociation
	uint32_t seed=0;
	uint32_t key=0;
	while(!device.readable())
	{
		if(can1.read(can1_msg) && can1_msg.id == rID)//target side
		{
			if(pwn == 1)//grab the reply to request
			{
				if(can1_msg.data[0] == 0x6 && can1_msg.data[1] == 0x67 && can1_msg.data[2] == lvl)
				{
					seed = can1_msg.data[3];
					seed = ((seed << 8) + can1_msg.data[4]);
					seed = ((seed << 8) + can1_msg.data[5]);
					seed = ((seed << 8) + can1_msg.data[6]);
					pwn++;
				}
			}
			else if(pwn == 3)//grab the reply to key
			{
				if(can1_msg.data[0] == 0x2 && can1_msg.data[1] == 0x67 && can1_msg.data[2] == (lvl + 1))
				{
					if(lvl == 3)
					{
						uint32_t goodie = LVL3AuthExtract(seed,key);
						device.printf("\nPossible key used for level 0x%x: 0x%x\n", lvl, goodie);
					}
					return lvl;//we return the level we got access to
				}
				else if(can1_msg.data[0] == 0x3 && can1_msg.data[1] == 0x7F && can1_msg.data[2] == 0x27 && can1_msg.data[3] == 0x35)//authentication failed
				{
					pwn = 0;
					seed = 0;
					key = 0;
					cnnt = 0;
				}
			}
			if(pwn > 0)
			{
				cnnt++;
				if (cnnt > 100)//timeout
				{
					pwn = 0;
					cnnt = 0;
				}
			}
			uint8_t timeout=0;
			while (!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && !device.readable() && timeout < 10)
			{
				timeout++;
			}//make sure the msg goes out
		}
		if(can2.read(can2_msg) && can2_msg.id == ownID)//tool side
		{
			if(pwn  == 0)//first we need to grab the request
			{
				if(can2_msg.data[0] == 0x2 && can2_msg.data[1] == 0x27 && level == 0)
				{
					if(sessionType !=0)
					{
						UDSSetSession(sessionType,0);
					}
					pwn++;
					lvl = can2_msg.data[2];
				}
				else if(can2_msg.data[0] == 0x2 && can2_msg.data[1] == 0x27 && level == can2_msg.data[2])
				{
					pwn++;
					lvl = can2_msg.data[2];
				}
			}
			else if(pwn == 2)//then we grab the reply from tool
			{
				if(can2_msg.data[0] == 0x6 && can2_msg.data[1] == 0x27 && can2_msg.data[2] == (lvl + 1))
				{
					key = can2_msg.data[3];
					key = ((key << 8) + can2_msg.data[4]);
					key = ((key << 8) + can2_msg.data[5]);
					key = ((key << 8) + can2_msg.data[6]);
					pwn++;
				}
			}
			if(pwn > 0)
			{
				cnnt++;
				if (cnnt > 100)//timeout
				{
					pwn = 0;
					cnnt = 0;
				}
			}
			  uint8_t timeout=0;
			  while (!can1.write(CANMessage(can2_msg.id, reinterpret_cast<char*>(&can2_msg.data), can2_msg.len)) && timeout < 10)
			  {
				  timeout++;
			  }//make sure the msg goes out
		}
	}
	dumpSerial();
	device.printf("\nAborted by user\n");
	return 0;
}

uint8_t TPSecurityHijack(uint8_t level, uint8_t sessionType)
{
	if(level == 0)
	{
		device.printf("\nWaiting for Security Access on any level for IDS 0x%x and 0x%x... Press any key to abort\n",ownID,rID);
	}
	else
	{
		device.printf("\nWaiting for Security Access on level 0x%x for IDS 0x%x and 0x%x... Press any key to abort\n",level,ownID,rID);
	}
	CANMessage can1_msg(0,CANStandard);
	CANMessage can2_msg(0,CANStandard);
	uint8_t pwn=0;//used as a counter for actions
	uint8_t lvl = 0;//to grab the security level
	uint8_t cnnt=0;//to count frames and discard a false channel negociation
	uint32_t seed=0;
	uint32_t key=0;
	while(!device.readable())
	{
		if(can1.read(can1_msg))//target side
		{
			if(can1_msg.id == rID)
			{
				if(pwn == 1)//grab the reply to request
				{
					if(can1_msg.data[1] == 0x0 && can1_msg.data[2] == 0x6 && can1_msg.data[3] == 0x67 && can1_msg.data[4] == lvl)
					{
						seed = can1_msg.data[5];
						seed = ((seed << 8) + can1_msg.data[6]);
						seed = ((seed << 8) + can1_msg.data[7]);
						uint8_t timeout=0;
						while (!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && !device.readable() && timeout < 10)
						{
							timeout++;
						}//make sure the msg goes out
						while(!can1.read(can1_msg) || can1_msg.id != rID){}//wait for the next one
						seed = ((seed << 8) + can1_msg.data[1]);
						pwn++;
					}
				}
				else if(pwn == 3)//grab the reply to key
				{
					if(can1_msg.data[3] == 0x67 && can1_msg.data[4] == (lvl + 1))
					{
						if(lvl == 3)
						{
							uint32_t goodie = LVL3AuthExtract(seed,key);
							device.printf("\nPossible key used for level 0x%x: 0x%x\n", lvl, goodie);
						}
						if((can1_msg.data[0] & 0xF0) == 0x10)//if the msg has an ack request
						{
							uint8_t ackCounter = (can1_msg.data[0]& 0x0F);//Store the counter for ACK
							TPSendACK(ackCounter, 1);
						}
						return lvl;//we return the level we got access to
					}
					else if(can1_msg.data[3] == 0x7F && can1_msg.data[4] == 0x27 && can1_msg.data[5] == 0x35)//authentication failed
					{
						pwn = 0;
						seed = 0;
						key = 0;
						cnnt = 0;
					}
				}
				if(pwn > 0)
				{
					cnnt++;
					if (cnnt > 100)//timeout
					{
						pwn = 0;
						cnnt = 0;
					}
				}
			}
			uint8_t pp=0;//just for timeout
			while (!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && pp<3)//retry to send it three times
			{
				pp++;
			}//make sure the msg goes out
		}
		if(can2.read(can2_msg))//tool side
		{
			if(can2_msg.id == ownID)
			{
				if(pwn  == 0)//first we need to grab the request
				{
					if(can2_msg.data[1] == 0x0 && can2_msg.data[2] == 0x2 && can2_msg.data[3] == 0x27 && level == 0)
					{
						pwn++;
						lvl = can2_msg.data[4];
						if(sessionType !=0)
						{
							TPSetSession(sessionType,0);
						}
					}
					else if(can2_msg.data[1] == 0x0 && can2_msg.data[2] == 0x2 && can2_msg.data[3] == 0x27 && level == can2_msg.data[4])
					{
						pwn++;
						lvl = can2_msg.data[4];
						if(sessionType !=0)
						{
							TPSetSession(sessionType,0);
						}
					}
				}
				else if(pwn == 2)//then we grab the reply from tool
				{
					if(can2_msg.data[1] == 0x0 && can2_msg.data[2] == 0x6 && can2_msg.data[3] == 0x27 && can2_msg.data[4] == (lvl + 1))
					{
						key = can2_msg.data[5];
						key = ((key << 8) + can2_msg.data[6]);
						key = ((key << 8) + can2_msg.data[7]);
						uint8_t timeout=0;
						while (!can1.write(CANMessage(can2_msg.id, reinterpret_cast<char*>(&can2_msg.data), can2_msg.len)) && timeout < 10)
						{
							timeout++;
						}//make sure the msg goes out
						while(!can2.read(can2_msg) || can2_msg.id != ownID){}//wait for the next one
						key = ((key << 8) + can2_msg.data[1]);//grab the last bit of the key
						frameCounter = (can2_msg.data[0] & 0x0F);
						if(frameCounter == 0xF)
						{
							frameCounter = 0;
						}
						else
						{
							frameCounter++;
						}
						pwn++;
					}
				}
				if(pwn > 0)
				{
					cnnt++;
					if (cnnt > 100)//timeout
					{
						pwn = 0;
						cnnt = 0;
					}
				}
			}
			uint8_t pp=0;//just for timeout
			while (!can1.write(CANMessage(can2_msg.id, reinterpret_cast<char*>(&can2_msg.data), can2_msg.len)) && pp<3)
			{
				pp++;
			}//make sure the msg goes out
		}
	}
	dumpSerial();
	device.printf("\nAborted by user\n");
	return 0;
}

void CANBridge()
{
	device.printf("\nCAN Bridge mode started, press any key to stop\n");
	CANMessage can1_msg(0,CANStandard);
	CANMessage can2_msg(0,CANStandard);
	while(!device.readable())
	{
		if(can1.read(can1_msg))
		{
			uint8_t timeout=0;
			while(!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && !device.readable() && timeout < 10)
			{
				timeout++;
			}//make sure the msg goes out
		}
		if(can2.read(can2_msg))
		{
			uint8_t timeout=0;
			while(!can1.write(CANMessage(can2_msg.id, reinterpret_cast<char*>(&can2_msg.data), can2_msg.len)) && !device.readable() && timeout < 10)
			{
				timeout++;
			}//make sure the msg goes out
		}
	}
	dumpSerial();
	device.printf("\nAborted by user\n");
}

uint32_t grabASCIIValue()
{
	char tmp[8]={0};
	char chh[2]={0};
	uint8_t cnnt = 0;
	while(chh[0] != ',' && chh[0] != 0x0A && cnnt < 7)//read a value until separator or until EOL
	{
		uint8_t aa = file->read(chh, 1);
		if(aa != 1)
		{
			return 0xFFFFFFFF;//end of file
		}
		if(chh[0] != 0x0D && chh[0] != 0x0A)
		{
			tmp[cnnt] = chh[0];
		}
		cnnt++;
	}
	return atoh<uint32_t>(tmp);
}

uint32_t tableLookUp(uint32_t canID)//will return the pointer to the address where the rules are stored.
{
	uint32_t a=0;
	while(a < 0x800)
	{
		uint32_t checkedID = BSBuffer[a];
		checkedID = ((checkedID << 8) + BSBuffer[a+1]);
		if(canID == checkedID)
		{
			uint32_t b = BSBuffer[a+2];
			b = ((b << 8) + BSBuffer[a+3]);
			b = ((b << 8) + BSBuffer[a+4]);
			return b;
		}
		else if(checkedID ==  0xFFFF)
		{
			return 0xFFFFFFFF;//if not found, return this value
		}
		a = a + 5;
	}
	return 0xFFFFFFFF;//if not found, return this value
}

bool addRule(uint32_t offset, uint32_t cType, uint8_t *tPayload, uint32_t Action, uint8_t *aPayload)//checks if rules already exists and adds them if they dont
{
	uint32_t c=0;
	while(c < 102)//max number of rules per ID to be able to store them in IRAM when fetching later
	{
		ram_read((offset + (c * 20)),BSBuffer,20);//grab a rule from RAM
		uint16_t condCheck= BSBuffer[0];
		condCheck= ((condCheck << 8) + BSBuffer[1]);
		if(condCheck == 0xFFFF)//free space for rule, assuming it was not found earlier
		{
			uint8_t ruleW[22]={(cType >> 8), cType, tPayload[0], tPayload[1], tPayload[2], tPayload[3], tPayload[4], tPayload[5], tPayload[6], tPayload[7],(Action >> 8), Action, aPayload[0], aPayload[1],aPayload[2],aPayload[3],aPayload[4],aPayload[5],aPayload[6], aPayload[7], 0xFF, 0xFF};
			ram_write((offset + (c * 20)), ruleW, 22);//write the rule with the two termination bytes
			return 1;
		}
		else //check if the rule was already added
		{
			uint8_t tPayloadA[8] = {BSBuffer[2],  BSBuffer[3], BSBuffer[4], BSBuffer[5], BSBuffer[6], BSBuffer[7], BSBuffer[8], BSBuffer[9]};
			uint32_t ActionA = BSBuffer[10];
			ActionA = ((ActionA << 8) + BSBuffer[11]);
			uint8_t aPayloadA[8] = {BSBuffer[12],  BSBuffer[13], BSBuffer[14], BSBuffer[15], BSBuffer[16], BSBuffer[17], BSBuffer[18], BSBuffer[19]};
			if(cType == condCheck && Action == ActionA && memcmp(tPayload,tPayloadA,8) == 0 && memcmp(aPayload,aPayloadA,8) == 0)//if rule already exists
			{
				return 1;
			}
		}
		c++;
	}
	return 0;
}

uint32_t getLastRuleOffset()
{
	uint32_t a=0;
	while(a < 0x800)
	{
		uint32_t checkedID = BSBuffer[a];
		checkedID = ((checkedID << 8) + BSBuffer[a+1]);
		if(checkedID ==  0xFFFF)
		{
			return a;
		}
		a = a + 5;
	}
	return 0xFFFFFFFF;//if no more space left, return this value
}

uint32_t allocRAM(uint32_t ttargetID, uint32_t tOffset)
{
	uint32_t maddr = 0;
	if(tOffset != 0)
	{
		ram_read((tOffset - 5), BSBuffer, 5);
		maddr = BSBuffer[2];
		maddr = ((maddr << 8) + BSBuffer[3]);
		maddr = ((maddr << 8) + BSBuffer[4]);
		maddr = (maddr + 2048);
		uint8_t allocdata[5]={(ttargetID >> 8), ttargetID, (maddr >> 16), (maddr >> 8), maddr};
		ram_write(tOffset, allocdata, 5);
	}
	else
	{
		maddr = 0x800;
		uint8_t allocdata[5]={(ttargetID >> 8), ttargetID, (maddr >> 16), (maddr >> 8), maddr};
		ram_write(tOffset, allocdata, 5);
	}
	return maddr;
}

bool checkRule(uint8_t busno, uint32_t offset, uint32_t ID, uint8_t *data, uint8_t leng)//checks if there are rules for specific payload and applies them
{
	uint8_t condType = 0;
	uint8_t condByteMask=0;
	uint8_t actionType=0;
	uint8_t actionByteMask=0;
	uint8_t tmpbf[2048]={0};
	uint16_t cnt=0;//to handle rule
	ram_read(offset,tmpbf,0x800); //Grab the rules from XRAM and store it in IRAM for faster handling
	while(cnt < 0x800)//max rule storage offset
	{
		condByteMask=tmpbf[cnt];
		condType=tmpbf[(cnt+1)];
		actionType = tmpbf[(cnt+11)];
		actionByteMask = tmpbf[(cnt+10)];
		switch (condType)
		{
			case 0://If entire frame matches
			{
				uint8_t ccnt=0;
				for(uint8_t a=0;a<leng;a++)
				{
					if(data[a] != tmpbf[((cnt+2) + a)] )
					{
						break;
					}
					ccnt++;
				}
				if(ccnt == leng)
				{
					switch (actionType)
					{
						case 0://Swap an entire frame
						{
							uint8_t toSend[8] = {tmpbf[(cnt+12)], tmpbf[(cnt+13)],tmpbf[(cnt+14)],tmpbf[(cnt+15)],tmpbf[(cnt+16)],tmpbf[(cnt+17)],tmpbf[(cnt+18)],tmpbf[(cnt+19)]};
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;//rule applied, so nothing more to see here
						}
						case 1://Swap specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=tmpbf[((cnt+12) + a)];
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 2://add a fixed value to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] + tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 3://substract a fixed value to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] - tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 4://Multiply specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] * tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 5://Divide specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] / tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 6://Increase a percent to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] + ((data[a] * tmpbf[((cnt+12) + a)]) / 100));
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 7://Decrease a percent to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] - ((data[a] * tmpbf[((cnt+12) + a)]) / 100));
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 8://drop the frame
						{
							return 1;//do nothing so the frame will be dropped
						}
						default:
						{
							return 0;//unknown rule, so just forward the frame as it is and dont check for more rules
						}
					}
				}
				break;
			}
			case 1://check for specific bytes if equal
			{
				uint8_t ccnt=0;
				for(uint8_t a=0;a<leng;a++)
				{
					if(getBit(condByteMask,a))
					{
						if(data[a] != tmpbf[((cnt+2) + a)] )
						{
							break;
						}
					}
					ccnt++;
				}
				if (ccnt == leng)
				{
					switch (actionType)
					{
						case 0://Swap an entire frame
						{
							uint8_t toSend[8] = {tmpbf[(cnt+12)], tmpbf[(cnt+13)],tmpbf[(cnt+14)],tmpbf[(cnt+15)],tmpbf[(cnt+16)],tmpbf[(cnt+17)],tmpbf[(cnt+18)],tmpbf[(cnt+19)]};
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;//rule applied, so nothing more to see here
						}
						case 1://Swap specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=tmpbf[((cnt+12) + a)];
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 2://add a fixed value to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] + tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 3://substract a fixed value to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] - tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 4://Multiply specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] * tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 5://Divide specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] / tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 6://Increase a percent to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(((data[a] * tmpbf[((cnt+12) + a)]) / 100) + data[a]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 7://Decrease a percent to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] - ((data[a] * tmpbf[((cnt+12) + a)]) / 100));
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 8://drop the frame
						{
							return 1;//do nothing so the frame will be dropped
						}
						default:
						{
							return 0;//unknown rule, so just forward the frame as it is and dont check for more rules
						}
					}

				}
			}
			case 2://check for specific bytes if greater
			{
				uint8_t ccnt=0;
				for(uint8_t a=0;a<leng;a++)
				{
					if(getBit(condByteMask,a))
					{
						if(data[a] <= tmpbf[((cnt+2) + a)] )
						{
							break;
						}
					}
					ccnt++;
				}
				if (ccnt == leng)
				{
					switch (actionType)
					{
						case 0://Swap an entire frame
						{
							uint8_t toSend[8] = {tmpbf[(cnt+12)], tmpbf[(cnt+13)],tmpbf[(cnt+14)],tmpbf[(cnt+15)],tmpbf[(cnt+16)],tmpbf[(cnt+17)],tmpbf[(cnt+18)],tmpbf[(cnt+19)]};
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;//rule applied, so nothing more to see here
						}
						case 1://Swap specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=tmpbf[((cnt+12) + a)];
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 2://add a fixed value to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] + tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 3://substract a fixed value to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] - tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 4://Multiply specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] * tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 5://Divide specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] / tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 6://Increase a percent to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] + ((data[a] * tmpbf[((cnt+12) + a)]) / 100));
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 7://Decrease a percent to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] - ((data[a] * tmpbf[((cnt+12) + a)]) / 100));
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 8://drop the frame
						{
							return 1;//do nothing so the frame will be dropped
						}
						default:
						{
							return 0;//unknown rule, so just forward the frame as it is and dont check for more rules
						}
					}

				}
			}
			case 3://check for specific bytes if less
			{
				uint8_t ccnt=0;
				for(uint8_t a=0;a<leng;a++)
				{
					if(getBit(condByteMask,a))
					{
						if(data[a] >= tmpbf[((cnt+2) + a)] )
						{
							break;
						}
					}
					ccnt++;
				}
				if (ccnt == leng)
				{
					switch (actionType)
					{
						case 0://Swap an entire frame
						{
							uint8_t toSend[8] = {tmpbf[(cnt+12)], tmpbf[(cnt+13)],tmpbf[(cnt+14)],tmpbf[(cnt+15)],tmpbf[(cnt+16)],tmpbf[(cnt+17)],tmpbf[(cnt+18)],tmpbf[(cnt+19)]};
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;//rule applied, so nothing more to see here
						}
						case 1://Swap specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=tmpbf[((cnt+12) + a)];
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 2://add a fixed value to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] + tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 3://substract a fixed value to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] - tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 4://Multiply specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] * tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 5://Divide specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] / tmpbf[((cnt+12) + a)]);
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 6://Increase a percent to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] + ((data[a] * tmpbf[((cnt+12) + a)]) / 100));
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 7://Decrease a percent to specific bytes
						{
							uint8_t toSend[8]={0};
							for(uint8_t a=0;a<leng;a++)
							{
								if(getBit(actionByteMask,a))
								{
									toSend[a]=(data[a] - ((data[a] * tmpbf[((cnt+12) + a)]) / 100));
								}
								else
								{
									toSend[a]=data[a];
								}
							}
							if (busno == 1)
							{
								uint8_t timeout=0;
								while(!can2.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							else
							{
								uint8_t timeout=0;
								while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(&toSend), leng)) && !device.readable() && timeout < 10)
								{
									timeout++;
								}//make sure the msg goes out
							}
							return 1;
						}
						case 8://drop the frame
						{
							return 1;//do nothing so the frame will be dropped
						}
						default:
						{
							return 0;//unknown rule, so just forward the frame as it is and dont check for more rules
						}
					}

				}
			}
			default:
			{
				return 0;//unknown type or the end of the rules, so just forward the frame as it is and dont check for more rules
			}
		}
		cnt = (cnt + 20);
	}
    return 0;
}

void doMITM()
{
	ram_read(0x0,BSBuffer,0x800); //Grab the Index from XRAM and store it in IRAM for faster handling
	CANMessage can1_msg(0,CANStandard);
	CANMessage can2_msg(0,CANStandard);
	uint32_t checkTable=0;
	while(!device.readable() && counter.read() == 0)
	{
		if(!uartMode && !settings->currentActionIsRunning)
			break;

		if(can1.read(can1_msg))
		{
			checkTable=tableLookUp(can1_msg.id);
			if(checkTable == 0xFFFFFFFF)//if the ID has no rules
			{
				uint8_t timeout=0;
				while(!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && !device.readable() && timeout < 10)
				{
					timeout++;
				}//make sure the msg goes out
			}
			else
			{
				if (!checkRule(1,checkTable,can1_msg.id,can1_msg.data,can1_msg.len))//check for rules
				{
					uint8_t timeout=0;
					while(!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && !device.readable() && timeout < 10)
					{
						timeout++;
					}//make sure the msg goes out
				}
			}
		}
		if(can2.read(can2_msg))
		{
			checkTable=tableLookUp(can2_msg.id);
			if(checkTable == 0xFFFFFFFF)//if the ID has no rules
			{
				uint8_t timeout=0;
				while(!can1.write(CANMessage(can2_msg.id, reinterpret_cast<char*>(&can2_msg.data), can2_msg.len)) && !device.readable() && timeout < 10)
				{
					timeout++;
				}//make sure the msg goes out
			}
			else
			{
				if(!checkRule(2,checkTable,can2_msg.id,can2_msg.data,can2_msg.len))//check for rules
				{
					uint8_t timeout=0;
					while(!can1.write(CANMessage(can2_msg.id, reinterpret_cast<char*>(&can2_msg.data), can2_msg.len)) && !device.readable() && timeout < 10)
					{
						timeout++;
					}//make sure the msg goes out
				}
			}
		}
	}
	dumpSerial();
	if(counter.read() >0)
	{
		counter.reset();
	}
}

/*
MITM loads rules from /MITM/rules.txt and allocates them in RAM.
An index is allocated in BSBuffer (IRAM) containing the offsets in XRAM for the rules.
The structure is as follows:
IRAM index:
	-Target ID (2 bytes)
	-Rule offset in XRAM (3 bytes)

XRAM Rules:
	-Condition bytes (2 bytes)
	-Target payload (8 bytes)
	-Action bytes (2 bytes)
	-Action payload (8 bytes)

Rules are consecutively stored in XRAM, using 0xFFFF in the Condition bytes to indicate that there are no more rules following.
*/
void MITMMode()
{
	setLED(LED_ORANGE);
	if(uartMode)
		device.printf("Parsing rules, please wait...\n");
	else
		ethernetManager->debugLog("Parsing rules..");
	clearRAM();//we fill RAM with 0xFF
	char filename[]="/MITM/rules.txt";//path to rule file
	if(!doesFileExist(filename))
	{
		blinkLED(5,LED_RED);
		if(uartMode)
			device.printf("Rules not found in SD, aborting...\n\n");
		else
			ethernetManager->debugLog("Rules not found on SD! Aborting!");
		setLED(LED_GREEN);
		return;
	}

	file = sd.open(filename, O_RDONLY);
	uint32_t rulesAllocated = 0;
	uint32_t IDsAllocated = 0;

	file->lseek(0, SEEK_SET);

	while(1)//to read until the end file
	{
		uint32_t CondType = grabASCIIValue();//grab the condition type
		if (CondType == 0xFFFFFFFF)//check if EOL
		{
			break; //reached EOF
		}
		uint32_t targetID = grabASCIIValue();//grab the target ID
		uint8_t targetPayload[8] = {0};
		for (uint8_t a = 0;a<8; a++) //grab the target payload
		{
			targetPayload[a] = grabASCIIValue();
		}
		uint32_t action = grabASCIIValue();//grab the action data
		uint8_t actionPayload[8] = {0};
		for (uint8_t a = 0;a<8; a++) //grab the action payload
		{
			actionPayload[a] = grabASCIIValue();
		}
		ram_read(0x0,BSBuffer,0x800); //now we grab the index from RAM, to check if there is already an entry for that ID
		uint32_t ruleOffset = tableLookUp(targetID);
		if(ruleOffset != 0xFFFFFFFF)//if an entry is found
		{
			if(!addRule(ruleOffset, CondType,targetPayload,action,actionPayload))
			{
				if(uartMode)
					device.printf("Too many rules for ID 0x%x, Maximum number of rules per ID is 102...\n", targetID);
				else
					ethernetManager->sendFormattedDebugMessage("Too many rules for ID 0x%x, max. is 102!", targetID);
			}
			else
			{
				rulesAllocated++;
			}
		}
		else //if an entry is not found
		{
			ruleOffset = getLastRuleOffset();//retrieve the last offset where a rule was stored
			if (ruleOffset == 0xFFFFFFFF)
			{
				if(uartMode)
					device.printf("Reached maximum number of IDs, will not be able to store rules for ID 0x%x...\n", targetID);
				else
					ethernetManager->sendFormattedDebugMessage("Reached max. number of IDs, will not be able to store rules for ID 0x%x..", targetID);
			}
			else
			{
				uint32_t oPos = ruleOffset;
				ruleOffset = allocRAM(targetID, oPos);
				addRule(ruleOffset, CondType,targetPayload,action,actionPayload);
				IDsAllocated++;
				rulesAllocated++;
			}
		}
	}
	device.printf("Done!\n\nLoaded a total of %d rules for %d IDs\n\n", rulesAllocated, IDsAllocated);
	file->close();
	if(uartMode)
		device.printf("Starting MITM mode, press any key to stop...\n\n");
	else
		ethernetManager->debugLog("Done loading rules, starting MITM mode..");
	doMITM();
	setLED(LED_GREEN);
	if(!uartMode)
		ethernetManager->debugLog("MITM stopped!");
}

void logMenu()
{
		device.printf("\nLogging options:\n");
	    device.printf("\n");
	    device.printf("1)Log all CAN traffic on CAN1 to SD\n");
	    device.printf("2)Log UDS channel traffic on CAN1 to SD\n");
	    device.printf("3)Log TP 2.0 channel traffic on CAN1 to SD\n");
	    device.printf("4)Log all CAN traffic (Bridge mode) to SD\n");
	    device.printf("5)Log UDS channel traffic (Bridge mode) to SD\n");
	    device.printf("6)Log TP 2.0 channel traffic (Bridge mode) to SD\n");
	    device.printf("7)Manage logs\n");
	    device.printf("\n");
	    while(!device.readable()){}
	    char option=device.getc();
	    dumpSerial();
	    switch (option)
	    {
        case '1':
        {
            CANLogger(0, true);
            break;
        }
        case '2':
        {
            UDSLogger(0);
            break;
        }
        case '3':
        {
            TPLogger(0);
            break;
        }
        case '4':
        {
            CANLogger(1, true);
            break;
        }
        case '5':
        {
            UDSLogger(1);
            break;
        }
        case '6':
        {
            TPLogger(1);
            break;
        }
        case '7':
        {
             logManager();
             break;
        }
        default:
        {
            device.printf("Unknown option!\n");
            break;
        }
	}
}

void transferMenu()
{
		device.printf("\nData Transfer options:\n");
	    device.printf("\n");
	    device.printf("1)Dump UDS transfers to SD\n");
	    device.printf("2)Dump TP 2.0 transfers to SD\n");
	    device.printf("\n");
	    while(!device.readable()){}
	    char option=device.getc();
	    dumpSerial();
	    switch (option)
	    {
	        case '1':
	        {
	        	//UDSTransferDump();
	            break;
	        }
	        case '2':
	        {
	        	TPTransferDump();
	            break;
	        }
	        default:
	        {
	            device.printf("Unknown option!\n");
	            break;
	        }
	    }
}

void SecurityMenu()///make the SecurityAccess Hijack stuff. Each ID must be broadcasting on a different CAN connector, and the target module must be connected on CAN1
{
    device.printf("\nPlease, select an option:\n");
    device.printf("\n");
    device.printf("1)Hijack UDS Security Access\n");
    device.printf("2)Hijack TP 2.0 Security Access\n");
    device.printf("\n");
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
        	device.printf("In order for this to work, the target must be on CAN1 and the rest of the network on CAN2\n\n");
        	device.printf("Please enter the three digits for the first ID in HEX (f.ex: 123, 205, 7E0):\n");
            ownID=inputToHex();
            device.printf("\n");
            device.printf("Please enter the three digits for the second ID in HEX (f.ex: 456, 306, 7E8):\n");
            rID=inputToHex();
            device.printf("\n");
            device.printf("Please enter the Security Access level to listen to in HEX (f.ex: 01, 0A, 11), or 00 for any\n");
            uint8_t level=inputToHex();
            device.printf("Please enter the Diagnostics Mode to use (f.ex: 01, 03, 4F), or 00 to keep default one\n\n");
            uint8_t diagMode=inputToHex();
            device.printf("Mapping target IDs, please make sure that they are broadcasting... Press any key to abort\n");
            CANMessage can1_msg(0,CANStandard);
            CANMessage can2_msg(0,CANStandard);
            uint8_t bus=0;
            while(bus == 0 && !device.readable())
            {
        		if(can1.read(can1_msg))
        		{
        			if(can1_msg.id == ownID || can1_msg.id == rID)
        			{
        				if(can1_msg.id == rID)
        				{
        					//all good, no need to do anythimg
        				}
        				else //swap the IDS
        				{
        					ownID = rID;
        					rID = can1_msg.id;
        				}
        				bus=1;
        			}
        			uint8_t timeout=0;
        			while (!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && !device.readable() && timeout < 10)
        			{
        				timeout++;
        			}//make sure the msg goes out
        		}
        		if(can2.read(can2_msg))//we need to forward messages so the second id replies
        		{
        			if(can2_msg.id == ownID || can2_msg.id == rID)
        			{
        				if(can2_msg.id == ownID)
        				{
        					//all good, no need to do anythimg
        				}
        				else //swap the IDS
        				{
        					rID = ownID;
        					ownID = can2_msg.id;
        				}
        				bus=1;
        			}
    				uint8_t timeout=0;
    				while (!can1.write(CANMessage(can2_msg.id, reinterpret_cast<char*>(&can2_msg.data), can2_msg.len)) && !device.readable() && timeout < 10)
    				{
    					timeout++;
    				}//make sure the msg goes out
        		}
            }
            if(device.readable())
            {
            	device.printf("Aborted by user...\n");
            	dumpSerial();
            	break;
            }
            uint8_t poo= UDSSecurityHijack(level,diagMode); //yeah, we know you love those funky var names
            if(poo != 0)
            {
            	device.printf("Successfully hijacked Security Access with level 0x%x.\n\n Have Fun!\n",poo);
            	//CAN_wrFilter(1,rID,CANStandard);//set the filter
                inSession=1;
                protocol=1;
            }
            break;
        }
        case '2':
        {
        	device.printf("In order for this to work, the target must be on CAN1 and the rest of the network on CAN2\n\n");
        	device.printf("Please enter the three digits for the first ID in HEX (f.ex: 123, 205, 7E0):\n");
            ownID=inputToHex();
            device.printf("\n");
            device.printf("Please enter the three digits for the second ID in HEX (f.ex: 456, 306, 7E8):\n");
            rID=inputToHex();
            device.printf("\n");
            device.printf("Please enter the Security Access level to listen to in HEX (f.ex: 01, 0A, 11), or 00 for any\n\n");
            uint8_t level=inputToHex();
            device.printf("Please enter the Diagnostics Mode to use (f.ex: 01, 03, 4F), or 00 to keep default one\n\n");
            uint8_t diagMode=inputToHex();
            device.printf("Mapping target IDs, please make sure that they are broadcasting... Press any key to abort\n");
            CANMessage can1_msg(0,CANStandard);
            CANMessage can2_msg(0,CANStandard);
            uint8_t bus=0;
            while(bus == 0 && !device.readable())
            {
        		if(can1.read(can1_msg))
        		{
        			if(can1_msg.id == ownID || can1_msg.id == rID)
        			{
        				if(can1_msg.id == rID)
        				{
        					//all good, no need to do anythimg
        				}
        				else //swap the IDS
        				{
        					ownID = rID;
        					rID = can1_msg.id;
        				}
        				bus=1;
        			}
        			uint cnnt=0;//used for timeout
        			while (!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && cnnt<10)
        			{
        				cnnt++;
        			}//make sure the msg goes out
        		}
        		if(can2.read(can2_msg))//we need to forward messages so the second id replies
        		{
        			if(can2_msg.id == ownID || can2_msg.id == rID)
        			{
        				if(can2_msg.id == ownID)
        				{
        					//all good, no need to do anythimg
        				}
        				else //swap the IDS
        				{
        					rID = ownID;
        					ownID = can2_msg.id;
        				}
        				bus=1;
        			}
        			uint cnnt=0;//used for timeout
        			while (!can1.write(CANMessage(can2_msg.id, reinterpret_cast<char*>(&can2_msg.data), can2_msg.len)) && cnnt<10)
        			{
        				cnnt++;
        			}//make sure the msg goes out
        		}
            }
            if(device.readable())
            {
            	device.printf("Aborted by user...\n");
            	dumpSerial();
            	break;
            }
            uint8_t poo= TPSecurityHijack(level,diagMode); //yeah, we know you love those funky var names
            if(poo != 0)
            {
            	device.printf("Successfully hijacked Security Access with level 0x%x.\n\n Have Fun!\n",poo);
                inSession=1;
                protocol=0;
                TPSendCA(1);
            }
            break;
        }
    }

}

void replayCAN(uint8_t mode)
{
	device.printf("Checking if CAN log exists...\n");
    char filename[]="/Logging/CAN.txt";
    char filename2[]="/Replay/replay.txt";
    if(mode == 0)
    {
		if(!doesFileExist(filename))
		{
			device.printf("Log does not exist\n");
			return;
		}
		file2 = sd.open(filename, O_RDONLY);
    }
    else
    {
		if(!doesFileExist(filename2))
		{
			blinkLED(3,LED_RED);
			setLED(LED_GREEN);
			return;
		}
		file2 = sd.open(filename2, O_RDONLY);
    }
    uint32_t ID=0;
    uint8_t ln=0;
    uint8_t data[8]={0};
    uint32_t time1=0;;
    device.printf("Starting replay, press any key to abort...\n\n");
    timer.reset();
    timer.start();
    while(logToRawFrame(&time1,&ID,&ln,data) && !device.readable())//get the whole line
    {
		uint8_t timeout=0;
		while(timer.read_ms() < time1){}//doesnt contemplate overflow, but who cares
		while(!can1.write(CANMessage(ID, reinterpret_cast<char*>(data),ln)) && !device.readable() && timeout < 10)
		{
			timeout++;
		}//make sure the msg goes out
    }
    timer.stop();
    file2->close();
    if(device.readable())
    {
    	device.printf("Aborted by user\n\n");
    	dumpSerial();
    }
    else
    {
    	device.printf("Done!\n\n");
    }

}

void getSIDStats(char *filename, uint8_t tSID, uint16_t *paramms, char protocol)
{
	file2 = sd.open(filename, O_RDONLY);
    uint32_t ID=0;
    uint8_t ln=0;
    uint8_t data[8]={0};
    uint16_t tmpParam[256]={0};//to store them for sorting later
    uint32_t time1=0;
    while(logToRawFrame(&time1,&ID,&ln,data))//get the whole line
    {
    	if(ID == ownID && protocol == '2' && ((data[0] & 0xF0) == 0 || data[0] == 0x10 || data[0] == 0x20))//make sure we grab a single frame or the first one of a multiframe
    	{
    	    uint8_t iLikeTurtles=0;// used to know flag if multi or single frame
    		if((data[0] & 0xF0) != 0)//need to check if its a single or multi frame
    	    {
    	    	iLikeTurtles=1;
    	    }
    		if(data[(1+iLikeTurtles)] == (tSID + 0x40))
    		{
				uint8_t tParam = data[(2+iLikeTurtles)];
    			for(uint16_t cnta=0;cnta<256;cnta++)
				{
					if(tParam == tmpParam[cnta])
					{
						break;
					}
					else if(tmpParam[cnta] == 0)//if we reached the end of the stored SIDs
					{
						tmpParam[cnta] = tParam;//store the current one
						break;
					}
				}
    		}
    	}
    	else if(protocol == '1' && ID == 0x200 && data[0] == ownID)//grab a negotiation for the target ID
    	{
    		while(ID != (0x200 + ownID))
    		{
            	if(!logToRawFrame(&time1,&ID,&ln,data))
            	{
            		break;
            	}
    		}
    		uint32_t serverID=data[3];
    		serverID = (serverID << 8);
    		while(1)
    		{
            	if(!logToRawFrame(&time1,&ID,&ln,data))
            	{
            		break;
            	}
            	if(ID == serverID && ln > 1 && ((data[0] & 0xF0) == ACK_FOLLOW || (data[0] & 0xF0) == NOACK_FOLLOW || (data[0] & 0xF0) == ACK_LAST || (data[0] & 0xF0) == NOACK_LAST))//need to grab all frames until channel disconnect
            	{
            		if((data[0] & 0xF0) == ACK_FOLLOW || (data[0] & 0xF0) == NOACK_FOLLOW)
            		{
            			if((tSID + 0x40) == data[3])
            			{
            				uint8_t tParam = data[4];
            				for(uint16_t cnta=0;cnta<256;cnta++)
            				{
            					if(tParam == tmpParam[cnta])
            					{
            						break;
            					}
            					else if(tmpParam[cnta] == 0)//if we reached the end of the stored SIDs
            					{
            						tmpParam[cnta] = tParam;//store the current one
            						break;
            					}
            				}
            			}
                		while(1)
                		{
                        	if(!logToRawFrame(&time1,&ID,&ln,data))
                        	{
                        		break;
                        	}
                        	if(ID == serverID && ((data[0] & 0xF0) == ACK_LAST || (data[0] & 0xF0) == NOACK_LAST))//once we get the last frame of the stream
                        	{
                        		break;
                        	}
                		}
            		}
            		else if((data[0] & 0xF0) == ACK_LAST || (data[0] & 0xF0) == NOACK_LAST)
            		{
            			if((tSID + 0x40) == data[3])
            			{
            				uint8_t tParam = data[4];
            				for(uint16_t cnta=0;cnta<256;cnta++)
							{
								if(tParam == tmpParam[cnta])
								{
									break;
								}
								else if(tmpParam[cnta] == 0)//if we reached the end of the stored SIDs
								{
									tmpParam[cnta] = tParam;//store the current one
									break;
								}
							}
            			}
            		}
            	}
            	else if(ID == serverID && ln == 1 && data[0] == CHANNEL_DISCONNECT)
            	{
            		break;//we reached the end of the channel comms
            	}
    		}
    	}
    }
    uint8_t pos=0;
    for(uint16_t cnta=1; cnta<256;cnta++)//now put the SIDs in order
    {
    	for(uint16_t cntb=0; cntb<256;cntb++)
    	{
    		if(tmpParam[cntb] == cnta)
    		{
    			paramms[pos]=tmpParam[cntb];
    			pos++;
    			break;
    		}
    	}
    }
    file2->close();
    return;
}

bool SIDScan(char *filename, uint8_t *SIDs, char protocol)//grabs a list of SIDs being used by an ID
{
	file2 = sd.open(filename, O_RDONLY);
    uint32_t ID=0;
    uint8_t ln=0;
    uint8_t data[8]={0};
    uint8_t tmpSIDs[256]={0};//to store them for sorting later
    uint32_t time1=0;
    while(logToRawFrame(&time1,&ID,&ln,data))//get the whole line
    {
    	if(ID == ownID && protocol == '2' && ((data[0] & 0xF0) == 0 || data[0] == 0x10 || data[0] == 0x20))//make sure we grab a single frame or the first one of a multiframe
    	{
    	    uint8_t iLikeTurtles=0;// used to know flag if multi or single frame
    		if((data[0] & 0xF0) != 0)//need to check if its a single or multi frame
    	    {
    	    	iLikeTurtles=1;
    	    }
    		uint8_t tmpSID=data[(1+iLikeTurtles)];//grab the SID
    		for(uint16_t cnta=0;cnta<256;cnta++)
    		{
    			if(tmpSID == tmpSIDs[cnta])
    			{
    				break;
    			}
    			else if(tmpSIDs[cnta] == 0)//if we reached the end of the stored SIDs
    			{
    				tmpSIDs[cnta] = tmpSID;//store the current one
    				break;
    			}
    		}
    	}
    	else if(protocol == '1' && ID == 0x200 && data[0] == ownID)//grab a negotiation for the target ID
    	{
    		while(ID != (0x200 + ownID))
    		{
            	if(!logToRawFrame(&time1,&ID,&ln,data))
            	{
            		break;
            	}
    		}
    		uint32_t serverID=data[3];
    		serverID = (serverID << 8);
    		while(1)
    		{
            	if(!logToRawFrame(&time1,&ID,&ln,data))
            	{
            		break;
            	}
            	if(ID == serverID && ln > 1 && ((data[0] & 0xF0) == ACK_FOLLOW || (data[0] & 0xF0) == NOACK_FOLLOW || (data[0] & 0xF0) == ACK_LAST || (data[0] & 0xF0) == NOACK_LAST))//need to grab all frames until channel disconnect
            	{
            		if((data[0] & 0xF0) == ACK_FOLLOW || (data[0] & 0xF0) == NOACK_FOLLOW)
            		{
            			uint8_t tmpSID=data[3];//grab the SID
                		for(uint16_t cnta=0;cnta<256;cnta++)
                		{
                			if(tmpSID == tmpSIDs[cnta])
                			{
                				break;
                			}
                			else if(tmpSIDs[cnta] == 0)//if we reached the end of the stored SIDs
                			{
                				tmpSIDs[cnta] = tmpSID;//store the current one
                				break;
                			}
                		}
                		while(1)
                		{
                        	if(!logToRawFrame(&time1,&ID,&ln,data))
                        	{
                        		break;
                        	}
                        	if(ID == serverID && ((data[0] & 0xF0) == ACK_LAST || (data[0] & 0xF0) == NOACK_LAST))//once we get the last frame of the stream
                        	{
                        		break;
                        	}
                		}
            		}
            		else if((data[0] & 0xF0) == ACK_LAST || (data[0] & 0xF0) == NOACK_LAST)
            		{
            			uint8_t tmpSID=data[3];//grab the SID
                		for(uint16_t cnta=0;cnta<256;cnta++)
                		{
                			if(tmpSID == tmpSIDs[cnta])
                			{
                				break;
                			}
                			else if(tmpSIDs[cnta] == 0)//if we reached the end of the stored SIDs
                			{
                				tmpSIDs[cnta] = tmpSID;//store the current one
                				break;
                			}
                		}
            		}
            	}
            	else if(ID == serverID && ln == 1 && data[0] == CHANNEL_DISCONNECT)
            	{
            		break;//we reached the end of the channel comms
            	}
    		}
    	}
    }
    uint8_t pos=0;
    for(uint16_t cnta=1; cnta<256;cnta++)//now put the SIDs in order
    {
    	for(uint16_t cntb=0; cntb<256;cntb++)
    	{
    		if(tmpSIDs[cntb] == cnta)
    		{
    			SIDs[pos]=tmpSIDs[cntb];
    			pos++;
    			break;
    		}
    	}
    }
    if(pos==0)
    {
    	file2->close();
    	return 0;
    }
    file2->close();
    return 1;
}

void dumpToEmu(uint8_t tSID,uint8_t tParam,uint8_t protocol)
{
	file2 = sd.open("/Logging/CAN.txt", O_RDONLY);
    uint32_t ID=0;
    uint8_t ln=0;
    uint8_t data[8]={0};
    uint8_t tmpdata2[265]={0};
    uint32_t time1=0;
    char lastFrame[1024]={0};
    bool wasLast=false;
    BSCounter1=0;
    BSCounter2=0;
    while(1)//we will break once there is nothing more to be read
    {
    	if(!logToRawFrame(&time1,&ID,&ln,data))//grab the first line
    	{
    		if(wasLast == true)
    		{
    			writeToLog(lastFrame);//write the last grabbed frame, in case the value was constant, so at least we have a last time reference
    		}
    		break;
    	}
    	if(ID == ownID && protocol == '2' && ((data[0] & 0xF0) == 0 || data[0] == 0x10 || data[0] == 0x20))//make sure we grab a single frame or the first one of a multiframe
    	{
    		uint8_t iLikeTurtles=0;// used to know flag if multi or single frame
    		if((data[0] & 0xF0) != 0)//need to check if its a single or multi frame
    	    {
    	    	iLikeTurtles=1;
    	    }
    		ln=data[(0+iLikeTurtles)];
    		if(data[(1+iLikeTurtles)] == (tSID + 0x40) && data[(2+iLikeTurtles)] == tParam)
    		{
    			if((data[0] & 0xF0) == 0)//single frame
    			{
    				uint8_t tmpdata[7]={data[1],data[2],data[3],data[4],data[5],data[6],data[7]};//copy just the data bytes
					char tmpbuf2[64]={0};
    				char tmpbuf[128]={0};
					sprintf(tmpbuf,"%d,%d",time1,ln);//add the time and payload length
					for (uint8_t a=1;a<(ln + 1);a++)//dump the data bytes to string
					{
						sprintf(tmpbuf2,",0x%x",data[a]);
						strngcat (tmpbuf,tmpbuf2);
					}
					sprintf(tmpbuf2,"\n");
					strngcat (tmpbuf,tmpbuf2);
    				if(memcmp(tmpdata,tmpdata2,ln) != 0)//make sure that the payload is different, to save space in emu files
					{
    					writeToLog(tmpbuf);//and write the whole line
    					for(uint16_t a=0; a<ln; a++)//and copy the new values
    					{
    						tmpdata2[a] = data[(a + 1)];
    					}
    					wasLast=false;
					}
    				else
    				{
    					memset(lastFrame,0,1024);
    					strngcat (lastFrame,tmpbuf);
    					wasLast=true;
    				}
    			}
    			else if(data[0] == 0x10 || data[0] == 0x20)//multiframe reply
    			{
    				uint8_t tmpdata[265]={0};//create a temporary array to store the whole transmission
    				memcpy(tmpdata, data, 8);//copy the data
    				uint8_t tLength = data[1];
    				uint8_t cntt=6;//we already got 6 bytes of data!
					char tmpbuf[1024]={0};
					for(uint8_t a=0; a< 6; a++)//copy the existing data
					{
						tmpdata[a] = data[(a + 2)];
					}
					sprintf(tmpbuf,"%d,%d,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x",time1,data[1],data[2],data[3],data[4],data[5],data[6],data[7]);
    				while(cntt < tLength)
    				{
    	            	if(!logToRawFrame(&time1,&ID,&ln,data))
    	            	{
    	            		break;
    	            	}
    	            	if(ID == ownID)
    	            	{
    						char tmpchar[128]={0};
    						if((tLength - cntt) > 6)
    						{
								sprintf(tmpchar,",0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x",data[1],data[2],data[3],data[4],data[5],data[6],data[7]);
								strngcat(tmpbuf,tmpchar);
	    						for(uint8_t a=cntt; a< (cntt + 7); a++)//copy the 7 data bytes
	    						{
	    							tmpdata[a] = data[(a -  (cntt - 1))];
	    						}
    						}
    						else
    						{
    							uint8_t rem = (tLength - cntt);//store how many bytes remain
	    						for(uint8_t a=0; a< rem; a++)//copy the remaining data bytes
	    						{
	    							sprintf(tmpchar,",0x%x",data[(a + 1)]);
	    							strngcat(tmpbuf,tmpchar);
	    						}
	    						for(uint8_t a=cntt; a< (cntt + rem); a++)//copy the remaining data bytes
	    						{
	    							tmpdata[a] = data[(a -  (cntt - 1))];
	    						}
    						}
    						cntt = (cntt + 7);//doesnt matter how many are remaining, the end is the end. Unless you're Chuck Norris.
    	            	}
    				}
	            	strngcat(tmpbuf,"\n");
	            	if(memcmp(tmpdata,tmpdata2,tLength) !=0)//if the payload changes
	            	{
	            		writeToLog(tmpbuf);
	            		wasLast=false;
	            	}
	            	else
	            	{
	            		memset(lastFrame,0,1024);
	            		strngcat (lastFrame,tmpbuf);
	            		wasLast=true;
	            	}
    			}
    		}
    	}
    	else if(protocol == '1' && ID == 0x200 && data[0] == ownID)//grab a negotiation for the target ID
    	{
    		while(ID != (0x200 + ownID))
    		{
            	if(!logToRawFrame(&time1,&ID,&ln,data))
            	{
            		break;
            	}
    		}
    		uint32_t serverID=data[3];
    		serverID = (serverID << 8);
    		while(1)
    		{
            	if(!logToRawFrame(&time1,&ID,&ln,data))
            	{
            		break;
            	}
            	if(ID == serverID && ln > 1 && ((data[0] & 0xF0) == ACK_FOLLOW || (data[0] & 0xF0) == NOACK_FOLLOW || (data[0] & 0xF0) == ACK_LAST || (data[0] & 0xF0) == NOACK_LAST))//need to grab all frames until channel disconnect
            	{
            		if((data[0] & 0xF0) == ACK_FOLLOW || (data[0] & 0xF0) == NOACK_FOLLOW)
            		{
            			if((tSID + 0x40) == data[3] && tParam == data[4])
            			{
            				char  tmpbuf[1024]={0};
        					sprintf(tmpbuf,"%d,%d,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x",time1,(data[2] + 3),data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7]);
            				while(1)
							{
								if(!logToRawFrame(&time1,&ID,&ln,data))
								{
									break;
								}
								if(ID == serverID && ((data[0] & 0xF0) == ACK_FOLLOW || (data[0] & 0xF0) == NOACK_FOLLOW))//while we get the stream
								{
		    						char tmpchar[128]={0};
		    	            		sprintf(tmpchar,",0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x",ownID,ln,data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7]);
		    	            		strngcat(tmpbuf,tmpchar);
								}
								else if(ID == serverID && ((data[0] & 0xF0) == ACK_LAST || (data[0] & 0xF0) == NOACK_LAST))//once we get the last frame of the stream
								{
		    						char tmpchar[128]={0};
		    	            		sprintf(tmpchar,",0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x\n",ownID,ln,data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7]);
		    	            		strngcat(tmpbuf,tmpchar);
		    	            		writeToLog(tmpbuf);
									break;
								}
							}
            			}
            		}
            		else if((data[0] & 0xF0) == ACK_LAST || (data[0] & 0xF0) == NOACK_LAST)
            		{
            			if((tSID + 0x40) == data[3] && tParam == data[4])
            			{
            				char  tmpbuf[128]={0};
        					sprintf(tmpbuf,"%d,%d,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x,0x%x\n",time1,ln,data[0],data[1],data[2],data[3],data[4],data[5],data[6],data[7]);
        					writeToLog(tmpbuf);
            			}
            		}
            	}
            	else if(ID == serverID && ln == 1 && data[0] == CHANNEL_DISCONNECT)
            	{
            		break;//we reached the end of the channel comms
            	}
    		}
    	}
    }
    file2->close();
    return;
}

void addEmulation()
{
    char filename[]="/Logging/CAN.txt";
    if(!doesFileExist(filename))
    {
        device.printf("CAN log does not exist.\n\n");
        return;
    }
	device.printf("\nPlease, select the protocol type:\n");
	device.printf("\n");
	device.printf("1)TP 2.0\n");
	device.printf("2)UDS\n");
	device.printf("\n");
	while(!device.readable()){}
	char option=device.getc();
	dumpSerial();
	if(option != '1' && option != '2')
	{
		device.printf("Unknown option!\n\n");
		return;
	}
    device.printf("Please enter the Target ID in HEX (2 bytes for TP, 3 for UDS) (f.ex: 123, 205, 7E0):\n");
    ownID=inputToHex();
    device.printf("\nMapping SIDs used by ID 0x%x...\n\n");
    uint8_t SIDs[256]={0};//to store the SIDs used
    if(!SIDScan(filename,SIDs,option))
    {
    	device.printf("No SIDs found for the selected ID and protocol...\n");
    	return;
    }
     while(1)
     {
    	    device.printf("Found the following SIDs:\n");
    	    for(uint16_t a=0;a<256;a++)
    	    {
    	        if(SIDs[a] == 0)//if we are done
    	        {
    	        	break;
    	        }
    	        uint8_t currentSID = (SIDs[a] - 0x40);
    	        device.printf("0x%x - ",currentSID);
    	        switch (currentSID)
    	        {
    	        	case REQUEST_POWERTRAIN_DIAG_DATA:
    	        	{
    	        		device.printf("PowerTrain Diag Data Service\n");
    	        		break;
    	        	}
    	        	case REQUEST_FREEZE_FRAME:
    	        	{
    	        		device.printf("Freeze Frame Service\n");
    	        		break;
    	        	}
    	        	case REQUEST_STORED_DTCS:
    	        	{
    	        		device.printf("Stored DTCs Service\n");
    	        		break;
    	        	}
    	        	case REQUEST_CLEAR_DTCS:
    	        	{
    	        		device.printf("Clear DTCs Service\n");
    	        		break;
    	        	}
    	        	case REQUEST_PENDING_DTCS:
    	        	{
    	        		device.printf("Pending DTCs Service\n");
    	        		break;
    	        	}
    	        	case REQUEST_PERMANENT_DTCS:
    	        	{
    	        		device.printf("Permanent DTCs Service\n");
    	        		break;
    	        	}
    	        	case REQUEST_VEHICLE_INFORMATION:
    	        	{
    	        		device.printf("Vehicle Information Service\n");
    	        		break;
    	        	}
    	            case DISABLE_NORMAL_MSG_TRANSMISSION:
    	            {
    	            	device.printf("Disable non-diagnostics messages transmission Service\n");
    	            	break;
    	            }
    	            case ENABLE_NORMAL_MSG_TRANSMISSION:
    	            {
    	            	device.printf("Enable non-diagnostics messages transmission Service\n");
    	                break;
    	            }
    	            case ACCESS_TIMING_PARAMETERS:
    	            {
    	            	device.printf("Access Timing Parameters Service\n");
    	                break;
    	            }
    	            case READ_DTC_BY_STATUS:
    	            {
    	                device.printf("Read DTC by Status Service\n");
    	                break;
    	            }
    	            case START_DIAG_SESSION: //startDiagnosticsSession
    	            {
    	            	device.printf("Start Diagnostics Session Service\n");
    	                break;
    	            }
    	            case STOP_DIAGNOSTICS_SESSION:
    	            {
    	            	device.printf("Stop Diagnostics Session Service\n");
    	                break;
    	            }
    	            case CLEAR_DTCS:
    	            {
    	            	device.printf("Clear DTCs Service");
    	                break;
    	            }
    	            case SECURITY_ACCESS:
    	            {
    	            	device.printf("Security Access Service\n");
    	                break;
    	            }
    	            case TESTER_PRESENT:
    	            {
    	            	device.printf("Tester Present Service\n");
    	                break;
    	            }
    	            case READ_ECU_ID://readECUIdentification
    	            {
    	            	device.printf("Read ECU ID Service\n");
    	                break;
    	            }
    	            case READ_DATA_BY_LOCAL_ID:
    	            {
    	            	device.printf("Read Data by Local ID Service\n");
    	                break;
    	            }
    	            case WRITE_DATA_BY_LOCAL_ID:
    	            {
    	            	device.printf("Write Data by Local ID Service\n");
    	                break;
    	            }
    	            case ECU_RESET:
    	            {
    	            	device.printf("ECU Reset Service\n");
    	                break;
    	            }
    	            case READ_MEMORY_BY_ADDRESS:
    	            {
    	            	device.printf("Read Memory by Address Service\n");
    	                break;
    	            }
    	            case WRITE_MEMORY_BY_ADDRESS:
    	            {
    	            	device.printf("Write Memory by Address Service\n");
    	                break;
    	            }
    	            case START_COMMUNICATION:
    	            {
    	            	device.printf("Start Communication Service\n");
    	                break;
    	            }
    	            case STOP_COMMUNICATION:
    	            {
    	            	device.printf("Stop Communication Service\n");
    	                break;
    	            }
    	            case IO_CONTROL_BY_LOCAL_ID:
    	            {
    	            	device.printf("I/O Control by Local ID Service\n");
    	                break;
    	            }
    	            case START_ROUTINE_BY_LOCAL_ID:
    	            {
    	            	device.printf("Start Routine by Local ID Service\n");
    	                break;
    	            }
    	            case STOP_ROUTINE_BY_LOCAL_ID:
    	            {
    	            	device.printf("Stop Routine by Local ID Service\n");
    	                break;
    	            }
    	            case REQUEST_ROUTINE_RESULTS_BY_LOCAL_ID:
    	            {
    	            	device.printf("Request Routine results by Local ID Service\n");
    	                break;
    	            }
    	            case REQUEST_DOWNLOAD:
    	            {
    	            	device.printf("Request Download Service\n");
    	                break;
    	            }
    	            case REQUEST_UPLOAD:
    	            {
    	            	device.printf("Request Upload Service\n");
    	                break;
    	            }
    	            case TRANSFER_DATA:
    	            {
    	            	device.printf("Transfer Data Service\n");
    	                break;
    	            }
    	            case REQUEST_TRANSFER_EXIT:
    	            {
    	            	device.printf("Transfer Exit Service\n");
    	                break;
    	            }
    	            case REQUEST_FILE_TRANSFER:
    	            {
    	            	device.printf("File Transfer Service\n");
    	                break;
    	            }
    	            case DYNAMICALLY_DEFINE_DATA_ID:
    	            {
    	            	device.printf("Dinamically Define Data by ID Service\n");
    	                break;
    	            }
    	        	default:
    	        	{
    	        		device.printf("Request not cataloged\n");
    	        		break;
    	        	}
    	        }
    	     }
    	 device.printf("\nPlease enter the SID to emulate in HEX (2, 21, 3F...) or press 0 to exit\n");
		 uint8_t chosenSID=inputToHex();
		 if(chosenSID == 0)
		 {
			 return;
		 }
		 bool b=false;
 	     for(uint16_t a=0;a<256;a++)
 	     {
 	         if(SIDs[a] == (chosenSID + 0x40))//if we are done
 	         {
 	         	b=true;
 	        	break;
 	         }
 	         if(SIDs[a] == 0)//if we reach the end of stored SIDs
 	         {
 	        	 break;
 	         }
 	     }
 	     if(!b)
 	     {
 	    	device.printf("\nSID 0x%x is not used in log, please choose a valid one!\n\n", chosenSID);
 	     }
 	     else
 	     {
			 device.printf("\nChecking for parameters passed to SID 0x%x...\n", chosenSID);
			 uint16_t params[256]={0};
			 memset(params,0xFFFF,256);
			 getSIDStats(filename,chosenSID,params,option);
			 if(params[0] == 0xFFFF)
			 {
				 device.printf("No Params found for the selected SID\n\n");
			 }
			 else
			 {
				 while(1)
				 {
					 device.printf("\nFound the following Parameters for SID 0x%x: \n\n", chosenSID);
					 for(uint16_t a=0;a<256;a++)
					 {
						 if(params[a] == 0xFFFF)//if we are done
						 {
							break;
						 }
						 device.printf("- 0x%x \n",params[a]);

					 }
					 device.printf("\nPlease enter the Parameter to emulate in HEX, or 0 to go back (2, 21, 3F...)\n");
					 uint8_t chosenParam=inputToHex();
					 if(chosenParam == 0)
					 {
						 break;
					 }
					 b = false;
					 for(uint16_t a=0;a<256;a++)
					 {
						 if(params[a] == chosenParam)//if we are done
						 {
							b=true;
							break;
						 }
						 if(params[a] == 0xFFFF)//if we reach the end of params stored
						 {
							break;
						 }
					 }
					 if(!b)
					 {
						device.printf("\nParameter 0x%x is not used by chosen SID in log file, please choose a valid one!\n\n", chosenParam);
					 }
					 else
					 {
						 char emuFile[128]="/Emulator/";
						 char tmpbuf[64]={0};
						 sprintf(tmpbuf,"%x/%x/%x",ownID,chosenSID,chosenParam);
						 strngcat(emuFile,tmpbuf);
						 while(1)
						 {
							 if(doesFileExist(emuFile))
							 {
								device.printf("Emulator file already exists, overwrite? (y/n)\n");
								while(!device.readable()){}
								char y=device.getc();
								if(y != 'y' && y != 'Y')
								{
									break;
								}
								sprintf(tmpbuf,"/sd/Emulator/%x/%x/%x",ownID,chosenSID,chosenParam);
								remove(tmpbuf);
							 }
							 sprintf(tmpbuf,"/sd/Emulator");
							 if(!doesDirExist(tmpbuf))
							 {
								 mkdir(tmpbuf, 0777);
							 }
							 sprintf(tmpbuf,"/sd/Emulator/%x",ownID);
							 if(!doesDirExist(tmpbuf))
							 {
								 mkdir(tmpbuf, 0777);
							 }
							 sprintf(tmpbuf,"/sd/Emulator/%x/%x",ownID,chosenSID);
							 if(!doesDirExist(tmpbuf))
							 {
								 mkdir(tmpbuf, 0777);
							 }
							 device.printf("Generating Emulator file, please wait...\n");
							 toggleLogging(emuFile,1);//open the file for writing
							 sprintf(tmpbuf,"0x%x,0x%x,0x%x,0x%x\n",ownID,chosenSID,chosenParam,(option - 0x30));
							 writeToLog(tmpbuf);//write the header
							 dumpToEmu(chosenSID,chosenParam,option);//finally create the emu file
							 toggleLogging(emuFile,0);
							 device.printf("Emulator file created for Parameter 0x%x of SID 0x%x\n\n",chosenParam,chosenSID);
							 break;
						 }
					 }
				 }
			 }
 	     }
     }

}

bool getEmuLine()//gets an emulator line and puts it in BSBuffer
{
	uint32_t time1 = grabHexValue();
	if(time1 == 0xFFFFFFFF)
	{
		return 0;
	}
	BSBuffer[0] = (time1 >> 24);
	BSBuffer[1] = (time1 >> 16);
	BSBuffer[2] = (time1 >> 8);
	BSBuffer[3] = time1;
	BSBuffer[4] = grabHexValue();//grab the length
	for(uint32_t a=0; a<BSBuffer[4];a++)//and finally, grab the data
	{
		BSBuffer[(a + 5)]=grabHexValue();
	}
	return 1;
}

bool getEmuFileHeader(uint32_t *tID, uint8_t *tSID, uint8_t *tParam, uint8_t *tProtocol)
{
	uint32_t tmpval = grabHexValue();
	if (tmpval == 0xFFFFFFFF)//EOF
	{
		return 0;
	}
	*tID=tmpval;
	*tSID = grabHexValue();
	*tParam = grabHexValue();
	*tProtocol = grabHexValue();
	return 1;
}

bool mapEmulatorFile()//maps an emulator file into XRAM, adding the entry for the index too
{
	uint32_t ID=0;
	uint8_t SID=0;
	uint8_t Param=0;
	uint8_t Protocol=0;
	getEmuFileHeader(&ID,&SID,&Param,&Protocol);
	ram_read(0x0,BSBuffer,0x800); //grab the index from XRAM, to check the last entry
	uint32_t cnt=0;
	uint32_t lastOffset=0;
	for(cnt=0;cnt<0x7F4;cnt = (cnt + 11))
	{
		uint32_t cID=BSBuffer[cnt];
		cID=((cID << 8 ) + BSBuffer[(cnt + 1)]);
		if(cID == 0xFFFF)
		{
			break;//found a free spot
		}
		lastOffset=BSBuffer[(cnt + 8)];
		lastOffset=((lastOffset << 8 ) + BSBuffer[(cnt + 9)]);//grab the offset of the last one
		lastOffset=((lastOffset << 8 ) + BSBuffer[(cnt + 10)]);
		lastOffset++;
	}
	if(cnt >= 0x7F4)//if no more space for emus
	{
		return 0;
	}
	uint8_t toWrite[11]={(ID >> 8), ID, Protocol, SID, Param, 0,0,0,0,0,0};//prepare the index entry
	if(cnt == 0)//starts in 0x800
	{
		lastOffset=0x800;
	}
	toWrite[5]=(lastOffset >> 16);
	toWrite[6]=(lastOffset >> 8);
	toWrite[7]=lastOffset;
	BSCounter1=lastOffset;
	while(getEmuLine())//write the emulation data to XRAM
	{
		if(BSCounter1 > 0x1FEF0)//if we run out of XRAM space
		{
			return 0;
		}
		ram_write(BSCounter1, BSBuffer, (BSBuffer[4] + 5));
		BSCounter1=(BSCounter1 + (BSBuffer[4] + 5));
	}
	BSCounter1--;//to compensate for later
	toWrite[8]=(BSCounter1 >> 16);
	toWrite[9]=(BSCounter1 >> 8);
	toWrite[10]=BSCounter1;
	ram_write(cnt,toWrite,11);//now write the index entry
	return 1;
}



void generateUDSEmulatorData()
{
	char fileName[64]="/Emulator/PIDs.txt";
	if(!doesFileExist(fileName))
	{
		blinkLED(3,LED_RED);
		device.printf("PID list not found, aborting...\n");
		setLED(LED_GREEN);
		return;
	}
	device.printf("PID list found, parsing data...\n");
	file2 = sd.open(fileName, O_RDONLY);
	clearRAM();//we fill RAM with 0xFF
	ownID=grabHexValue();//get the request ID
	uint32_t tst=grabHexValue();
	uint32_t ramPointer=0;
	uint8_t data[265]={0};
	while(tst != 0xFFFFFFFF)//grab all the things
	{
		data[0]=tst;
		for(uint8_t a = 0; a< data[0];a++)
		{
			tst=grabHexValue();
			data[(a + 1)]=tst;
		}
		ram_write(ramPointer,data,(data[0] + 1));
		ramPointer=(ramPointer + (data[0] + 1));
		tst=grabHexValue();
	}
	data[0]=0;
	ram_write(ramPointer,data,1);//to indicate the end of the data
	file2->close();
	device.printf("Done!\n\n");
	CANLogger(2, true);
}

uint8_t getIDProtocol(uint32_t tID)//fetches the protocol used by that ID from index
{
	uint32_t cnt=0;
	while(cnt < 0x7F4)
	{
		uint32_t cID = BSBuffer[cnt];
		cID = ((cID << 8 ) + BSBuffer[(cnt + 1)]);//grab the offset of the last one
		if(cID == 0xFFFFFF)//if end of index
		{
			return 0;
		}
		if(cID == tID)//if end of index
		{
			return BSBuffer[(cnt + 2)];
		}
		cnt=(cnt + 11);
	}
	return 0;
}

uint8_t checkEmuEntry(uint32_t ID, uint8_t tSID, uint8_t tParam,uint8_t tProtocol)
{
    uint32_t cnt=0;
    while(cnt < 170)
    {
    	uint32_t ttID=BSBuffer[(11 * cnt)];
    	ttID = ((ttID << 8) + BSBuffer[((11 * cnt) + 1)]);
    	uint8_t ttSID = BSBuffer[((11 * cnt) + 3)];
    	uint8_t ttParam = BSBuffer[((11 * cnt) + 4)];
    	uint8_t ttProtocol = BSBuffer[((11 * cnt) + 2)];
    	if(ID == ttID && tSID == (ttSID + 0x40) && tParam == ttParam && tProtocol == ttProtocol)
    	{
    		return cnt;
    	}
    	else if(ttID == 0xFFFF)
    	{
    		return 0xFF;
    	}
    	cnt++;
    }
    return 0xff;
}

void grabEmuData(uint32_t offset, uint32_t *tim, uint8_t *len, uint8_t *data)//grabs emulator data line and returns it
{
	uint8_t llen[6];
	ram_read(offset,llen,4);//grab the timer
	*tim=llen[0];
	*tim = ((*tim << 8) + llen[1]);
	*tim = ((*tim << 8) + llen[2]);
	*tim = ((*tim << 8) + llen[3]);
	ram_read((offset + 4),llen,1);
	*len= llen[0];//grab the payload length
	ram_read((offset + 5), data,llen[0]);//grab the data
}


void doEmulate(uint8_t tmaps)//actual emulation, only UDS is supported in this release
{
	ram_read(0x0,BSBuffer,0x800);//load the index in IRAM
	uint8_t protocol=0;
	uint32_t tID=0;
	uint8_t tSID=0;
	uint8_t tParam=0;
	uint8_t entryNo=0;
	uint32_t currentOffset[170]={0};//to store the current offsets
	uint8_t timeout=0;
	bool timerSet=0;//to know when to start the timer
	for(uint8_t a=0;a<tmaps;a++)//load the offsets for emulators
	{
		currentOffset[a]=BSBuffer[(5 + (a * 11))];
		currentOffset[a]= ((currentOffset[a] << 8) + BSBuffer[(6 + (a * 11))]);
		currentOffset[a]= ((currentOffset[a] << 8) + BSBuffer[(7 + (a * 11))]);
	}
	CANMessage can1_msg(0,CANExtended);
	CANMessage can2_msg(0,CANExtended);
	counter.reset();
	while(!device.readable() && counter.read() == 0)
	{
		if(can1.read(can1_msg))
		{
			tID=can1_msg.id;//grab the ID
			protocol = getIDProtocol(tID);//look it up
			if(protocol == 2)//means there is an UDS entry!
			{
				uint8_t data[264]={0};
				uint8_t len=0;
				uint8_t data2[264]={0};
				uint8_t len2=0;
				uint8_t iLikeTurtles=0;// used to know flag if multi or single frame
				uint32_t time1=0;
				uint32_t time2=0;
	    		if((can1_msg.data[0] & 0xF0) != 0)//need to check if its a single or multi frame
	    	    {
	    	    	iLikeTurtles=1;
	    	    }
	    		tSID=can1_msg.data[(1+iLikeTurtles)];//grab the SID
	    		tParam=can1_msg.data[(2+iLikeTurtles)];//grab the Param
				entryNo=checkEmuEntry(tID,tSID,tParam,protocol);
				if(entryNo !=0xFF)//if we find an emulator entry for this specific SID and param
				{
					if(!timerSet)
					{
						timer.start();
						timerSet=1;
					}
					grabEmuData(currentOffset[entryNo],&time1,&len,data);//take the current frame data to be sent
					grabEmuData((currentOffset[entryNo] + (len + 5)),&time2,&len2,data2);//take also the next one to make sure we will send the proper one
					ownID=can1_msg.id;
					uint32_t currentTime = timer.read_ms();
					if((currentTime < time1 && currentTime < time2) || (currentTime > time1 && currentTime < time2))
					{
						UDSWrite(data,len,2);
					}
					else//otherwise, find the appropiate one
					{
						uint32_t limit = BSBuffer[((entryNo * 11) + 8)];
						limit = ((limit << 8 ) + BSBuffer[((entryNo * 11) + 9)]);//grab the offset of the last one
						limit = ((limit << 8 ) + BSBuffer[((entryNo * 11) + 10)]);
						while(currentTime > time1 && currentTime > time2)
						{
							currentOffset[entryNo]=(currentOffset[entryNo] + (len + 5));
							grabEmuData(currentOffset[entryNo],&time1,&len,data);
							currentTime = timer.read_ms();
							if(currentOffset[entryNo] >= limit)//rewind the whole thing if we no longer have data
							{
								for(uint8_t a=0;a<tmaps;a++)//load the start offsets for emulators
								{
									currentOffset[a]=BSBuffer[(5 + (a * 11))];
									currentOffset[a]= ((currentOffset[a] << 8) + BSBuffer[(6 + (a * 11))]);
									currentOffset[a]= ((currentOffset[a] << 8) + BSBuffer[(7 + (a * 11))]);
								}
								timer.reset();
								grabEmuData(currentOffset[entryNo],&time1,&len,data);
								break;
							}
							grabEmuData((currentOffset[entryNo] + (len + 5)),&time2,&len2,data2);
						}
						UDSWrite(data,len,2);
					}
					uint32_t limit = BSBuffer[((entryNo * 11) + 8)];
					limit = ((limit << 8 ) + BSBuffer[((entryNo * 11) + 9)]);//grab the offset of the last one
					limit = ((limit << 8 ) + BSBuffer[((entryNo * 11) + 10)]);
					if(currentOffset[entryNo] >= limit)//rewind the whole thing if we no longer have data
					{
						for(uint8_t a=0;a<tmaps;a++)//load the start offsets for emulators
						{
							currentOffset[a]=BSBuffer[(5 + (a * 11))];
							currentOffset[a]= ((currentOffset[a] << 8) + BSBuffer[(6 + (a * 11))]);
							currentOffset[a]= ((currentOffset[a] << 8) + BSBuffer[(7 + (a * 11))]);
						}
						timer.reset();
					}
				}
				else
				{
					timeout=0;
					while(!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && !device.readable() && timeout < 10 && counter.read() == 0)
					{
						timeout++;
					}//make sure the msg goes out, but dont try more than 10 times
				}
			}
			else
			{
				timeout=0;
				while(!can2.write(CANMessage(can1_msg.id, reinterpret_cast<char*>(&can1_msg.data), can1_msg.len)) && !device.readable() && timeout < 10 && counter.read() == 0)
				{
					timeout++;
				}//make sure the msg goes out, but dont try more than 10 times
			}
		}
		if(can2.read(can2_msg))
		{
			timeout=0;
			while(!can1.write(CANMessage(can2_msg.id, reinterpret_cast<char*>(&can2_msg.data), can2_msg.len)) && !device.readable() && timeout < 10 && counter.read() == 0)
			{
				timeout++;
			}//make sure the msg goes out, but dont try more than 10 times
		}
	}
	if(timerSet)
	{
		timer.stop();
	}
	dumpSerial();
	counter.reset();
	device.printf("\nAborted by user\n");
}

void emulate()
{
	device.printf("Don't forget that device to be emulated needs to be plugged in CAN1, and victim must be plugged in CAN2!\n\n");
	device.printf("Loading Maps...\n");
	char fileName[64]="/Emulator/plugins.txt";
	if(!doesFileExist(fileName))
	{
		blinkLED(3,LED_RED);
		device.printf("Plugin list not found, aborting...\n");
		setLED(LED_GREEN);
		return;
	}
	file = sd.open(fileName, O_RDONLY);
	clearRAM();//we fill RAM with 0xFF
	uint8_t caunt=0;
	while(getLine(fileName, 1) != -1)
	{
		if(doesFileExist(fileName))
		{
			file2 = sd.open(fileName, O_RDONLY);
			if(!mapEmulatorFile())
			{
				device.printf("Could not load %s. No space left in RAM\n\n", fileName);
				break;
			}
			caunt++;
			file2->close();
		}
		else
		{
			device.printf("File %s does not exist.\n");
		}
	}
	file->close();
	device.printf("%d Maps loaded, starting emulation.\n\nPress any key to stop...\n",caunt);
	setLED(LED_ORANGE);
	doEmulate(caunt);
	setLED(LED_GREEN);
}

void emulatorMenu()
{
    device.printf("\nEmulation Menu:\n");
    device.printf("\n");
    device.printf("1)Create Emulation files\n");
    device.printf("2)Start Emulation\n");
    device.printf("3)Generate UDS Emulation data\n");
    device.printf("\n");
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
            addEmulation();
            break;
        }
        case '2':
        {
            emulate();
            break;
        }
        case '3':
        {
        	generateUDSEmulatorData();
            break;
        }
        default:
        {
            device.printf("Unknown option!\n");
            break;
        }
    }


}

void protocolMenu()
{
    device.printf("\nProtocol Menu:\n");
    device.printf("\n");
    device.printf("1)Scan current traffic for known protocols\n");
    device.printf("2)Logging\n");
    device.printf("3)Data Transfers\n");
    device.printf("4)SecurityAccess\n");
    device.printf("5)Replay\n");
    device.printf("6)Emulator\n");
    device.printf("7)Enable Bridge Mode\n");
    device.printf("8)Tester Address\n");
    device.printf("\n");
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
            protocolScan();
            break;
        }
        case '2':
        {
            logMenu();
            break;
        }
        case '3':
        {
        	transferMenu();
            break;
        }
        case '4':
        {
            SecurityMenu();
            break;
        }
        case '5':
        {
        	device.printf("Replay will be done on CAN1\n");
        	replayCAN(0);
            break;
        }
        case '6':
        {
            emulatorMenu();
            break;
        }
        case '7':
        {
            CANBridge();
            break;
        }
        case '8':
        {
        	TesterAddressMenu();
        	break;
        }
        default:
        {
            device.printf("Unknown option!\n");
            break;
        }
    }
}

void ECUPowerMenu()
{
	device.printf("Input voltage is %fV\n",getVoltage());
	device.printf("\nPower Status for ECU CTRL header is currently ");
	if(ECU_CTRL == 0)
	{
		device.printf("OFF\n\nWould you like to turn it ON? (y/n)");
	}
	else
	{
		device.printf("ON\n\nWould you like to turn it OFF? (y/n)");
	}
	while(!device.readable()){}
	char tt=device.getc();
	wait(0.1);
	dumpSerial();
	if(tt == 'y' || tt == 'Y')
	{
		if(ECU_CTRL == 0)
		{
			ECU_CTRL = 1;
		}
		else
		{
			ECU_CTRL = 0;
		}
		device.printf("\nPower Status for ECU CTRL header has been set to ");
		if(ECU_CTRL == 0)
		{
			device.printf("OFF\n\n");
		}
		else
		{
			device.printf("ON\n\n");
		}
	}
}

// checks if the button has been pressed and triggers actions accordingly
void checkBtn() {
	if(counter.read() > 0)//if button has been pressed
	{
		uint8_t bpress=0;
		blinkLED(3,LED_GREEN);
		setLED(LED_RED);
		counter.reset();
		timer.start();
		while(timer.read() < 2)
		{
			if(counter.read() > bpress)
			{
				blinkLED(1,LED_RED);
				bpress++;
				timer.reset();
			}
		}
		counter.reset();
		timer.stop();
		timer.reset();
		blinkLED(bpress,LED_GREEN);
		switch (bpress)
		{
			case 1://MITM mode
			{
				MITMMode();
				break;
			}
			case 2://log RAW can traffic in Bridge mode
			{
				CANLogger(1, true);
				break;
			}
			case 3://log RAW can traffic in standard mode
			{
				CANLogger(0, true);
				break;
			}
			case 4://Delete CAN log
			{
				if(doesFileExist("/Logging/CAN.txt"))
				{
					remove("/sd/Logging/CAN.txt");
					remove("/sd/Logging/CANRAW.txt");
					blinkLED(3,LED_GREEN);
					setLED(LED_GREEN);
				}
				else
				{
					wait(1);
					blinkLED(3,LED_RED);
					setLED(LED_GREEN);
				}
				break;
			}
			case 5:
			{
				emulate();
				setLED(LED_GREEN);
				break;
			}
			case 6:
			{
				replayCAN(1);
				break;
			}
			case 7:
			{
				generateUDSEmulatorData();
				break;
			}
			default://unknown option
			{
				wait(0.5);
				setLED(LED_RED);
				wait(1);
				setLED(LED_GREEN);
				break;
			}

		}
	}
}

void menu()
{
	setLED(LED_GREEN);
	device.printf("\nPlease, select an option:\n");
    device.printf("\n");
    device.printf("1)Start diag session via TP 2.0\n");
    device.printf("2)Start diag session via UDS\n");
    device.printf("3)Scan for TP 2.0 IDs (11-bit IDs)\n");
    device.printf("4)Scan for UDS IDs (11-bit IDs)\n");
    device.printf("5)Change CAN speed\n");
    device.printf("6)Detect CAN speed\n");
    device.printf("7)Protocol Menu\n");
    device.printf("8)MITM Mode\n");
    device.printf("9)ECU Power\n");
    device.printf("\n");
    while(!device.readable())
    {
    	checkBtn();
    }
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
            device.printf("Please enter the three digits for the Channel Negotiation ID in HEX (f.ex: 200, 300, 0A0):\n");
            uint32_t channelID=inputToHex();
        	device.printf("Please enter the two digits for the Target ID byte in HEX (f.ex: 01, 23, A2):\n");
            uint32_t custom=inputToHex();
            device.printf("\n");
            TPChannelSetup(channelID,(uint8_t)custom);
            break;
        }
        case '2':
        {
            device.printf("Please enter the three digits for the own ID in HEX (f.ex: 123, 205, 7E0):");
            ownID=inputToHex();
            device.printf("\n");
            device.printf("Please enter the three digits for the remote ID in HEX (f.ex: 456, 306, 7E8):");
            rID=inputToHex();
            device.printf("\n");
            device.printf("Please enter the Session type to attempt to establish in HEX (Normally 01):");
            uint8_t seType=inputToHex();
            device.printf("\n");
            UDSChannelSetup(seType);
            break;
        }
        case '3':
        {
            device.printf("Please enter the three digits for the Channel Negotiation ID in HEX (f.ex: 200, 300, 0A0):\n");
            uint32_t channelID=inputToHex();
        	TPScan(channelID);
            break;
        }
        case '4':
        {
        	device.printf("Please enter the three digits for the first ID to scan in HEX (f.ex: 123, 205, 7E0):");
        	uint32_t first=inputToHex();
        	device.printf("\n");
        	device.printf("Please enter the three digits for the last ID to scan in HEX (f.ex: 456, 306, 7E8):");
        	uint32_t last=inputToHex();
        	device.printf("\n");
        	UDSScan(first,last);
            break;
        }
        case '5':
        {
            setCANSpeed();
            break;
        }
        case '6':
        {
        	device.printf("Please select a CAN to detect speed on (1 or 2)\n");
        	uint8_t busno = inputToHex();
        	if (busno != 1 && busno != 2)
        	{
        		device.printf("CAN%d does not exist\n\n");
        		return;
        	}
        	detectCANSpeed(busno);
            break;
        }
        case '7':
        {
            protocolMenu();
            break;
        }
        case '8':
        {
            MITMMode();
            break;
        }
        case '9':
        {
        	//parseCANStream("/Logging/tmp.txt", "/Logging/CAN.txt",0xFFFFFFFF);
        	ECUPowerMenu();
        	break;
        }
        default:
        {
            device.printf("Unknown option!\n");
            break;
        }
    }
}

void UDSSetSessionScan()
{
  for(uint32_t SessionType=0;SessionType<0x100;SessionType++)
  {
      uint8_t data[8]={0x10,SessionType,0,0,0,0};
      uint8_t buffer[256];
      uint8_t len=UDSResponseHandler(data,2,buffer,2,1);
      if (len !=0)
      {
          device.printf("Session was stablished correctly for ID %x\n", SessionType);
      }
      /*else if(len ==0 && rejectReason==1)
      {
    	  device.printf("->Scanned Session type was 0x%x\n\n",SessionType);
      }*/
  }
  
}

void TPSetSessionScan()
{
    device.printf("Scanning for available Session types, press any key to abort...\n");
	for(uint32_t SessionType=0;SessionType<0x100;SessionType++)
    {
      uint8_t data[8]={0x10,SessionType};
      uint8_t buffer[256];
      uint8_t len=TPResponseHandler(data,2,buffer,2,1);
      wait(0.01);
      if (len > 1 && buffer[0]==0x50)
      {
          device.printf("Session was stablished correctly with ID 0x%x\n\n", SessionType);
      }
      else if(len == 1)
      {
    	  device.printf("Scanned Session ID was 0x%x\n\n",SessionType);
      }
      else if(device.readable())
      {
    	  dumpSerial();
    	  device.printf("Aborted by user\n\n");
    	  return;
      }
      /*else if(len == 1)
      {
          device.printf("Scanned Session Type was 0x%x\n\n",SessionType);
      }*/
    }
}

void UDSReadDataByID2(uint8_t dataType)
{
  uint8_t data[7]={0x22,dataType,0,0,0,0};
  uint8_t buffer[256];
  uint8_t len=UDSResponseHandler(data,2,buffer,1,1);
  if (len !=0)
  {
      device.printf("Reply from ECU: ");
      for (uint8_t a=1;a<len;a++)
      {
        device.printf("%c", buffer[a]);//First two characters are the KWP2000 tunneling params
      }
      device.printf("\n");
  } 
}

void UDSReadDataByIDScan()
{
  device.printf("Scanning for Supported IDs, press any key to stop...\n\n");
  for(uint32_t testID=0; testID<0x100;testID++)
  {
      uint8_t data[7]={0x22,testID,0,0,0,0};
      uint8_t buffer[256];
      uint8_t len=UDSResponseHandler(data,2,buffer,2,1);
      if (len !=0)
      {
    	  device.printf("ECU replied to ID 0x%x with %d bytes: \n-ASCII:",testID ,(len - 2));
          for (uint8_t a=0;a<(len-2);a++)
          {
            device.printf("%c", buffer[a+2]);//First two characters are the KWP2000 tunneling params
          }
          device.printf("\n-HEX: ");
          for (uint8_t a=0;a<(len-2);a++)
          {
             device.printf("0x%x ", buffer[a+2]);//First two characters are the KWP2000 tunneling params
          }
          device.printf("\n\n");
      }
      if(device.readable())
      {
    	  dumpSerial();
    	  device.printf("Aborted by user\n\n");
    	  return;
      }
      /*else if(rejectReason==1 && len ==0)
      {
          device.printf("->Scanned Data ID was 0x%x\n\n",testID);
      }*/
  } 
}

void TPReadDataByID2(uint8_t dataType)
{

  uint8_t data[256]={0x1A,dataType};
  uint8_t buffer[256];
  uint8_t len=TPResponseHandler(data,2,buffer,1,1);
  if (len !=0)
  {
      device.printf("ECU replied with %d bytes: ", (len - 2));
      for (uint8_t a=2;a<len;a++)
      {
        device.printf("%c", buffer[a]);//First two characters are the KWP2000 tunneling params
      }
      device.printf("\n");
  } 
}

void TPReadDataByIDScan()
{
	device.printf("Scanning for Supported IDs, press any key to stop...\n\n");
	for(uint32_t dataType=0;dataType<0x100;dataType++)
    {
      uint8_t data[256]={0x1A,dataType};
      uint8_t buffer[256];
      uint8_t len=TPResponseHandler(data,2,buffer,2,1);
      wait(0.01);
      if (len >1)
      {
    	  device.printf("ECU replied to ID 0x%x with %d bytes: \n-ASCII:",dataType ,(len - 2));
    	  for (uint8_t a=0;a<(len-2);a++)
    	  {
    	      device.printf("%c", buffer[a+2]);//First two characters are the KWP2000 tunneling params
    	  }
    	  device.printf("\n-HEX: ");
    	  for (uint8_t a=0;a<(len-2);a++)
    	  {
    	      device.printf("0x%x ", buffer[a+2]);//First two characters are the KWP2000 tunneling params
    	  }
    	  device.printf("\n\n");
      }
      else if(len == 1)
      {
          device.printf("Scanned Data Type was 0x%x\n\n",dataType);
      }
      if(device.readable())
      {
    	  dumpSerial();
    	  device.printf("Aborted by user\n\n");
    	  return;
      }
    }
}


//******************Seed/Key operations**********//


bool RequestSeed(uint8_t accmod, uint8_t *buffer)
{
  uint8_t data[8]={0x27,accmod};
  if(protocol==0)
  {
	  if (TPResponseHandler(data,2,buffer,1,1))
	  {
		  return 1;
	  }
  }
  else
  {
	  if (UDSResponseHandler(data,2,buffer,1,1))
	  {
		  return 1;
	  }
  }
  return 0;
}  
  
bool SendKey(uint32_t key, uint8_t accmod)//need to remake this to work with any key size
{
  uint8_t data[8]={0x27,(accmod+1),(key>>24),(key>>16),(key>>8),key};
  uint8_t buffer[64]={0};//actually dummy
  if(protocol==0)
  {
	  if (TPResponseHandler(data,6,buffer,1,1))
	  {
		  return 1;
	  }
  }
  else
  {
	  if (UDSResponseHandler(data,6,buffer,1,1))
	  {
		  return 1;
	  }
  }
  return 0;
}
  
bool LVL3Auth(uint32_t Key, uint8_t accmod)
{
	 uint8_t buffer[24]={0};
	 if (!RequestSeed(accmod, buffer))
	  {
		return 0;
	  }
	  //now we handle the seed bytes
	  uint32_t Keyread=0;
	  if(protocol == 0)
	  {
		  Keyread = buffer[2];
		  Keyread = Keyread<<8;
		  Keyread = Keyread+buffer[3];
		  Keyread = Keyread<<8;
		  Keyread = Keyread+buffer[4];
		  Keyread = Keyread<<8;
		  Keyread = Keyread+buffer[5];
	  }
	  else
	  {
		  Keyread = buffer[1];
		  Keyread = Keyread<<8;
		  Keyread = Keyread+buffer[2];
		  Keyread = Keyread<<8;
		  Keyread = Keyread+buffer[3];
		  Keyread = Keyread<<8;
		  Keyread = Keyread+buffer[4];
	  }
	  Keyread=Keyread+Key;//here is where the key is used
	 //Done with the key generation
	  if (!SendKey(Keyread, accmod))
	  {
		return 0;
	  }
	  return 1;
}



bool LVL1Auth(uint32_t Key, uint8_t accmod)
{
  uint8_t buffer[24];
  if (!RequestSeed(accmod, buffer))
  {
    return 0;
  }
  //now we handle the seed bytes 
  uint32_t Keyread=0;
  if(protocol==0)
  {
	  Keyread=buffer[2];
	  Keyread=(Keyread<<8)+buffer[3];
	  Keyread=(Keyread<<8)+buffer[4];
	  Keyread=(Keyread<<8)+buffer[5];
  }
  else
  {
	  Keyread=buffer[1];
	  Keyread=(Keyread<<8)+buffer[2];
	  Keyread=(Keyread<<8)+buffer[3];
	  Keyread=(Keyread<<8)+buffer[4];
  }
  uint32_t cnt=0;//new algo, much cleaner! wow!
  for (cnt=0;cnt<5;cnt++)
  {
    uint32_t tmp=Keyread;
    Keyread=Keyread<<1;
    if ((tmp&0x80000000) != 0)
    {
      Keyread=Keyread^Key;
      Keyread=Keyread&0xFFFFFFFE;
    }
  }
  
  if (!SendKey(Keyread, accmod))
  {
    return 0;
  }
  return 1;
}



bool securityAccess(uint32_t (&Key), uint8_t accmod)
{
    /*long key: It is the key to be used to process the algorithm, and it is unique to each EDC15/EDC16 variant.
    accmod: Access mode (Security level to be accessed)
    */
  if (accmod == 0x01)
  {
    return LVL1Auth(Key, accmod);
  }
  else if (accmod == 0x03)
  {
    return LVL3Auth(Key, accmod);
  }
  else
  {
    return 0;
  }
}

/*******************End of Seed/Key***************/


bool UDSReadMemToFile(uint32_t lower, uint32_t upper,uint8_t steps)
{
	device.printf("Dumping all readable data between offset 0x%x to 0x%x onto SD...\n",lower,upper);
	uint32_t length;
	char filename[32]="/MemDumps/";
	char tmpbuf[64]={0};
	sprintf(tmpbuf,"M_%x_%x.bin",lower,upper);
	strngcat (filename,tmpbuf);
	if (!toggleDump(filename,2))//create or rewrite!
	{
		return 0;
	}
	uint32_t current=lower;
	length=1;
	while(length != 0 && !device.readable())
	{
		uint8_t request[8]={READ_MEMORY_BY_ADDRESS,0,0,0,0,0,0};
		if(current < 0x00000100)
		{
			request[1]=0x11;
			request[2]=current;
			request[3]=steps;
			length=4;
		}
		else if(current > 0x100 && current < 0x10000)
		{
			request[1]=0x12;
			request[2]=(current>>8);
			request[3]=current;
			request[4]=steps;
			length=5;
		}
		else if(current > 0x10000 && current < 0x1000000)
		{
			request[1]=0x13;
			request[2]=(current>>16);
			request[3]=(current>>8);
			request[4]=current;
			request[5]=steps;
			length=6;
		}
		else
		{
			request[1]=0x14;
			request[2]=(current>>24);
			request[3]=(current>>16);
			request[4]=(current>>8);
			request[5]=current;
			request[6]=steps;
			length=7;
		}
		current=(current+steps);
		uint8_t buffer[260]={0};
		length=UDSResponseHandler(request,length,buffer,0,1);
		if(length != 0)
		{
			file->write(buffer,length);//write to file
			file->fsync();
		}
		if(current>=upper)
		{
			length=0;//exit the loop if we have read what we want
		}
	}
	if(device.readable())
	{
		device.printf("Dump cancelled by user. Data is has been saved up to offset 0x%x\n\n",current);
		dumpSerial();
	}
	toggleDump(filename,0);
	device.printf("Done!\n");
	return 1;


}

bool UDSReadMemByAddress(uint32_t addr, uint8_t len2)//Reads memory by address. len is length for the address, and len2 is how many bytes to read
{
	uint8_t len=0;
	if((addr & 0x000000FF) == 0 &&  (addr & 0x0000FF00) == 0 && (addr & 0x00FF0000) == 0 && (addr & 0xFF000000) == 0)//enabled for fuzzing, though not realistic in normal usage. but thats where the fun is, right?
	{
		len = 0;
	}
	else if ((addr & 0x000000FF) != 0 &&  (addr & 0x0000FF00) == 0 && (addr & 0x00FF0000) == 0 && (addr & 0xFF000000) == 0)
	{
		len = 1;
	}
	else if ((addr & 0x000000FF) == 0 &&  (addr & 0x0000FF00) != 0 && (addr & 0x00FF0000) == 0 && (addr & 0xFF000000) == 0)
	{
		len = 2;
	}
	else if ((addr & 0x000000FF) == 0 &&  (addr & 0x0000FF00) == 0 && (addr & 0x00FF0000) != 0 && (addr & 0xFF000000) == 0)
	{
		len = 3;
	}
	else
	{
		len = 4;
	}
	uint8_t addrID=0x10+len;//address/length identifier
	uint8_t request[8]={READ_MEMORY_BY_ADDRESS,addrID,0,0,0,0,0};
	switch(len)
	{
		case 1:
		{
			request[2]=addr;
			request[3]=len2;
			break;
		}
		case 2:
		{
			request[2]=(addr>>8);
			request[3]=addr;
			request[4]=len2;
			break;
		}
		case 3:
		{
			request[2]=(addr>>16);
			request[3]=(addr>>8);
			request[4]=(addr);
			request[5]=len2;
			break;
		}
		case 4:
		{
			request[2]=(addr>>24);
			request[3]=(addr>>16);
			request[4]=(addr>>8);
			request[5]=addr;
			request[6]=len2;
			break;
		}
		default:
		{
			device.printf("you should never get here, wtf are you doin?\n");
			return 0;
		}
	}
	uint8_t buffer[265]={0};
	uint8_t lng=UDSResponseHandler(request,(3+len),buffer,1,1);
	if(lng==0)
	{
		return 0;
	}
	device.printf("\nReply from Server:\n-HEX: ");
	for(uint8_t a=1; a<lng; a++)
	{
		device.printf("0x%x ",buffer[a]);
	}
	device.printf("\n-ASCII: ");
	for(uint8_t a=1; a<lng; a++)
	{
		device.printf("%c",buffer[a]);
	}
	return 1;


}

void UDSReadMemFastScan()
{
	device.printf("Fast Scanning for readable offsets with current Diagnostics Session type and Security Access level...\n");
	uint32_t range=0;
	uint32_t lower=0;//lower range
	uint32_t upper=0;//upper range
	uint32_t remember=0;
	uint8_t ln=0;
	uint8_t flag=0;
	uint32_t ranges[33]={0};//to store the ranges for retrieving the files. the first byte is the counter for stored ranges
	while(range < 0x10000 && !device.readable())
	{
			uint8_t request[8]={READ_MEMORY_BY_ADDRESS,0,0,0,0,0,0,0};
			if(range<0x100)
			{
				request[1]=0x13;
				request[2]=range;
				request[5]=1;
				ln=6;
			}
			else
			{
				request[1]=0x14;
				request[2]=(range>>8);
				request[3]=range;
				request[6]=1;
				ln=7;
			}
			uint8_t buffer[265]={0};
			uint8_t lng=UDSResponseHandler(request,ln,buffer,0,1);
			if(lng!=0 && lower==0)
			{
				lower=(range<<16);
			}
			else if (lng==0 && lower !=0)
			{
				upper=(range<<16);
				device.printf("Device is readable between 0x%x and 0x%x\n",lower,upper);
				uint8_t p=ranges[0];
				ranges[((p * 2) + 1)]=lower;
				ranges[((p * 2) + 2)]=upper;
				ranges[0]=(p + 1);
				lower=0;
				upper=0;
				//add the code to choose which range to dump to SD
			}
			else
			{
				if(lng==0 && rejectReason==0 && flag==0)
				{
					device.printf("ECU is timing out, which can mean that the request is not valid or the connection died.\nThis will drastically slow down the scan...\n");
					flag=1;
				}
			}
			range++;
			if(range == (remember + 0x1000) && range !=0)
			{
				device.printf("Now scanning 0x%x\n",(range<<16));
				remember=range;
			}
	}
	if (device.readable())
	{
		device.printf("Scan aborted by user request\n");
		dumpSerial();
	}
	if(ranges[0] != 0)
	{
		char b=0;
		while(b != '0')
		{
			tick.attach(sendTesterPresent,0.5);
			device.printf("\nChoose a range/offset to dump to SD or hit 0 to exit:\n");
			for(uint8_t a=0;a<ranges[0];a++)//show the offsets;
			{
				device.printf("%d)0x%x - 0x%x\n",(a+1),ranges[((a * 2) + 1)],ranges[((a * 2) + 2)]);//doesnt show correct offsets, fix!
			}
			uint8_t choice=inputToDec();
			if(choice==0)
			{
				return;
			}
			if(choice>ranges[0])
			{
				device.printf("Please, select a number within the list\n");
			}
			else
			{
				lower=ranges[((choice * 2) -1)];
				upper=ranges[(choice * 2)];
				UDSReadMemToFile(lower,upper,1);
			}

		}
	}

}

void UDSMemoryMenu()
{
	device.printf("1)Read Memory by Address\n");
	device.printf("2)Fast Scan for readable offsets\n");
	device.printf("\n");
	while(!device.readable()){}
	char option=device.getc();
	dumpSerial();
	switch (option)
	{
		case '1':
		{
			device.printf("Please enter the address to read from, max 8 chars.\n");
	        uint32_t addr=inputToHex();
	        device.printf("Please enter how many bytes to read (01-FE)\n");
	        uint8_t len=0;
	        len=inputToHex();
	        device.printf("Attempting to read 0x%x bytes from address 0x%x\n",len,addr);
	        UDSReadMemByAddress(addr,len);
	        break;
		}
		case '2':
		{
			UDSReadMemFastScan();
			break;
		}
		default:
		{
		    device.printf("Unknown option\n");
		    break;
		}
	}


}


void UDSReadDataByID()
{
    device.printf("1)All data\n");
    device.printf("2)VIN\n");
    device.printf("3)ECU Hardware\n");
    device.printf("4)Supplier ECU Hardware\n"); 
    device.printf("5)ECU HW Version\n");
    device.printf("6)Supplier ECU SW\n");
    device.printf("7)ECU SW versio\n");
    device.printf("8)Custom ID\n");
    device.printf("9)Scan for supported IDs\n");   
    device.printf("\n"); 
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
        	device.printf("Requesting ALL data...\n\n");
        	UDSReadDataByID2(ALL_DATA);
            break;
        }
        case '2':
        {
        	device.printf("Requesting VIN...\n\n");
        	UDSReadDataByID2(VIN);
            break;
        }
        case '3':
        {
        	device.printf("Requesting ECU Version...\n\n");
        	UDSReadDataByID2(ECU_HW_VERSION);
            break;
        }
        case '4':
        {
        	device.printf("Requesting Supplier ECU HW...\n\n");
        	UDSReadDataByID2(SUPPLIER_ECU_HW);
            break;
        }
        case '5':
        {
        	device.printf("Requesting ECU HW Version...\n\n");
        	UDSReadDataByID2(ECU_HW_VERSION);
            break;
        }
        case '6':
        {
        	device.printf("Requesting Supplier ECU SW...\n\n");
        	UDSReadDataByID2(SUPPLIER_ECU_SW);
            break;
        }
        case '7':
        {
        	device.printf("Requesting ECU SW Version...\n\n");
        	UDSReadDataByID2(ECU_SW_VERSION);
            break;
        }
        case '8':
        {
            device.printf("Please enter the two digits for the Data type byte in HEX (f.ex: 23, 05, A2):");
            uint32_t custom=inputToHex();
            device.printf("\n\n");
            device.printf("Requesting Data for ID 0x%x...\n\n",custom);
            UDSReadDataByID2((uint8_t)custom);
            break;
        }
        case '9':
        {
            UDSReadDataByIDScan();
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }
}

void TPReadDataByID()
{
	device.printf("Select Data to request:\n\n");
	device.printf("1)All data\n");
    device.printf("2)VIN\n");
    device.printf("3)ECU Hardware\n");
    device.printf("4)Supplier ECU Hardware\n"); 
    device.printf("5)ECU HW Version\n");
    device.printf("6)Supplier ECU SW\n");
    device.printf("7)ECU SW versio\n");
    device.printf("8)Custom ID\n");
    device.printf("9)Scan for available IDs\n");   
    device.printf("\n"); 
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
        	device.printf("Requesting ALL data...\n\n");
        	TPReadDataByID2(ALL_DATA);
            break;
        }
        case '2':
        {
        	device.printf("Requesting VIN...\n\n");
        	TPReadDataByID2(VIN);
            break;
        }
        case '3':
        {
        	device.printf("Requesting ECU Version...\n\n");
        	TPReadDataByID2(ECU_HW_VERSION);
            break;
        }
        case '4':
        {
        	device.printf("Requesting Supplier ECU HW...\n\n");
        	TPReadDataByID2(SUPPLIER_ECU_HW);
            break;
        }
        case '5':
        {
        	device.printf("Requesting ECU HW Version...\n\n");
        	TPReadDataByID2(ECU_HW_VERSION);
            break;
        }
        case '6':
        {
        	device.printf("Requesting Supplier ECU SW...\n\n");
        	TPReadDataByID2(SUPPLIER_ECU_SW);
            break;
        }
        case '7':
        {
        	device.printf("Requesting ECU SW Version...\n\n");
        	TPReadDataByID2(ECU_SW_VERSION);
            break;
        }
        case '8':
        {
            device.printf("Please enter the two digits for the Data type byte in HEX (f.ex: 23, 05, A2):");
            uint32_t custom=inputToHex();
            device.printf("\n\n");
            device.printf("Requesting Data for ID 0x%x...\n\n",custom);
            TPReadDataByID2((uint8_t)custom);
            break;
        }
        case '9':
        {
            TPReadDataByIDScan();
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }
}



void UDSECUReset()
{
   device.printf("Select type of Reset:\n\n");
   device.printf("1)Hard Reset\n");
   device.printf("2)Ignition on/off Reset\n");
   device.printf("3)Soft reset\n");
   device.printf("4)Custom\n");  
   device.printf("\n");
   uint8_t resetType=0; 
   while(!device.readable()){}
   char option=device.getc();
   dumpSerial();
   switch (option)
   {
        case '1':
        {
            resetType=HARD_RESET;
            break;
        }
        case '2':
        {
            resetType=IGNITION_RESET;
            break;
        }
        case '3':
        {
            resetType=SOFT_RESET;
            break;
        }
        case '4':
        {
            device.printf("Please enter the two digits for the Reset type byte in HEX (f.ex: 23, 05, A2):");
            uint32_t custom=inputToHex();
            device.printf("\n");
            resetType=(uint8_t)custom;
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            return;
        }
   }    
   uint8_t data[7]={ECU_RESET,resetType,0,0,0,0,0};
   uint8_t buffer[256];
   uint8_t len=UDSResponseHandler(data,2,buffer,1,1);
   if (len !=0)
   { 
        device.printf("ECU has been reset!");
        inSession=0;
   }
}

void TPECUReset()
{
   device.printf("Select type of Reset:\n\n");
   device.printf("1)Hard Reset\n");
   device.printf("2)Ignition on/off Reset\n");
   device.printf("3)Soft reset\n");
   device.printf("4)Custom\n");  
   device.printf("\n");
   uint8_t resetType=0; 
   while(!device.readable()){}
   char option=device.getc();
   dumpSerial();
   switch (option)
   {
        case '1':
        {
            resetType=HARD_RESET;
            break;
        }
        case '2':
        {
            resetType=IGNITION_RESET;
            break;
        }
        case '3':
        {
            resetType=SOFT_RESET;
            break;
        }
        case '4':
        {
            device.printf("Please enter the two digits for the Reset type byte in HEX (f.ex: 23, 05, A2):");
            uint32_t custom=inputToHex();
            device.printf("\n");
            resetType=(uint8_t)custom;
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            return;
        }
   }
   uint8_t data[256]={ECU_RESET,resetType};
   uint8_t buffer[256];
   uint8_t len=TPResponseHandler(data,2,buffer,1,1);
   if (len !=0 && buffer[0]==(ECU_RESET + 0x40))
   { 
        device.printf("ECU has been reset!");
        inSession=0;//exit session
   }
}

void UDSDiagSessionControl()
{
    device.printf("1)Switch to Default session\n");
    device.printf("2)Switch to Programming Session\n");
    device.printf("3)Switch to Extended Session\n");
    device.printf("4)Switch to Custom session\n");
    device.printf("5)Scan for available Session types\n");    
    device.printf("\n"); 
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
            UDSSetSession(UDS_DEFAULT_SESSION, 1);
            break;
        }
        case '2':
        {
            UDSSetSession(UDS_PROGRAMMING_SESSION, 1);
            break;
        }
        case '3':
        {
            UDSSetSession(UDS_EXTENDED_SESSION, 1);
            break;
        }
        case '4':
        {
            device.printf("Please enter the two digits for the Session type byte in HEX (f.ex: 23, 05, A2):");
            uint32_t custom=inputToHex();
            device.printf("\n");
            UDSSetSession((uint8_t)custom, 1);
            break;
        }
        case '5':
        {
            UDSSetSessionScan();
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }
}

void TPDiagSessionControl()
{
    device.printf("1)Switch to Default Session\n");
    device.printf("2)Switch to EOL System Supplier Session\n");
    device.printf("3)Switch to ECU Programming Session\n");
    device.printf("4)Switch to Component Starting session\n");
    device.printf("5)Switch to Custom session\n");
    device.printf("6)Scan for available Session types\n");    
    device.printf("\n"); 
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
            TPSetSession(KWP_DEFAULT_SESSION, 1);
            break;
        }
        case '2':
        {
            TPSetSession(KWP_EOL_SYSTEM_SUPPLIER, 1);
            break;
        }
        case '3':
        {
            TPSetSession(KWP_ECU_PROGRAMMING_MODE, 1);
            break;
        }
        case '4':
        {
            TPSetSession(KWP_COMPONENT_STARTING, 1);
            break;
        }
        case '5':
        {
            device.printf("Please enter the two digits for the Session type byte in HEX (f.ex: 23, 05, A2):");
            uint32_t custom=inputToHex();
            device.printf("\n");
            TPSetSession((uint8_t)custom, 1);
            break;
        }
        case '6':
        {
            TPSetSessionScan();
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }
}

void TPSecurityAccessAuto()
{
    device.printf("Please select security level (algorithm)\n");
    device.printf("1)Level 01\n");
    device.printf("2)Level 03\n");
    device.printf("\n"); 
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    uint8_t levl;
    uint32_t key;
    switch (option)
    {
        case '1':
        {
            levl=1;
            break;
        }
        case '2':
        {
            levl=3;
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            return;
        }
    }
    device.printf("Please enter the  the key (2FC9, 5A5B5C5D, A05B..)\n");
    device.printf("\n");
    key=inputToHex();
    device.printf("\nAttempting Security Access for level %d with key 0x%x\n",levl,key);
    if(securityAccess(key,levl))
    {
        device.printf("\nSecurity Access granted!\n");
    }  
}

void UDSSecurityAccessAuto()
{
    device.printf("Please select security level (algorithm)\n");
    device.printf("1)Level 01\n");
    device.printf("2)Level 03\n");
    device.printf("\n"); 
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    uint8_t levl;
    uint32_t key;
    switch (option)
    {
        case '1':
        {
            levl=1;
            break;
        }
        case '2':
        {
            levl=3;
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            return;
        }
    }
    device.printf("Please enter the  the key (2FC9, 5A5B5C5D, A05B..)\n");
    device.printf("\n"); 
    key=inputToHex();
    device.printf("\nAttempting Security Access for level %d with key 0x%x\n",levl,key);
    if(securityAccess(key,levl))
    {
    	device.printf("\nSecurity Access granted!\n");
    }  
}


void UDSSecurityAccessManual()
{
    
}

void TPSecurityAccessManual()
{
    
}

void UDSSecurityAccess()
{
    device.printf("1)Use known algorithm\n");
    device.printf("2)Manual authentication\n");  
    device.printf("\n"); 
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
            UDSSecurityAccessAuto();
            break;
        }
        case '2':
        {
            UDSSecurityAccessManual();
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }
}

void TPSecurityAccess()
{
    device.printf("1)Use known algorithm\n");
    device.printf("2)Manual authentication\n");  
    device.printf("\n"); 
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
            TPSecurityAccessAuto();
            break;
        }
        case '2':
        {
            TPSecurityAccessManual();
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }
}


void TPUploadScan(uint32_t delay)
{
	device.printf("Scanning for readable offsets, this will take a while... \nPress any key to cancel...\n");
	uint32_t start=0;
	uint32_t end = 0;
	uint8_t burp=0;
	uint32_t lastOffset=0;
	for(uint32_t a=0; a<0xffffffff; a = (a + 0x1000))
	{
		if(a == (lastOffset + 0x100000))
		{
			device.printf("Now trying offset 0x%x \n", a);
			lastOffset=a;
		}
		if(TPUpload(a, 0xff,0))
		{
			if(burp == 0)
			{
				start = a;
				burp = 1;
			}
			end = a;
		}
		else
		{
			if(burp != 0)
			{
				if (start == end)
				{
					device.printf("Device is readable from offset 0x%x but for less than 0x1000 bytes\n", start);
				}
				else
				{
					device.printf("Device is readable from offset 0x%x to 0x%x\n", start, a);
				}
				start=0;
				burp=0;
			}
		}
		wait_ms(delay);//to throttle it down a little bit, so we dont miss the channel keep alive
		if(device.readable())
		{
			wait(0.1);
			dumpSerial();
			device.printf("Aborted by user\n");
			return;
		}
	}
	device.printf("Done!\n");
}


void TPMemoryMenu()
{
    device.printf("Please select an option\n");
    device.printf("1)Request Upload (from target)\n");
    device.printf("2)Scan for Upload ranges\n");
    device.printf("\n");
    while(!device.readable()){}
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
		case '1':
		{
			device.printf("Please enter the  target Address (180000, 0, A0000..). Max is FFFFFF\n");
			device.printf("\n");
			uint32_t addr=inputToHex();
			device.printf("Please enter the  Length to read (10, FF, 1000..). Max is FFFFFF\n");
			device.printf("\n");
			uint32_t llen=inputToHex();
			TPUpload(addr, llen,1);
			break;
		}
		case '2':
		{
			device.printf("Please enter delay between requests in ms. Suggested value is 80, as lower values might cause disconnection or timeouts\n");
			uint32_t delayy=inputToDec();
			TPUploadScan(delayy);
		}
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }

}

void TPMenu()
{
	device.printf("\nPlease, select an option:\n");
    device.printf("\n");
    device.printf("1)Diagnostic Session Control\n");
    device.printf("2)Read Data by ID\n");
    device.printf("3)Security Access\n");
    device.printf("4)ECU Reset\n");
    device.printf("5)Memory Operations\n");
    device.printf("6)Terminate current session\n");
    device.printf("\n"); 
    while(!device.readable())
    {
    	if(inSession == 0)
    	{
    		device.printf("Connection with ECU is lost...\n");
    		TPSendCA(0);
    		return;
    	}
    }
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
            TPDiagSessionControl();
            break;
        }
        case '2':
        {
            TPReadDataByID();
            break;
        }
        case '3':
        {
            TPSecurityAccess();
            break;
        }
        case '4':
        {
            TPECUReset();
            break;
        }
        case'5':
        {
        	TPMemoryMenu();
        	break;
        }
        case '6': //apparently, disconnect is not sent. need to check it
        {
            inSession=0;
            device.printf("Terminating current session\n");
            TPSendCA(0);
            uint8_t data1[8]={CHANNEL_DISCONNECT,0,0,0,0,0,0,0};
			uint8_t timeout=0;
			while (!can1.write(CANMessage(ownID, reinterpret_cast<char*>(&data1), 1)) && !device.readable() && timeout < 10)
			{
				timeout++;
			}//make sure the msg goes out
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }
      
}

void UDSMenu()
{
	device.printf("\nPlease, select an option:\n");
    device.printf("\n");
    device.printf("1)Diagnostic Session Control\n");
    device.printf("2)Read Data by ID\n");
    device.printf("3)Security Access\n");
    device.printf("4)ECU Reset\n");
    device.printf("5)Memory Operations\n");
    device.printf("6)Tester Address\n");
    device.printf("7)Terminate current session\n");
    device.printf("\n"); 
    while(!device.readable())
    {
        if(inSession == 0)
        {
        	device.printf("Connection with ECU is lost...\n\n");
        	return;
        }
    }
    char option=device.getc();
    dumpSerial();
    switch (option)
    {
        case '1':
        {
            UDSDiagSessionControl();
            break;
        }
        case '2':
        {
            UDSReadDataByID();
            break;
        }
        case '3':
        {
            UDSSecurityAccess();
            break;
        }
        case '4':
        {
            UDSECUReset();
            break;
        }
        case '5':
        {
        	UDSMemoryMenu();
        	break;
        }
        case '6':
        {
        	TesterAddressMenu();
        	break;
        }
        case '7':
        {
            inSession=0;
            device.printf("Terminating current session\n");
            tick.detach();
            break;
        }
        default:
        {
            device.printf("Unknown option\n");
            break;
        }
    }
    
}

void deviceInit()
{
    //device.baud(115200);
	wait(3);
	device.printf("****************************************************************\n");
	device.printf("   ______ ___     _   __ ____              __                   \n");
	device.printf("  / ____//   |   / | / // __ ) ____ _ ____/ /____ _ ___   _____ \n");
	device.printf(" / /    / /| |  /  |/ // __  |/ __ `// __  // __ `// _ \\ / ___/\n");
	device.printf("/ /___ / ___ | / /|  // /_/ // /_/ // /_/ // /_/ //  __// /     \n");
	device.printf("\\____//_/  |_|/_/ |_//_____/ \\__,_/ \\__,_/ \\__, / \\___//_/ \n");
	device.printf("By Code White GmbH 2016                   /____/                \n");
    device.printf("****************************************************************\n\n");
    device.printf("\n\nSystem is booting\n");
    setLED(LED_RED);
    device.printf("Setting up CAN interfaces at 500kbps by default...\n");
    can1.frequency(500000);
    can2.frequency(500000);
    device.printf("Setting up XRAM speed at 20MHz...\n");
    ram.frequency(20000000);//pimp up the XRAM speed
    device.printf("Checking folders...\n");
    if(!doesDirExist("/sd"))
    {
    	device.printf("SD card not detected, functionality will be limited!\n");
    }
    else
    {
		if(!doesDirExist("/sd/Emulator"))
		{
			if(mkdir("/sd/Emulator", 0777) != 0)
			{
				device.printf("Failed to create Emulator folder\n");
			}
			else
			{
				device.printf("-Emulator folder created\n");
			}
		}
		if(!doesDirExist("/sd/MemDumps"))
		{
			if(mkdir("/sd/MemDumps", 0777) == 0)
			{
				device.printf("-MemDumps folder created\n");
			}
			else
			{
				device.printf("Failed to create MemDumps folder\n");
			}
		}
		if(!doesDirExist("/sd/Logging"))
		{
			if(mkdir("/sd/Logging", 0777) == 0)
			{
				device.printf("-Logging folder created\n");
			}
			else
			{
				device.printf("Failed to create Logging folder\n");
			}
		}
		if(!doesDirExist("/sd/MITM"))
		{
			if(mkdir("/sd/MITM", 0777) == 0)
			{
				device.printf("-MITM folder created\n");
			}
			else
			{
				device.printf("Failed to create MITM folder\n");
			}
		}
		if(!doesDirExist("/sd/Replay"))
		{
			if(mkdir("/sd/Replay", 0777) == 0)
			{
				device.printf("-Replay folder created\n");
			}
			else
			{
				device.printf("Failed to create Replay folder\n");
			}
		}
		if(!doesDirExist("/sd/Transfers"))
		{
			if(mkdir("/sd/Transfers", 0777) == 0)
			{
				device.printf("-Transfers folder created\n");
			}
			else
			{
				device.printf("Failed to create Transfers folder\n");
			}
		}
    }
    double iv=getVoltage();
    if(iv < 11.5)
    {
    	device.printf("Power Supply voltage too low or not connected\n");
    }
    else if(iv >= 14.7)
    {
    	device.printf("Power Supply voltage too high, damage to CANBadger and/or ECU might occur!\n");
    }
    else
    {
    	device.printf("Power Supply voltage: %fV\n",getVoltage());
    }
    device.printf("System is ready!\n\n");
    setLED(LED_GREEN);
    device.printf("\n");
}

void ethernetMainThread(void const *args) {
    deviceInit();
    ECU_CTRL=1;

    while(1) {
    	checkBtn();

    	osEvent evt = commandQueue->get(25);
    	if(evt.status == osEventMail) {
    		EthernetMessage *msg;
			msg = 0;
			msg = (EthernetMessage*) evt.value.p;
			if(msg != 0) {
				// execute
				switch(msg->actionType) {
				case LOG_RAW_CAN_TRAFFIC:
					CANLogger(0, false);
					Thread::wait(10); // give it a little rest
					break;
				case SETTINGS:
					if(settingsHandler->handleSettingsUpdate(msg))
						ethernetManager->sendMessage(ACK, new char[1], 0);
					else
						ethernetManager->sendMessage(NACK, new char[1], 0);
					break;
				case RESET:
					TestManager::resetDevice();
					break;
				case START_UDS: {
					ownID = getUInt32FromData(0, msg->data);
					rID = getUInt32FromData(4, msg->data);

					if(UDSChannelSetup(0x1)) {
						ethernetManager->sendMessage(ACK, new char[1], 0);
						UDSMenu(); // TODO
					} else {
						ethernetManager->sendMessage(NACK, new char[1], 0);
					}

					break;
				}
				case START_TP: {
					uint32_t moduleID = getUInt32FromData(0, msg->data);
					uint32_t channelNegotiationId = getUInt32FromData(4, msg->data);

					// TODO: check if the parameters are the right ones
					if(TPChannelSetup(moduleID, channelNegotiationId)) {
						ethernetManager->sendMessage(ACK, new char[1], 0);
						TPMenu(); // TODO
					} else {
						ethernetManager->sendMessage(NACK, new char[1], 0);
					}

					break;
				}
				// SD
				case UPDATE_SD:
					SdHelper::updateSdContents(&sd, ethernetManager, msg->data);
					break;
				case DOWNLOAD_FILE:
					SdHelper::downloadFile(&sd, ethernetManager, msg->data);
					break;
				case DELETE_FILE:
					SdHelper::deleteFile(&sd, ethernetManager, msg->data);
					break;
				// MITM
				case CLEAR_RULES:
					MitmHelper::clearRules(&sd, ethernetManager);
					break;
				case ADD_RULE:
					MitmHelper::addRule(&sd, msg, ethernetManager);
					break;
				case ENABLE_MITM_MODE:
					settings->currentActionIsRunning = true;
					MITMMode();
					break;
				// REPLAY
				case START_REPLAY: {
					setLED(LED_ORANGE);
					ReplayHelper rh(ethernetManager, settings, commandQueue, &can1, &can2);
					rh.sendFramesLoop();
					setLED(LED_GREEN);
					break;
				}
				default:
					break;
				}
				commandQueue->free(msg); // clean up
			}
    	}
    }
}

void uartMain()
{
    deviceInit();
    ECU_CTRL=0;
    while(1)
    {
        if(inSession==0)
        {
            menu();
        }
        else
        {
        	setLED(LED_ORANGE);//to indicate that the badger is busy
        	if(protocol==0)
            {
            	TPSendCA(1);
            	TPMenu();
            }
            else
            {
            	tick.attach(sendTesterPresent,0.5);//send the TesterPresent packet every half second if we are idle but in a session
                UDSMenu();
            }
        }
    } 
}

int main()
{
AnalogIn entropyPin(p20);
	srand(getVoltage() + entropyPin.read());

	setLED(LED_OFF);
	uartMode = false;
	for(int i = 0; i < 2; i++) {
		wait(1);
		if(device.configured()) {
			setLED(LED_GREEN);
			uartMode = true;
		} else {
			setLED(LED_RED);
			uartMode = false;
		}
	}

	commandQueue = new Mail<EthernetMessage, 16>();
	settings = CanbadgerSettings::restore(&sd);
	settingsHandler = new SettingsHandler(settings);
	if(uartMode) {
		blinkLED(1, LED_GREEN);
		uartMain();
	} else {
		blinkLED(1, LED_RED);
		Thread thr(ethernetMainThread, NULL, osPriorityNormal, (DEFAULT_STACK_SIZE * 2));
		ethernetManager = new EthernetManager(rand() % 255, settingsHandler, commandQueue, &ram, &ramCS1);
		ethernetManager->run();
	}
}
