//  Based on https://github.com/tftelkamp/single_chan_pkt_fwd/blob/master/main.cpp
/*******************************************************************************
 *
 * Copyright (c) 2015 Thomas Telkamp
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 *******************************************************************************/

#include <string>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <iostream>
#include <cstdlib>
#include <sys/time.h>
#include <cstring>

#include <sys/ioctl.h>
#include <net/if.h>

using namespace std;

////#include "base64.h"

#include <wiringPi.h>
#include <wiringPiSPI.h>
#include "hope_rfm96.h"
#include "dragino_gps_hat.h"
#include "lora_interface.h"

typedef bool boolean;
typedef unsigned char byte;

static const int CHANNEL = 0;

byte currentMode = 0x81;

extern char lora_packet[];
char b64[256];

bool is_sx1272 = true;

byte receivedbytes;

struct sockaddr_in si_other;
int s, slen=sizeof(si_other);
struct ifreq ifr;

uint32_t cp_nb_rx_rcv;
uint32_t cp_nb_rx_ok;
uint32_t cp_nb_rx_bad;
uint32_t cp_nb_rx_nocrc;
uint32_t cp_up_pkt_fwd;

enum sf_t { SF7=7, SF8, SF9, SF10, SF11, SF12 };

/*******************************************************************************
 *
 * Configure these values!
 *
 *******************************************************************************/

////  TP-IoT: Mode 1 is max range. TP-IoT Gateway runs on:
////    case 1:     setCR(CR_5);        // CR = 4/5
////                setSF(SF_12);       // SF = 12
////                setBW(BW_125);      // BW = 125 KHz
//setModemConfig(Bw125Cr45Sf4096);  ////  TP-IoT Mode 1
int transmission_mode = 1;

// SX1272 - Raspberry connections
int ssPin = 6;
int dio0  = 7;
int RST   = 0;

// Set spreading factor (SF7 - SF12)
////sf_t sf = SF7;
sf_t sf =
    (transmission_mode == 1) ? SF12 :  ////  TP-IoT Mode 1.
    (transmission_mode == 5) ? SF10 :  ////  TP-IoT Mode 5.
    SF10;

// Set center frequency
////uint32_t  freq = 868100000; // in Mhz! (868.1)
////  TP-IoT: uint32_t LORA_CH_10_868 = CH_10_868; //  0xD84CCC; // channel 10, central freq = 865.20MHz  ////  Lup Yuen
uint32_t  freq = 865200000; //// TP-IoT: 865.20 MHz

// Set location
float lat=0.0;
float lon=0.0;
int   alt=0;

/* Informal status fields */
static char platform[24]    = "Single Channel Gateway";  /* platform definition */
static char email[40]       = "";                        /* used for contact email */
static char description[64] = "";                        /* used for free form description */

// define servers
// TODO: use host names and dns
////#define SERVER1 "54.72.145.119"    // The Things Network: croft.thethings.girovito.nl
#define SERVER1 "127.0.0.1"    ////  TP-IoT: Bypass Things Network.
//#define SERVER2 "192.168.1.10"      // local
#define PORT 1700                   // The port on which to send data

// #############################################
// #############################################

#define REG_FIFO                    0x00
#define REG_FIFO_ADDR_PTR           0x0D
#define REG_FIFO_TX_BASE_AD         0x0E
#define REG_FIFO_RX_BASE_AD         0x0F
#define REG_RX_NB_BYTES             0x13
#define REG_OPMODE                  0x01
#define REG_FIFO_RX_CURRENT_ADDR    0x10
#define REG_IRQ_FLAGS               0x12
#define REG_DIO_MAPPING_1           0x40
#define REG_DIO_MAPPING_2           0x41
#define REG_MODEM_CONFIG            0x1D
#define REG_MODEM_CONFIG2           0x1E
#define REG_MODEM_CONFIG3           0x26
#define REG_SYMB_TIMEOUT_LSB  		0x1F
#define REG_PKT_SNR_VALUE			0x19
#define REG_PAYLOAD_LENGTH          0x22
#define REG_IRQ_FLAGS_MASK          0x11
#define REG_MAX_PAYLOAD_LENGTH 		0x23
#define REG_HOP_PERIOD              0x24
#define REG_SYNC_WORD				0x39
#define REG_VERSION	  				0x42

#define SX72_MODE_RX_CONTINUOS      0x85
#define SX72_MODE_TX                0x83
#define SX72_MODE_SLEEP             0x80
#define SX72_MODE_STANDBY           0x81

#define PAYLOAD_LENGTH              0x40

// LOW NOISE AMPLIFIER
#define REG_LNA                     0x0C
#define LNA_MAX_GAIN                0x23
#define LNA_OFF_GAIN                0x00
#define LNA_LOW_GAIN		    	0x20

// CONF REG
#define REG1                        0x0A
#define REG2                        0x84

#define SX72_MC2_FSK                0x00
#define SX72_MC2_SF7                0x70
#define SX72_MC2_SF8                0x80
#define SX72_MC2_SF9                0x90
#define SX72_MC2_SF10               0xA0
#define SX72_MC2_SF11               0xB0
#define SX72_MC2_SF12               0xC0

#define SX72_MC1_LOW_DATA_RATE_OPTIMIZE  0x01 // mandated for SF11 and SF12

// FRF
#define        REG_FRF_MSB              0x06
#define        REG_FRF_MID              0x07
#define        REG_FRF_LSB              0x08

#define        FRF_MSB                  0xD9 // 868.1 Mhz
#define        FRF_MID                  0x06
#define        FRF_LSB                  0x66

#define BUFLEN 2048  //Max length of buffer

#define PROTOCOL_VERSION  1
#define PKT_PUSH_DATA 0
#define PKT_PUSH_ACK  1
#define PKT_PULL_DATA 2
#define PKT_PULL_RESP 3
#define PKT_PULL_ACK  4

#define TX_BUFF_SIZE  2048
#define STATUS_SIZE	  1024

void die(const char *s)
{
    perror(s);
    //exit(1);
}

void selectreceiver()
{
    digitalWrite(ssPin, LOW);
}

void unselectreceiver()
{
    digitalWrite(ssPin, HIGH);
}

uint8_t readDraginoRegister(uint8_t addr)
{
    unsigned char spibuf[2];

    selectreceiver();
    spibuf[0] = addr & 0x7F;
    spibuf[1] = 0x00;
    wiringPiSPIDataRW(CHANNEL, spibuf, 2);
    unselectreceiver();

    return spibuf[1];
}

void writeDraginoRegister(uint8_t addr, uint8_t value)
{
    unsigned char spibuf[2];

    spibuf[0] = addr | 0x80;
    spibuf[1] = value;
    selectreceiver();
    wiringPiSPIDataRW(CHANNEL, spibuf, 2);

    unselectreceiver();
}


boolean receivePkt(char *payload)
{

    // clear rxDone
    writeDraginoRegister(REG_IRQ_FLAGS, 0x40);

    int irqflags = readDraginoRegister(REG_IRQ_FLAGS);

    cp_nb_rx_rcv++;

    //  payload crc: 0x20
    if((irqflags & 0x20) == 0x20)
    {
        printf("CRC error\n");
        writeDraginoRegister(REG_IRQ_FLAGS, 0x20);
        return false;
    } else {

        cp_nb_rx_ok++;

        byte currentAddr = readDraginoRegister(REG_FIFO_RX_CURRENT_ADDR);
        byte receivedCount = readDraginoRegister(REG_RX_NB_BYTES);
        receivedbytes = receivedCount;

        writeDraginoRegister(REG_FIFO_ADDR_PTR, currentAddr);

        for(int i = 0; i < receivedCount; i++)
        {
            payload[i] = (char)readDraginoRegister(REG_FIFO);
        }
    }
    return true;
}

int setupDraginoLoRa(int address, int mode, uint32_t channel, char *power)
{
    //  Initialise the Pi pins.
    wiringPiSetup ();
    pinMode(ssPin, OUTPUT);
    pinMode(dio0, INPUT);
    pinMode(RST, OUTPUT);

    //int fd =
    wiringPiSPISetup(CHANNEL, 500000);
    //cout << "Init result: " << fd << endl;

    transmission_mode = mode;
    digitalWrite(RST, HIGH);
    delay(100);
    digitalWrite(RST, LOW);
    delay(100);

    byte version = readDraginoRegister(REG_VERSION);

    if (version == 0x22) {
        // sx1272
        printf("SX1272 detected, starting.\n");
        is_sx1272 = true;
    } else {
        // sx1276?
        digitalWrite(RST, LOW);
        delay(100);
        digitalWrite(RST, HIGH);
        delay(100);
        version = readDraginoRegister(REG_VERSION);
        if (version == 0x12) {
            // sx1276
            printf("SX1276 detected, starting.\n");
            is_sx1272 = false;
        } else {
            printf("Unrecognized transceiver.\n");
            //printf("Version: 0x%x\n",version);
            return -1;
        }
    }

    writeDraginoRegister(REG_OPMODE, SX72_MODE_SLEEP);

    // set frequency
    uint64_t frf = ((uint64_t)freq << 19) / 32000000;
    writeDraginoRegister(REG_FRF_MSB, (uint8_t)(frf>>16) );
    writeDraginoRegister(REG_FRF_MID, (uint8_t)(frf>> 8) );
    writeDraginoRegister(REG_FRF_LSB, (uint8_t)(frf>> 0) );

    //  TODO: This code may be redundant due to settings below.
    if (is_sx1272) {
        if (sf == SF11 || sf == SF12) {
            writeDraginoRegister(REG_MODEM_CONFIG,0x0B);
        } else {
            writeDraginoRegister(REG_MODEM_CONFIG,0x0A);
        }
        writeDraginoRegister(REG_MODEM_CONFIG2,(sf<<4) | 0x04);
    } else {
        if (sf == SF11 || sf == SF12) {
            writeDraginoRegister(REG_MODEM_CONFIG3,0x0C);
        } else {
            writeDraginoRegister(REG_MODEM_CONFIG3,0x04);
        }
        writeDraginoRegister(REG_MODEM_CONFIG,0x72);
        writeDraginoRegister(REG_MODEM_CONFIG2,(sf<<4) | 0x04);
    }

    if (sf == SF10 || sf == SF11 || sf == SF12) {
        writeDraginoRegister(REG_SYMB_TIMEOUT_LSB,0x05);
    } else {
        writeDraginoRegister(REG_SYMB_TIMEOUT_LSB,0x08);
    }
    writeDraginoRegister(REG_MAX_PAYLOAD_LENGTH,0x80);
    writeDraginoRegister(REG_PAYLOAD_LENGTH,PAYLOAD_LENGTH);
    writeDraginoRegister(REG_HOP_PERIOD,0xFF);
    writeDraginoRegister(REG_FIFO_ADDR_PTR, readDraginoRegister(REG_FIFO_RX_BASE_AD));

    //  TP-IoT: Fixed constants according to http://www.hoperf.com/upload/rf/RFM95_96_97_98W.pdf
    const int FIXED_RH_RF95_BW_125KHZ                             = 0x70;
    const int FIXED_RH_RF95_BW_250KHZ                             = 0x80;
    const int FIXED_RH_RF95_CODING_RATE_4_5                       = 0x02;
    const int FIXED_RH_RF95_CODING_RATE_4_6                       = 0x04;
    const int FIXED_RH_RF95_CODING_RATE_4_7                       = 0x06;
    const int FIXED_RH_RF95_CODING_RATE_4_8                       = 0x08;
    const int FIXED_RH_RF95_RX_PAYLOAD_CRC_IS_ON                  = 0x04;
    const int RH_RF95_SPREADING_FACTOR                            = 0xf0;
    const int RH_RF95_SPREADING_FACTOR_64CPS                      = 0x60;
    const int RH_RF95_SPREADING_FACTOR_128CPS                     = 0x70;
    const int RH_RF95_SPREADING_FACTOR_256CPS                     = 0x80;
    const int RH_RF95_SPREADING_FACTOR_512CPS                     = 0x90;
    const int RH_RF95_SPREADING_FACTOR_1024CPS                    = 0xa0;
    const int RH_RF95_SPREADING_FACTOR_2048CPS                    = 0xb0;
    const int RH_RF95_SPREADING_FACTOR_4096CPS                    = 0xc0;
    switch (transmission_mode) {
        case 1: {
            //  Mode 1 is max range. TP-IoT Gateway runs on:
            //    case 1:     setCR(CR_5);        // CR = 4/5
            //                setSF(SF_12);       // SF = 12
            //                setBW(BW_125);      // BW = 125 KHz
            //  TP-IoT Mode 1: Bw125Cr45Sf4096
            writeDraginoRegister(REG_RegModemConfig1, FIXED_RH_RF95_BW_125KHZ + FIXED_RH_RF95_CODING_RATE_4_5);
            writeDraginoRegister(REG_RegModemConfig2, RH_RF95_SPREADING_FACTOR_4096CPS /* + FIXED_RH_RF95_RX_PAYLOAD_CRC_IS_ON */);
            break;
        }
        default:
            printf("Unknown transmission_mode %d\n", transmission_mode);
    }

    //  TP-IoT: Preamble length 8.
    const int preamble_length = 8;
    writeDraginoRegister(REG_RegPreambleMsb, preamble_length >> 8);
    writeDraginoRegister(REG_RegPreambleLsb, preamble_length & 0xff);
    //  TP-IoT sync.
    const int RH_RF69_REG_39_NODEADRS = 0x39;
    writeDraginoRegister(RH_RF69_REG_39_NODEADRS, 0x12);

    int tmp = 0x04;  //  AGC ON
    if (transmission_mode == 1) //  Low Data Rate Optimisation mandated for when the symbol length exceeds 16ms
        tmp |= 0x08;  //  Data will be scrambled if you don't use this.
    writeDraginoRegister(REG_RegModemConfig3, tmp);
    //  Setting Testmode.
    writeDraginoRegister(0x31, 0x43);
    //  Set LowPnTxPllOff.
    writeDraginoRegister(REG_RegPaRamp, 0x09);

    // Set Continous Receive Mode
    writeDraginoRegister(REG_LNA, LNA_MAX_GAIN);  // Important for reception: max lna gain
    writeDraginoRegister(REG_OPMODE, SX72_MODE_RX_CONTINUOS);

    return 0;
}

void sendudp(char *msg, int length) {

//send the update
#ifdef SERVER1
    inet_aton(SERVER1 , &si_other.sin_addr);
    if (sendto(s, (char *)msg, length, 0 , (struct sockaddr *) &si_other, slen)==-1)
    {
        die("sendto()");
    }
#endif

#ifdef SERVER2
    inet_aton(SERVER2 , &si_other.sin_addr);
    if (sendto(s, (char *)msg, length , 0 , (struct sockaddr *) &si_other, slen)==-1)
    {
        die("sendto()");
    }
#endif
}

void sendstat() {

    static char status_report[STATUS_SIZE]; /* status report as a JSON object */
    char stat_timestamp[24];
    time_t t;

    int stat_index=0;

    /* pre-fill the data buffer with fixed fields */
    status_report[0] = PROTOCOL_VERSION;
    status_report[3] = PKT_PUSH_DATA;

    status_report[4] = (unsigned char)ifr.ifr_hwaddr.sa_data[0];
    status_report[5] = (unsigned char)ifr.ifr_hwaddr.sa_data[1];
    status_report[6] = (unsigned char)ifr.ifr_hwaddr.sa_data[2];
    status_report[7] = 0xFF;
    status_report[8] = 0xFF;
    status_report[9] = (unsigned char)ifr.ifr_hwaddr.sa_data[3];
    status_report[10] = (unsigned char)ifr.ifr_hwaddr.sa_data[4];
    status_report[11] = (unsigned char)ifr.ifr_hwaddr.sa_data[5];

    /* start composing datagram with the header */
    uint8_t token_h = (uint8_t)rand(); /* random token */
    uint8_t token_l = (uint8_t)rand(); /* random token */
    status_report[1] = token_h;
    status_report[2] = token_l;
    stat_index = 12; /* 12-byte header */

    /* get timestamp for statistics */
    t = time(NULL);
    strftime(stat_timestamp, sizeof stat_timestamp, "%F %T %Z", gmtime(&t));

    int j = snprintf((char *)(status_report + stat_index), STATUS_SIZE-stat_index, "{\"stat\":{\"time\":\"%s\",\"lati\":%.5f,\"long\":%.5f,\"alti\":%i,\"rxnb\":%u,\"rxok\":%u,\"rxfw\":%u,\"ackr\":%.1f,\"dwnb\":%u,\"txnb\":%u,\"pfrm\":\"%s\",\"mail\":\"%s\",\"desc\":\"%s\"}}", stat_timestamp, lat, lon, (int)alt, cp_nb_rx_rcv, cp_nb_rx_ok, cp_up_pkt_fwd, (float)0, 0, 0,platform,email,description);
    stat_index += j;
    status_report[stat_index] = 0; /* add string terminator, for safety */

    printf("stat update: %s\n", (char *)(status_report+12)); /* DEBUG: display JSON stat */

    //  Send the update to LoRaWAN.
    ////sendudp(status_report, stat_index);

}

void dumpMessage(char *msg, int len) {
  for (int r = 0; r < len; r++)
    printf("Msg[0x%X] = 0x%X\n", r, msg[r]);
}

int receiveDraginoPacket() {
    //  Check for incoming packets and return 1 if a packet was received, else 0.
    extern int lora_snr;
    int rssicorr;

    if(digitalRead(dio0) == 1)
    {
        if(receivePkt(lora_packet)) {
            byte value = readDraginoRegister(REG_PKT_SNR_VALUE);
            if( value & 0x80 ) // The SNR sign bit is 1
            {
                // Invert and divide by 4
                value = ( ( ~value + 1 ) & 0xFF ) >> 2;
                lora_snr = -value;
            }
            else
            {
                // Divide by 4
                lora_snr = ( value & 0xFF ) >> 2;
            }
            
            if (is_sx1272) {
                rssicorr = 139;
            } else {
                rssicorr = 157;
            }

            printf("Packet RSSI: %d, ",readDraginoRegister(0x1A)-rssicorr);
            printf("RSSI: %d, ",readDraginoRegister(0x1B)-rssicorr);
            printf("SNR: %li, ",lora_snr);
            printf("Length: %i",(int)receivedbytes);
            printf("\n");

            int j;
            ////j = bin_to_b64((uint8_t *)lora_packet, receivedbytes, (char *)(b64), 341);
            //fwrite(b64, sizeof(char), j, stdout);

            char buff_up[TX_BUFF_SIZE]; /* buffer to compose the upstream packet */
            int buff_index=0;

            /* gateway <-> MAC protocol variables */
            //static uint32_t net_mac_h; /* Most Significant Nibble, network order */
            //static uint32_t net_mac_l; /* Least Significant Nibble, network order */

            /* pre-fill the data buffer with fixed fields */
            buff_up[0] = PROTOCOL_VERSION;
            buff_up[3] = PKT_PUSH_DATA;

            /* process some of the configuration variables */
            //net_mac_h = htonl((uint32_t)(0xFFFFFFFF & (lgwm>>32)));
            //net_mac_l = htonl((uint32_t)(0xFFFFFFFF &  lgwm  ));
            //*(uint32_t *)(buff_up + 4) = net_mac_h;
            //*(uint32_t *)(buff_up + 8) = net_mac_l;

            buff_up[4] = (unsigned char)ifr.ifr_hwaddr.sa_data[0];
            buff_up[5] = (unsigned char)ifr.ifr_hwaddr.sa_data[1];
            buff_up[6] = (unsigned char)ifr.ifr_hwaddr.sa_data[2];
            buff_up[7] = 0xFF;
            buff_up[8] = 0xFF;
            buff_up[9] = (unsigned char)ifr.ifr_hwaddr.sa_data[3];
            buff_up[10] = (unsigned char)ifr.ifr_hwaddr.sa_data[4];
            buff_up[11] = (unsigned char)ifr.ifr_hwaddr.sa_data[5];

            /* start composing datagram with the header */
            uint8_t token_h = (uint8_t)rand(); /* random token */
            uint8_t token_l = (uint8_t)rand(); /* random token */
            buff_up[1] = token_h;
            buff_up[2] = token_l;
            buff_index = 12; /* 12-byte header */

            // TODO: tmst can jump is time is (re)set, not good.
            struct timeval now;
            gettimeofday(&now, NULL);
            uint32_t tmst = (uint32_t)(now.tv_sec*1000000 + now.tv_usec);

            /* start of JSON structure */
            memcpy((void *)(buff_up + buff_index), (void *)"{\"rxpk\":[", 9);
            buff_index += 9;
            buff_up[buff_index] = '{';
            ++buff_index;
            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, "\"tmst\":%u", tmst);
            buff_index += j;
            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"chan\":%1u,\"rfch\":%1u,\"freq\":%.6lf", 0, 0, (double)freq/1000000);
            buff_index += j;
            memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":1", 9);
            buff_index += 9;
            memcpy((void *)(buff_up + buff_index), (void *)",\"modu\":\"LORA\"", 14);
            buff_index += 14;
            /* Lora datarate & bandwidth, 16-19 useful chars */
            switch (sf) {
            case SF7:
                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF7", 12);
                buff_index += 12;
                break;
            case SF8:
                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF8", 12);
                buff_index += 12;
                break;
            case SF9:
                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF9", 12);
                buff_index += 12;
                break;
            case SF10:
                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF10", 13);
                buff_index += 13;
                break;
            case SF11:
                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF11", 13);
                buff_index += 13;
                break;
            case SF12:
                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF12", 13);
                buff_index += 13;
                break;
            default:
                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF?", 12);
                buff_index += 12;
            }
            memcpy((void *)(buff_up + buff_index), (void *)"BW125\"", 6);
            buff_index += 6;
            memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/5\"", 13);
            buff_index += 13;
            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"lsnr\":%li", lora_snr);
            buff_index += j;
            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"rssi\":%d,\"size\":%u", readDraginoRegister(0x1A)-rssicorr, receivedbytes);
            buff_index += j;
            memcpy((void *)(buff_up + buff_index), (void *)",\"data\":\"", 9);
            buff_index += 9;
            ////j = bin_to_b64((uint8_t *)lora_packet, receivedbytes, (char *)(buff_up + buff_index), 341);
            j = 0;////
            extern int lora_packet_length;
            lora_packet_length = receivedbytes;
            dumpMessage((char *) lora_packet, receivedbytes); ////

            buff_index += j;
            buff_up[buff_index] = '"';
            ++buff_index;

            /* End of packet serialization */
            buff_up[buff_index] = '}';
            ++buff_index;
            buff_up[buff_index] = ']';
            ++buff_index;
            /* end of JSON datagram payload */
            buff_up[buff_index] = '}';
            ++buff_index;
            buff_up[buff_index] = 0; /* add string terminator, for safety */

            printf("rxpk update: %s\n", (char *)(buff_up + 12)); /* DEBUG: display JSON payload */

            //send the messages
            ////sendudp(buff_up, buff_index);

            fflush(stdout);
            return 0;
        } // received a message
    } // dio0=1
    return 1;
}

int draginoMain () {

    struct timeval nowtime;
    uint32_t lasttime;

    const int address = 2;
    const int mode = 1;
    unsigned int channel = LORA_CH_10_868;
    char *power = (char *) "H";
    int setupStatus = setupLoRa(address, mode, channel, power);

    if ( (s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }
    memset((char *) &si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(PORT);

    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ-1);  // can we rely on eth0?
    ioctl(s, SIOCGIFHWADDR, &ifr);

    /* display result */
    printf("Gateway ID: %.2x:%.2x:%.2x:ff:ff:%.2x:%.2x:%.2x\n",
           (unsigned char)ifr.ifr_hwaddr.sa_data[0],
           (unsigned char)ifr.ifr_hwaddr.sa_data[1],
           (unsigned char)ifr.ifr_hwaddr.sa_data[2],
           (unsigned char)ifr.ifr_hwaddr.sa_data[3],
           (unsigned char)ifr.ifr_hwaddr.sa_data[4],
           (unsigned char)ifr.ifr_hwaddr.sa_data[5]);

    printf("Listening at SF%i on %.6lf Mhz.\n", sf,(double)freq/1000000);
    printf("------------------\n");

    while(1) {

        receiveDraginoPacket();

        gettimeofday(&nowtime, NULL);
        uint32_t nowseconds = (uint32_t)(nowtime.tv_sec);
        if (nowseconds - lasttime >= 30) {
            lasttime = nowseconds;
            sendstat();
            cp_nb_rx_rcv = 0;
            cp_nb_rx_ok = 0;
            cp_up_pkt_fwd = 0;
        }
        delay(1);
    }

    return (0);

}

//  This fixes the missing __dso_handle error during linking.
extern "C" {
    extern void *__dso_handle __attribute__((__visibility__ ("hidden")));
    void *__dso_handle;
}
