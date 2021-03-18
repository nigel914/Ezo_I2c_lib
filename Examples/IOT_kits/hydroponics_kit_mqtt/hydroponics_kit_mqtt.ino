#include <iot_cmd.h>
#include <ESP8266WiFi.h>                                         //include esp8266 wifi library 
#include <sequencer4.h>                                          //imports a 4 function sequencer 
#include <sequencer1.h>                                          //imports a 1 function sequencer 
#include <Ezo_i2c_util.h>                                        //brings in common print statements
#include <Ezo_i2c.h>                                             //include the EZO I2C library from https://github.com/Atlas-Scientific/Ezo_I2c_lib
#include <Wire.h>                                                //include arduinos i2c library
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>

WiFiClient client;                                              //declare that this device connects to a Wi-Fi network,create a connection to a specified internet IP address

//----------------Fill in your Wi-Fi / MQTT Credentials-------
const String ssid = "Wifi Name";                                //The name of the Wi-Fi network you are connecting to
const String pass = "Wifi Password";                            //Your WiFi network password
#define MQTT_SERVER     "XXX.XXX.XXX.XXX"                       //Your MQTT server address
#define MQTT_SERVERPORT 1883                                    //Your MQTT server port (default is usually 1883)
#define MQTT_USERNAME   "MQTT User"                             //Your MQTT server username
#define MQTT_KEY        "MQTT Password"                         //Your MQTT server password

#define PH_CHANNEL      "sensors/hydroponics-kit/ph"
#define TEMP_CHANNEL    "sensors/hydroponics-kit/temp"
#define EC_CHANNEL      "sensors/hydroponics-kit/ec"
//------------------------------------------------------------------

Adafruit_MQTT_Client mqtt(&client, MQTT_SERVER, MQTT_SERVERPORT, MQTT_USERNAME, MQTT_KEY);
Adafruit_MQTT_Publish tempFeed = Adafruit_MQTT_Publish(&mqtt, TEMP_CHANNEL);
Adafruit_MQTT_Publish phFeed = Adafruit_MQTT_Publish(&mqtt, PH_CHANNEL);
Adafruit_MQTT_Publish ecFeed = Adafruit_MQTT_Publish(&mqtt, EC_CHANNEL);

Ezo_board PH = Ezo_board(99, "PH");       //create a PH circuit object, who's address is 99 and name is "PH"
Ezo_board EC = Ezo_board(100, "EC");      //create an EC circuit object who's address is 100 and name is "EC"
Ezo_board RTD = Ezo_board(102, "RTD");    //create an RTD circuit object who's address is 102 and name is "RTD"
Ezo_board PMP = Ezo_board(103, "PMP");    //create an PMP circuit object who's address is 103 and name is "PMP"

Ezo_board device_list[] = {   //an array of boards used for sending commands to all or specific boards
  PH,
  EC,
  RTD,
  PMP
};

bool send_to_mqtt = false;

Ezo_board* default_board = &device_list[0]; //used to store the board were talking to

//gets the length of the array automatically so we dont have to change the number every time we add new boards
const uint8_t device_list_len = sizeof(device_list) / sizeof(device_list[0]);

//enable pins for each circuit
const int EN_PH = 14;
const int EN_EC = 12;
const int EN_RTD = 15;
const int EN_AUX = 13;

const unsigned long reading_delay = 1000;                 //how long we wait to receive a response, in milliseconds 
const unsigned long thingspeak_delay = 15000;             //how long we wait to send values to thingspeak, in milliseconds

unsigned int poll_delay = 2000 - reading_delay * 2 - 300; //how long to wait between polls after accounting for the times it takes to send readings

const unsigned long mqtt_delay = 15000;       //how long we wait to send something to mqtt

const unsigned long short_delay = 300;              //how long we wait for most commands and queries
const unsigned long long_delay = 1200;              //how long we wait for commands like cal and R (see datasheets for which commands have longer wait times)

//parameters for setting the pump output
#define PUMP_BOARD        PMP       //the pump that will do the output (if theres more than one)
#define PUMP_DOSE         -0.5      //the dose that the pump will dispense
#define EZO_BOARD         EC        //the circuit that will be the target of comparison
#define IS_GREATER_THAN   true      //true means the circuit's reading has to be greater than the comparison value, false mean it has to be less than
#define COMPARISON_VALUE  1000      //the threshold above or below which the pump is activated

float k_val = 0;                                          //holds the k value for determining what to print in the help menu

bool polling  = true;                                     //variable to determine whether or not were polling the circuits
bool send_to_thingspeak = false;                           //variable to determine whether or not were sending data to thingspeak

bool wifi_isconnected(){                            //function to check if wifi is connected
  return (WiFi.status() == WL_CONNECTED);
}

void reconnect_wifi(){                                    //function to reconnect wifi if its not connected
  if(!wifi_isconnected()){
    WiFi.begin(ssid, pass);
    Serial.println("connecting to wifi");
    if (mqtt.connected()) {
      Serial.println("Connected to MQTT");
    }else{
      Serial.print("Connecting to MQTT... ");
       
      int8_t ret;
      while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
         Serial.println(mqtt.connectErrorString(ret));
         Serial.println("Retrying MQTT connection in 5 seconds...");
         mqtt.disconnect();
         delay(5000);  // wait 5 seconds
       }
       Serial.println("MQTT Connected!");
     }
    }else{
      Serial.println("Failed to connect to Wifi");
    }
 //   print_help();                                 //print our options on startup

    Serial.println("---------------------------------------------");
    Serial.println("Starting automatic datalogging to mqtt.");
    start_datalogging();
}

void step1();      //forward declarations of functions to use them in the sequencer before defining them
void step2();
void step3();
void step4();
Sequencer4 Seq(&step1, reading_delay,   //calls the steps in sequence with time in between them
               &step2, 300, 
               &step3, reading_delay,
               &step4, poll_delay);

Sequencer1 Wifi_Seq(&reconnect_wifi, 10000);  //calls the wifi reconnect function every 10 seconds

//Sequencer1 Thingspeak_seq(&thingspeak_send, thingspeak_delay); //sends data to thingspeak with the time determined by thingspeak delay

void setup() {

  pinMode(EN_PH, OUTPUT);                                                         //set enable pins as outputs
  pinMode(EN_EC, OUTPUT);
  pinMode(EN_RTD, OUTPUT);
  digitalWrite(EN_PH, LOW);                                                       //set enable pins to enable the circuits
  digitalWrite(EN_EC, LOW);
  digitalWrite(EN_RTD, HIGH);

  Wire.begin();                           //start the I2C
  Serial.begin(9600);                     //start the serial communication to the computer

  WiFi.mode(WIFI_STA);                    //set ESP8266 mode as a station to be connected to wifi network
  Wifi_Seq.reset();                       //initialize the sequencers
  Seq.reset();
}

void loop() {
 String cmd;                            //variable to hold commands we send to the kit

  Wifi_Seq.run();                        //run the sequncer to do the polling
  
  if (receive_command(cmd)) {            //if we sent the kit a command it gets put into the cmd variable
    polling = false;                     //we stop polling  
    send_to_thingspeak = false;          //and sending data to thingspeak
    if(!process_coms(cmd)){              //then we evaluate the cmd for kit specific commands
      process_command(cmd, device_list, device_list_len, default_board);    //then if its not kit specific, pass the cmd to the IOT command processing function
    }
  }
  
  if (polling == true) {                 //if polling is turned on, run the sequencer
    Seq.run();
  }
}

//function that controls the pumps activation and output
void pump_function(Ezo_board &pump, Ezo_board &sensor, float value, float dose, bool greater_than){
 if (sensor.get_error() == Ezo_board::SUCCESS) {                    //make sure we have a valid reading before we make any decisions
    bool comparison = false;                                        //variable for holding the reuslt of the comparison
    if(greater_than){                                               //we do different comparisons depending on what the user wants
      comparison = (sensor.get_last_received_reading() >= value);   //compare the reading of the circuit to the comparison value to determine whether we actiavte the pump
    }else{
      comparison = (sensor.get_last_received_reading() <= value);
    }
    if (comparison) {                                               //if the result of the comparison means we should activate the pump
      pump.send_cmd_with_num("d,", dose);                           //dispense the dose
      delay(100);                                                   //wait a few milliseconds before getting pump results
      Serial.print(pump.get_name());                                //get pump data to tell the user if the command was received successfully
      Serial.print(" ");
      char response[20]; 
      if(pump.receive_cmd(response, 20) == Ezo_board::SUCCESS){
        Serial.print("pump dispensed ");
      }else{
        Serial.print("pump error ");
      }
      Serial.println(response);
    }else {
      pump.send_cmd("x");                                          //if we're not supposed to dispense, stop the pump
    }
  }
}

void step1() {
  //send a read command. we use this command instead of RTD.send_cmd("R"); 
  //to let the library know to parse the reading
  RTD.send_read_cmd();
}

void step2() {
  receive_and_print_reading(RTD);             //get the reading from the RTD circuit

  if ((RTD.get_error() == Ezo_board::SUCCESS) && (RTD.get_last_received_reading() > -1000.0)) { //if the temperature reading has been received and it is valid
    PH.send_cmd_with_num("T,", RTD.get_last_received_reading());
    EC.send_cmd_with_num("T,", RTD.get_last_received_reading());
    tempFeed.publish(RTD.get_last_received_reading());
  } else {                                                                                      //if the temperature reading is invalid
    PH.send_cmd_with_num("T,", 25.0);
    EC.send_cmd_with_num("T,", 25.0);                                                          //send default temp = 25 deg C to EC sensor
//    tempFeed.publish("T,", 25.0);
  }

  Serial.print(" ");
}

void step3() {
  //send a read command. we use this command instead of PH.send_cmd("R");
  //to let the library know to parse the reading
  PH.send_read_cmd();
  EC.send_read_cmd();
}

void step4() {
  receive_and_print_reading(PH);             //get the reading from the PH circuit
  if (PH.get_error() == Ezo_board::SUCCESS) {                                          //if the PH reading was successful (back in step 1)
     phFeed.publish(PH.get_last_received_reading());
  }
  Serial.print("  ");
  receive_and_print_reading(EC);             //get the reading from the EC circuit
  if (EC.get_error() == Ezo_board::SUCCESS) {                                          //if the EC reading was successful (back in step 1)
     ecFeed.publish(EC.get_last_received_reading());                                   //assign EC readings
  }

  Serial.println();
  pump_function(PUMP_BOARD, EZO_BOARD, COMPARISON_VALUE, PUMP_DOSE, IS_GREATER_THAN);
}

void start_datalogging() {
  polling = true;                                             //set poll to true to start the polling loop
  poll_delay = mqtt_delay - reading_delay * 2 - short_delay; //polling delay is how long how often we upload to mqtt minus the time it takes to take the readings
  send_to_mqtt = true;
}

bool process_coms(const String &string_buffer) {      //function to process commands that manipulate global variables and are specifc to certain kits
  if (string_buffer == "HELP") {
    print_help();
    return true;
  }
  else if (string_buffer.startsWith("DATALOG")) {
     start_datalogging();
    return true;
  }
  else if (string_buffer.startsWith("POLL")) {
    polling = true;  
    Seq.reset();
    
    int16_t index = string_buffer.indexOf(',');                    //check if were passing a polling delay parameter
    if (index != -1) {                                              //if there is a polling delay
      float new_delay = string_buffer.substring(index + 1).toFloat(); //turn it into a float

      float mintime = reading_delay*2 + 300;
      if (new_delay >= (mintime/1000.0)) {                                       //make sure its greater than our minimum time
        Seq.set_step4_time((new_delay * 1000.0) - mintime);          //convert to milliseconds and remove the reading delay from our wait
      } else {
        Serial.println("delay too short");                          //print an error if the polling time isnt valid
      }
    }
    return true;
  }
  return false;                         //return false if the command is not in the list, so we can scan the other list or pass it to the circuit
}

void get_ec_k_value(){                                    //function to query the value of the ec circuit
  char rx_buf[10];                                        //buffer to hold the string we receive from the circuit
  EC.send_cmd("k,?");                                     //query the k value
  delay(300);
  if(EC.receive_cmd(rx_buf, 10) == Ezo_board::SUCCESS){   //if the reading is successful
    k_val = String(rx_buf).substring(3).toFloat();        //parse the reading into a float
  }
}

void print_help() {
  get_ec_k_value();
  Serial.println(F("Atlas Scientific I2C hydroponics kit                                       "));
  Serial.println(F("Commands:                                                                  "));
  Serial.println(F("datalog      Takes readings of all sensors every 15 sec send to thingspeak "));
  Serial.println(F("             Entering any commands stops datalog mode.                     "));
  Serial.println(F("poll         Takes readings continuously of all sensors                    "));
  Serial.println(F("                                                                           "));
  Serial.println(F("ph:cal,mid,7     calibrate to pH 7                                         "));
  Serial.println(F("ph:cal,low,4     calibrate to pH 4                                         "));
  Serial.println(F("ph:cal,high,10   calibrate to pH 10                                        "));
  Serial.println(F("ph:cal,clear     clear calibration                                         "));
  Serial.println(F("                                                                           "));
  Serial.println(F("ec:cal,dry           calibrate a dry EC probe                              "));
  Serial.println(F("ec:k,[n]             used to switch K values, standard probes values are 0.1, 1, and 10 "));
  Serial.println(F("ec:cal,clear         clear calibration                                     "));

  if(k_val > 9){
     Serial.println(F("For K10 probes, these are the recommended calibration values:            "));
     Serial.println(F("  ec:cal,low,12880     calibrate EC probe to 12,880us                    "));
     Serial.println(F("  ec:cal,high,150000   calibrate EC probe to 150,000us                   "));
  }
  else if(k_val > .9){
     Serial.println(F("For K1 probes, these are the recommended calibration values:             "));
     Serial.println(F("  ec:cal,low,12880     calibrate EC probe to 12,880us                    "));
     Serial.println(F("  ec:cal,high,80000    calibrate EC probe to 80,000us                    "));
  }
  else if(k_val > .09){
     Serial.println(F("For K0.1 probes, these are the recommended calibration values:           "));
     Serial.println(F("  ec:cal,low,84        calibrate EC probe to 84us                        "));
     Serial.println(F("  ec:cal,high,1413     calibrate EC probe to 1413us                      "));
  }
  
  Serial.println(F("                                                                           ")); 
  Serial.println(F("rtd:cal,t            calibrate the temp probe to any temp value            "));
  Serial.println(F("                     t= the temperature you have chosen                    "));
  Serial.println(F("rtd:cal,clear        clear calibration                                     "));
 }