#pragma once
// Settings kept in NVS. Only the main loop calls these.
#include <Preferences.h>
#include <esp_random.h>
#include "config.h"

struct Settings { uint8_t rows[4]; char wifiPassword[64]; };

inline bool validRow(long row){return row>=1 && row<=62;}
inline bool validWifiPassword(const char *p){size_t n=strlen(p);return n>=8 && n<=63;} // WPA2 limits

// First boot: generate a random WiFi password and store it. Call after the radio is on
// (WiFi.mode) so esp_random() draws from the hardware RNG.
inline void loadSettings(Settings &s){
  Preferences p;p.begin("flexcan",false);
  for(unsigned i=0;i<4;++i){
    char key[]="row0";key[3]+=i;
    uint8_t row=p.getUChar(key,OUTPUT_ROW[i]);
    s.rows[i]=validRow(row)?row:OUTPUT_ROW[i];
  }
  String pass=p.getString("wifi","");
  if(!validWifiPassword(pass.c_str())){
    static const char alphabet[]="abcdefghjkmnpqrstuvwxyz23456789"; // No look-alike characters.
    char fresh[13]={};
    for(unsigned i=0;i<12;++i) fresh[i]=alphabet[esp_random()%(sizeof(alphabet)-1)];
    p.putString("wifi",fresh);pass=fresh;
  }
  strlcpy(s.wifiPassword,pass.c_str(),sizeof(s.wifiPassword));
  p.end();
}
inline bool saveRows(const uint8_t rows[4]){
  Preferences p;if(!p.begin("flexcan",false)) return false;
  bool ok=true;
  for(unsigned i=0;i<4;++i){char key[]="row0";key[3]+=i;ok=ok && p.putUChar(key,rows[i])==1;}
  p.end();return ok;
}
inline bool saveWifiPassword(const char *pass){
  Preferences p;if(!p.begin("flexcan",false)) return false;
  bool ok=p.putString("wifi",pass)==strlen(pass);p.end();return ok;
}
