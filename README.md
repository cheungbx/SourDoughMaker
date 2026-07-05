I want to write an arduino sketch  to control the digital level of pins connected to a bread machine. When the digital level of a pin is set  to low, it has the same effect as pressing  the corresponding button on the bread machine. This will in turn control the bread machine to make  Sourdough Bread.

Create a sketch for an Async Web Server  using  ESP8266 (WEMOS D1 mini) that will display a  home page and accept  input on a  home page. 

When the program starts, retrieve the OperationMode WIFI SSID and password from EEPROM and store them into variables for later references.

If OperationMode == standalone,  call the Standalone-AP Process.
 then proceed to the Menu-Selection Process, otherwise continue on.

If OperationMode == WIFI, proceed to  the WIFI-Connection Process.

If OperationMode does not exist on EEPROM continue on. 
Call the standalone-AP process.
Show a web page with title “Sourdough Maker Configuration”.  
Show the parameter Operation Mode, with options WIFI  Standalone default WIFI.
If WIFI SSID and password Exists in EEPROMM, shows  the WIFI SSID and password retrieved from EEPROM, otherwise shows blank.
The WIFI SSID and password can be input or modified by users.
Show a button “Save Configurations”.
When the “Save Configurations” button is pressed, Store the Operation Mode into EEPROM.
If OperationMode==WIFI, store the SSID and Password input by user  into EEPROM, then restart the program from the very beginning.
If OperationMode==Standalone, Restart the program from the very beginning.

the Standalone-AP Process. 
function as a WIFI AP with the SSID=”SourDough”, Password=”12376254”. Broadcast the SSID, 

The WIFI-Connection Process.
Connects to WIFI. If the WIFI connection fails after retrying for 1 minute,  logs the wifi connection error, erase the EEPROM WIFI configurations, and restart the program from the very beginning.
If WIFI connects successfully,  proceed to  the Menu-Selection Process.

The Menu-Selection Process:
Logs the Operation Mode, the SSID  and the  IP Address.

Set up  a constant flag ServoMode default False.
Logs the ServoMode.

Assign stable Pins on ESP8266 that can be used to control either the Pins directly or the ServoMotors. 
ColourPin drives the ColourSrvo
MenuPin drives the MenuServo
MinusPin drives the MinusServo
RunResetPin drives the RunResetServo

At the start of the program, If ServoMode = true, set up all 4 ServoMotors to turn to 0 degree, otherwise,  set up  all pins for digitalOutput, and pull to high.

Retrieve the  parameters default options from the EEPROM. If it does not exist, use the defaults as shown in the list of  parameters. Store the defaults  in the corresponding variable for later reference:
BakeColour shown on web page as: “Bake Colour”  with  options Light Medium Dark  default Dark
KneadMin shown on web page as: “Knead (min)”  with  options 15 30 45 60 90 default 30
DegasMin  shown on web page as “Degas (min)” with options 15 30 45 60 90 120 150 180 default 30
HotProofHr  shown on web page as “HotProof (hr)” with options  0 0.5 1 2 3 6 9 12 15 18 21 24  default 1 
ProofHr  shown on web page as “Proof (hr)” with options  0 0.5 1 2 3 6 9 12 15 18 21 24  default 1 
BakeMin shown on web page as “Bake (min)” with  options 10 20 30 40 50  60 70 80 90 100 110 120 default 60


Create a home  page that accepts input any time this program is running as an Async Web Server. 
Display the title “SourDough Maker” on the home page..
Skip one line.
Display the Status Line: “Select Options then press Run” and “ Total Time:” followed by RemainingMin in hh:mm format where hh is the hour, mm is the minute (see calculation for RemainingMin below).
show  the “Erase Settings” button.
If OperationMode == standalone , show the “Switch to WIFI” button.
If OperationMode == WIFI, show the “Switch to Standalone” button.


Display a line separator.
Then display a menu screen  that shows the  parameters above with options to be chosen.
As options are selected, store the selected option for each parameter  in the corresponding variable for later referencing. Then recalculate the RemainingMin according to the Calculation for RemainingMin. Then display that on the status line as hh:mm where hh is hour, mm is minute..


Calculation for the RemainingMin:
RemainingMin = KneadMin + DegasMin + ProofHr * 60 + BakeMin

Skip one line.
Show a Run button.
Skip one line.
Show the Restart  button.

when the “Erase  Settings” button is pressed, replace it with the “Confirm to Erase Settings” button.
When the “Confirm to Erase settings” button is pressed, erase all EEPROM configurations stored, restart the program.

when the “Switch to WIFI”  button is pressed, replace it with the “Confirm to switch to WIFI” button.
when the “Switch to Standalone”  button is pressed, replace it with the “Confirm to switch to ”Standalone” button.

When the “Confirm to Switch to WIFI” button is pressed, set  the OperationMode in EEPROM to WIFI, then restart the program.
When the “Confirm to Switch to Standalone” button is pressed, set the OperationMode in EEPROM to Standalone, then restart the program.

When the Restart button is pressed, display “System Rebooting…”, then reboot and re-initiate from the very beginning.  
 
When the Run button is pressed, start the SourDough Process.

The SourDough Process
Stores the chosen options into EEPROM and retrieves these at the start of the Menu-Selection process as the default options.

Locks down all the options so they are  no longer selectable.
Replace the Run button on the  home page with the Pause button.
Count down the time remaining using RemainingMin.

When the Pause button is pressed and the SourDough process is running, display “Pausing:” on the status line, then pause the SourDough process, and pause the TotalMin count down.
When the Pause button is pressed and the SourDough process is pausing, display “Running”, then continue the SourDough process, and continue  the TotalMin count down.

When the Restart button is pressed, reboot and re-initiate from the very beginning.  


Runs the following processes in sequence : The Kneading Process then the Degas Process, then the HotProof Process, then the Proof Process then the Bake Process then returns to the Menu-Selection Process.
At the start of each process, highlight the line for that process on the  home page. E.g. When the Kneading Process starts, highlight the line for “KneadMin”.

Any time during these processes, continue to display the RemainingMin in hh:mm format where hh is the hour, mm is the minute, and display a spinning sequence indicator (/, -, \, |) next to the RemainingMin  to show that the SourDough Process is currently running. Any time during the SourDough Process, continue to accept input on the Reset Button and the  Pause button on the  home page. Ignore any other input from the  home page.

The ShortPress process.
The ShortPress  process  takes two inputs: Pin and Number of Times  (defaults to 1).
Logs the current RemainingMin in hh:mm format, followed by “ShortPress “ with the Pin name and the Number of times. For each Number of times:
If ServoMode is false, turn the Pin LOW and pause for 0.2 second, then turn the pin HIGH and pause for 0.3 seconds.
If ServoMode is true, move the servo motor to 90 degree, pause for 0.2 second, then move back to 0 degree and pause for 0.3 seconds.

The LongPress process.
The Long Press process  takes two inputs: Pin and Number of Times (defaults to 1).
Logs the current RemainingMin in hh:mm format, followed by “LongPress “ with the Pin name and the Number of Times. For each  Number of Times:
If ServoMode is false, turn the Pin LOW and pause for 2 seconds, then turn the pin HIGH and pause for 1 second.
If ServoMode is true, move the servo motor to 90 degree, pause for 2 seconds, then move back to 0 degree and pause for 1 second.

The Knead Process
Set cycleMin to 30.
Divide KneadMin by cycleMin and store the quotient as NoOfLoop, store  the remainder as FinalMin.
Logs the CycleMin, NoOfLoop, FinalMin as debug messages with the RemainingMin as header.
For each NoOfLoop, Logs the LoopCounter and CycleMin as debug messages with the RemainingMin as header, longPresss the RunResetPin,  then  ShortPresss the MenuPin 7 times,  ShortPresss the MinusPin 10 times. then shortPresss the RunResetPin, pause for cycleMin minutes
If FinalMin > 0, Logs the FinalMin  as debug messages with the RemainingMin as header,  longPresss the RunResetPin,  then  ShortPresss the MenuPin  7 times, then shortPresss the MinusPin 10 times, then shortPresss the RunResetPin, pause for FinalMin minutes


The Degas Process
Set cycleMin to 30.
Divide DegasMin by cycleMin and store the quotient as NoOfLoop, store  the remainder as FinalMin.
For each NoOfLoop, Logs the LoopCounter and CycleMin as debug messages with the RemainingMin as header, longPresss the RunResetPin,  then  ShortPresss the MenuPin  7 times. then shortPresss the MinusPin 9 times, then shortPresss the RunResetPin , pause for cycleMin  minutes
If FinalMin > 0, Logs the FinalMin  as debug messages with the RemainingMin as header,  longPresss the RunResetPin,  then  ShortPresss the MenuPin  7 times, then shortPresss the MinusPin 9 times, then shortPresss the RunResetPin, pause for FinalMin minutes

 
The Proof Process
LongPresss the RunResetPin.
Set cycleMin to 60.
Set NoOfLoop to 0, FinalMin=ProofHr * 60.
Logs the CycleMin, NoOfLoop, FinalMin as debug messages with the RemainingMin as header.
Pause for FinalMin minutes.


The HotProof Process
Set cycleMin to 60.
Divide HotProofMin by cycleMin  and store the quotient as NoOfLoop, store  the remainder as FinalMin.
Logs the CycleMin, NoOfLoop, FinalMin as debug messages with the RemainingMin as header.
For each NoOfLoop, Logs the LoopCounter and CycleMin as debug messages with the RemainingMin as header, longPresss the RunResetPin,  then call the BakeColour process, then  ShortPresss the MenuPin  9 times. then shortPresss the MinusPin 10 times, then shortPresss the RunResetPin 1 time, pause for CycleMin
If FinalMin > 0, Logs the FinalMin  as debug messages with the RemainingMin as header,  longPresss the RunResetPin,  then call the BakeColour process, then  ShortPresss the MenuPin  9 times. then shortPresss the MinusPin 10 times, then shortPresss the RunResetPin, pause for FinalMin minutes


The Bake Process
Set cycleMin to 60.
Divide BakeMin by cycleMin  and store the quotient as NoOfLoop, store  the remainder as FinalMin.
Logs the CycleMin, NoOfLoop, FinalMin as debug messages with the RemainingMin as header.
For each NoOfLoop, Logs the LoopCounter and CycleMin as debug messages with the RemainingMin as header, longPresss the RunResetPin,  then call the BakeColour process, then  ShortPresss the MenuPin  13 times. then shortPresss the MinusPin 10 times, then shortPresss the RunResetPin 1 time, pause for CycleMin minutes
If FinalMin > 0, Logs the FinalMin  as debug messages with the RemainingMin as header,  longPresss the RunResetPin,  then call the BakeColour process, then  ShortPresss the MenuPin  13 times. then shortPresss the MinusPin 10 times, then shortPresss the RunResetPin, pause for FinalMin minutes

At the end of the Bake process, longPresss the RunResetPin. Display “Sourdough Done” on the status line. Remove the Run button  and Pause buttons. Keep the Restart button.
When the Restart button is pressed, reboot and re-initiate from the very beginning.  

The BakeColour process.
Log the BakeColour process and the BakeColour Option with the  remainingMin as header.
If BakeColour == Dark ShortPress the ColourPin 1 time.
If BakeColour == Light  ShortPress the ColourPin 2 times.

Debugging messages:
logs the following debug messages to the serial monitor with the RemainingMin as the header:
At the start of the menu-selection process.
When the run button, the pause  button and the restart buttons are pressed.
The chosen options for the parameters at the start of the Sourdough process.
When starting and completing  each of the knead, degas,  proof and bake process.
Logs the Remaining NoOfLoop and the  finalMin of the above processes.
The ShortPress and LongPress and  the name of the Pin, with the RemainingMin as the header.

Sets up a constant variable called DebugLevel defaults to 2.
If DebugLevel == 0 do not log any debug messages.
If DebugLevel == 2, log all debug messages. If DebugLevel ==1 log all debug messages except that from the shortPress and longPress processes. 



As the timing for the complete sourdough process takes a long time, to make testing easier, set a flag called TestRun, default to false. When testRun is true, condense the timing of 1 minute to 1 second during the Sourdough process, the Knead process, the degas process, the proof process and the bake process  but do not impact the menu-selection process.
When testRun is false, it assumes the normal timing of 1 minute=1 minute.

