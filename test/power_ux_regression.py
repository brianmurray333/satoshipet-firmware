"""Host regression checks of production power/alert snippets with mocked hardware.
Run: python3 test/power_ux_regression.py (requires clang++).
These do not replace board current/wake measurements or Wokwi integration tests.
"""
from pathlib import Path
import subprocess, tempfile
s=(Path(__file__).resolve().parents[1]/'satoshi_pet_heltec/satoshi_pet_heltec.ino').read_text()
def block(marker):
 a=s.index(marker);i=s.index('{',a);depth=1;j=i+1
 while depth:
  depth += (s[j]=='{')-(s[j]=='}');j+=1
 return s[a:j]
code=r'''
#include <cassert>
#include <cstdio>
unsigned long tick=1000;
unsigned long millis(){return tick;}
void delay(int n){tick+=n;}
bool notificationActive=false, notificationReturnToSleep=false;
bool isDisplayOff=false,isScreensaverActive=false,isBitcoinFactsActive=false;
bool isLowBatteryWarningActive=false,showCelebration=false,showNewJobNotification=false,showRejection=false;
unsigned long notificationStarted=0;
const unsigned long NOTIFICATION_DURATION=6000;
int petWarning=0, powered=0,led=0;
const int NORMAL_BRIGHTNESS=255, RGB_LED=35, LOW=0, WIFI_OFF=0;
void VextON(){powered=1;} void VextOFF(){powered=0;}
void digitalWrite(int,int v){led=v;}
void setOLEDContrast(int){}
struct {void init(){}} display;
struct Radio {bool off=false; void disconnect(bool){off=true;} void mode(int){off=true;}} WiFi;
'''
code+=block('  void wakeForNotification()')+'\n'
code+=block('        struct IdleRadioCleanup')+';\n'
code+='void expire(unsigned long now){'+block('    if (notificationActive && now - notificationStarted')+'}\n'
code+='bool consumeButtonUntilRelease=false;\nvoid consume(bool currentButtonState){'+block('    if (consumeButtonUntilRelease)')+'}\n'
code+=r'''
void poll(bool sleeping, bool fail) {IdleRadioCleanup cleanup{sleeping};if(fail)return;}
int main(){
 for(bool fail:{false,true}){WiFi.off=false;poll(true,fail);assert(WiFi.off);}
 WiFi.off=false;poll(false,false);assert(!WiFi.off);
 isDisplayOff=isScreensaverActive=true;
 wakeForNotification();assert(powered&&notificationReturnToSleep&&!isDisplayOff);
 auto started=notificationStarted;
 expire(started+5999);assert(notificationActive&&!isDisplayOff);
 expire(started+6000);assert(!notificationActive&&isDisplayOff&&WiFi.off&&!powered);
 // A second notification must retain the original return-to-sleep intention.
 wakeForNotification();wakeForNotification();assert(notificationReturnToSleep);
 // A user acknowledgement cancels automatic sleep.
 notificationActive=notificationReturnToSleep=false;
 expire(tick+7000);assert(!isDisplayOff);
 consumeButtonUntilRelease=true;consume(true);assert(consumeButtonUntilRelease);
 consume(false);assert(!consumeButtonUntilRelease);
 puts("PASS: radio cleanup on success/failure, active radio retained, timed alert sleep, repeated alerts, user acknowledgement, wake-release consumption");
}
'''
code=code.replace('#include <cassert>', '#include <cassert>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='satoshipet-check-') as tmp:
 source=Path(tmp)/'check.cpp'; binary=Path(tmp)/'check'
 source.write_text(code)
 subprocess.run(['clang++','-std=c++11',str(source),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
