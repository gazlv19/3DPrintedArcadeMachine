

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define OLED_DC  16   // RX2
#define OLED_RST 17   // TX2
#define OLED_CS  5    // D5

const int VRX = 34;
const int VRY = 35;
const int BTN = 33;                      
const int LED_PINS[3] = {25, 26, 27};    

Adafruit_SSD1306 display(128, 64, &SPI, OLED_DC, OLED_RST, OLED_CS);

const int DEADZONE = 700;                     
const unsigned long PAC_MS          = 130;   
const unsigned long GHOST_MS        = 190;     
const unsigned long GHOST_SCARED_MS = 290;   
const unsigned long FRIGHT_MS       = 7000;  
const unsigned long RELEASE_MS[4]   = {0, 3000, 7000, 11000};

// ======================= MAZE =======================
// Left half only (16 columns); the right half is mirrored.
// '#' wall  '.' pellet  'o' power pellet  ' ' empty  'G' ghost-house door
const int W = 32, H = 15, T = 4, OY = 2;
const char* LEFT_HALF[H] = {
  "################",
  "#o..............",
  "#.####.#######.#",
  "#...............",
  "#...###.####....",
  "#...............",
  "#.###.#.###.###G",
  "#...........#   ",
  "#.#...#.##..####",
  "#...............",
  "#.###.#.#####.##",
  "#...............",
  "#.####.#######.#",
  "#o..............",
  "################"
};

char maze[H][W];

// ======================= ACTORS =======================
struct Actor {
  int x, y, px, py, dx, dy;
  unsigned long lastMove;
};

struct Ghost {
  int x, y, px, py, dx, dy;
  bool inPen, scared;
  unsigned long releaseAt, nextStep, lastMove, stepDur;
};

Actor pac;
Ghost ghosts[4];
int faceDx = 1, faceDy = 0;
bool mouthOpen = true;

// scatter-mode corners (one per ghost)
const int CORNER_X[4] = {30, 1, 30, 1};
const int CORNER_Y[4] = {1, 1, 13, 13};

// ======================= GAME STATE =======================
enum State { START, PLAY, DYING, OVER };
State state = START;
bool won = false;

int pellets = 0, score = 0, finalScore = 0, chain = 0;
int wantDx = 0, wantDy = 0;
int joyCx = 2048, joyCy = 2048;
unsigned long gameStart = 0, stateStart = 0, frightUntil = 0, effectUntil = 0;

bool btnLast = false;
unsigned long btnLastChange = 0;

// ======================= HELPERS =======================
bool buttonEdge(unsigned long now) {
  bool cur = (digitalRead(BTN) == LOW);
  bool edge = false;
  if (cur != btnLast && now - btnLastChange > 40) {
    btnLast = cur;
    btnLastChange = now;
    if (cur) edge = true;
  }
  return edge;
}

bool pacFree(int x, int y) {
  return x >= 0 && x < W && y >= 0 && y < H && maze[y][x] != '#' && maze[y][x] != 'G';
}

bool ghostFree(int x, int y) {
  return x >= 0 && x < W && y >= 0 && y < H && maze[y][x] != '#' && maze[y][x] != 'G';
}

int lerpPos(int prev, int cur, unsigned long since, unsigned long dur) {
  if (since > dur) since = dur;
  return prev * T + ((cur - prev) * T * (long)since) / (long)dur;
}

// Dither-based fade for a rectangular region. level 0 (invisible) .. 16 (full)
const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

void fadeRegion(int x0, int y0, int w, int h, int level) {
  uint8_t* buf = display.getBuffer();
  for (int y = y0; y < y0 + h && y < 64; y++) {
    for (int x = x0; x < x0 + w && x < 128; x++) {
      int idx = x + (y >> 3) * 128;
      uint8_t bit = 1 << (y & 7);
      if ((buf[idx] & bit) && BAYER[y & 3][x & 3] >= level) buf[idx] &= ~bit;
    }
  }
}

int fadeLevel(unsigned long now) {
  unsigned long p = now % 2400;
  long tri = (p < 1200) ? p : 2400 - p;
  long lvl = (tri * 20) / 1200;
  return lvl > 16 ? 16 : (int)lvl;
}

// ======================= GAME SETUP =======================
void resetGame(unsigned long now) {
  pellets = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      char c = (x < 16) ? LEFT_HALF[y][x] : LEFT_HALF[y][31 - x];
      maze[y][x] = c;
      if (c == '.' || c == 'o') pellets++;
    }

  pac = {15, 9, 15, 9, 0, 0, now};
  faceDx = 1; faceDy = 0; mouthOpen = true;
  wantDx = wantDy = 0;
  score = 0; chain = 0;
  frightUntil = 0; effectUntil = 0;
  gameStart = now;

  // Ghost 0 starts outside the pen, the rest wait inside
  int sx[4] = {16, 14, 17, 16};
  int sy[4] = {5, 7, 7, 7};
  for (int i = 0; i < 4; i++) {
    Ghost &g = ghosts[i];
    g.x = g.px = sx[i];
    g.y = g.py = sy[i];
    g.dx = (i == 0) ? -1 : 0;
    g.dy = 0;
    g.inPen = (i != 0);
    g.scared = false;
    g.releaseAt = now + RELEASE_MS[i];
    g.stepDur = GHOST_MS;
    g.nextStep = now + GHOST_MS;
    g.lastMove = now;
  }

  // eat the pellet under Pac-Man's start tile, then zero the score
  if (maze[pac.y][pac.x] == '.') { maze[pac.y][pac.x] = ' '; pellets--; }
}

// ======================= GAME LOGIC =======================
void readInput() {
  int x = analogRead(VRX) - joyCx;
  int y = analogRead(VRY) - joyCy;
  if (abs(x) < DEADZONE && abs(y) < DEADZONE) return;
  // Joystick is mounted rotated: x HIGH = left, x LOW = right, y HIGH = up, y LOW = down
  if (abs(x) > abs(y)) { wantDx = (x > 0) ? -1 : 1; wantDy = 0; }
  else                 { wantDx = 0; wantDy = (y > 0) ? -1 : 1; }
}

void powerUp(unsigned long now) {
  frightUntil = now + FRIGHT_MS;
  chain = 0;
  effectUntil = now + 450;
  for (int i = 0; i < 4; i++) {
    Ghost &g = ghosts[i];
    if (!g.inPen) {
      g.scared = true;
      g.dx = -g.dx; g.dy = -g.dy;   // ghosts reverse when frightened
    }
  }
}

void eat(int x, int y, unsigned long now) {
  char c = maze[y][x];
  if (c == '.') { maze[y][x] = ' '; pellets--; score += 10; }
  else if (c == 'o') { maze[y][x] = ' '; pellets--; score += 50; powerUp(now); }
}

void stepPac(unsigned long now) {
  pac.px = pac.x; pac.py = pac.y;
  if (pacFree(pac.x + wantDx, pac.y + wantDy)) { pac.dx = wantDx; pac.dy = wantDy; }
  if (pacFree(pac.x + pac.dx, pac.y + pac.dy) && (pac.dx || pac.dy)) {
    pac.x += pac.dx; pac.y += pac.dy;
    faceDx = pac.dx; faceDy = pac.dy;
    mouthOpen = !mouthOpen;
  }
  pac.lastMove = now;
  eat(pac.x, pac.y, now);
}

void getTarget(int i, int &tx, int &ty, unsigned long now) {
  bool scatter = ((now - gameStart) % 24000UL) < 6000UL;   // 6s scatter, 18s chase
  if (scatter) { tx = CORNER_X[i]; ty = CORNER_Y[i]; return; }
  switch (i) {
    case 0:  // Blinky: chases Pac-Man directly
      tx = pac.x; ty = pac.y; break;
    case 1:  // Pinky: aims ahead of Pac-Man
      tx = pac.x + 4 * pac.dx; ty = pac.y + 4 * pac.dy; break;
    case 2: {  // Inky: flanks using Blinky's position
      int ax = pac.x + 2 * pac.dx, ay = pac.y + 2 * pac.dy;
      tx = 2 * ax - ghosts[0].x; ty = 2 * ay - ghosts[0].y; break;
    }
    default: {  // Clyde: chases when far, retreats when close
      int dx = ghosts[i].x - pac.x, dy = ghosts[i].y - pac.y;
      if (dx * dx + dy * dy > 64) { tx = pac.x; ty = pac.y; }
      else { tx = CORNER_X[i]; ty = CORNER_Y[i]; }
    }
  }
}

void stepGhost(int i, unsigned long now) {
  Ghost &g = ghosts[i];
  g.px = g.x; g.py = g.y;
  g.lastMove = now;
  g.stepDur = g.scared ? GHOST_SCARED_MS : GHOST_MS;
  g.nextStep = now + g.stepDur;

  if (g.inPen) {
    if (now < g.releaseAt) return;                 // still waiting
    if (g.y == 7) {                                // head to the door column, then up
      if (g.x < 15) g.x++;
      else if (g.x > 15) g.x--;
      else g.y--;
    } else if (g.y == 6) {
      g.y--;
    }
    if (g.y == 5) { g.inPen = false; g.dx = (i & 1) ? 1 : -1; g.dy = 0; }
    return;
  }

  static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  int tx, ty;
  getTarget(i, tx, ty, now);

  int best = -1;
  long bestD = 999999;
  for (int k = 0; k < 4; k++) {
    int nx = g.x + dirs[k][0], ny = g.y + dirs[k][1];
    if (!ghostFree(nx, ny)) continue;
    if ((g.dx || g.dy) && dirs[k][0] == -g.dx && dirs[k][1] == -g.dy) continue;  // no U-turns
    long d = g.scared ? random(0, 1000)
                      : (long)(nx - tx) * (nx - tx) + (long)(ny - ty) * (ny - ty);
    if (d < bestD) { bestD = d; best = k; }
  }
  if (best < 0) { g.dx = -g.dx; g.dy = -g.dy; }       // dead end: turn around
  else { g.dx = dirs[best][0]; g.dy = dirs[best][1]; }
  g.x += g.dx; g.y += g.dy;
}

void eatGhost(int i, unsigned long now) {
  Ghost &g = ghosts[i];
  int c = chain > 3 ? 3 : chain;
  score += 200 << c;                // 200, 400, 800, 1600
  chain++;
  g.x = g.px = 15; g.y = g.py = 7;  // send back to the pen
  g.dx = g.dy = 0;
  g.inPen = true;
  g.scared = false;
  g.releaseAt = now + 2500;
  g.nextStep = now + GHOST_MS;
  effectUntil = now + 350;
}

void checkCollisions(unsigned long now) {
  for (int i = 0; i < 4; i++) {
    Ghost &g = ghosts[i];
    if (g.inPen) continue;
    bool same = (g.x == pac.x && g.y == pac.y);
    bool swap = (g.x == pac.px && g.y == pac.py && pac.x == g.px && pac.y == g.py);
    if (same || swap) {
      if (g.scared) {
        eatGhost(i, now);
      } else {
        state = DYING;
        stateStart = now;
        return;
      }
    }
  }
}

void goOver(unsigned long now, bool didWin) {
  won = didWin;
  finalScore = score;
  state = OVER;
  stateStart = now;
}

void playFrame(unsigned long now);
void drawGame(unsigned long now);

void playFrame(unsigned long now) {
  readInput();

  if (now - pac.lastMove >= PAC_MS) {
    stepPac(now);
    checkCollisions(now);
  }

  for (int i = 0; i < 4 && state == PLAY; i++) {
    if (now >= ghosts[i].nextStep) {
      stepGhost(i, now);
      checkCollisions(now);
    }
  }

  if (state == PLAY && frightUntil && now > frightUntil) {
    frightUntil = 0;
    for (int i = 0; i < 4; i++) ghosts[i].scared = false;
  }

  if (state == PLAY && pellets <= 0) { goOver(now, true); return; }

  drawGame(now);
}

// ======================= DRAWING =======================
static const char* GHOST_SPRITE[5] = {
  ".###.",
  "#.#.#",
  "#####",
  "#####",
  "#.#.#"
};

void drawGhost(int px, int py, bool scared, bool blinkNormal) {
  bool dither = scared && !blinkNormal;
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 5; c++) {
      if (GHOST_SPRITE[r][c] != '#') continue;
      if (dither && ((r + c) & 1)) continue;
      display.drawPixel(px + c, py + r, WHITE);
    }
}

void drawPac(int cx, int cy) {
  display.fillCircle(cx, cy, 2, WHITE);
  if (mouthOpen) {
    int fx = faceDx, fy = faceDy;
    display.drawPixel(cx + fx, cy + fy, BLACK);
    display.drawPixel(cx + 2 * fx, cy + 2 * fy, BLACK);
    display.drawPixel(cx + 2 * fx + fy, cy + 2 * fy + fx, BLACK);
    display.drawPixel(cx + 2 * fx - fy, cy + 2 * fy - fx, BLACK);
  }
}

void drawGame(unsigned long now) {
  display.clearDisplay();
  bool bigPelletOn = ((now / 250) % 2) == 0;

  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      char c = maze[y][x];
      int cx = x * T + 2, cy = y * T + OY + 2;
      if (c == '#') {
        display.drawPixel(cx, cy, WHITE);
        if (x + 1 < W && maze[y][x + 1] == '#') display.drawFastHLine(cx, cy, T + 1, WHITE);
        if (y + 1 < H && maze[y + 1][x] == '#') display.drawFastVLine(cx, cy, T + 1, WHITE);
      } else if (c == '.') {
        display.drawPixel(cx, cy, WHITE);
      } else if (c == 'o') {
        if (bigPelletOn) display.fillRect(cx - 1, cy - 1, 3, 3, WHITE);
      } else if (c == 'G') {
        display.drawFastHLine(x * T, cy, T, WHITE);
      }
    }
  }

  // ghosts
  for (int i = 0; i < 4; i++) {
    Ghost &g = ghosts[i];
    int gx = lerpPos(g.px, g.x, now - g.lastMove, g.stepDur);
    int gy = lerpPos(g.py, g.y, now - g.lastMove, g.stepDur) + OY;
    bool blinkNormal = false;
    if (g.scared && frightUntil > now && (frightUntil - now) < 2000)
      blinkNormal = ((now / 200) % 2) == 0;
    drawGhost(gx, gy, g.scared, blinkNormal);
  }

  // Pac-Man (blinks while dying)
  bool showPac = !(state == DYING && ((now - stateStart) / 120) % 2);
  if (showPac) {
    int cx = lerpPos(pac.px, pac.x, now - pac.lastMove, PAC_MS) + 2;
    int cy = lerpPos(pac.py, pac.y, now - pac.lastMove, PAC_MS) + OY + 2;
    drawPac(cx, cy);
  }

  display.display();
}

void drawStart(unsigned long now) {
  display.clearDisplay();
  display.setTextColor(WHITE);

  // Big animated Pac-Man icon
  int cx = 20, cy = 24;
  long p = now % 500;
  int h = (int)((p < 250 ? p : 500 - p) * 8 / 250);   // mouth opening 0..8
  display.fillCircle(cx, cy, 10, WHITE);
  display.fillTriangle(cx, cy, cx + 13, cy - h - 1, cx + 13, cy + h + 1, BLACK);
  display.drawPixel(cx - 1, cy - 5, BLACK);             // eye

  // Title
  display.setTextSize(2);
  display.setCursor(40, 17);
  display.print("PACMAN");

  // Row of pellets
  for (int x = 12; x <= 116; x += 8) display.fillRect(x, 40, 2, 2, WHITE);

  // Fading prompt
  display.setTextSize(1);
  display.setCursor(34, 52);
  display.print("START GAME");
  fadeRegion(34, 52, 60, 8, fadeLevel(now));

  display.display();
}

void drawOver(unsigned long now) {
  display.clearDisplay();
  display.setTextColor(WHITE);

  const char* title = won ? "YOU WIN!" : "GAME OVER";
  int len = strlen(title);
  int tx = (128 - len * 12) / 2;
  display.setTextSize(2);
  display.setCursor(tx, 4);
  display.print(title);
  fadeRegion(tx, 4, len * 12, 16, fadeLevel(now));

  display.setTextSize(1);
  display.setCursor(49, 28);
  display.print("SCORE");

  char buf[12];
  snprintf(buf, sizeof(buf), "%d", finalScore);
  int sl = strlen(buf);
  display.setTextSize(2);
  display.setCursor((128 - sl * 12) / 2, 38);
  display.print(buf);

  // EXIT "button" at the bottom
  display.fillRoundRect(46, 54, 36, 10, 2, WHITE);
  display.setTextSize(1);
  display.setTextColor(BLACK);
  display.setCursor(52, 55);
  display.print("EXIT");
  display.setTextColor(WHITE);

  display.display();
}

void setLeds(uint8_t mask) {
  for (int i = 0; i < 3; i++) digitalWrite(LED_PINS[i], (mask >> i) & 1);
}

void updateLeds(unsigned long now) {
  uint8_t m = 0;

  if (state == START) {
    static const uint8_t seq[4] = {1, 2, 4, 2};
    m = seq[(now / 800) % 4];

  } else if (state == PLAY) {
    bool anyScared = false;
    int nearest = 99;
    for (int i = 0; i < 4; i++) {
      Ghost &g = ghosts[i];
      if (g.scared) anyScared = true;
      if (g.inPen || g.scared) continue;
      int d = abs(g.x - pac.x) + abs(g.y - pac.y);
      if (d < nearest) nearest = d;
    }

    if (effectUntil > now) {
      m = ((now / 80) % 2 == 0) ? 7 : 0;          
    } else if (anyScared) {
      m = ((now / 110) % 2) ? 5 : 2;              
    } else if (nearest <= 4) {
      m = ((now / 70) % 2 == 0) ? 7 : 0;          
    } else if (nearest <= 7) {
      m = 7;
    } else if (nearest <= 12) {
      m = 5;
    } else {
      m = 2;                                     
    }

  } else if (state == DYING) {
    m = ((now / 70) % 2 == 0) ? 7 : 0;

  } else {  
    unsigned long t = now - stateStart;
    if (t < 2000) {
      if (won) m = ((now / 120) % 2) ? 5 : 2;      
      else     m = ((now / 100) % 2 == 0) ? 7 : 0; 
    } else {
      m = (fadeLevel(now) >= 8) ? 7 : 0;          
    }
  }
  setLeds(m);
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN, INPUT_PULLUP);
  for (int i = 0; i < 3; i++) pinMode(LED_PINS[i], OUTPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println("Display init failed - check wiring");
    while (true);
  }

  long sx = 0, sy = 0;
  for (int i = 0; i < 30; i++) { sx += analogRead(VRX); sy += analogRead(VRY); delay(5); }
  joyCx = sx / 30; joyCy = sy / 30;
  if (joyCx < 1500 || joyCx > 2600) joyCx = 2048;  
  if (joyCy < 1500 || joyCy > 2600) joyCy = 2048;

  randomSeed(micros());
  btnLast = (digitalRead(BTN) == LOW);             
  btnLastChange = millis();

  setLeds(7); delay(250); setLeds(0);

  state = START;
}

void loop() {
  unsigned long now = millis();
  bool pressed = buttonEdge(now);

  switch (state) {
    case START:
      drawStart(now);
      if (pressed) {
        resetGame(now);
        state = PLAY;
      }
      break;

    case PLAY:
      playFrame(now);
      break;

    case DYING:
      drawGame(now);
      if (now - stateStart > 1000) goOver(now, false);
      break;

    case OVER:
      drawOver(now);
      if (pressed && now - stateStart > 1200) state = START;   
      break;
  }

  updateLeds(now);
}
