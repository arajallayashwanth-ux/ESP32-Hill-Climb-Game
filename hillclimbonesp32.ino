
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <esp_system.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

#define BUTTON_A 25
#define BUTTON_B 26

Adafruit_SSD1306 display(
  SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET
);

enum GameState { TITLE_SCREEN, PLAYING, LEVEL_COMPLETE, GAME_OVER };
GameState gameState = TITLE_SCREEN;

float worldX = 0;
float speed = 0;
float jeepY = 0;
float verticalSpeed = 0;
float fuel = 100;

int coins = 0;
int distanceM = 0;
int level = 1;
int levelReward = 0;

bool grounded = true;
bool rainEnabled = false;
bool previousA = false;
bool previousB = false;

unsigned long lastFrame = 0;
unsigned long stateTimer = 0;
unsigned long lastCoinWorld = 0;
unsigned long lastRewardedLevel = 1;

const int JEEP_X = 27;
const int LEVEL_DISTANCE = 500;
const int OBSTACLE_SPACING = 180;

struct RainDrop {
  int x, y, speed;
};
RainDrop rain[18];

// ---------- GENERAL HELPERS ----------

bool pressed(int pin) {
  return digitalRead(pin) == LOW;
}

unsigned long hashValue(unsigned long n) {
  n = (n ^ 61UL) ^ (n >> 16);
  n *= 9UL;
  n ^= n >> 4;
  n *= 0x27d4eb2dUL;
  n ^= n >> 15;
  return n;
}

int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// ---------- ENVIRONMENTS ----------
// Levels 1-5: green-hill style
// Levels 6-10: rocky mountains
// Levels 11-20: night
// Levels 21+: storm

int environment() {
  if (level <= 5) return 0;
  if (level <= 10) return 1;
  if (level <= 20) return 2;
  return 3;
}

void drawBackground() {
  int env = environment();

  if (env == 0) {
    // Distant rolling hills.
    for (int x = 0; x < 128; x += 3) {
      int y = 28 + (int)(sinf((x + worldX * 0.15f) * 0.04f) * 5);
      display.drawPixel(x, y, SSD1306_WHITE);
    }
  } else if (env == 1) {
    // Jagged mountain silhouettes.
    for (int x = 0; x < 128; x += 16) {
      int y = 28 + (int)(hashValue(x / 16 + level * 13) % 12);
      display.drawLine(x, 40, x + 8, y, SSD1306_WHITE);
      display.drawLine(x + 8, y, x + 16, 40, SSD1306_WHITE);
    }
  } else if (env == 2) {
    // Moon and stars.
    display.drawCircle(105, 25, 8, SSD1306_WHITE);
    display.drawPixel(102, 22, SSD1306_BLACK);
    display.drawPixel(108, 28, SSD1306_BLACK);

    for (int i = 0; i < 18; i++) {
      unsigned long h = hashValue(i * 31 + level * 7);
      int x = h % 128;
      int y = 20 + ((h >> 8) % 17);
      display.drawPixel(x, y, SSD1306_WHITE);
    }
  } else {
    // Storm clouds and lightning.
    display.drawLine(7, 24, 25, 24, SSD1306_WHITE);
    display.drawLine(14, 20, 20, 20, SSD1306_WHITE);
    display.drawLine(76, 25, 98, 25, SSD1306_WHITE);
    display.drawLine(83, 21, 93, 21, SSD1306_WHITE);

    if ((millis() / 700) % 7 == 0) {
      display.drawLine(70, 27, 65, 34, SSD1306_WHITE);
      display.drawLine(65, 34, 71, 34, SSD1306_WHITE);
      display.drawLine(71, 34, 67, 40, SSD1306_WHITE);
    }
  }
}

const char* environmentName() {
  switch (environment()) {
    case 0: return "HILLS";
    case 1: return "ROCKS";
    case 2: return "NIGHT";
    default: return "STORM";
  }
}

// ---------- TERRAIN ----------

int terrainY(float x) {
  float h = sinf(x * 0.035f) * 4.0f
          + sinf(x * 0.012f) * 5.0f
          + sinf(x * 0.075f) * 1.5f;

  int env = environment();

  if (env == 1) {
    h += sinf(x * 0.09f) * 3.0f;
    h += sinf(x * 0.025f) * 2.0f;
  } else if (env == 2) {
    h += sinf(x * 0.055f) * 3.0f;
  } else if (env == 3) {
    h += sinf(x * 0.10f) * 3.5f;
    h += sinf(x * 0.022f) * 3.0f;
  }

  int y = 49 - (int)h;
  return clampInt(y, 35, 57);
}

void drawTerrain() {
  int previousY = terrainY(worldX);

  for (int x = 1; x < SCREEN_WIDTH; x++) {
    int y = terrainY(worldX + x);
    display.drawLine(x - 1, previousY, x, y, SSD1306_WHITE);
    previousY = y;
  }
}

// ---------- JEEP ----------

void drawWheel(int x, int y, int frame) {
  display.drawCircle(x, y, 4, SSD1306_WHITE);

  switch (frame % 4) {
    case 0:
      display.drawLine(x - 2, y, x + 2, y, SSD1306_WHITE);
      break;
    case 1:
      display.drawLine(x, y - 2, x, y + 2, SSD1306_WHITE);
      break;
    case 2:
      display.drawLine(x - 2, y - 2, x + 2, y + 2,
                       SSD1306_WHITE);
      break;
    default:
      display.drawLine(x - 2, y + 2, x + 2, y - 2,
                       SSD1306_WHITE);
  }
}

void drawJeep() {
  int bodyY = (int)jeepY;

  display.drawRoundRect(
    JEEP_X, bodyY, 27, 9, 2, SSD1306_WHITE
  );

  // Cabin and windshield.
  display.drawLine(JEEP_X + 5, bodyY, JEEP_X + 9,
                   bodyY - 6, SSD1306_WHITE);
  display.drawLine(JEEP_X + 9, bodyY - 6, JEEP_X + 19,
                   bodyY - 6, SSD1306_WHITE);
  display.drawLine(JEEP_X + 19, bodyY - 6, JEEP_X + 23,
                   bodyY, SSD1306_WHITE);

  // Driver.
  display.drawCircle(JEEP_X + 14, bodyY - 3, 2, SSD1306_WHITE);

  // Bumper.
  display.drawLine(JEEP_X + 23, bodyY + 5, JEEP_X + 29,
                   bodyY + 5, SSD1306_WHITE);

  int wheelY = bodyY + 10;
  int frame = (int)(worldX / 3);

  drawWheel(JEEP_X + 6, wheelY, frame);
  drawWheel(JEEP_X + 22, wheelY, frame);
}

// ---------- COINS ----------

long coinPosition(int segment) {
  unsigned long h = hashValue((unsigned long)segment + 7919UL);
  return (long)segment * 150L + 60L + (h % 60UL);
}

void drawCoins() {
  int segment = (int)(worldX / 150.0f);

  for (int i = segment - 1; i <= segment + 2; i++) {
    long pos = coinPosition(i);
    int sx = (int)(pos - worldX);

    if (sx < -5 || sx > 132) continue;

    int cy = terrainY(pos) - 15;
    display.drawCircle(sx, cy, 3, SSD1306_WHITE);
    display.drawPixel(sx, cy, SSD1306_WHITE);
  }
}

void collectCoins() {
  int segment = (int)(worldX / 150.0f);

  for (int i = segment - 1; i <= segment + 2; i++) {
    long pos = coinPosition(i);

    if (pos >= (long)worldX + JEEP_X - 4 &&
        pos <= (long)worldX + JEEP_X + 4 &&
        pos > (long)lastCoinWorld) {
      coins++;
      lastCoinWorld = pos;
    }
  }
}

// ---------- OBSTACLES ----------
// Obstacle types: rock, log, spike and large boulder.
// Extra obstacles become available as levels increase.

long obstaclePosition(int segment, int which) {
  unsigned long h = hashValue(
    (unsigned long)segment * 97UL + which * 313UL + 1777UL
  );

  return (long)segment * OBSTACLE_SPACING
       + 65L + (h % 40UL) + which * 55L;
}

int obstacleType(int segment, int which) {
  unsigned long h = hashValue(
    (unsigned long)segment * 41UL + which * 887UL + 919UL
  );

  int maxType = 1;

  if (level >= 3) maxType = 2;
  if (level >= 6) maxType = 3;
  if (level >= 11) maxType = 4;

  return h % maxType;
}

bool obstacleExists(int segment, int which) {
  if (which == 0) return true;

  unsigned long h = hashValue(
    (unsigned long)segment * 101UL + which * 431UL + 37UL
  );

  // More secondary obstacles at higher levels.
  int chance = 0;

  if (level >= 3) chance = 25;
  if (level >= 6) chance = 50;
  if (level >= 11) chance = 70;
  if (level >= 21) chance = 90;

  return (h % 100UL) < (unsigned long)chance;
}

void drawOneObstacle(long pos, int type) {
  int sx = (int)(pos - worldX);

  if (sx < -10 || sx > 138) return;

  int ground = terrainY(pos);

  if (type == 0) {
    // Rock.
    display.drawLine(sx - 5, ground, sx, ground - 7,
                     SSD1306_WHITE);
    display.drawLine(sx, ground - 7, sx + 5, ground,
                     SSD1306_WHITE);
    display.drawLine(sx - 5, ground, sx + 5, ground,
                     SSD1306_WHITE);
  } else if (type == 1) {
    // Wooden log.
    display.drawRect(sx - 5, ground - 5, 11, 5,
                     SSD1306_WHITE);
    display.drawLine(sx - 4, ground - 4, sx + 4,
                     ground - 1, SSD1306_WHITE);
  } else if (type == 2) {
    // Spikes.
    display.drawLine(sx - 5, ground, sx - 2,
                     ground - 7, SSD1306_WHITE);
    display.drawLine(sx - 2, ground - 7, sx + 1,
                     ground, SSD1306_WHITE);
    display.drawLine(sx + 1, ground, sx + 4,
                     ground - 7, SSD1306_WHITE);
    display.drawLine(sx + 4, ground - 7, sx + 7,
                     ground, SSD1306_WHITE);
  } else {
    // Large boulder.
    display.drawCircle(sx, ground - 4, 5, SSD1306_WHITE);
    display.drawPixel(sx - 1, ground - 5, SSD1306_WHITE);
  }
}

void drawObstacles() {
  int segment = (int)(worldX / OBSTACLE_SPACING);

  for (int i = segment - 1; i <= segment + 2; i++) {
    for (int which = 0; which < 3; which++) {
      if (!obstacleExists(i, which)) continue;

      long pos = obstaclePosition(i, which);
      drawOneObstacle(pos, obstacleType(i, which));
    }
  }
}

bool hitObstacle() {
  int segment = (int)(worldX / OBSTACLE_SPACING);

  for (int i = segment - 1; i <= segment + 2; i++) {
    for (int which = 0; which < 3; which++) {
      if (!obstacleExists(i, which)) continue;

      long pos = obstaclePosition(i, which);
      float sx = pos - worldX;

      if (sx >= JEEP_X + 2 && sx <= JEEP_X + 25) {
        // Ground contact is required for a crash.
        if (grounded) return true;
      }
    }
  }

  return false;
}

// ---------- RAIN ----------

void initializeRain() {
  for (int i = 0; i < 18; i++) {
    rain[i].x = esp_random() % SCREEN_WIDTH;
    rain[i].y = esp_random() % SCREEN_HEIGHT;
    rain[i].speed = 2 + esp_random() % 3;
  }
}

void updateRain() {
  rainEnabled = environment() == 3;

  if (!rainEnabled) return;

  for (int i = 0; i < 18; i++) {
    rain[i].x--;
    rain[i].y += rain[i].speed;

    if (rain[i].y >= SCREEN_HEIGHT) {
      rain[i].y = 0;
      rain[i].x = esp_random() % SCREEN_WIDTH;
    }

    if (rain[i].x < 0) rain[i].x = 127;
  }
}

void drawRain() {
  if (!rainEnabled) return;

  for (int i = 0; i < 18; i++) {
    display.drawLine(
      rain[i].x, rain[i].y,
      rain[i].x - 1, rain[i].y + 2,
      SSD1306_WHITE
    );
  }
}

// ---------- HUD ----------

void drawHUD() {
  display.fillRect(0, 0, 128, 12, SSD1306_BLACK);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 1);
  display.print("C:");
  display.print(coins);

  display.setCursor(43, 1);
  display.print("D:");
  display.print(distanceM);

  display.setCursor(94, 1);
  display.print("L:");
  display.print(level);

  display.drawRect(0, 13, 43, 6, SSD1306_WHITE);

  int fw = clampInt((int)(fuel * 0.39f), 0, 39);
  display.fillRect(2, 15, fw, 2, SSD1306_WHITE);

  display.setCursor(48, 13);
  display.print("FUEL");

  display.setCursor(87, 13);
  display.print(environmentName());
}

// ---------- SCREENS ----------

void drawTitleScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(5, 5);
  display.print("HILL CLIMB");

  display.setTextSize(1);
  display.setCursor(26, 28);
  display.print("ENDLESS MODE");

  display.drawLine(0, 49, 25, 43, SSD1306_WHITE);
  display.drawLine(25, 43, 50, 50, SSD1306_WHITE);
  display.drawLine(50, 50, 77, 42, SSD1306_WHITE);
  display.drawLine(77, 42, 127, 50, SSD1306_WHITE);

  display.setCursor(10, 55);
  display.print("A: DRIVE  B: START");
  display.display();
}

void drawGameOver() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(13, 5);
  display.print("GAME OVER");

  display.setTextSize(1);
  display.setCursor(16, 30);
  display.print("Distance:");
  display.print(distanceM);

  display.setCursor(16, 40);
  display.print("Coins:");
  display.print(coins);

  display.setCursor(3, 55);
  display.print("PRESS B TO RESTART");
  display.display();
}

void drawLevelComplete() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(4, 5);
  display.print("LEVEL UP!");

  display.setTextSize(1);
  display.setCursor(10, 30);
  display.print("Environment:");
  display.print(environmentName());

  display.setCursor(10, 41);
  display.print("Reward:");
  display.print(levelReward);

  display.setCursor(7, 55);
  display.print("GET READY!");
  display.display();
}

// ---------- GAME CONTROL ----------

void resetGame() {
  worldX = 0;
  speed = 0;
  verticalSpeed = 0;
  fuel = 100;

  coins = 0;
  distanceM = 0;
  level = 1;
  levelReward = 0;

  grounded = true;
  lastCoinWorld = 0;
  lastRewardedLevel = 1;

  jeepY = terrainY(JEEP_X) - 10;

  gameState = PLAYING;
  stateTimer = millis();
}

void advanceLevel(int newLevel) {
  level = newLevel;

  levelReward = 10 + level * 5;
  coins += levelReward;

  fuel += 15;
  if (fuel > 100) fuel = 100;

  gameState = LEVEL_COMPLETE;
  stateTimer = millis();
}

// ---------- SETUP ----------

void setup() {
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);

  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    while (true) delay(100);
  }

  display.clearDisplay();
  display.display();

  initializeRain();
  drawTitleScreen();
}

// ---------- MAIN LOOP ----------

void loop() {
  unsigned long now = millis();

  if (now - lastFrame < 35) return;
  lastFrame = now;

  bool a = pressed(BUTTON_A);
  bool b = pressed(BUTTON_B);

  bool bPressed = b && !previousB;

  previousA = a;
  previousB = b;

  if (gameState == TITLE_SCREEN) {
    drawTitleScreen();

    if (a || b) resetGame();
    return;
  }

  if (gameState == GAME_OVER) {
    drawGameOver();

    if (bPressed) resetGame();
    return;
  }

  if (gameState == LEVEL_COMPLETE) {
    drawLevelComplete();

    if (now - stateTimer >= 1400) {
      gameState = PLAYING;
    }
    return;
  }

  // ---------- ACCELERATION ----------
  if (a && fuel > 0) {
    speed += 0.10f;
    fuel -= 0.035f;
  } else {
    speed -= 0.04f;
  }

  speed = fminf(2.5f, fmaxf(0, speed));
  worldX += speed;

  distanceM = (int)(worldX / 3.0f);

  // ---------- JUMP ----------
  if (bPressed && grounded) {
    verticalSpeed = -3.4f;
    grounded = false;
  }

  verticalSpeed += 0.22f;
  jeepY += verticalSpeed;

  float groundY = terrainY(worldX + JEEP_X) - 10;

  if (jeepY >= groundY) {
    jeepY = groundY;
    verticalSpeed = 0;
    grounded = true;
  }

  // ---------- LEVEL PROGRESSION ----------
  int newLevel = distanceM / LEVEL_DISTANCE + 1;

  if (newLevel > level) {
    advanceLevel(newLevel);
    drawLevelComplete();
    return;
  }

  // ---------- COLLISIONS ----------
  collectCoins();

  if (hitObstacle()) {
    gameState = GAME_OVER;
    drawGameOver();
    return;
  }

  // End the run when fuel is gone and the jeep stops.
  if (fuel <= 0) {
    fuel = 0;

    if (speed < 0.1f) {
      gameState = GAME_OVER;
      drawGameOver();
      return;
    }
  }

  // ---------- DRAW FRAME ----------
  updateRain();

  display.clearDisplay();
  drawBackground();
  drawTerrain();
  drawCoins();
  drawObstacles();
  drawJeep();
  drawRain();
  drawHUD();
  display.display();
}
