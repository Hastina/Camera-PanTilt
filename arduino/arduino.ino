//------------------------------------------------------------------------------
// Pan/Tilt controller - supports raprapdiscount RUMBA controller
// dan@marginallycelver.com 2013-11-28
// RUMBA should be treated like a MEGA 2560 Arduino.
//------------------------------------------------------------------------------
// Copyright at end of file.
// please see http://www.github.com/MarginallyClever/PanTilt for more information.


//------------------------------------------------------------------------------
// CONSTANTS
//------------------------------------------------------------------------------
//#define VERBOSE              (1)  // add to get a lot more serial output.

#define VERSION              (2)  // firmware version

#define BAUD                 (57600)  // How fast is the Arduino talking?
#define MAX_BUF              (64)  // What is the longest message Arduino can store?

#define STEPS_PER_TURN       (400)  // depends on your stepper motor.  most are 200.
#define MAX_FEEDRATE         (200)
#define MIN_FEEDRATE         (0.01)

#define NUM_AXIES            (2)

#define CLOCK_FREQ           (16000000L)
#define MAX_COUNTER          (65536L)
#define MAX_SEGMENTS         (32)


//------------------------------------------------------------------------------
// STRUCTS
//------------------------------------------------------------------------------
// for line()
typedef struct {
  long step_count;
  long delta;  // number of steps to move
  long absdelta;
  long over;  // for dx/dy bresenham calculations
  int dir;
} Axis;


typedef struct {
  int step_pin;
  int dir_pin;
  int enable_pin;
  int limit_switch_pin;
  int limit_switch_state;
} Motor;


typedef struct {
  Axis a[NUM_AXIES];
  int steps;
  int steps_left;
  long feed_rate;
} Segment;


//------------------------------------------------------------------------------
// GLOBALS
//------------------------------------------------------------------------------
Axis a[NUM_AXIES];  // for line()
Axis atemp;  // for line()
Motor motors[NUM_AXIES];

Segment line_segments[MAX_SEGMENTS];
volatile int current_segment;
volatile int last_segment;

char buffer[MAX_BUF];  // where we store the message until we get a ';'
int sofar;  // how much is in the buffer

// speeds
float feed_rate=0;  // human version

float px,py;  // position

// settings
char mode_abs=1;  // absolute mode?


//------------------------------------------------------------------------------
// METHODS
//------------------------------------------------------------------------------


/**
 * delay for the appropriate number of microseconds
 * @input ms how many milliseconds to wait
 */
void pause(long ms) {
  delay(ms/1000);
  delayMicroseconds(ms%1000);  // delayMicroseconds doesn't work for values > ~16k.
}


/**
 * Set the feedrate (speed motors will move)
 * @input nfr the new speed in steps/second
 */
float feedrate(float nfr) {
  if(feed_rate==nfr) return nfr;  // same as last time?  quit now.

  if(nfr>MAX_FEEDRATE) {
    Serial.print(F("Feedrate set to maximum ("));
    Serial.print(MAX_FEEDRATE);
    Serial.println(F("steps/s)"));
    nfr=MAX_FEEDRATE;
  }
  if(nfr<MIN_FEEDRATE) {  // don't allow crazy feed rates
    Serial.print(F("Feedrate set to minimum ("));
    Serial.print(MIN_FEEDRATE);
    Serial.println(F("steps/s)"));
    nfr=MIN_FEEDRATE;
  }
  feed_rate=nfr;
  
  return feed_rate;
}


/**
 * Set the logical position
 * @input npx new position x
 * @input npy new position y
 */
void position(float npx,float npy) {
  // here is a good place to add sanity tests
  px=npx;
  py=npy;
}


int get_next_segment(int i) {
  return ( i + 1 ) % MAX_SEGMENTS;
}


int get_prev_segment(int i) {
  return ( i + MAX_SEGMENTS - 1 ) % MAX_SEGMENTS;
}


/**
 * Set the clock 1 timer frequency.
 * @input desired_freq_hz the desired frequency
 */
void timer_set_frequency(long desired_freq_hz) {
  // Source: http://letsmakerobots.com/node/28278
  // Different clock sources can be selected for each timer independently. 
  // To calculate the timer frequency (for example 2Hz using timer1) you will need:
  
  //  CPU frequency 16Mhz for Arduino
  //  maximum timer counter value (256 for 8bit, 65536 for 16bit timer)
  int prescaler_index=-1;
  int prescalers[] = {1,8,64,256,1024};
  long counter_value;
  do {
    ++prescaler_index;
    //  Divide CPU frequency through the choosen prescaler (16000000 / 256 = 62500)
    counter_value = CLOCK_FREQ / prescalers[prescaler_index];
    //  Divide result through the desired frequency (62500 / 2Hz = 31250)
    counter_value /= desired_freq_hz;
    //  Verify counter_value < maximum timer. if fail, choose bigger prescaler.
  } while(counter_value > MAX_COUNTER && prescaler_index<4);
  
  if( prescaler_index>=5 ) {
    Serial.println(F("Timer could not be set: Desired frequency out of bounds."));
    return;
  }

#ifdef VERBOSE
  Serial.print(F("counter_value  ="));  Serial.print(counter_value);
  Serial.print(F(" prescaler_index="));  Serial.print(prescaler_index);
  Serial.print(F(" = "));  Serial.print(((prescaler_index&0x1)   ));
  Serial.print(F("/"));  Serial.print(((prescaler_index&0x2)>>1));
  Serial.print(F("/"));  Serial.println(((prescaler_index&0x4)>>2));
#endif

  // disable global interrupts
  noInterrupts();
  
  // set entire TCCR1A register to 0
  TCCR1A = 0;
  // set entire TCCR1B register to 0
  TCCR1B = 0;
  // set the overflow clock to 0
  TCNT1  = 0;
  // set compare match register to desired timer count
  OCR1A = counter_value;
  // turn on CTC mode
  TCCR1B |= (1 << WGM12);
  // Set CS10, CS11, and CS12 bits for prescaler
  TCCR1B |= ( (( prescaler_index&0x1 )   ) << CS10);
  TCCR1B |= ( (( prescaler_index&0x2 )>>1) << CS11);
  TCCR1B |= ( (( prescaler_index&0x4 )>>2) << CS12);
  // enable timer compare interrupt
  TIMSK1 |= (1 << OCIE1A);

  interrupts();  // enable global interrupts
}


/**
 * Supports movement with both styles of Motor Shield
 * @input newx the destination x position
 * @input newy the destination y position
 **/
void onestep(int motor) {
#ifdef VERBOSE
  char *letter="XYZUVW";
  Serial.print(letter[motor]);
#endif
  
  digitalWrite(motors[motor].step_pin,HIGH);
  digitalWrite(motors[motor].step_pin,LOW);
}

 
/**
 * Process all line segments in the ring buffer.  Uses bresenham's line algorithm to move all motors.
 */
ISR(TIMER1_COMPA_vect) {
  // segment buffer empty? do nothing
  if( current_segment == last_segment ) return;
  
  // Is this segment done?
  if( line_segments[current_segment].steps_left <= 0 ) {
    // Move on to next segment without wasting an interrupt tick.
    current_segment = get_next_segment(current_segment);
    if( current_segment == last_segment ) return;
  }
  
  int j;
  Segment &seg = line_segments[current_segment];
  // is this a fresh new segment?
  if( seg.steps == seg.steps_left ) {
    // set the direction pins
    for(j=0;j<NUM_AXIES;++j) {
      digitalWrite( motors[j].dir_pin, line_segments[current_segment].a[j].dir );
    }
    // set frequency to segment feed rate
    timer_set_frequency(seg.feed_rate);
  }

  // make a step
  --seg.steps_left;

  // move each axis
  for(j=0;j<NUM_AXIES;++j) {
    Axis &a = seg.a[j];
    
    a.over += a.absdelta;
    if(a.over >= seg.steps) {
      digitalWrite(motors[j].step_pin,LOW);
      a.over -= seg.steps;
      digitalWrite(motors[j].step_pin,HIGH);
    }
  }
}


/**
 * Uses bresenham's line algorithm to move both motors
 **/
void line(int n0,int n1,float new_feed_rate) {
  int next_segment = get_next_segment(last_segment);
  while( next_segment == current_segment ) {
    // the buffer is full, we are way ahead of the motion system
    delay(1);
  }

  Segment &new_seg = line_segments[last_segment];
  new_seg.a[0].step_count = n0;
  new_seg.a[1].step_count = n1;
  
  int i;

  Segment &old_seg = line_segments[get_prev_segment(last_segment)];
  new_seg.a[0].delta = n0 - old_seg.a[0].step_count;
  new_seg.a[1].delta = n1 - old_seg.a[1].step_count;

  new_seg.steps=0;
  new_seg.feed_rate=new_feed_rate;

  for(i=0;i<NUM_AXIES;++i) {
    new_seg.a[i].over = 0;
    new_seg.a[i].dir = (new_seg.a[i].delta > 0 ? LOW:HIGH);
    new_seg.a[i].absdelta = abs(new_seg.a[i].delta);
    if( new_seg.steps < new_seg.a[i].absdelta ) {
      new_seg.steps = new_seg.a[i].absdelta;
    }
  }

  if( new_seg.steps==0 ) return;

  new_seg.steps_left = new_seg.steps;
  
  if( current_segment==last_segment ) {
    timer_set_frequency(new_feed_rate);
  }
  
#ifdef VERBOSE
  Serial.print(F("At "));  Serial.println(current_segment);
  Serial.print(F("Adding "));  Serial.println(last_segment);
  Serial.print(F("Steps= "));  Serial.println(new_seg.steps_left);
#endif
  last_segment = next_segment;
  
  px = n0;
  py = n1;
}


/**
 * Look for character /code/ in the buffer and read the float that immediately follows it.
 * @return the value found.  If nothing is found, /val/ is returned.
 * @input code the character to look for.
 * @input val the return value if /code/ is not found.
 **/
float parsenumber(char code,float val) {
  char *ptr=buffer;
  while(ptr && *ptr && ptr<buffer+sofar) {
    if(*ptr==code) {
      return atof(ptr+1);
    }
    ptr=strchr(ptr,' ')+1;
  }
  return val;
} 


/**
 * write a string followed by a float to the serial line.  Convenient for debugging.
 * @input code the string.
 * @input val the float.
 */
void output(char *code,float val) {
  Serial.print(code);
  Serial.println(val);
}


/**
 * print the current position, feedrate, and absolute mode.
 */
void where() {
  output("X",px);
  output("Y",py);
  output("F",feed_rate);
  Serial.println(mode_abs?"ABS":"REL");
} 


/**
 * display helpful information
 */
void help() {
  Serial.print(F("Pan/Tilt Robot v8-"));
  Serial.println(VERSION);
  Serial.println(F("Commands:"));
  Serial.println(F("G00/G01 [X/Y(steps)] [F(feedrate)]; - linear move"));
  Serial.println(F("G04 P[seconds]; - delay"));
  Serial.println(F("G90; - absolute mode"));
  Serial.println(F("G91; - relative mode"));
  Serial.println(F("G92; - find home"));
  Serial.println(F("M18; - disable motors"));
  Serial.println(F("M100; - this help message"));
  Serial.println(F("M114; - report position and feedrate"));
}


/**
 * Read the input buffer and find any recognized commands.  One G or M command per line.
 */
void processCommand() {
  int cmd = parsenumber('G',-1);
  switch(cmd) {
  case  0: // move linear
  case  1: // move linear
    feedrate(parsenumber('F',feed_rate));
    line( parsenumber('X',(mode_abs?px:0)) + (mode_abs?0:px),
          parsenumber('Y',(mode_abs?py:0)) + (mode_abs?0:py),
          feed_rate );
    break;
  case  4:  pause(parsenumber('P',0)*1000);  break;  // dwell
  case 90:  mode_abs=1;  break;  // absolute mode
  case 91:  mode_abs=0;  break;  // relative mode
  case 92:  // set logical position
    find_home();
    break;
  default:  break;
  }

  cmd = parsenumber('M',-1);
  switch(cmd) {
  case 17:  motor_enable();  break;
  case 18:  motor_disable();  break;
  case 100:  help();  break;
  case 114:  where();  break;
  default:  break;
  }
}


/**
 * read the limit switch states
 * @return 1 if a switch is being hit
 */
char read_switches() {
  char i, hit=0;
  int state;
  
  for(i=0;i<NUM_AXIES;++i) {
    state=digitalRead(motors[i].limit_switch_pin);
#ifdef DEBUG_SWITCHES
    Serial.print(state);
    Serial.print('\t');
#endif
    if(motors[i].limit_switch_state != state) {
      motors[i].limit_switch_state = state;
#ifdef DEBUG_SWITCHES
      Serial.print(F("Switch "));
      Serial.println(i,DEC);
#endif
    }
    if(state == LOW) ++hit;
  }
#ifdef DEBUG_SWITCHES
  Serial.print('\n');
#endif
  return hit;
}


void find_home() {
  Serial.print(F("Finding Home X..."));

  // if we are on the switches already, back off.
  digitalWrite(motors[0].dir_pin, LOW );
  digitalWrite(motors[1].dir_pin, LOW );
  while(read_switches()!=0) {
    onestep(0);
    onestep(1);
    delayMicroseconds(500);
  }
  
  // move stepper 1 clockwise until the switch is hit
  digitalWrite( motors[0].dir_pin, HIGH );
  while(read_switches()==0) {
    onestep(0);
    delayMicroseconds(150);
  }
  // back off
  digitalWrite(motors[0].dir_pin, LOW );
  while(read_switches()==1) {
    onestep(0);
    delayMicroseconds(500);
  }
  
  Serial.print(F("Finding Home Y..."));
  // move stepper 2 clockwise until the switch is hit
  digitalWrite( motors[1].dir_pin, HIGH );
  while(read_switches()==0) {
    onestep(1);
    delayMicroseconds(150);
  }
  // back off
  digitalWrite(motors[1].dir_pin, LOW );
  while(read_switches()==1) {
    onestep(1);
    delayMicroseconds(500);
  }
  
  px=py=0;
  Serial.println(F("Found!"));
}


/**
 * prepares the input buffer to receive a new message and tells the serial connected device it is ready for more.
 */
void ready() {
  sofar=0;  // clear input buffer
  Serial.print(F(">"));  // signal ready to receive input
}


/**
 * set up the pins for each motor
 */
void motor_setup() {
  motors[0].step_pin=17;
  motors[0].dir_pin=16;
  motors[0].enable_pin=48;
  motors[0].limit_switch_pin=37;

  motors[1].step_pin=54;
  motors[1].dir_pin=47;
  motors[1].enable_pin=55;
  motors[1].limit_switch_pin=36;
  
  int i;
  for(i=0;i<NUM_AXIES;++i) {  
    // set the motor pin & scale
    pinMode(motors[i].step_pin,OUTPUT);
    pinMode(motors[i].dir_pin,OUTPUT);
    pinMode(motors[i].enable_pin,OUTPUT);
    
    // set the switch pin
    motors[i].limit_switch_state=HIGH;
    pinMode(motors[i].limit_switch_pin,INPUT);
    digitalWrite(motors[i].limit_switch_pin,HIGH);
  }
}


void motor_enable() {
  int i;
  for(i=0;i<NUM_AXIES;++i) {  
    digitalWrite(motors[i].enable_pin,LOW);
  }
}


void motor_disable() {
  int i;
  for(i=0;i<NUM_AXIES;++i) {  
    digitalWrite(motors[i].enable_pin,HIGH);
  }
}


/**
 * First thing this machine does on startup.  Runs only once.
 */
void setup() {
  Serial.begin(BAUD);  // open coms

  motor_setup();
  motor_enable();
  
  help();  // say hello
  position(0,0);  // set staring position
  feedrate(200);  // set default speed
  ready();
}


/**
 * After setup() this machine will repeat loop() forever.
 */
void loop() {
  // listen for serial commands
  while(Serial.available() > 0) {  // if something is available
    char c=Serial.read();  // get it
    Serial.print(c);  // repeat it back so I know you got the message
    if(sofar<MAX_BUF) buffer[sofar++]=c;  // store it
    if(buffer[sofar-1]==';') break;  // entire message received
  }

  if(sofar>0 && buffer[sofar-1]==';') {
    // we got a message and it ends with a semicolon
    buffer[sofar]=0;  // end the buffer so string functions work right
    Serial.print(F("\r\n"));  // echo a return character for humans
    processCommand();  // do something with the command
    ready();
  }
}


/**
* This file is part of PanTilt.
*
* PanTilt is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* PanTilt is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with PanTilt. If not, see <http://www.gnu.org/licenses/>.
*/
