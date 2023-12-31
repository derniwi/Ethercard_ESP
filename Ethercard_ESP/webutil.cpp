// Some common utilities needed for IP and web applications
// Author: Guido Socher
// Copyright: GPL V2
//
// 2010-05-20 <jc@wippler.nl>

#include "EtherCard.h"

void EtherCard::copyIp(uint8_t *dst, const uint8_t *src) {
  memcpy(dst, src, IP_LEN);
}

void EtherCard::copyMac(uint8_t *dst, const uint8_t *src) {
  memcpy(dst, src, ETH_LEN);
}

void EtherCard::printIp(const char *msg, const uint8_t *buf) {
  Serial.print(msg);
  EtherCard::printIp(buf);
  Serial.println();
}

void EtherCard::printIp(const uint8_t *buf) {
  String info;
  for (uint8_t i = 0; i < IP_LEN; ++i) {
    info = info + buf[i];
    if (i < (IP_LEN - 1)) {
      info = info + ".";
    }
  }
  Serial.println(info);
}

// convert a single hex digit character to its integer value
unsigned char h2int(char c) {
  if (c >= '0' && c <= '9') {
    return ((unsigned char)c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return ((unsigned char)c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
    return ((unsigned char)c - 'A' + 10);
  }
  return (0);
}

// convert a single character to a 2 digit hex str
// a terminating '\0' is added
void int2h(char c, char *hstr) {
  hstr[1] = (c & 0xf) + '0';
  if ((c & 0xf) > 9) {
    hstr[1] = (c & 0xf) - 10 + 'a';
  }
  c = (c >> 4) & 0xf;
  hstr[0] = c + '0';
  if (c > 9) {
    hstr[0] = c - 10 + 'a';
  }
  hstr[2] = '\0';
}

// parse a string and extract the IP to bytestr
uint8_t EtherCard::parseIp(uint8_t *bytestr, char *str) {
  char *sptr;
  uint8_t i = 0;
  sptr = NULL;
  while (i < IP_LEN) {
    bytestr[i] = 0;
    i++;
  }
  i = 0;
  while (*str && i < IP_LEN) {
    // if a number then start
    if (sptr == NULL && isdigit(*str)) {
      sptr = str;
    }
    if (*str == '.') {
      *str = '\0';
      bytestr[i] = (atoi(sptr) & 0xff);
      i++;
      sptr = NULL;
    }
    str++;
  }
  *str = '\0';
  if (i == 3) {
    bytestr[i] = (atoi(sptr) & 0xff);
    return (0);
  }
  return (1);
}

// take a byte string and convert it to a human readable display string
// (base is 10 for ip and 16 for mac addr), len is 4 for IP addr and 6 for mac.
void EtherCard::makeNetStr(char *resultstr, uint8_t *bytestr, uint8_t len,
                           char separator, uint8_t base) {
  uint8_t i = 0;
  uint8_t j = 0;
  while (i < len) {
    ltoa((long)bytestr[i], &resultstr[j], base);
    // search end of str:
    while (resultstr[j]) {
      j++;
    }
    resultstr[j] = separator;
    j++;
    i++;
  }
  j--;
  resultstr[j] = '\0';
}

// end of webutil.c
