#define IS_BREADBOARD true // switch to false if compiling code for PCB

// include mozzi library files - currently requires particular version of mozzi for compatibility:
// https://github.com/sensorium/Mozzi/archive/e1eba4410200842157763f1471dca34bf4867138.zip
#include <MozziGuts.h>
#include <Sample.h>
#include <EventDelay.h>
#include <mozzi_rand.h>

// include debouncing library
#include <Bounce2.h>

// include EEPROM library for saving data
#include <EEPROM.h>

// include audio data files
#include "kick.h"
#include "closedhat.h"
#include "snare.h"
#include "click.h"

// define pins
byte breadboardLedPins[5] = {5,6,7,8,13};
byte breadboardButtonPins[6] = {2,3,4,10,11,12};
byte pcbLedPins[5] = {6,5,4,3,2};
byte pcbButtonPins[6] = {13,12,11,10,8,7};
byte (&ledPins)[5] = IS_BREADBOARD ? breadboardLedPins : pcbLedPins;
byte (&buttonPins)[6] = IS_BREADBOARD ? breadboardButtonPins : pcbButtonPins;

// define buttons
Bounce buttonA = Bounce();
Bounce buttonB = Bounce();
Bounce buttonC = Bounce();
Bounce buttonX = Bounce();
Bounce buttonY = Bounce();
Bounce buttonZ = Bounce();

#define CONTROL_RATE 256 // tweak this value if performance is bad, must be power of 2 (64, 128, etc)
#define MAX_BEAT_STEPS 32
#define NUM_PARAM_GROUPS 5
#define NUM_KNOBS 4
#define NUM_LEDS 5
#define NUM_BUTTONS 6
#define NUM_SAMPLES 4

bool sequencePlaying = false; // false when stopped, true when playing
byte currentStep;
byte numSteps = 4 * 16; // 64 steps = 4/4 time signature, 48 = 3/4 or 6/8, 80 = 5/4, 112 = 7/8 etc
float nextNoteTime;
float scheduleAheadTime = 100; // ms
float lookahead = 25; // ms
const byte numEventDelays = 20;
EventDelay schedulerEventDelay;
EventDelay eventDelays[numEventDelays];
bool delayInUse[numEventDelays];
byte delayChannel[numEventDelays];
byte delayVelocity[numEventDelays];
byte eventDelayIndex = 0;
bool beatLedsActive = true;
bool firstLoop = true;
byte sampleVolumes[NUM_SAMPLES] = {255,255,255,255};
unsigned long tapTempoTaps[8] = {0,0,0,0,0,0,0,0};
bool readyToSave = true;
bool readyToChooseSaveLocation = false;

// variables relating to knob values
byte controlSet = 0;
bool knobLocked[NUM_KNOBS] = {true,true,true,true};
int analogValues[NUM_KNOBS] = {0,0,0,0};
int initValues[NUM_KNOBS] = {0,0,0,0};
int storedValues[NUM_PARAM_GROUPS][NUM_KNOBS] = { {512,512,512,512},    // A
                                                  {512,512,512,512},    // B
                                                  {512,512,512,512},    // C
                                                  {512,512,512,512},    // X
                                                  {512,512,512,512},};  // Y

// parameters
byte paramChance = 0;
byte paramMidpoint = 0;
byte paramRange = 0;
byte paramZoom = 0;
byte paramPitch = 0;
byte paramCrush = 0;
byte crushCompensation = 0;
int paramSlop = 0;
float paramTempo = 120.0;
byte paramTimeSignature = 4;

// define samples
Sample <kick_NUM_CELLS, AUDIO_RATE> kick1(kick_DATA);
Sample <closedhat_NUM_CELLS, AUDIO_RATE> closedhat1(closedhat_DATA);
Sample <snare_NUM_CELLS, AUDIO_RATE> snare1(snare_DATA);
Sample <click_NUM_CELLS, AUDIO_RATE> click1(click_DATA);

const byte beat1[NUM_SAMPLES][MAX_BEAT_STEPS] PROGMEM = {  {255,255,0,0,0,0,0,0,255,0,0,0,0,0,0,0,255,255,0,0,0,0,0,0,255,0,0,0,0,0,0,0,},
                                  {255,128,255,128,255,128,255,128,255,128,255,128,255,128,255,128,255,128,255,128,255,128,255,128,255,128,255,128,255,128,255,128,},
                                  {0,0,0,0,255,0,0,0,0,0,0,0,255,128,64,32,0,0,0,0,255,0,0,0,0,0,0,0,255,128,64,32,},
                                  {0,128,128,0,0,0,0,0,0,0,0,0,0,0,0,0,0,128,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},};
              

void setup() {
  startMozzi(CONTROL_RATE);
  randSeed();
  kick1.setFreq((float) kick_SAMPLERATE / (float) kick_NUM_CELLS);
  closedhat1.setFreq((float) closedhat_SAMPLERATE / (float) closedhat_NUM_CELLS);
  snare1.setFreq((float) snare_SAMPLERATE / (float) snare_NUM_CELLS);
  click1.setFreq((float) click_SAMPLERATE / (float) click_NUM_CELLS);
  kick1.setEnd(7000);
  //Serial.begin(9600);
  for(byte i=0;i<NUM_LEDS;i++) {
    pinMode(ledPins[i], OUTPUT);
  }

  buttonA.interval(25);
  buttonB.interval(25);
  buttonC.interval(25);
  buttonX.interval(25);
  buttonY.interval(25);
  buttonZ.interval(25);
  buttonA.attach(buttonPins[0], INPUT_PULLUP);
  buttonB.attach(buttonPins[1], INPUT_PULLUP);
  buttonC.attach(buttonPins[2], INPUT_PULLUP);
  buttonX.attach(buttonPins[3], INPUT_PULLUP);
  buttonY.attach(buttonPins[4], INPUT_PULLUP);
  buttonZ.attach(buttonPins[5], INPUT_PULLUP);

  for(int i=0;i<20000;i++) {
    kick1.next();
    closedhat1.next();
    snare1.next();
    click1.next();
  }
  startupLedSequence();
}

void loop() {
  audioHook();
}

void nextNote() {
  float msPerBeat = (60.0 / paramTempo) * 1000.0;
  nextNoteTime += 0.25 * msPerBeat / 4;
  currentStep ++;
  currentStep = currentStep % numSteps;
}

void scheduleNote(byte channelNumber, byte beatNumber, byte velocity, float delayTime) {
  // schedule a drum hit to occur after [delayTime] milliseconds
  eventDelays[eventDelayIndex].set(delayTime);
  eventDelays[eventDelayIndex].start();
  delayInUse[eventDelayIndex] = true;
  delayChannel[eventDelayIndex] = channelNumber;
  delayVelocity[eventDelayIndex] = velocity;
  eventDelayIndex = (eventDelayIndex + 1) % numEventDelays; // replace with "find next free slot" function
}

void scheduler() {
  byte thisBeatVelocity;
  while(nextNoteTime < (float) millis() + scheduleAheadTime) {
    byte thisStep = currentStep/16;
    byte stepLED = constrain(thisStep, 0, 4);
    if(beatLedsActive) {
      if(currentStep%16==0) digitalWrite(ledPins[stepLED], HIGH);
      else if(currentStep%4==0) digitalWrite(ledPins[stepLED], LOW);
    }
    for(byte i=0;i<NUM_SAMPLES;i++) {
      int slopRand = rand(0,paramSlop) - paramSlop / 2;
      thisBeatVelocity = pgm_read_byte(&beat1[i][currentStep/4]);
      if(currentStep%4==0&&thisBeatVelocity>0) scheduleNote(i, currentStep, thisBeatVelocity, nextNoteTime + slopRand - (float) millis());
      else {
        // temp, playing around
        if(currentStep%4==0) {
          byte yesNoRand = rand(0,255);
          if(yesNoRand < paramChance) {
            long velocityRand = rand(0,255); // is long necessary?
            long velocity = paramMidpoint - paramRange/2 + ((velocityRand * paramRange)>>8); // is long necessary?
            if(velocity > 0) scheduleNote(i, currentStep, velocity, nextNoteTime - (float) millis());
          }
        }
      }
    }
    nextNote();
  }
  schedulerEventDelay.set(lookahead);
  schedulerEventDelay.start();
}

void toggleSequence() {
  if(sequencePlaying) stopSequence();
  else startSequence();
}

void startSequence() {
  sequencePlaying = true;
  currentStep = 0;
  nextNoteTime = millis();
  scheduler();
}

void stopSequence() {
  sequencePlaying = false;
  for(byte i=0; i<NUM_LEDS; i++) {
    digitalWrite(ledPins[i], LOW);
  }
}

void updateControl() {
  
  buttonA.update();
  buttonB.update();
  buttonC.update();
  buttonX.update();
  buttonY.update();
  buttonZ.update();

  if(!buttonC.read() && buttonC.duration()>2000) {
    readyToSave = false;
    readyToChooseSaveLocation = true;
  } else {
    readyToSave = true;
  }

  // switch active set of control knobs if button pressed
  byte prevControlSet = controlSet;
  if(buttonA.fell()) {
    if(readyToChooseSaveLocation) saveParams(0);
    else {
      controlSet = 0;
      doTapTempo();
    }
  } else if(buttonB.fell()) {
    if(readyToChooseSaveLocation) saveParams(1);
    else controlSet = 1;
  } else if(buttonC.fell()) {
    if(readyToChooseSaveLocation) saveParams(2);
    else controlSet = 2;
  } else if(buttonX.fell()) {
    if(readyToChooseSaveLocation) saveParams(3);
    else controlSet = 3;
  } else if(buttonY.fell()) {
    if(readyToChooseSaveLocation) saveParams(4);
    else controlSet = 4;
  }
  bool controlSetChanged = (prevControlSet != controlSet);
  
  if(buttonZ.fell()) toggleSequence();
  byte i;
  if(sequencePlaying) {
    if(schedulerEventDelay.ready()) scheduler();
    for(i=0;i<numEventDelays;i++) {
      if(eventDelays[i].ready() && delayInUse[i]) {
        delayInUse[i] = false;
        if(delayVelocity[i] > 8) { // don't bother with very quiet notes
          if(delayChannel[i]==0) {
            kick1.start();
          } else if(delayChannel[i]==1) {
            closedhat1.start();
          } else if(delayChannel[i]==2) {
            snare1.start();
          } else if(delayChannel[i]==3) {
            click1.start();
          }
          sampleVolumes[delayChannel[i]] = delayVelocity[i];
        }
      }
    }
  }

  for(i=0;i<NUM_KNOBS;i++) {
    analogValues[i] = mozziAnalogRead(i); // read all analog values
  }
  if(controlSetChanged || firstLoop) {
    // "lock" all knobs when control set changes
    for(byte i=0;i<NUM_KNOBS;i++) {
      knobLocked[i] = !firstLoop;
      initValues[i] = analogValues[i];
      if(firstLoop) {
        storedValues[controlSet][i] = analogValues[i];
        //Serial.println(mozziAnalogRead(i));
        //Serial.println(storedValues[controlSet][i]);
      }
    }
  } else {
    // unlock knobs if passing through stored value position
    for(i=0;i<NUM_KNOBS;i++) {
      if(knobLocked[i]) {
        if(initValues[i]<storedValues[controlSet][i]) {
          if(analogValues[i]>=storedValues[controlSet][i]) {
            knobLocked[i] = false;
          }
        } else {
          if(analogValues[i]<=storedValues[controlSet][i]) {
            knobLocked[i] = false;
          }
        }
      }
      if(!knobLocked[i]) {
        storedValues[controlSet][i] = analogValues[i]; // store new analog value if knob active
      }
    }
  }

  // do logic based on params
  switch(controlSet) {
    case 0:
    paramChance = storedValues[0][0]>>2;
    paramMidpoint = storedValues[0][1]>>2;
    paramRange = storedValues[0][2]>>2;
    paramZoom = storedValues[0][3]>>2;
    break;

    case 1:
    {
      //float newKickFreq = ((float) storedValues[1][0] / 255.0f) * (float) kick_SAMPLERATE / (float) kick_NUM_CELLS;
      float newHatFreq = ((float) storedValues[1][0] / 255.0f) * (float) closedhat_SAMPLERATE / (float) closedhat_NUM_CELLS;
      float newSnareFreq = ((float) storedValues[1][0] / 255.0f) * (float) snare_SAMPLERATE / (float) snare_NUM_CELLS;
      float newClickFreq = ((float) storedValues[1][0] / 255.0f) * (float) click_SAMPLERATE / (float) click_NUM_CELLS;
      //kick1.setFreq(newKickFreq);
      closedhat1.setFreq(newHatFreq);
      snare1.setFreq(newSnareFreq);
      click1.setFreq(newClickFreq);
      paramCrush = 7-(storedValues[1][1]>>7);
      crushCompensation = paramCrush;
      if(paramCrush >= 6) crushCompensation --;
      if(paramCrush >= 7) crushCompensation --;
    }
    break;

    case 2:
    paramSlop = storedValues[2][0]>>2; // between 0ms and 255ms?
    break;

    case 3:
    paramTempo = 40.0 + ((float) storedValues[3][0]) / 5.0;
    paramTimeSignature = (storedValues[3][1]>>7)+1;
    numSteps = paramTimeSignature * 16;
  }

  firstLoop = false;
}

const byte atten = 9;
int updateAudio() {
  char asig = ((sampleVolumes[0]*kick1.next())>>atten)+((sampleVolumes[1]*closedhat1.next())>>atten)+((sampleVolumes[2]*snare1.next())>>atten)+((sampleVolumes[3]*click1.next())>>atten);
  asig = (asig>>paramCrush)<<crushCompensation;
  return (int) asig;
}

void startupLedSequence() {
  byte seq[15] = {0,4,3,1,4,2,0,3,1,0,4,3,0,4,2};
  for(byte i=0; i<15; i++) {
    digitalWrite(ledPins[seq[i]], HIGH);
    delay(25);
    digitalWrite(ledPins[seq[i]], LOW);
  }
}

void doTapTempo() {
  unsigned long now = millis();
  byte numValid = 1;
  byte i;
  for(i=7;i>0;i--) {
    if(tapTempoTaps[i-1]-tapTempoTaps[i]<5000) numValid ++; // this part needs some work
    tapTempoTaps[i] = tapTempoTaps[i-1];
  }
  tapTempoTaps[0] = now;
  unsigned long averageTime = 0;
  for(i=1;i<numValid;i++) {
    averageTime += (tapTempoTaps[i-1]-tapTempoTaps[i])/(numValid-1); // losing some accuracy here, change this code if tap tempo is inaccurate
  }
  if(numValid >= 4) {
    paramTempo = 60000.0 / (float) averageTime;
  }
}

void saveParams(byte saveLocation) {
  readyToChooseSaveLocation = false;
  for(byte i=0; i<NUM_PARAM_GROUPS; i++) {
    // for each group of params...
    for(byte j=0; j<4; j++) {
      // for each param in the group...
      //EEPROM.write(
    }
  }
}
