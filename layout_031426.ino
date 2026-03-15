// Define pins
const int outputPin = 9;          // PWM output pin (must be a PWM-capable pin)
const int switchPin = 2;          // Main cycle start button (active LOW via pull-up)
const int shutdownPin = 3;        // Emergency shutdown switch (active LOW via pull-up)
const int testModePin = 8;        // Test mode switch (active LOW via pull-up) - runs cycle with relay 2 disabled
const int holdTimePin = 11;       // Hold time selector switch (active LOW via pull-up) - LOW = 30 sec, HIGH = 2 min

// Relay pins (4 relays, turned on/off sequentially)
const int relayPin[4] = {4, 5, 6, 7};
const unsigned long relayStaggerDelay = 250;  // 250ms between each relay turning on/off

// DC output parameters
int outputLevel = 0;              // Current PWM output level (0-255)
const int maxOutputLevel = 255;   // Maximum PWM output (100%)

// Timing variables
// stateStartTime is reused for ramp timing (RAMP_UP/RAMP_DOWN) and hold countdown (HOLD)
unsigned long stateStartTime = 0;

// Ramp and hold parameters
const unsigned long rampUpTime = 5000;         // Time to ramp from 0 to 100% (5 seconds)
const unsigned long holdTimeLong  = 120000;    // Hold duration in normal mode (2 minutes)
const unsigned long holdTimeShort =  30000;    // Hold duration in short mode (30 seconds)
unsigned long holdTime = holdTimeLong;         // Active hold duration - set at cycle start based on switch
const unsigned long rampDownTime = 5000;       // Time to ramp from 100% to 0 (5 seconds)

// Update interval for smooth PWM ramping
const unsigned long updateInterval = 50;  // Update output every 50ms
unsigned long lastUpdateTime = 0;         // Timestamp of last output update

// Main button debounce
const unsigned long debounceDelay = 20;   // 20ms debounce window
unsigned long lastDebounceTime = 0;       // Timestamp of last raw state change
bool rawSwitchState = HIGH;               // Most recent unfiltered pin reading

// Main button lockout (prevents immediate restart after a completed cycle)
const unsigned long buttonLockoutTime = 5000;  // Lockout duration after cycle ends (5 seconds)
unsigned long cycleCompleteTime = 0;           // Timestamp when last cycle completed
bool buttonLockout = false;                    // True while main button is locked out

// Shutdown switch debounce
const unsigned long shutdownDebounceDelay = 20;  // 20ms debounce window
unsigned long lastShutdownDebounceTime = 0;      // Timestamp of last raw shutdown state change
bool rawShutdownState = HIGH;                    // Most recent unfiltered shutdown pin reading
bool shutdownSwitchState = HIGH;                 // Stable debounced shutdown switch state
bool previousShutdownSwitchState = HIGH;         // Previous stable state (for edge detection)

// Shutdown / reactivation logic
bool shutdownActive = false;                       // True while system is in shutdown
unsigned long shutdownTime = 0;                    // Timestamp when shutdown was triggered
const unsigned long shutdownLockoutTime = 15000;   // System stays locked after shutdown (15 seconds)
const unsigned long reactivateHoldTime = 5000;     // Must hold shutdown switch to reactivate (5 seconds)
unsigned long reactivateHoldStart = 0;             // Timestamp when reactivation hold began
bool waitingForReactivate = false;                 // True once lockout expires, waiting for hold

// Test mode
bool testMode = false;                             // True when test mode switch is active (relay 2 disabled)
bool previousTestMode = false;                     // Previous test mode state (for edge detection)

// Relay sequencing state
int relaysEnabled = 0;               // Number of relays currently ON (0-4)
unsigned long relaySequenceStart = 0; // Timestamp when current relay sequence started

// System state machine
enum SystemState {
  STATE_OFF,        // System idle, output off, all relays off
  STATE_RELAY_ON,   // Staging relays on one at a time before ramp up
  STATE_RAMP_UP,    // PWM ramping from 0 to 100%
  STATE_HOLD,       // Holding at 100% for holdTime duration
  STATE_RAMP_DOWN,  // PWM ramping from 100% to 0%
  STATE_RELAY_OFF   // Staging relays off one at a time after ramp down
};

SystemState currentState = STATE_OFF;
bool switchState = HIGH;          // Stable debounced state of main button
bool previousSwitchState = HIGH;  // Previous stable state (for edge detection)

// -------------------------------------------------------
// Forward declarations
// -------------------------------------------------------
void immediateShutdown();
void allRelaysOff();
void startRelayOn();
void startRelayOff();
void startRampUp();
void startRampUpFromCurrentLevel();
void startHold();
void startRampDown();
void startRampDownFromCurrentLevel();
void resetHoldTimer();
void printPercent(int pwmValue);
void updateOutput();

// -------------------------------------------------------
void setup() {
  Serial.begin(9600);
  
  pinMode(outputPin, OUTPUT);
  analogWrite(outputPin, 0);  // Ensure PWM starts at 0

  // Initialize all relay pins as outputs, default OFF
  // Relay modules are active-LOW: HIGH = de-energized (off), LOW = energized (on)
  for (int i = 0; i < 4; i++) {
    pinMode(relayPin[i], OUTPUT);
    digitalWrite(relayPin[i], HIGH);  // HIGH = relay OFF for active-LOW modules
  }
  
  // Input pins use internal pull-up; switches read LOW when pressed
  pinMode(switchPin, INPUT_PULLUP);
  pinMode(shutdownPin, INPUT_PULLUP);
  pinMode(testModePin, INPUT_PULLUP);
  pinMode(holdTimePin, INPUT_PULLUP);

  // Read test mode switch at startup
  testMode = (digitalRead(testModePin) == LOW);

  // Read hold time switch at startup and set holdTime accordingly
  holdTime = (digitalRead(holdTimePin) == LOW) ? holdTimeShort : holdTimeLong;

  Serial.println("DC Power Supply Controller");
  Serial.println("Press start button to begin cycle");
  Serial.println("Shutdown switch: immediately cuts PWM and all relays");
  Serial.println("  -> 15s lockout, then hold shutdown switch 5s to reactivate");
  if (testMode) {
    Serial.println("*** TEST MODE ACTIVE - Relay 2 disabled ***");
  }
  Serial.print("Hold time: ");
  Serial.print(holdTime / 1000);
  Serial.println("s");
}

// -------------------------------------------------------
void loop() {
  unsigned long currentTime = millis();

  // Read test mode switch (active LOW) and handle edges
  testMode = (digitalRead(testModePin) == LOW);

  if (testMode && !previousTestMode) {
    // Rising edge: test switch just turned ON - start cycle immediately if idle
    if (currentState == STATE_OFF && !shutdownActive) {
      buttonLockout = false;  // Override any lockout so test mode starts without delay
      startRelayOn();
      Serial.println("*** TEST MODE ON - Ramping to 100% and holding (relay 2 disabled) ***");
    } else if (currentState != STATE_OFF) {
      // Already running a normal cycle - just note that test mode hold is now active
      Serial.println("*** TEST MODE ON - Will hold at 100% until switch is turned off ***");
    }
  } else if (!testMode && previousTestMode) {
    // Falling edge: test switch turned OFF - begin graceful ramp down from wherever we are
    if (currentState == STATE_RELAY_ON || currentState == STATE_RAMP_UP || currentState == STATE_HOLD) {
      // Still on the way up or holding - ramp down from current level
      startRampDownFromCurrentLevel();
      Serial.println("*** TEST MODE OFF - Ramping down ***");
    }
    // If already in STATE_RAMP_DOWN or STATE_RELAY_OFF, let the cycle finish naturally
  }
  previousTestMode = testMode;

  // === Debounce main button ===
  // Reset timer on any raw change; only update stable state after debounceDelay
  bool reading = digitalRead(switchPin);
  if (reading != rawSwitchState) {
    rawSwitchState = reading;
    lastDebounceTime = currentTime;
  }
  if (currentTime - lastDebounceTime >= debounceDelay) {
    switchState = rawSwitchState;
  }

  // === Debounce shutdown switch ===
  bool shutdownReading = digitalRead(shutdownPin);
  if (shutdownReading != rawShutdownState) {
    rawShutdownState = shutdownReading;
    lastShutdownDebounceTime = currentTime;
  }
  if (currentTime - lastShutdownDebounceTime >= shutdownDebounceDelay) {
    shutdownSwitchState = rawShutdownState;
  }

  // === Shutdown switch logic ===
  if (!shutdownActive) {
    // Trigger shutdown on falling edge (switch pressed while system is running)
    if (shutdownSwitchState == LOW && previousShutdownSwitchState == HIGH) {
      immediateShutdown();
    }
  } else {
    // --- System is in shutdown ---
    bool lockoutExpired = (currentTime - shutdownTime >= shutdownLockoutTime);

    if (!lockoutExpired) {
      // Still within 15s lockout window - print countdown every second
      static unsigned long lastLockoutReport = 0;
      if (currentTime - lastLockoutReport > 1000) {
        unsigned long remaining = shutdownLockoutTime - (currentTime - shutdownTime);
        Serial.print("Shutdown lockout: ");
        Serial.print(remaining / 1000);
        Serial.println("s remaining");
        lastLockoutReport = currentTime;
      }
      // Cancel any reactivation hold attempt that started during lockout
      waitingForReactivate = false;
      reactivateHoldStart = 0;

    } else {
      // Lockout expired - waiting for 5-second hold on shutdown switch to reactivate
      if (!waitingForReactivate) {
        Serial.println("Lockout expired - Hold shutdown switch 5 seconds to reactivate");
        waitingForReactivate = true;
      }

      if (shutdownSwitchState == LOW) {
        // Switch is being held down
        if (reactivateHoldStart == 0) {
          // First detection of hold - record start time
          reactivateHoldStart = currentTime;
          Serial.println("Holding for reactivation...");
        } else if (currentTime - reactivateHoldStart >= reactivateHoldTime) {
          // Held for full 5 seconds - reactivate system
          shutdownActive = false;
          waitingForReactivate = false;
          reactivateHoldStart = 0;
          Serial.println("System REACTIVATED - Press start button to begin cycle");
        } else {
          // Still holding - print countdown every second
          static unsigned long lastHoldReport = 0;
          if (currentTime - lastHoldReport > 1000) {
            unsigned long held = currentTime - reactivateHoldStart;
            unsigned long remaining = reactivateHoldTime - held;
            Serial.print("Hold to reactivate: ");
            Serial.print(remaining / 1000 + 1);
            Serial.println("s remaining");
            lastHoldReport = currentTime;
          }
        }
      } else {
        // Switch released before 5 seconds elapsed - reset hold timer
        if (reactivateHoldStart != 0) {
          Serial.println("Hold released early - hold for 5 full seconds to reactivate");
          reactivateHoldStart = 0;
        }
      }
    }
  }
  previousShutdownSwitchState = shutdownSwitchState;

  // === Main button and output logic (bypassed while shutdown is active) ===
  if (!shutdownActive) {
    // Release main button lockout once buttonLockoutTime has elapsed
    if (buttonLockout && (currentTime - cycleCompleteTime >= buttonLockoutTime)) {
      buttonLockout = false;
      Serial.println("Button lockout released - Ready for next cycle");
    }

    // Detect falling edge on main button (LOW = pressed)
    if (switchState != previousSwitchState) {
      if (switchState == LOW) {
        if (currentState == STATE_OFF && !buttonLockout) {
          // Begin relay-on sequence to start a new cycle
          startRelayOn();
        } else if (currentState == STATE_HOLD) {
          // Reset the 30-second hold countdown
          resetHoldTimer();
        } else if (currentState == STATE_RAMP_DOWN) {
          // Button pressed during ramp down - reverse direction and ramp back up
          // from current PWM level, then reset the hold timer
          startRampUpFromCurrentLevel();
        } else if (buttonLockout) {
          // Inform user how long until button is available
          unsigned long remainingTime = buttonLockoutTime - (currentTime - cycleCompleteTime);
          Serial.print("Button locked for ");
          Serial.print(remainingTime / 1000);
          Serial.println(" more seconds");
        }
        // Button presses during STATE_RELAY_ON, STATE_RAMP_UP,
        // and STATE_RELAY_OFF are intentionally ignored
      }
      previousSwitchState = switchState;
    }

    updateOutput();
    analogWrite(outputPin, outputLevel);
  }
}

// -------------------------------------------------------
// immediateShutdown: instantly cuts PWM and all relays,
// then locks the system for shutdownLockoutTime (15 seconds)
// -------------------------------------------------------
void immediateShutdown() {
  outputLevel = 0;
  analogWrite(outputPin, 0);  // Cut PWM output immediately

  allRelaysOff();             // Cut all relays immediately (no staged sequence)

  // Reset state machine and sequencing variables
  currentState = STATE_OFF;
  relaysEnabled = 0;
  buttonLockout = false;

  // Arm shutdown lockout
  shutdownActive = true;
  shutdownTime = millis();
  waitingForReactivate = false;
  reactivateHoldStart = 0;

  Serial.println("!!! SHUTDOWN TRIGGERED !!!");
  Serial.println("PWM off - all relays off");
  Serial.println("System locked for 15 seconds");
}

// allRelaysOff: turns off all 4 relays simultaneously (used by emergency shutdown)
void allRelaysOff() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(relayPin[i], HIGH);  // HIGH = relay OFF for active-LOW modules
  }
  Serial.println("All relays OFF");
}

// -------------------------------------------------------
// updateOutput: called every loop when not in shutdown.
// Throttled to updateInterval (50ms) for smooth PWM ramping.
// -------------------------------------------------------
void updateOutput() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastUpdateTime < updateInterval) {
    return;
  }
  lastUpdateTime = currentTime;
  
  static unsigned long lastDebugTime = 0;
  bool debugOutput = (currentTime - lastDebugTime > 500);  // Print status every 500ms
  
  switch (currentState) {

    case STATE_OFF:
      // System idle - ensure output stays at zero
      outputLevel = 0;
      break;

    case STATE_RELAY_ON:
      // Turn on relays one at a time, 250ms apart (relay 1 -> 2 -> 3 -> 4)
      // Once all 4 are on, transition to STATE_RAMP_UP
      {
        int targetRelays = (int)((currentTime - relaySequenceStart) / relayStaggerDelay) + 1;
        targetRelays = min(targetRelays, 4);
        while (relaysEnabled < targetRelays) {
          if (relaysEnabled == 1 && testMode) {
            // Test mode: skip relay 2 (index 1), leave it de-energized
            Serial.println("Relay 2 SKIPPED (test mode)");
          } else {
            digitalWrite(relayPin[relaysEnabled], LOW);   // LOW = relay ON for active-LOW modules
            Serial.print("Relay ");
            Serial.print(relaysEnabled + 1);
            Serial.println(" ON");
          }
          relaysEnabled++;
        }
        if (relaysEnabled >= 4) {
          if (testMode) {
            Serial.println("Relays ON (relay 2 skipped - test mode) - Starting RAMP UP sequence");
          } else {
            Serial.println("All relays ON - Starting RAMP UP sequence");
          }
          startRampUp();
        }
      }
      break;
      
    case STATE_RAMP_UP:
      // Linearly ramp PWM from 0 to maxOutputLevel over rampUpTime
      if (currentTime - stateStartTime <= rampUpTime) {
        float percentComplete = (float)(currentTime - stateStartTime) / rampUpTime;
        outputLevel = (int)(percentComplete * maxOutputLevel);
        if (debugOutput) {
          Serial.print("Ramping UP: ");
          printPercent(outputLevel);
          lastDebugTime = currentTime;
        }
      } else {
        startHold();  // Ramp complete - transition to hold
      }
      break;
      
    case STATE_HOLD:
      // Hold at 100% output for holdTime (30 sec or 2 min per switch), or indefinitely in test mode
      outputLevel = maxOutputLevel;
      if (debugOutput) {
        if (testMode) {
          Serial.println("HOLDING at 100% - TEST MODE (holding until switch off)");
        } else {
          unsigned long elapsed = currentTime - stateStartTime;
          unsigned long remaining = holdTime - elapsed;
          Serial.print("HOLDING at 100% - ");
          Serial.print(remaining / 1000);
          Serial.print("s remaining (press button to reset ");
          Serial.print(holdTime / 1000);
          Serial.println("s timer)");
        }
        lastDebugTime = currentTime;
      }
      if (!testMode && currentTime - stateStartTime >= holdTime) {
        startRampDown();  // Hold complete - begin ramp down (suppressed in test mode)
      }
      break;
      
    case STATE_RAMP_DOWN:
      // Linearly ramp PWM from maxOutputLevel to 0 over rampDownTime
      if (currentTime - stateStartTime <= rampDownTime) {
        float percentComplete = (float)(currentTime - stateStartTime) / rampDownTime;
        outputLevel = (int)(maxOutputLevel * (1.0 - percentComplete));
        if (debugOutput) {
          Serial.print("Ramping DOWN: ");
          printPercent(outputLevel);
          lastDebugTime = currentTime;
        }
      } else {
        outputLevel = 0;  // Ensure output is fully zeroed before relays turn off
        startRelayOff();
      }
      break;

    case STATE_RELAY_OFF:
      // Turn off relays one at a time, 250ms apart in reverse order (relay 4 -> 3 -> 2 -> 1)
      // Once all are off, transition to STATE_OFF and start button lockout
      {
        int targetRelays = 4 - (int)((currentTime - relaySequenceStart) / relayStaggerDelay) - 1;
        targetRelays = max(targetRelays, 0);
        while (relaysEnabled > targetRelays) {
          relaysEnabled--;
          if (relaysEnabled == 1 && testMode) {
            Serial.println("Relay 2 SKIPPED (test mode)");
          } else {
            digitalWrite(relayPin[relaysEnabled], HIGH);  // HIGH = relay OFF for active-LOW modules
            Serial.print("Relay ");
            Serial.print(relaysEnabled + 1);
            Serial.println(" OFF");
          }
        }
        if (relaysEnabled <= 0) {
          Serial.println("All relays OFF - Cycle complete");
          currentState = STATE_OFF;
          outputLevel = 0;
          cycleCompleteTime = millis();
          buttonLockout = true;  // Lock button for 5 seconds after cycle ends
          Serial.println("System OFF - Button locked for 5 seconds");
        }
      }
      break;
  }
}

// -------------------------------------------------------
void printPercent(int pwmValue) {
  // Convert 0-255 PWM value to 0-100% and print to serial
  int percent = (pwmValue * 100) / 255;
  Serial.print(percent);
  Serial.println("%");
}

// startRelayOn: begin staged relay-on sequence (transitions to STATE_RELAY_ON)
// Also latches the hold time from the selector switch so it can't change mid-cycle
void startRelayOn() {
  holdTime = (digitalRead(holdTimePin) == LOW) ? holdTimeShort : holdTimeLong;
  currentState = STATE_RELAY_ON;
  relaysEnabled = 0;
  relaySequenceStart = millis();
  Serial.print("Starting relay ON sequence (250ms per relay) - Hold time: ");
  Serial.print(holdTime / 1000);
  Serial.println("s");
}

// startRelayOff: begin staged relay-off sequence in reverse order (transitions to STATE_RELAY_OFF)
void startRelayOff() {
  currentState = STATE_RELAY_OFF;
  relaysEnabled = 4;
  relaySequenceStart = millis();
  Serial.println("Starting relay OFF sequence (250ms per relay)...");
}

// startRampUp: begin PWM ramp from 0 to 100% over rampUpTime (transitions to STATE_RAMP_UP)
void startRampUp() {
  currentState = STATE_RAMP_UP;
  stateStartTime = millis();
  Serial.println("Starting RAMP UP sequence (5 seconds)");
}

// startRampUpFromCurrentLevel: resume ramp up from the current PWM level mid-ramp-down.
// Adjusts stateStartTime backward so the ramp calculation starts at the correct position.
void startRampUpFromCurrentLevel() {
  // Calculate how far into a ramp-up we would be at the current output level
  float currentPercent = (float)outputLevel / maxOutputLevel;
  unsigned long elapsedEquivalent = (unsigned long)(currentPercent * rampUpTime);

  currentState = STATE_RAMP_UP;
  stateStartTime = millis() - elapsedEquivalent;  // Back-date start so ramp resumes smoothly
  Serial.print("Button pressed during ramp down - Reversing to RAMP UP from ");
  int percent = (outputLevel * 100) / 255;
  Serial.print(percent);
  Serial.println("%");
  Serial.println("Hold timer will reset when 100% is reached");
}

// startHold: hold PWM at 100% for holdTime (transitions to STATE_HOLD)
void startHold() {
  currentState = STATE_HOLD;
  stateStartTime = millis();
  outputLevel = maxOutputLevel;  // Ensure output is at full before entering hold
  Serial.print("At maximum output (100%) - HOLDING for ");
  Serial.print(holdTime / 1000);
  Serial.println("s");
  Serial.print("Press start button during hold to reset ");
  Serial.print(holdTime / 1000);
  Serial.println("s timer");
}

// startRampDown: begin PWM ramp from 100% to 0 over rampDownTime (transitions to STATE_RAMP_DOWN)
void startRampDown() {
  currentState = STATE_RAMP_DOWN;
  stateStartTime = millis();
  Serial.println("Starting RAMP DOWN sequence (5 seconds)");
}

// startRampDownFromCurrentLevel: begin ramp down from current PWM level (used when test mode switch turns off).
// Back-dates stateStartTime so the ramp calculation starts at the correct position.
void startRampDownFromCurrentLevel() {
  float currentPercent = (float)outputLevel / maxOutputLevel;
  unsigned long elapsedEquivalent = (unsigned long)((1.0 - currentPercent) * rampDownTime);
  currentState = STATE_RAMP_DOWN;
  stateStartTime = millis() - elapsedEquivalent;
  Serial.print("Ramping DOWN from ");
  printPercent(outputLevel);
}

// resetHoldTimer: resets the hold countdown back to the active hold duration
void resetHoldTimer() {
  stateStartTime = millis();
  Serial.print("Hold timer RESET - ");
  Serial.print(holdTime / 1000);
  Serial.println("s remaining");
}
