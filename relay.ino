#include <ESP8266WiFi.h>
#include <EEPROM.h>

// A magic number to store in first 2 bytes of EEPROM to check its validity
#define MAGIC 0b10100101


/*
 * Relay class receives a pin number in its constructor and manages
 * a relay connected to that pin. One can call setState to set the
 * state of the relay or call getState to just read it
 */
class Relay {
private:
  int pin;
  bool closed;

public:
  Relay(int _pin) {
    this->pin = _pin;
    this->closed = false;
    pinMode(_pin, OUTPUT);
  }

  void setState(bool state) {
    this->closed = state;
    digitalWrite(this->pin, state ? HIGH : LOW);
  }

  bool getState() {
    return this->closed;
  }
};


/*
 * Sensor class gets a pin number in its constructor and creates an
 * interface to read data from touch sensor.
 * getState function return true in a cycle that state of sensor
 * changes from LOW to HIGH.
 */
class Sensor {
private:
  int pin;
  bool lastState;

public:
  Sensor(int _pin) {
    this->pin = _pin;
    this->lastState = false;
    pinMode(_pin, INPUT);
  }

  bool getState() {
    bool currentState = digitalRead(this->pin) == HIGH;
    bool result = currentState and !lastState;
    lastState = currentState;
    return result;
  }
};


/*
 * Relay Controller class combines Sensor and Relay class
 * It's used to connect these two elements and change the state
 * of the relay based on sensor's state.
 * Also it listena to a TCP socket and waits for commands from
 * the master.
 * The available commands are:
 *    000 -> sets the relay state to open
 *    111 -> sets the relay state to closed
 *    ??? -> asks about current state of the relay
 * In response to each command, the server will respond with
 * the new state of relay.
 * 
 * In each of the main program's iterations the stepForward
 * function should be called in order to process new commands
 * and sensor events.
 */
class RelayController {
private:
  Sensor *sensor;
  Relay *relay;
  WiFiServer *server;

public:
  RelayController(int sensorPin, int relayPin, int port) {
    sensor = new Sensor(sensorPin);
    relay = new Relay(relayPin);

    server = new WiFiServer(port);
    server->begin();
    server->setNoDelay(true);
  }

  ~RelayController() {
    server->close();
    delete server;
    delete sensor;
    delete relay;
  }

  void stepForward() {
    relay->setState(sensor->getState() ^ relay->getState());

    WiFiClient client = server->available();
    if (!client)
      return;
    while (client.connected()) {
      if (client.available()) {
        String line = client.readStringUntil('\n');
        if (line[1] == '0') {
          relay->setState(0);
          client.println(getState());
        }
        else if (line[1] == '1') {
          relay->setState(1);
          client.println(getState());
        }
        else if (line[1] == '?') {
          client.println(getState());
        }
      }
    }
    client.stop();
  }

  bool getState() {
    return relay->getState();
  }
};

// Create a list of relay controllers
// There are 2 relays so the length of the array is 2
RelayController *controller[2] = {NULL, NULL};


// Receive a string and cast it to IPAddress
IPAddress str2ip(String ip, int *errno) {
  int pos0 = ip.indexOf('.');
  int pos1 = ip.indexOf('.', pos0 + 1);
  int pos2 = ip.indexOf('.', pos1 + 1);
  int pos3 = ip.indexOf('.', pos2 + 1);
  if (pos0 == -1 or pos1 == -1 or pos2 == -1 or pos3 != -1) {
    (*errno) = 1;
    return IPAddress();
  }
  return IPAddress(
    ip.substring(0, pos0).toInt(),
    ip.substring(pos0 + 1, pos1).toInt(),
    ip.substring(pos1 + 1, pos2).toInt(),
    ip.substring(pos2 + 1).toInt()
   );
}

// Store a network config on EEPROM
// The config contains ssid, password, myIp, netmask and default gateway's IP address
void saveNetConfig(String ssid, String passwd, IPAddress myIp, IPAddress netmask, IPAddress gateway) {
  int pos = 0;
  EEPROM.begin(64);
  EEPROM.write(pos++, MAGIC);
  EEPROM.write(pos++, MAGIC);
  for (int i = 0; i < 4; i++) EEPROM.write(pos++, myIp[i]);
  for (int i = 0; i < 4; i++) EEPROM.write(pos++, netmask[i]);
  for (int i = 0; i < 4; i++) EEPROM.write(pos++, gateway[i]);
  for (int i = 0; i < ssid.length(); i++) EEPROM.write(pos++, ssid[i]);
  EEPROM.write(pos++, '\0');
  for (int i = 0; i < passwd.length(); i++) EEPROM.write(pos++, passwd[i]);
  EEPROM.write(pos++, '\0');
  EEPROM.commit();
  EEPROM.end();
}

// Load network config from EEPROM
void loadNetConfig() {
  int pos = 0;
  byte value = 0;

  bool valid = true;
  EEPROM.begin(64);

  /*
   * If we have written data on EEPROM, the first 2 bytes
   * must be equal to MAGIC
   */
  value = EEPROM.read(pos++);
  if (value != MAGIC) valid = false;
  value = EEPROM.read(pos++);
  if (value != MAGIC) valid = false;
  if (valid) {
    IPAddress myIp, netmask, gateway;
    String ssid, passwd;
    for (int i = 0; i < 4; i++) myIp[i] = EEPROM.read(pos++);
    for (int i = 0; i < 4; i++) netmask[i] = EEPROM.read(pos++);
    for (int i = 0; i < 4; i++) gateway[i] = EEPROM.read(pos++);
    for (char c; c = EEPROM.read(pos++); ssid += c);
    for (char c; c = EEPROM.read(pos++); passwd += c);
    WiFi.config(myIp, gateway, netmask);
    WiFi.begin(ssid, passwd);
  } else {
    WiFi.begin("Karim", "ashghalpedarsagmadarsag");
  }
  EEPROM.end();
  WiFi.setAutoReconnect(true);
  WiFi.waitForConnectResult();
}

/*
 * A handler function to handle network config command
 * in serial port
 * This is all about parsing the string and passing the
 * data to saveNetConfig function
 */
void networkHandler(String cmd) {
  if (cmd[0] == '?') {
    Serial.print("Local IP:\t");
    Serial.println(WiFi.localIP());
    Serial.print("Subnet Mask:\t");
    Serial.println(WiFi.subnetMask());
    Serial.print("Gateway:\t");
    Serial.println(WiFi.gatewayIP());
    Serial.print("SSID:\t");
    Serial.println(WiFi.SSID());
    Serial.print("Password:\t");
    Serial.println(WiFi.psk());
    Serial.print("Status:\t");
    Serial.println(WiFi.status());
    return;
  }
  int pos0 = cmd.indexOf(' ');
  if (pos0 == -1) {
    Serial.println("Error: Invalid format");
    return;
  }

  int pos1 = cmd.indexOf(' ', pos0 + 1);
  if (pos1 == -1) {
    Serial.println("Error: Invalid format");
    return;
  }

  int pos2 = cmd.indexOf(' ', pos1 + 1);
  if (pos2 == -1) {
    Serial.println("Error: Invalid format");
    return;
  }

  int pos3 = cmd.indexOf(' ', pos2 + 1);
  if (pos3 == -1) {
    Serial.println("Error: Invalid format");
    return;
  }

  String ssid = cmd.substring(0, pos0);
  String passwd = cmd.substring(pos0 + 1, pos1);
  int myErrno = 0;
  IPAddress myIp = str2ip(cmd.substring(pos1 + 1, pos2), &myErrno);
  IPAddress netmask = str2ip(cmd.substring(pos2 + 1, pos3), &myErrno);
  IPAddress gateway = str2ip(cmd.substring(pos3 + 1), &myErrno);
  if (myErrno) {
    Serial.println("Error: Invalid format");
    return;
  }
  saveNetConfig(ssid, passwd, myIp, netmask, gateway);
  Serial.println("Saved new config. Reboot to make effect.");
}

/*
 * A function to manage serial shell
 * It should be called in each iteration of main loop
 */
void processShell() {
  if (!Serial.available())
    return;
  String cmd = Serial.readStringUntil('\n');
  if (cmd.startsWith("net "))
    networkHandler(cmd.substring(4));
  else if (cmd.startsWith("relay "))
    ;
  else
    Serial.println("ERROR: Invalid command");
}

void setup() {
  // Start serial port, it's used for configuration commands
  Serial.begin(9600);

  // Load last network config from EEPROM
  loadNetConfig();

  // Initialize relay controllers
  // 16, 5, 13, 12 are the pins connected to sensors and relays
  controller[0] = new RelayController(16, 5, 8000);
  controller[1] = new RelayController(13, 12, 8001);
}

void loop() {
  controller[0]->stepForward();
  controller[1]->stepForward();
  processShell();
  delay(100);
}
