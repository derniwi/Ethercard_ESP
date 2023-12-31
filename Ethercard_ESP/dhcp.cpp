// DHCP look-up functions based on the udp client
// http://www.ietf.org/rfc/rfc2131.txt
//
// Author: Andrew Lindsay
// Rewritten and optimized by Jean-Claude Wippler, http://jeelabs.org/
//
// Rewritten dhcpStateMachine by Chris van den Hooven
// as to implement dhcp-renew when lease expires (jun 2012)
//
// Various modifications and bug fixes contributed by Victor Aprea (oct 2012)
//
// Copyright: GPL V2
// See http://www.gnu.org/licenses/gpl.html


/* from Wikipedia https://de.wikipedia.org/wiki/Dynamic_Host_Configuration_Protocol#/media/Datei:16a_DHCP.png
         Server                Client                 Server
      (not selected)                                (selected)
                         starts initialization
                                 / \
            /--- DHCPDISCOVER --- | --- DHCPDISCOVER ---\
            |                     |                     |
  detemines configuration         |           detemines configuration
            |                     |                     |
            |                     |/----- DHCPOFFER ----/
            |                     |
            \---- DHCPOFFER -----\|
                                  |
                            collect anwsers
                                  |
                           choose configuration
            /---- DHCPREQUEST ---/|\--- DHCPREQUEST ----\
            |                     |                     |
            |                     |          acknowedges configuration
                                  |                     |
                                  |/------ DHCPACK -----/
                                  |
                       initialization finished
                                  |
                              shutdown
                                  |
                                  |\--- DHCPRELEASE ----\
                                  |                     |
                                  |               drops reservation

DHCP Package format:
-----------------------------
           32 bit
-----------------------------
 op   | htype | hlen  | hops
             xid
     secs     |    flags
            ciaddr
            yiaddr
            siaddr
            giaddr
         chaddr (16 byte)
         sname (64 byte)
         file (128 byte)
       options (var byte)

   op            1  Message op code / message type.
                    1 = BOOTREQUEST, 2 = BOOTPREPLY
   htype         1  Hardware address type, see ARP section in "Assigned
                    Numbers" RFC; e.g., '1' = ethernet, '6' = IEEE 802 Netzwerke or '7' = ARCNET
   hlen          1  Hardware address length (e.g.  '6' for MAC ethernet).
   hops          1  Client sets to zero, optionally used by relay agents
                    when booting via a relay agent.
   xid           4  Transaction ID, a random number chosen by the
                    client, used by the client and server to associate
                    messages and responses between a client and a
                    server.
   secs          2  Filled in by client, seconds elapsed since client
                    began address acquisition or renewal process.
   flags         2  Flags (see figure 2).
   ciaddr        4  Client IP address; only filled in if client is in
                    BOUND, RENEW or REBINDING state and can respond
                    to ARP requests.
   yiaddr        4  'your' (client) IP address.
   siaddr        4  IP address of next server to use in bootstrap;
                    returned in DHCPOFFER, DHCPACK by server.
   giaddr        4  Relay agent IP address, used in booting via a
                    relay agent.
   chaddr       16  Client hardware address.
   sname        64  Optional server host name, null terminated string.
   file        128  Boot file name, null terminated string; "generic"
                    name or null in DHCPDISCOVER, fully qualified
                    directory-path name in DHCPOFFER.
   options     var  Optional parameters field.  See the options
                    documents for a list of defined options.
 */

#define DHCPDEBUG

#include "EtherCard.h"
#include "net.h"

#define gPB ether.buffer

#define DHCP_BOOTP_REQUEST 1
#define DHCP_BOOTP_RESPONSE 2

// DHCP Message Type (option 53) (ref RFC 2132)
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_DECLINE 4
#define DHCP_ACK 5
#define DHCP_NAK 6
#define DHCP_RELEASE 7
#define DHCP_INFORM 8

// size 236 byte
typedef struct
{
  byte op, htype, hlen, hops;
  uint32_t xid;
  uint16_t secs, flags;
  byte ciaddr[IP_LEN], yiaddr[IP_LEN], siaddr[IP_LEN], giaddr[IP_LEN];
  byte chaddr[16], sname[64], file[128];
} DHCPdata;

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

// timeouts im ms
#define DHCP_REQUEST_TIMEOUT 10000

#define DHCP_HOSTNAME_MAX_LEN 32

// RFC 2132 Section 3.3:
// The time value of 0xffffffff is reserved to represent "infinity".
#define DHCP_INFINITE_LEASE 0xffffffff

static char hostname[DHCP_HOSTNAME_MAX_LEN] = "Arduino-ENC28j60-00";  // Last two characters will be filled by last 2 MAC digits ;
static uint32_t currentXid;
static uint32_t stateTimer;
static uint32_t leaseStart;
static uint32_t leaseTime;
static byte *bufPtr;

static uint8_t dhcpCustomOptionNum = 0;
static DhcpOptionCallback dhcpCustomOptionCallback = NULL;

extern uint8_t allOnes[];  // = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void addToBuf(byte b) {
  *bufPtr++ = b;
}

static void addBytes(byte len, const byte *data) {
  while (len-- > 0)
    addToBuf(*data++);
}

static void addOption(byte opt, byte len, const byte *data) {
  addToBuf(opt);
  addToBuf(len);
  addBytes(len, data);
}


// Main DHCP sending function

// implemented
// state             / msgtype
// INIT              / DHCPDISCOVER
// SELECTING         / DHCPREQUEST
// BOUND (RENEWING)  / DHCPREQUEST
// RELEASING         / DHCPRELEASE

// ----------------------------------------------------------
// |              |SELECTING    |RENEWING     |INIT         |
// ----------------------------------------------------------
// |broad/unicast |broadcast    |unicast      |broadcast    |
// |server-ip     |MUST         |MUST NOT     |MUST NOT     | option 54
// |requested-ip  |MUST         |MUST NOT     |MUST NOT     | option 50
// |ciaddr        |zero         |IP address   |zero         |
// ----------------------------------------------------------

// options used (both send/receive)
#define DHCP_OPT_SUBNET_MASK 1
#define DHCP_OPT_TIME_OFFSET 2
#define DHCP_OPT_ROUTERS 3
#define DHCP_OPT_TIME_SERVERS 4
#define DHCP_OPT_DOMAIN_NAME_SERVERS 6
#define DHCP_OPT_HOSTNAME 12
#define DHCP_OPT_NTP_SERVERS 42
#define DHCP_OPT_REQUESTED_ADDRESS 50
#define DHCP_OPT_LEASE_TIME 51
#define DHCP_OPT_MESSAGE_TYPE 53
#define DHCP_OPT_SERVER_IDENTIFIER 54
#define DHCP_OPT_PARAMETER_REQUEST_LIST 55
#define DHCP_OPT_RENEWAL_TIME 58
#define DHCP_OPT_CLIENT_IDENTIFIER 61
#define DHCP_OPT_DOMAIN_SEARCH_LIST 119
#define DHCP_OPT_END 255

#define DHCP_HTYPE_ETHER 1

static void send_dhcp_message(uint8_t *requestip) {
  DEBUG_PRINT("send_dhcp_message");
  memset(gPB, 0, UDP_DATA_P + sizeof(DHCPdata));

  EtherCard::udpPrepare(DHCP_CLIENT_PORT,
                        (EtherCard::dhcpState == EtherCard::DHCP_STATE_BOUND ? EtherCard::dhcpip : allOnes), DHCP_SERVER_PORT);

  // If we ever don't do this, the DHCP renewal gets sent to whatever random
  // destmacaddr was used by other code. Rather than cache the MAC address of
  // the DHCP server, just force a broadcast here in all cases.
  EtherCard::copyMac(gPB + ETH_DST_MAC, allOnes);  // force broadcast MAC

  // Build DHCP Packet from buf[ UDP_DATA_P ]
  DHCPdata *dhcpPtr = (DHCPdata *)(gPB + UDP_DATA_P);
  dhcpPtr->op = DHCP_BOOTP_REQUEST;
  dhcpPtr->htype = 1;
  dhcpPtr->hlen = 6;
  dhcpPtr->xid = currentXid;
  if (EtherCard::dhcpState == EtherCard::DHCP_STATE_BOUND) {
    EtherCard::copyIp(dhcpPtr->ciaddr, EtherCard::myip);
  }
  EtherCard::copyMac(dhcpPtr->chaddr, EtherCard::mymac);

  // options defined as option, length, value
  bufPtr = gPB + UDP_DATA_P + sizeof(DHCPdata);
  
  // DHCP magic cookie,
  static const byte cookie[] PROGMEM = { 0x63, 0x82, 0x53, 0x63 };
  for (byte i = 0; i < sizeof(cookie); i++)
    addToBuf(pgm_read_byte(&cookie[i]));

  addToBuf(DHCP_OPT_MESSAGE_TYPE);  // value 53=0x35, DHCP_STATE_SELECTING, DHCP_STATE_REQUESTING
  addToBuf(1);                      // Length

  addToBuf(EtherCard::dhcpState == EtherCard::DHCP_STATE_INIT ? DHCP_DISCOVER : DHCP_REQUEST);  // value 1 or 3

  // Client Identifier Option, this is the client mac address
  addToBuf(DHCP_OPT_CLIENT_IDENTIFIER);  // value 61 = 0x3d
  addToBuf(1 + ETH_LEN);                 // Length (hardware type + client MAC)
  addToBuf(DHCP_HTYPE_ETHER);            // value 1 = 0x01
  addBytes(ETH_LEN, EtherCard::mymac);

  if (hostname[0]) {
    addOption(DHCP_OPT_HOSTNAME, strlen(hostname), (byte *)hostname);  // value 0x0c + len + hostname
  }

  if (requestip != NULL) {
    addOption(DHCP_OPT_REQUESTED_ADDRESS, IP_LEN, requestip);          // value 50=0x32 + len + IP
    addOption(DHCP_OPT_SERVER_IDENTIFIER, IP_LEN, EtherCard::dhcpip);  // value 54=0x36 + len + IP
  }

  // Additional info in parameter list - minimal list for what we need
  byte len = 8;
  if (dhcpCustomOptionNum)
    len++;

  addToBuf(DHCP_OPT_PARAMETER_REQUEST_LIST);  // value 55 = 0x37
  addToBuf(len);                              // Length
  addToBuf(DHCP_OPT_SUBNET_MASK);             // value   1
  addToBuf(DHCP_OPT_TIME_OFFSET);             // value   2
  addToBuf(DHCP_OPT_ROUTERS);                 // value   3
  addToBuf(DHCP_OPT_TIME_SERVERS);            // value   4
  addToBuf(DHCP_OPT_DOMAIN_NAME_SERVERS);     // value   6
  addToBuf(DHCP_OPT_NTP_SERVERS);             // value  42
  addToBuf(DHCP_OPT_DOMAIN_SEARCH_LIST);      // value 119

  if (dhcpCustomOptionNum)
    addToBuf(dhcpCustomOptionNum);  // Custom option

  addToBuf(DHCP_OPT_END);  // value 255 = 0xff

  // packet size will be under 300 bytes
  EtherCard::udpTransmit((bufPtr - gPB) - UDP_DATA_P);
}

static void process_dhcp_offer(uint16_t len, uint8_t *offeredip) {
  DEBUG_PRINT("process_dhcp_offer");
  // Map struct onto payload
  DHCPdata *dhcpPtr = (DHCPdata *)(gPB + UDP_DATA_P);

  // Offered IP address is in yiaddr
  EtherCard::copyIp(offeredip, dhcpPtr->yiaddr);

  // Search for the server IP
  byte *ptr = (byte *)(dhcpPtr + 1) + 4;
  do {
    byte option = *ptr++;
    byte optionLen = *ptr++;
    if (option == DHCP_OPT_SERVER_IDENTIFIER)  // value 54=0x36
    {
      EtherCard::copyIp(EtherCard::dhcpip, ptr);
      break;
    }
    ptr += optionLen;
  } while (ptr < gPB + len);
}

static void process_dhcp_ack(uint16_t len) {
  // Map struct onto payload
  DHCPdata *dhcpPtr = (DHCPdata *)(gPB + UDP_DATA_P);

  // Allocated IP address is in yiaddr
  EtherCard::copyIp(EtherCard::myip, dhcpPtr->yiaddr);
  // Scan through variable length option list identifying options we want
  byte *ptr = (byte *)(dhcpPtr + 1) + 4;

  bool done = false;
  do {
    byte option = *ptr++;
    byte optionLen = *ptr++;

    switch (option) {
      case DHCP_OPT_SUBNET_MASK:
        EtherCard::copyIp(EtherCard::netmask, ptr);
        break;
      case DHCP_OPT_ROUTERS:
        EtherCard::copyIp(EtherCard::gwip, ptr);
        break;
      case DHCP_OPT_DOMAIN_NAME_SERVERS:
        EtherCard::copyIp(EtherCard::dnsip, ptr);
        break;
      case DHCP_OPT_LEASE_TIME:
      case DHCP_OPT_RENEWAL_TIME:
        leaseTime = 0;  // option 58 = Renewal Time, 51 = Lease Time
        for (byte i = 0; i < 4; i++)
          leaseTime = (leaseTime << 8) + ptr[i];
        if (leaseTime != DHCP_INFINITE_LEASE) {
          leaseTime *= 1000;  // milliseconds
        }
        break;
      case DHCP_OPT_END:
        done = true;
        break;

      default:
        // Is is a custom configured option? AsciiField( ptr, option, optionLen );
        //  if( dhcpCustomOptionCallback && option == dhcpCustomOptionNum )
        //  {
        //    dhcpCustomOptionCallback( option, ptr, optionLen );
        //  }
        break;
    }
    dhcpCustomOptionCallback(option, ptr, optionLen);
    ptr += optionLen;
  } while (!done && ptr < gPB + len);
}

static bool dhcp_received_message_type(uint16_t len, byte msgType) {
  // Map struct onto payload
  DEBUG_PRINT("dhcp_received_message_type");
  DEBUG_PRINT(msgType);
  DHCPdata *dhcpPtr = (DHCPdata *)(gPB + UDP_DATA_P);

  if ((len >= 70) && (gPB[UDP_SRC_PORT_L_P] == DHCP_SERVER_PORT) && (dhcpPtr->xid == currentXid)) {
    byte *ptr = (byte *)(dhcpPtr + 1) + 4;
    do {
      byte option = *ptr++;
      byte optionLen = *ptr++;
      if (option == DHCP_OPT_MESSAGE_TYPE && *ptr == msgType) {
        // DHCP Message type match found
        return true;
      }
      ptr += optionLen;
    } while (ptr < gPB + len);
  }
  return false;
}

static char toAsciiHex(byte b) {
  char c = b & 0x0f;
  c += (c <= 9) ? '0' : 'A' - 10;
  return c;
}

bool EtherCard::dhcpSetup(const char *hname, bool fromRam) {
  DEBUG_PRINT("EtherCard::dhcpSetup");
  // Use during setup, as this discards all incoming requests until it returns.
  // That shouldn't be a problem, because we don't have an IP-address yet.
  // Will try 60 secs to obtain DHCP-lease.
  using_dhcp = true;

  if (hname != NULL) {
    if (fromRam) {
      strncpy(hostname, hname, DHCP_HOSTNAME_MAX_LEN);
    } else {
      strncpy_P(hostname, hname, DHCP_HOSTNAME_MAX_LEN);
    }
  } else {
    // Set a unique hostname, use Arduino-?? with last octet of mac address
    hostname[strlen(hostname) - 2] = toAsciiHex(mymac[5] >> 4);  // Appends mac to last 2 digits of the hostname
    hostname[strlen(hostname) - 1] = toAsciiHex(mymac[5]);       // Even if it's smaller than the maximum <thus, strlen(hostname)>
  }

  dhcpState = EtherCard::DHCP_STATE_INIT;
  return true;
}


void EtherCard::dhcpRelease() {
  dhcpState = DHCP_STATE_RELEASING;

  memset(gPB, 0, UDP_DATA_P + sizeof(DHCPdata));

  EtherCard::udpPrepare(DHCP_CLIENT_PORT, allOnes, DHCP_SERVER_PORT);

  // If we ever don't do this, the DHCP renewal gets sent to whatever random
  // destmacaddr was used by other code. Rather than cache the MAC address of
  // the DHCP server, just force a broadcast here in all cases.
  EtherCard::copyMac(gPB + ETH_DST_MAC, allOnes);  // force broadcast MAC

  // Build DHCP Packet from buf[ UDP_DATA_P ]
  DHCPdata *dhcpPtr = (DHCPdata *)(gPB + UDP_DATA_P);
  dhcpPtr->op = DHCP_BOOTP_REQUEST;
  dhcpPtr->htype = 1;
  dhcpPtr->hlen = 6;
  dhcpPtr->xid = currentXid;

  EtherCard::copyIp(dhcpPtr->ciaddr, EtherCard::myip);
  EtherCard::copyMac(dhcpPtr->chaddr, EtherCard::mymac);
  EtherCard::copyIp(dhcpPtr->siaddr, EtherCard::dhcpip);  // Copy DHCP server IP address

  // options defined as option, length, value
  bufPtr = gPB + UDP_DATA_P + sizeof(DHCPdata);
  // DHCP magic cookie,
  static const byte cookie[] PROGMEM = { 0x63, 0x82, 0x53, 0x63 };
  for (byte i = 0; i < sizeof(cookie); i++)
    addToBuf(pgm_read_byte(&cookie[i]));

  addToBuf(DHCP_OPT_MESSAGE_TYPE);  // value 53=0x35, DHCP_STATE_SELECTING, DHCP_STATE_REQUESTING
  addToBuf(1);                      // Length

  addToBuf(DHCP_RELEASE);  // value 7

  // Client Identifier Option, this is the client mac address
  addToBuf(DHCP_OPT_CLIENT_IDENTIFIER);  // value 61 = 0x3d
  addToBuf(1 + ETH_LEN);                 // Length (hardware type + client MAC)
  addToBuf(DHCP_HTYPE_ETHER);            // value 1 = 0x01
  addBytes(ETH_LEN, EtherCard::mymac);

  addOption(DHCP_OPT_SERVER_IDENTIFIER, IP_LEN, EtherCard::dhcpip);  // value 54=0x36 + len + IP
  addToBuf(DHCP_OPT_END);                                            // value 255 = 0xff

  // packet size will be under 300 bytes
  EtherCard::udpTransmit((bufPtr - gPB) - UDP_DATA_P);

  // Clean up configuration, IP address, gateway...
  memset(gPB, 0, UDP_DATA_P + sizeof(DHCPdata));
  for (byte i = 0; i < 4; i++) {
    myip[i] = 0;         // IPv4 address
    netmask[i] = 0;      // Netmask
    broadcastip[i] = 0;  // Subnet IPv4 broadcast address
    gwip[i] = 0;         // Gateway IPv4
    dhcpip[i] = 0;       // DHCP server IPv4 address
    dnsip[i] = 0;        // DNS server IPv4 address
    hisip[i] = 0;        // DNS lookup result
  }

  dhcpState = DHCP_STATE_RELEASED;
  using_dhcp = false;
}

void EtherCard::dhcpAddOptionCallback(uint8_t option, DhcpOptionCallback callback) {
  dhcpCustomOptionNum = option;
  dhcpCustomOptionCallback = callback;
}

void EtherCard::DhcpStateMachine(uint16_t len) {
#ifdef DHCPDEBUG
  if (dhcpState != DHCP_STATE_BOUND) {
    DEBUG_PRINT(" State: ");
  }
  switch (dhcpState) {
    case DHCP_STATE_INIT:
      DEBUG_PRINT("Init");
      break;
    case DHCP_STATE_SELECTING:
      DEBUG_PRINT("Selecting");
      break;
    case DHCP_STATE_REQUESTING:
      DEBUG_PRINT("Requesting");
      break;
    case DHCP_STATE_RENEWING:
      DEBUG_PRINT("Renew");
      break;
    case DHCP_STATE_RELEASING:
      DEBUG_PRINT("Releasing");
      break;
    default:
      DEBUG_PRINT("unknown");
      break;
  }
#endif

  switch (dhcpState) {
    case DHCP_STATE_INIT:
      currentXid = millis();
      memset(myip, 0, IP_LEN);  // force ip 0.0.0.0
      send_dhcp_message(NULL);
      //enableBroadcast(true); // Temporarily enable broadcasts
      dhcpState = DHCP_STATE_SELECTING;
      stateTimer = millis();
      break;

    case DHCP_STATE_SELECTING:
      if (dhcp_received_message_type(len, DHCP_OFFER)) {
        uint8_t offeredip[IP_LEN];
        process_dhcp_offer(len, offeredip);
        send_dhcp_message(offeredip);
        dhcpState = DHCP_STATE_REQUESTING;
        stateTimer = millis();
      } else {
        if (millis() - stateTimer > DHCP_REQUEST_TIMEOUT) {
          dhcpState = DHCP_STATE_INIT;
        }
      }
      break;

    case DHCP_STATE_REQUESTING:
    case DHCP_STATE_RENEWING:
      if (dhcp_received_message_type(len, DHCP_ACK)) {
        //disableBroadcast( true ); //Disable broadcast after temporary enable
        process_dhcp_ack(len);
        leaseStart = millis();
        if (gwip[0] != 0) setGwIp(gwip);  // why is this? because it initiates an arp request
        dhcpState = DHCP_STATE_BOUND;
      } else {
        if (millis() - stateTimer > DHCP_REQUEST_TIMEOUT) {
          dhcpState = DHCP_STATE_INIT;
        }
      }
      break;

    case DHCP_STATE_BOUND:
      // !@todo Due to millis() wrap-around, DHCP renewal may not work if leaseTime is larger than 49days
      if (leaseTime != DHCP_INFINITE_LEASE && millis() - leaseStart >= leaseTime) {
        send_dhcp_message(myip);
        dhcpState = DHCP_STATE_RENEWING;
        stateTimer = millis();
      }
      break;

    case DHCP_STATE_RELEASING:
      break;

  }  // switch( dhcpState )
}
