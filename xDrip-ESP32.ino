#define DEBUG
#define INT_BLINK_LED

#include <SPI.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "cc2500_REG.h"
#include "webform.h"

#define D21        21              
#define D22        22
#define D34        34
#define GDO0_PIN   D22            // Цифровой канал, к которму подключен контакт GD0 платы CC2500
#define LEN_PIN    D21            // Цифровой канал, к которму подключен контакт LEN (усилитель слабого сигнала) платы CC2500
#define BAT_PIN    D34            // Аналоговый канал для измерения напряжения питания


#define NUM_CHANNELS (4)      // Кол-во проверяемых каналов
#define FIVE_MINUTE  300000    // 5 минут
#define TWO_MINUTE   120000    // 2 минуты

#define RADIO_BUFFER_LEN 200 // Размер буфера для приема данных от GSM модема

// assuming that there is a 10k ohm resistor between BAT+ and BAT_PIN, and a 27k ohm resistor between BAT_PIN and GND, as per xBridge circuit diagrams
#define BATTERY_MAXIMUM   973 //4.2V 1023*4.2*27(27+10)/3.3
#define BATTERY_MINIMUM   678 //3.0V 1023*3.0*27(27+10)/3.3

#define my_webservice_url    "http://parakeet.esen.ru/receiver.cgi"
#define my_webservice_reply  "!ACK"
#define my_user_agent        "parakeet-8266"
#define my_password_code     "12543"

#define my_wifi_ssid         "ssid"
#define my_wifi_pwd          "password"


unsigned long dex_tx_id;
char transmitter_id[] = "6E853";

IPAddress local_IP(192,168,70,1);
IPAddress gateway(192,168,70,1);
IPAddress subnet(255,255,255,0);

WiFiServer server(80);
WiFiClient client;

unsigned long web_server_start_time;

unsigned long packet_received = 0;

byte fOffset[NUM_CHANNELS] = { 0xE4, 0xE3, 0xE2, 0xE2 };
byte nChannels[NUM_CHANNELS] = { 0, 100, 199, 209 };
unsigned long waitTimes[NUM_CHANNELS] = { 0, 600, 600, 600 };

byte sequential_missed_packets = 0;
byte wait_after_time = 100;
unsigned long next_time = 0; // Время ожидания следующего пакета на канале 0
unsigned long catch_time = 0; // Время последнего пойманного пакета (приведенное к пакету на канале 0)

byte misses_until_failure = 2;                                                   //
// after how many missed packets should we just start a nonstop scan?                               //
// a high value is better for conserving batter life if you go out of wixel range a lot             //
// but it could also mean missing packets for MUCH longer periods of time                           //
// a value of zero is best if you dont care at all about battery life                               //

byte wifi_wait_tyme = 100; // Время ожидания соединения WiFi в секундах
byte default_bt_format = 0; // Формат обмена по протколу BlueTooth 0 - None 1 - xDrip, 2 - xBridge
byte old_bt_format;
unsigned int battery_milivolts;
int battery_percent;

char radio_buff[RADIO_BUFFER_LEN]; // Буффер для чтения данных и прочих нужд

// defines the xBridge protocol functional level.  Sent in each packet as the last byte.
#define DEXBRIDGE_PROTO_LEVEL (0x01)

// Коды ошибок мигают лампочкой в двоичной системе
// 1 (0001) - Нет модключения к WiFi
// 2 (0010) - Облачная служба не отвечает
// 3 (0011) - Облачная служба возвращает ошибку
// 4 (0100) - Неверный CRC в сохраненных настройках. Берем настройки по умолчанию


typedef struct _Dexcom_packet
{
  byte len;
  unsigned long dest_addr;
  unsigned long src_addr;
  byte port;
  byte device_info;
  byte txId;
  unsigned int raw;
  unsigned int filtered;
  byte battery;
  byte unknown;
  byte checksum;
  byte RSSI;
  byte LQI2;
} Dexcom_packet;

Dexcom_packet Pkt;

typedef struct _RawRecord
{
  byte size; //size of the packet.
  byte cmd_code; // code for this data packet.  Always 00 for a Dexcom data packet.
  unsigned long  raw;  //"raw" BGL value.
  unsigned long  filtered; //"filtered" BGL value 
  byte dex_battery;  //battery value
  byte my_battery; //xBridge battery value
  unsigned long  dex_src_id;   //raw TXID of the Dexcom Transmitter
  //int8  RSSI; //RSSI level of the transmitter, used to determine if it is in range.
  //uint8 txid; //ID of this transmission.  Essentially a sequence from 0-63
  byte function; // Byte representing the xBridge code funcitonality.  01 = this level.
} RawRecord;

typedef struct _parakeet_settings
{
  unsigned long dex_tx_id;     //4 bytes
  char http_url[56];
  char password_code[6];
  char wifi_ssid[17];
  char wifi_pwd[18];
  byte bt_format;
  unsigned long checksum; // needs to be aligned

} parakeet_settings;

parakeet_settings settings;

char SrcNameTable[32] = { '0', '1', '2', '3', '4', '5', '6', '7',
                          '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
                          'G', 'H', 'J', 'K', 'L', 'M', 'N', 'P',
                          'Q', 'R', 'S', 'T', 'U', 'W', 'X', 'Y'
                        };


void dexcom_src_to_ascii(unsigned long src, char addr[6]) {
  addr[0] = SrcNameTable[(src >> 20) & 0x1F];
  addr[1] = SrcNameTable[(src >> 15) & 0x1F];
  addr[2] = SrcNameTable[(src >> 10) & 0x1F];
  addr[3] = SrcNameTable[(src >> 5) & 0x1F];
  addr[4] = SrcNameTable[(src >> 0) & 0x1F];
  addr[5] = 0;
}


unsigned long getSrcValue(char srcVal) {
  byte i = 0;
  for (i = 0; i < 32; i++) {
    if (SrcNameTable[i] == srcVal) break;
  }
  return i & 0xFF;
}

unsigned long asciiToDexcomSrc(char addr[6]) {
  unsigned long src = 0;
  src |= (getSrcValue(addr[0]) << 20);
  src |= (getSrcValue(addr[1]) << 15);
  src |= (getSrcValue(addr[2]) << 10);
  src |= (getSrcValue(addr[3]) << 5);
  src |= getSrcValue(addr[4]);
  return src;
}

byte bit_reverse_byte (byte in)
{
    byte bRet = 0;
    if (in & 0x01)
        bRet |= 0x80;
    if (in & 0x02)
        bRet |= 0x40;
    if (in & 0x04)
        bRet |= 0x20;
    if (in & 0x08)
        bRet |= 0x10;
    if (in & 0x10)
        bRet |= 0x08;
    if (in & 0x20)
        bRet |= 0x04;
    if (in & 0x40)
        bRet |= 0x02;
    if (in & 0x80)
        bRet |= 0x01;
    return bRet;
}

void bit_reverse_bytes (byte * buf, byte nLen)
{
    byte i = 0;
    for (; i < nLen; i++)
    {
        buf[i] = bit_reverse_byte (buf[i]);
    }
}

unsigned long dex_num_decoder (unsigned int usShortFloat)
{
    unsigned int usReversed = usShortFloat;
    byte usExponent = 0;
    unsigned long usMantissa = 0;
    bit_reverse_bytes ((byte *) & usReversed, 2);
    usExponent = ((usReversed & 0xE000) >> 13);
    usMantissa = (usReversed & 0x1FFF);
    return usMantissa << usExponent;
}

void clearSettings()
{
  memset (&settings, 0, sizeof (settings));
  settings.dex_tx_id = asciiToDexcomSrc (transmitter_id);
  dex_tx_id = settings.dex_tx_id;
  sprintf(settings.http_url, my_webservice_url);
  sprintf(settings.password_code, my_password_code);
  sprintf(settings.wifi_ssid, my_wifi_ssid);
  sprintf(settings.wifi_pwd, my_wifi_pwd);
  settings.bt_format = default_bt_format;
  settings.checksum = 0;
}

unsigned long checksum_settings()
{
  char* flash_pointer;
  unsigned long chk = 0x12345678;
  byte i;
  //   flash_pointer = (char*)settings;
  flash_pointer = (char*)&settings;
  for (i = 0; i < sizeof(parakeet_settings) - 4; i++)
  {
    chk += (flash_pointer[i] * (i + 1));
    chk++;
  }
  return chk;
}

void saveSettingsToFlash()
{
  char* flash_pointer;
  byte i;

  EEPROM.begin(sizeof(parakeet_settings));
  
  settings.checksum = checksum_settings();
  flash_pointer = (char*)&settings;
  for (i = 0; i < sizeof(parakeet_settings); i++)
  {
    EEPROM.write(i,flash_pointer[i]);
  }
  EEPROM.commit();
//  EEPROM.put(0, settings);
}

void loadSettingsFromFlash()
{
  char* flash_pointer;
  byte i;

  EEPROM.begin(sizeof(parakeet_settings));
  flash_pointer = (char*)&settings;
  for (i = 0; i < sizeof(parakeet_settings); i++)
  {
    flash_pointer[i] = EEPROM.read(i);
  }
  
//  EEPROM.get(0, settings);
  dex_tx_id = settings.dex_tx_id;
  if (settings.checksum != checksum_settings()) {
    clearSettings();
#ifdef INT_BLINK_LED
    blink_sequence("0100");
#endif
  }
}

#ifdef INT_BLINK_LED
void blink_sequence(const char *sequence) {
  byte i;

  digitalWrite(LED_BUILTIN, LOW);
  delay(500); 
  for (i = 0; i < strlen(sequence); i++) {
    digitalWrite(LED_BUILTIN, HIGH);
    switch (sequence[i]) {
      case '0': 
        delay(500);
        break;
      case '1': 
        delay(1000);
        break;
      default:
        delay(2000);
        break;
    }
    digitalWrite(LED_BUILTIN, LOW);
    delay(500); 
  }  
}

void blink_builtin_led_quarter() {  // Blink quarter seconds
  if ((millis() / 250) % 2) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else
  {
    digitalWrite(LED_BUILTIN, LOW);
  }
}

void blink_builtin_led_half() {  // Blink half seconds
  if ((millis() / 500) % 2) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else
  {
    digitalWrite(LED_BUILTIN, LOW);
  }
}
#endif

void WriteReg(char addr, char value) {
  digitalWrite(SS, LOW);
  while (digitalRead(MISO) == HIGH) {
  };
  SPI.transfer(addr);
  SPI.transfer(value);
  digitalWrite(SS, HIGH);
  //  delay(10);
}

char SendStrobe(char strobe)
{
  digitalWrite(SS, LOW);

  while (digitalRead(MISO) == HIGH) {
  };

  char result =  SPI.transfer(strobe);
  digitalWrite(SS, HIGH);
  //  delay(10);
  return result;
}

void init_CC2500() {
//FSCTRL1 and MDMCFG4 have the biggest impact on sensitivity...
   
   WriteReg(PATABLE, 0x00);
//   WriteReg(IOCFG0, 0x01);
   WriteReg(IOCFG0, 0x06);
   WriteReg(PKTLEN, 0xff);
   WriteReg(PKTCTRL1, 0x0C); // CRC_AUTOFLUSH = 1 & APPEND_STATUS = 1
//   WriteReg(PKTCTRL1, 0x04);
   WriteReg(PKTCTRL0, 0x05);
   WriteReg(ADDR, 0x00);
   WriteReg(CHANNR, 0x00);

   WriteReg(FSCTRL1, 0x0f); 
   WriteReg(FSCTRL0, 0x00);  
  
   WriteReg(FREQ2, 0x5d);
   WriteReg(FREQ1, 0x44);
   WriteReg(FREQ0, 0xeb);
   
   WriteReg(FREND1, 0xb6);  
   WriteReg(FREND0, 0x10);  

   // Bandwidth
   //0x4a = 406 khz
   //0x5a = 325 khz
   // 300 khz is supposedly what dex uses...
   //0x6a = 271 khz
   //0x7a = 232 khz
   WriteReg(MDMCFG4, 0x7a); //appear to get better sensitivity
   WriteReg(MDMCFG3, 0xf8);
   WriteReg(MDMCFG2, 0x73);
   WriteReg(MDMCFG1, 0x23);
   WriteReg(MDMCFG0, 0x3b);
   
   WriteReg(DEVIATN, 0x40);

   WriteReg(MCSM2, 0x07);
   WriteReg(MCSM1, 0x30);
   WriteReg(MCSM0, 0x18);  
   WriteReg(FOCCFG, 0x16); //36
   WriteReg(FSCAL3, 0xa9);
   WriteReg(FSCAL2, 0x0a);
   WriteReg(FSCAL1, 0x00);
   WriteReg(FSCAL0, 0x11);
  
   WriteReg(AGCCTRL2, 0x03);  
   WriteReg(AGCCTRL1, 0x00);
   WriteReg(AGCCTRL0, 0x91);
   //
   WriteReg(TEST2, 0x81);
   WriteReg(TEST1, 0x35); 
   WriteReg(TEST0, 0x0b);  
   
   WriteReg(FOCCFG, 0x0A);    // allow range of +/1 FChan/4 = 375000/4 = 93750.  No CS GATE
   WriteReg(BSCFG, 0x6C);
 
}

char ReadReg(char addr) {
  addr = addr + 0x80;
  digitalWrite(SS, LOW);
  while (digitalRead(MISO) == HIGH) {
  };
  SPI.transfer(addr);
  char y = SPI.transfer(0);
  digitalWrite(SS, HIGH);
  //  delay(10);
  return y;
}

char ReadStatus(char addr) {
  addr = addr + 0xC0;
  digitalWrite(SS, LOW);
  while (digitalRead(MISO) == HIGH) {
  };
  SPI.transfer(addr);
  char y = SPI.transfer(0);
  digitalWrite(SS, HIGH);
  //  delay(10);
  return y;
}

String urlDecode(const String& text)
{
  String decoded = "";
  char temp[] = "0x00";
  unsigned int len = text.length();
  unsigned int i = 0;
  while (i < len)
  {
    char decodedChar;
    char encodedChar = text.charAt(i++);
    if ((encodedChar == '%') && (i + 1 < len))
    {
      temp[2] = text.charAt(i++);
      temp[3] = text.charAt(i++);

      decodedChar = strtol(temp, NULL, 16);
    }
    else {
      if (encodedChar == '+')
      {
        decodedChar = ' ';
      }
      else {
        decodedChar = encodedChar;  // normal ascii char
      }
    }
    decoded += decodedChar;
  }
  return decoded;
}

String paramByName(const String& param_string, const String& param_name) {
  unsigned int param_index;
  String param = "";

  param_index = param_string.indexOf(param_name + "=");
  if (param_index >= 0) {
    param_index += param_name.length() + 1;
    while (param_index < param_string.length() && param_string.charAt(param_index) != '&') {
      param += param_string.charAt(param_index);
      param_index++ ;
    }
  }
  return urlDecode(param);
}

void handleRoot() {
  char current_id[6];
  char temp[1400];
  char chk1[8];
  char chk2[8];
  char chk3[8];

#ifdef DEBUG
  Serial.println("http server root"); 
#endif
  dexcom_src_to_ascii(settings.dex_tx_id,current_id);
  switch (settings.bt_format) {
    case 0:
      sprintf(chk1,"%s","checked");
      chk2[0] = '\0';
      chk3[0] = '\0';
      break;
    case 1:
      chk1[0] = '\0';
      sprintf(chk2,"%s","checked");
      chk3[0] = '\0';
      break;
    case 2:
      chk1[0] = '\0';
      chk2[0] = '\0';
      sprintf(chk3,"%s","checked");
      break;
    default:  
      chk1[0] = '\0';
      chk2[0] = '\0';
      chk3[0] = '\0';
      break;
  } 
  sprintf(temp,edit_form,current_id,settings.password_code,settings.http_url,settings.wifi_ssid,settings.wifi_pwd,chk1,chk2,chk3);
//  server.send(200, "text/html", temp);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");  
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE HTML>");
  client.println(temp);
  client.println();
}

void handleNotFound() {
#ifdef DEBUG
  Serial.println("http server not found"); 
#endif
  client.println("HTTP/1.1 404 Not Found");
  client.println("Content-Type: text/plain");
  client.println("Connection: close");
  client.println();
  client.println("not found!");
  client.println();
//  server.send ( 404, "text/plain", "not found!" );
}

void handleSave(const String& param_string) {
  char new_id[6]; 
  String arg1;
  char temp[1400];
  char bt_frmt[8];

  arg1 = paramByName(param_string,"DexcomID");
  arg1.toCharArray(new_id,6);
  settings.dex_tx_id = asciiToDexcomSrc (new_id);
  dex_tx_id = settings.dex_tx_id;
  arg1 = paramByName(param_string,"PasswordCode");
  arg1.toCharArray(settings.password_code,6);
  arg1 = paramByName(param_string,"WebService");
  arg1.toCharArray(settings.http_url,56);
  arg1 = paramByName(param_string,"WiFiSSID");
  arg1.toCharArray(settings.wifi_ssid,16);
  arg1 = paramByName(param_string,"WiFiPwd");
  arg1.toCharArray(settings.wifi_pwd,16);
  arg1 = paramByName(param_string,"BtFormat");
  if (arg1 == "0") {
    settings.bt_format = 0;
    sprintf(bt_frmt,"%s","None");
  }
  else if (arg1 == "1") {   
    settings.bt_format = 1;
    sprintf(bt_frmt,"%s","xDrip");
  } else if (arg1 == "2") {    
    settings.bt_format = 2;
    sprintf(bt_frmt,"%s","xBridge");
  }
  saveSettingsToFlash();
  
  sprintf(temp, "Configuration saved!<br>DexcomID = %s<br>Password Code = %s<br>URL = %s<br>WiFi SSID = %s<br>WiFi Password = %s<br> BlueTooth format: %s<br>",
                new_id,settings.password_code,settings.http_url,settings.wifi_ssid,settings.wifi_pwd,bt_frmt);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");  
  client.println("Connection: close");
  client.println();
  client.print("<!DOCTYPE HTML>");
  client.println(temp);
  client.println();

//  server.send ( 200, "text/html",temp );
#ifdef DEBUG
  Serial.println("Configuration saved!");
#endif      
}

void PrepareWebServer() {
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP("Parakeet");
  server.begin(); 
  web_server_start_time = millis();   
}

void setup() {
#ifdef DEBUG
  byte b1;
#endif

#ifdef DEBUG
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }
#endif
  pinMode(LEN_PIN, OUTPUT);
  pinMode(GDO0_PIN, INPUT);
  pinMode(BAT_PIN, INPUT);
//  analogReference(DEFAULT); 
  
  // initialize digital pin LED_BUILTIN as an output.
#ifdef INT_BLINK_LED
  pinMode(LED_BUILTIN, OUTPUT);
#endif

  loadSettingsFromFlash();
#ifdef DEBUG
  Serial.print("Dexcom ID: ");
  Serial.println(dex_tx_id);
#endif

  SPI.begin();
  //  SPI.setClockDivider(SPI_CLOCK_DIV2);  // max SPI speed, 1/2 F_CLOCK
  digitalWrite(SS, HIGH);

  init_CC2500();  // initialise CC2500 registers
#ifdef DEBUG
  Serial.print("CC2500 PARTNUM=");
  b1 = ReadStatus(PARTNUM);
  Serial.println(b1,HEX);
  Serial.print("CC2500 VERSION=");
  b1 = ReadStatus(VERSION);
  Serial.println(b1,HEX);
#endif
  PrepareWebServer();
 
  old_bt_format = settings.bt_format;
#ifdef DEBUG
  Serial.println("Wait two minutes or configure device!");
#endif
}

void swap_channel(unsigned long channel, byte newFSCTRL0) {

  SendStrobe(SIDLE);
  SendStrobe(SFRX);
//  WriteReg(FSCTRL0,newFSCTRL0);
  WriteReg(CHANNR, channel);
  SendStrobe(SRX);  //RX
  while (ReadStatus(MARCSTATE) != 0x0d) {
    // Подождем пока включится режим приема
  }
}

byte ReadRadioBuffer() {
  byte len;
  byte i;
  byte rxbytes;

  memset (&radio_buff, 0, sizeof (Dexcom_packet));
  len = ReadStatus(RXBYTES);
#ifdef DEBUG
  Serial.print("Bytes in buffer: ");
  Serial.println(len);
#endif
  if (len > 0 && len < 65) {
    for (i = 0; i < len; i++) {
      if (i < sizeof (Dexcom_packet)) {
        radio_buff[i] = ReadReg(RXFIFO);
#ifdef DEBUG
        Serial.print(radio_buff[i],HEX);
        Serial.print("\t");
#endif
      }
    }
    Serial.println();
  }
//  memcpy(&Pkt, &radio_buff[0], sizeof (Dexcom_packet));
  Pkt.len = radio_buff[0];
  memcpy(&Pkt.dest_addr, &radio_buff[1], 4);
  memcpy(&Pkt.src_addr, &radio_buff[5], 4);
  Pkt.port = radio_buff[9];
  Pkt.device_info = radio_buff[10];
  Pkt.txId = radio_buff[11];
  memcpy(&Pkt.raw, &radio_buff[12], 2);
  memcpy(&Pkt.filtered, &radio_buff[14], 2);
  Pkt.battery = radio_buff[16];
  Pkt.unknown = radio_buff[17];
  Pkt.checksum = radio_buff[18];
  Pkt.RSSI = radio_buff[19];
  Pkt.LQI2 = radio_buff[20];
#ifdef DEBUG
  Serial.print("Dexcom ID: ");
  Serial.println(Pkt.src_addr);
#endif
  return len;
}

boolean WaitForPacket(unsigned int milliseconds_wait, byte channel_index)
{
  unsigned long start_time;
  unsigned long current_time;
  boolean nRet = false;
  boolean packet_on_board;
  byte packet_len;

  start_time = millis();
  swap_channel(nChannels[channel_index], fOffset[channel_index]);

#ifdef DEBUG
  Serial.print("Chanel = ");
  Serial.print(nChannels[channel_index]);
  Serial.print(" Time = ");
  Serial.print(start_time);
  Serial.print(" Next Time = ");
  Serial.println(next_time);
#endif
  current_time = 0;
  digitalWrite(LEN_PIN, HIGH); // Включаем усилитель слабого сигнала
  while (true) {
//    ESP.wdtFeed();
    current_time = millis();
    if (milliseconds_wait != 0 && current_time - start_time > milliseconds_wait) {
      break; // Если превысыли время ожидания на канале - выход
    }
    if (channel_index == 0 && next_time != 0 && current_time > (next_time + wait_after_time)) {
      break; // Если превысыли время следующего пакета на канале 0 - выход
    }
#ifdef INT_BLINK_LED
    blink_builtin_led_quarter();
#endif
    packet_on_board = false;
    while (digitalRead(GDO0_PIN) == HIGH) {
      packet_on_board = true;
      // Идет прием пакета
    }
    if (packet_on_board) {
      packet_len = ReadRadioBuffer();
      if (Pkt.src_addr == dex_tx_id) {
#ifdef DEBUG
        Serial.print("Catched.Ch=");
        Serial.print(nChannels[channel_index]);
        Serial.print(" Int=");
        if (catch_time != 0) {
          Serial.println(current_time - 500 * channel_index - catch_time);
        }
        else {
          Serial.println("unkn");
        }
#endif
        fOffset[channel_index] += ReadStatus(FREQEST);
        catch_time = current_time - 500 * channel_index; // Приводим к каналу 0
        nRet = true;
      } 
//      if (next_time != 0 && !nRet && channel_index == 0 && current_time < next_time && next_time-current_time < 2000) {
      if (next_time != 0 && !nRet && packet_len != 0) {
#ifdef DEBUG
        Serial.print("Try.Ch=");
        Serial.print(nChannels[channel_index]);
        Serial.print(" Time=");
        Serial.println(current_time);
#endif
        swap_channel(nChannels[channel_index], fOffset[channel_index]);
      }
      else {
        break;
      }
    }
  }
  digitalWrite(LEN_PIN, LOW); // Выключаем усилитель слабого сигнала

#ifdef INT_BLINK_LED
  digitalWrite(LED_BUILTIN, LOW);
#endif
  return nRet;
}

boolean get_packet (void) {
  byte nChannel;
  boolean nRet;

  nRet = false;
  for (nChannel = 0; nChannel < NUM_CHANNELS; nChannel++)
  {
    if (WaitForPacket (waitTimes[nChannel], nChannel)) {
      nRet = true;
      break;
    }
  }
  if (!nRet) {
    sequential_missed_packets++;
#ifdef DEBUG
    Serial.print("Missed-");
    Serial.println(sequential_missed_packets);
#endif
    if (sequential_missed_packets > misses_until_failure) { // Кол-во непойманных пакетов превысило заданное кол-во. Будем ловить пакеты непрерывно
      next_time = 0;
      sequential_missed_packets = 0; // Сбрасываем счетчик непойманных пакетов
    }  
  }
  else {
    next_time = catch_time; 
  }

  if (next_time != 0) {
    next_time += FIVE_MINUTE;
  }
  SendStrobe(SIDLE);
  SendStrobe(SFRX);

  return nRet;
}

void mesure_battery() {
  unsigned int val;

  val = analogRead(BAT_PIN);
  battery_milivolts = 1000*3.3*val/1023;
  battery_percent = (val - BATTERY_MINIMUM)/(BATTERY_MAXIMUM - BATTERY_MINIMUM) * 100;
  if (battery_percent < 0) battery_percent = 0;
  if (battery_percent > 100) battery_percent = 100;
#ifdef DEBUG
  Serial.print("Analog Read = ");
  Serial.println(val);
  Serial.print("Battery Milivolts = ");
  Serial.println(battery_milivolts);
  Serial.print("Battery Percent = ");
  Serial.println(battery_percent);
#endif
}

void print_packet() {
  
  HTTPClient http;
  int httpCode;
  String response;
  byte i;
  String request;
  unsigned long ts;
  
#ifdef DEBUG
  Serial.print(Pkt.len, HEX);
  Serial.print("\t");
  Serial.print(Pkt.dest_addr, HEX);
  Serial.print("\t");
  Serial.print(Pkt.src_addr, HEX);
  Serial.print("\t");
  Serial.print(Pkt.port, HEX);
  Serial.print("\t");
  Serial.print(Pkt.device_info, HEX);
  Serial.print("\t");
  Serial.print(Pkt.txId, HEX);
  Serial.print("\t");
  Serial.print(dex_num_decoder(Pkt.raw));
  Serial.print("\t");
  Serial.print(dex_num_decoder(Pkt.filtered)*2);
  Serial.print("\t");
  Serial.print(Pkt.battery, HEX);
  Serial.print("\t");
  Serial.print(Pkt.unknown, HEX);
  Serial.print("\t");
  Serial.print(Pkt.checksum, HEX);
  Serial.print("\t");
  Serial.print(Pkt.RSSI, HEX);
  Serial.print("\t");
  Serial.print(Pkt.LQI2, HEX);
  Serial.println(" OK");
#endif

  if (strlen(settings.wifi_ssid) == 0) {
#ifdef DEBUG
    Serial.println("WiFi not configred!");
#endif
    return;
  }
#ifdef INT_BLINK_LED    
  digitalWrite(LED_BUILTIN, HIGH);
#endif
    // wait for WiFi connection
  i = 0;  
  WiFi.begin(settings.wifi_ssid,settings.wifi_pwd);
#ifdef DEBUG
    Serial.print("Connecting WiFi: ");
#endif
  while (WiFi.status() != WL_CONNECTED) {
#ifdef DEBUG
    Serial.print(".");
#endif
    delay(500);
    i++;
    if (i == wifi_wait_tyme*2) break;
  }
#ifdef DEBUG
  Serial.println();
#endif
  if((WiFi.status() == WL_CONNECTED)) {
/*    
    sprintf(radio_buff,"%s?rr=%lu&zi=%lu&pc=%s&lv=%lu&lf=%lu&db=%hhu&ts=%lu&bp=%d&bm=%d&ct=%d&gl=%s\" ",my_webservice_url,millis(),dex_tx_id,my_password_code,
                                                                                                        dex_num_decoder(Pkt.raw),dex_num_decoder(Pkt.filtered)*2,
                                                                                                        Pkt.battery,millis()-catch_time,0, 0, 37, "");         
                                                                                                        
    sprintf(radio_buff,"%s?rr=%lu&zi=%lu&pc=%s&lv=%lu&lf=%lu&db=%hhu&ts=%lu&bp=%d&bm=%d&ct=%d",my_webservice_url,millis(),dex_tx_id,my_password_code,
                                                                                                        dex_num_decoder(Pkt.raw),dex_num_decoder(Pkt.filtered)*2,
                                                                                                        Pkt.battery,millis()-catch_time,0, 0, 37);         
*/                                                                                                        
    ts = millis()-catch_time;  
    request = my_webservice_url;                                                                                                      
    request = request + "?rr=" + millis() + "&zi=" + dex_tx_id + "&pc=" + my_password_code +
              "&lv=" + dex_num_decoder(Pkt.raw) + "&lf=" + dex_num_decoder(Pkt.filtered)*2 + "&db=" + Pkt.battery +
              "&ts=" + ts + "&bp=" + battery_percent + "&bm=" + battery_milivolts + "&ct=37"; 
    http.begin(request); //HTTP
#ifdef DEBUG
    Serial.println(request);
#endif
    httpCode = http.GET();
    if(httpCode > 0) {
#ifdef DEBUG
      Serial.print("HTTPCODE = ");
      Serial.println(httpCode);
#endif
      if(httpCode == HTTP_CODE_OK) {
        response = http.getString();
#ifdef DEBUG
        Serial.print("RESPONSE = ");
        Serial.println(response);
#endif
      } else
      {
#ifdef INT_BLINK_LED    
        blink_sequence("0011");
#endif        
      }
    } else
    {
#ifdef INT_BLINK_LED    
      blink_sequence("0010");
#endif        
    }
    WiFi.disconnect(true);
  }
  else {
#ifdef INT_BLINK_LED    
    blink_sequence("0001");
#endif   
#ifdef DEBUG
    Serial.print("WiFi CONNECT ERROR = ");
    Serial.println(WiFi.status());
#endif   
  }
#ifdef INT_BLINK_LED    
  digitalWrite(LED_BUILTIN, LOW);
#endif
}

void print_bt_packet() {
  RawRecord msg;  

  if (settings.bt_format == 0) {
    return;
  }
//  sprintf(dex_data,"%lu %d %d",275584,battery,3900);
#ifdef INT_BLINK_LED    
  digitalWrite(LED_BUILTIN, HIGH);
#endif
  if (settings.bt_format == 1) {
    sprintf(radio_buff,"%lu %d %d",dex_num_decoder(Pkt.raw),Pkt.battery,battery_milivolts);
//    bt_command(radio_buff,0,"OK",2);
  }  
  else if (settings.bt_format == 2) { 
    msg.cmd_code = 0x00;
    msg.raw = dex_num_decoder(Pkt.raw);
    msg.filtered = dex_num_decoder(Pkt.filtered)*2;
    msg.dex_battery = Pkt.battery;
    msg.my_battery = battery_percent;
//    msg.my_battery = 0;
    msg.dex_src_id = Pkt.src_addr;
//    msg.size = sizeof(msg);
    msg.size = 17;
    msg.function = DEXBRIDGE_PROTO_LEVEL; // basic functionality, data packet (with ack), TXID packet, beacon packet (also TXID ack).
//    memcpy(&radio_buff, &msg, sizeof(msg));

    radio_buff[0] = msg.size;
    radio_buff[1] = msg.cmd_code;
    memcpy(&radio_buff[2],&msg.raw , 4);
    memcpy(&radio_buff[6],&msg.filtered , 4);
    radio_buff[10] = msg.dex_battery;
    radio_buff[11] = msg.my_battery;
    memcpy(&radio_buff[12],&msg.dex_src_id , 4);
    radio_buff[16] = msg.function;
    radio_buff[sizeof(msg)] = '\0';
  }
#ifdef INT_BLINK_LED    
  digitalWrite(LED_BUILTIN, LOW);
#endif
}

void HandleWebClient() {
  String http_method;  
  String req2;

#ifdef DEBUG
  Serial.println("New client");  //  "Новый клиент"
#endif
  
  while (client.connected()) {
    if (client.available()) {
      String req = client.readStringUntil('\r');
#ifdef DEBUG
      Serial.print("http request = ");  
      Serial.println(req);
#endif
      int addr_start = req.indexOf(' ');
      int addr_end = req.indexOf(' ', addr_start + 1);
      if (addr_start == -1 || addr_end == -1) {
#ifdef DEBUG
        Serial.print("Invalid request: ");
        Serial.println(req);
#endif
        return;
      }
      http_method = req.substring(0,addr_start);
#ifdef DEBUG
      Serial.print("HTTP method = ");
      Serial.println(http_method);
#endif
      if (http_method == "POST") {
        req2 = client.readString();
#ifdef DEBUG
        Serial.print("post request = ");  
        Serial.println(req2);
#endif
      }
      req = req.substring(addr_start + 1, addr_end);
#ifdef DEBUG
      Serial.print("Request: ");
      Serial.println(req);
#endif
      client.flush();
      if (req == "/") {
        handleRoot();
      }
      else if (http_method == "POST" && req == "/save") {
        handleSave(req2);        
      }
      else 
      {
        handleNotFound();
      }
      client.stop();
    }    
  }  
}

void loop() {
  unsigned long current_time;

// Первые две минуты работает WebServer на адресе 192.168.70.1 для конфигурации устройства
  if (web_server_start_time > 0) {
    client = server.available();
    if (client) {
      HandleWebClient();
    }
    delay(1);
    if ((millis() - web_server_start_time) > TWO_MINUTE && !server.hasClient()) {
      server.stop();
      WiFi.softAPdisconnect(true);
      web_server_start_time = 0;  
#ifdef DEBUG
      Serial.println("Configuration mode is done!");
#endif
      if (old_bt_format != settings.bt_format) {
//        PrepareBlueTooth();
        delay(500);
      }
/*
      if (settings.bt_format == 2) {
        sendBeacon();
      }   
*/
    }
    return;
  }
  
  if (get_packet ())
  {
    mesure_battery();
    print_bt_packet();
    print_packet ();
  }

}
