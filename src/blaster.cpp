#include "common.cpp"
#include "common_touch.cpp"

#define AUDIO_PIN 26
#define MINY 28
#define SHIPSIZE 10
#define TARGETSIZE 16
#define SPRITE_SCALE 2

#define MAX_TARGETS 5
#define MAX_BULLETS 50
#define BULLET_SPEED 4
#define EXPLOSIONSPEED 4

static uint8_t gameFramePixels[SCREEN_WIDTH * SCREEN_HEIGHT];
static uint16_t gameFrameRow[SCREEN_WIDTH];

class BlasterCanvas : public GFXcanvas8 {
public:
  BlasterCanvas() : GFXcanvas8(SCREEN_WIDTH, SCREEN_HEIGHT, false) {
    buffer = gameFramePixels;
  }
};

BlasterCanvas gameFrame;

const uint8_t FRAME_BLACK = 0x00;
const uint8_t FRAME_DARKGREY = 0x49;
const uint8_t FRAME_WHITE = 0xFF;

/* ===== SOUND STATE ===== */
bool soundActive = false;
unsigned long soundEndTime = 0;
int currentSoundFreq = 0;

int shockwaveX =0 ;
int shockwavePoints=0; 
int shockwaveactive=0;
int shockwavesound=0;
#define POINTS_FOR_SHOCKWAVE 10
#define SHOCKWAVE_SPEED 18
#define SHOCKWAVE_SIZE 25

int looptimer =0;
#define MINLOOPTIME 40  // loop time should be at-least 40 milisecods. 

//gamemodes
#define GAMEMODE_SELECTOR 0
#define GAMEMODE_BLASTER GAMEMODE_SELECTOR+1  
struct GameEntry {
  const char* name;
  int mode;
};

GameEntry gameList[] = {
  {"Blaster", GAMEMODE_BLASTER},
  {"(future) Mode 2", 2},
  {"(future) Mode 3", 3},
  {"(future) Mode 4", 4},
  {"(future) Mode 5", 5},
  {"(future) Mode 6", 6},
  {"(future) Mode 7", 7}
};

const int GAME_COUNT = sizeof(gameList) / sizeof(gameList[0]);

int gamemode=GAMEMODE_SELECTOR; // default to selector
int selectedGame = 0;



/* ===== SHIP BITMAP (5x5) ===== */
const uint8_t shipBitmap[] PROGMEM = {
  0b10000000,
  0b01100000,
  0b01111000,
  0b01100000,
  0b10000000
};


/* ===== Explosion bitmap (8x8 ===== */
const uint8_t EXPLODETOP[] PROGMEM = {
  0b00011000,
  0b00111100,
  0b01111110,
  0b01100110,
  0b11000111,
  0b11000101,
  0b10100000,
  0b10010000
};

const uint8_t EXPLODEBOT[] PROGMEM = {
  0b10010000,
  0b10100000,
  0b11000101,
  0b11000111,
  0b01100110,
  0b01111110,
  0b00111100,
  0b00011000
};




const uint8_t bulletFrames[][4] PROGMEM = {

  // Frame 0
  {
    0b00000000,  // 0000
    0b01100000,  // 0110
    0b01100000,  // 0110
    0b00000000   // 0000
  },

  // Frame 1
  {
    0b10000000,  // 1000
    0b01100000,  // 0110
    0b01100000,  // 0110
    0b01110000   // 0111
  },

  // Frame 2
  {
    0b01100000,  // 0110
    0b01100000,  // 0110
    0b01100000,  // 0110
    0b01100000   // 0110
  },

  // Frame 3
  {
    0b00010000,  // 0001
    0b01100000,  // 0110
    0b01100000,  // 0110
    0b10000000   // 1000
  }
};

#define BULLET_FRAME_COUNT 4

// 8x8 rotating ring animation (vertical axis)
// Each frame is 8 rows (top to bottom)
const uint8_t ringFrames[][8] PROGMEM = {

  // Frame 0: full ring
  {
    0b00111100,
    0b01000010,
    0b10000001,
    0b10000001,
    0b10000001,
    0b10000001,
    0b01000010,
    0b00111100
  },

  // Frame 1: slightly turned
  {
    0b00011100,
    0b00100010,
    0b01000001,
    0b01000001,
    0b01000001,
    0b01000001,
    0b00100010,
    0b00011100
  },

  // Frame 2: more edge-on
  {
    0b00001100,
    0b00010010,
    0b00100001,
    0b00100001,
    0b00100001,
    0b00100001,
    0b00010010,
    0b00001100
  },

  // Frame 3: edge-on (vertical line)
  {
    0b00000100,
    0b00000100,
    0b00000100,
    0b00000100,
    0b00000100,
    0b00000100,
    0b00000100,
    0b00000100
  },

  // Frame 4: mirror of frame 2
  {
    0b00001100,
    0b01001000,
    0b10000100,
    0b10000100,
    0b10000100,
    0b10000100,
    0b01001000,
    0b00001100
  },

  // Frame 5: mirror of frame 1
  {
    0b00011100,
    0b01000100,
    0b10000010,
    0b10000010,
    0b10000010,
    0b10000010,
    0b01000100,
    0b00011100
  }
};

const uint8_t RING_FRAME_COUNT = sizeof(ringFrames) / 8;



/* ===== GAME STATE ===== */
bool gameOver = false;
int score = 0;
unsigned long gameStartTime = 0;
unsigned long gameEndSec = 0;

/* ===== BULLET ===== */
bool bulletActive = false;
int bulletX, bulletY;
uint8_t bulletFrame = 0;


/* ===== TARGET STRUCT ===== */
struct Target {
  bool active;
  int x;
  int y;
  int frame; // current frame in the animation
};

Target targets[MAX_TARGETS];
int current_max_bullets=1; 
int current_min_bullets=1;

/* ===== bullet STRUCT ===== */
struct bullet_struct {
  bool active;
  int x;
  int y;
  int frame; // current frame in the animation
};

bullet_struct bullets[MAX_BULLETS];

/* ===== Explosions STRUCT ===== */
struct Explosion {
  bool active;
  int x;
  int y;
  int lowery;
};

Explosion explosions[MAX_TARGETS];

/* ===== TIMING ===== */
unsigned long lastSpawn = 0;

/* ===== ROWS ===== */
int rows[] = {32, 62, 92, 122, 152, 182, 212};
const int ROWCOUNT = sizeof(rows) / sizeof(rows[0]);

bool touchDown = false;
bool touchTapped = false;
int touchY = SCREEN_HEIGHT / 2;
int shipY = SCREEN_HEIGHT / 2;
bool menuDrawn = false;
bool gameOverDrawn = false;

/*needed to compute diffaculy*/
unsigned long elapsedSeconds() {
  return (millis() - gameStartTime) / 1000;
}

void startSound(int freq, unsigned long durationMs) {
  if (soundActive) return;   // don't interrupt existing sound

  currentSoundFreq = freq;
  soundEndTime = millis() + durationMs;
  soundActive = true;

  tone(AUDIO_PIN, freq);
}

void fireSound() {
  startSound(2200, 40);   // short, sharp laser
}

void hitSound() {
  startSound(900, 90);    // slightly longer impact
}

void shockwaveSound(){
    startSound(600,300);
    shockwavesound=1;
}

void updateSound() {
  if (shockwavesound>0) {
    // Static / buzz effect
    int jitter = random(-100, 300);
    tone(AUDIO_PIN, 600 + jitter);
  };


  if (soundActive && millis() >= soundEndTime) {
    noTone(AUDIO_PIN);
    soundActive = false;
    shockwavesound=0;
  }
}




void soundCrash() {
  for (int f = 800; f > 200; f -= 20) {
    tone(AUDIO_PIN, f);
    delay(10);
  }
  noTone(AUDIO_PIN);
}


/* ===== HELPERS ===== */
int targetSpeed() {
  int t = elapsedSeconds();
  int speed = 1 + (t / 20);   // +1 speed every 20 seconds
  if (speed > 5) speed = 5;  // cap
  return speed;
}

unsigned long spawnDelay() {
  unsigned long t = elapsedSeconds();
  long d = 1400 - (t * 15);   // very slow reduction
  if (d < 350) d = 350;       // cap
  return d;
}


void spawnTarget() {
  for (int i = 0; i < MAX_TARGETS; i++) {
    if (!targets[i].active) {
      targets[i].active = true;
      targets[i].x = SCREEN_WIDTH - TARGETSIZE;
      targets[i].y = rows[random(ROWCOUNT)];
      targets[i].frame=0;
      return;
    }
  }
}

void spawnBullet(int x , int y) {
  for (int i = 0; i < current_max_bullets; i++) {
    if (!bullets[i].active) {
      bullets[i].active = true;
      bullets[i].x = x;
      bullets[i].y = y;
      bullets[i].frame=0;
      fireSound();
      return;
    }
  }
}



void spawnExplosion(int startx, int starty) {
  for (int i = 0; i < MAX_TARGETS; i++) {
    if (!explosions[i].active) {
      explosions[i].active = true;
      explosions[i].x = startx;
      explosions[i].y = starty;
      explosions[i].lowery = starty;
      return;
    }
  }
}



void resetGame() {
  score = 0;
  gameOver = false;
  gameOverDrawn = false;
  bulletActive = false;
  current_max_bullets=1; 
  shockwaveX =0 ;
  shockwavePoints=0; 
  shockwaveactive=0;
  shockwavesound=0;

  for (int i = 0; i < MAX_TARGETS; i++) {
    targets[i].active = false;
    explosions[i].active=false;
  }
  for (int i = 0; i < MAX_BULLETS; i++) {
    bullets[i].active = false;
  }
  

  gameStartTime = millis();
  lastSpawn = millis();
  gameEndSec=0;
}


/* ===== SETUP ===== */
void setup() {
  setup_display();
  setup_touch();
  randomSeed(micros());
  pinMode(AUDIO_PIN, OUTPUT);
  tft.fillScreen(ILI9341_BLACK);
  resetGame();
}

void updateTouch() {
  bool nowTouched = ts.touched();
  touchTapped = nowTouched && !touchDown;

  if (nowTouched) {
    TS_Point point = ts.getPoint();
    touchY = constrain(
      map(point.y, 200, 3800, 0, SCREEN_HEIGHT - 1),
      0,
      SCREEN_HEIGHT - 1
    );
  }

  touchDown = nowTouched;
}

void drawScaledBitmap(
  GFXcanvas8 &canvas,
  int x,
  int y,
  const uint8_t *bitmap,
  int width,
  int height,
  uint8_t color
) {
  for (int row = 0; row < height; row++) {
    uint8_t rowBits = pgm_read_byte(bitmap + row);
    for (int column = 0; column < width; column++) {
      if (rowBits & (0x80 >> column)) {
        canvas.fillRect(
          x + column * SPRITE_SCALE,
          y + row * SPRITE_SCALE,
          SPRITE_SCALE,
          SPRITE_SCALE,
          color
        );
      }
    }
  }
}

uint16_t rgb332To565(uint8_t color) {
  uint16_t red = (color >> 5) & 0x07;
  uint16_t green = (color >> 2) & 0x07;
  uint16_t blue = color & 0x03;

  red = (red << 2) | (red >> 1);
  green = (green << 3) | green;
  blue = (blue << 3) | (blue << 1) | (blue >> 1);

  return (red << 11) | (green << 5) | blue;
}

void presentGameFrame() {
  uint8_t *pixels = gameFrame.getBuffer();
  tft.startWrite();
  tft.setAddrWindow(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

  for (int y = 0; y < SCREEN_HEIGHT; y++) {
    int rowOffset = y * SCREEN_WIDTH;
    for (int x = 0; x < SCREEN_WIDTH; x++) {
      gameFrameRow[x] = rgb332To565(pixels[rowOffset + x]);
    }
    tft.writePixels(gameFrameRow, SCREEN_WIDTH);
  }

  tft.endWrite();
}

void game_selector() {
  if (!menuDrawn) {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(ILI9341_WHITE);
    tft.setCursor(8, 8);
    tft.println("SELECT GAME");

    for (int i = 0; i < GAME_COUNT; i++) {
      int y = 38 + i * 27;
      if (i == selectedGame) {
        tft.fillRect(0, y - 3, SCREEN_WIDTH, 24, ILI9341_DARKGREY);
      }
      tft.setCursor(8, y);
      tft.print(gameList[i].name);
    }

    menuDrawn = true;
  }

  if (touchTapped && touchY >= 38) {
    int tappedGame = (touchY - 38) / 27;
    if (tappedGame < GAME_COUNT) {
      selectedGame = tappedGame;
      resetGame();
      gamemode = gameList[selectedGame].mode;
    }
  }
}

void blaster_game()
{
  if (gameOver) {
    if (gameEndSec == 0) {
      gameEndSec = elapsedSeconds();
      soundCrash();
    }

    if (!gameOverDrawn) {
      tft.fillScreen(ILI9341_BLACK);
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(3);
      tft.setCursor(55, 55);
      tft.println("GAME OVER");
      tft.setTextSize(2);
      tft.setCursor(80, 110);
      tft.print("Score: ");
      tft.println(score);
      tft.setCursor(80, 140);
      tft.print("Time: ");
      tft.println(gameEndSec);
      tft.setCursor(40, 195);
      tft.println("Tap to play again");
      gameOverDrawn = true;
    }

    if (touchTapped) {
      resetGame();
    }
    return;
  }

  shipY = constrain(touchY - SHIPSIZE / 2, MINY, SCREEN_HEIGHT - SHIPSIZE);

  if (touchTapped) {
    if (shockwavePoints >= POINTS_FOR_SHOCKWAVE &&
        touchY < MINY) {
      shockwaveX = -SHOCKWAVE_SIZE;
      shockwaveactive = 1;
      shockwavePoints = 0;
      shockwaveSound();
    } else {
      spawnBullet(SHIPSIZE, shipY + SHIPSIZE / 2);
    }
  }

  /* ===== MOVE BULLETS ===== */
 for (int i = 0; i < MAX_BULLETS; i++) {
    if (bullets[i].active) {
      bullets[i].x += BULLET_SPEED;
      bullets[i].frame++;
          if (bullets[i].frame>=BULLET_FRAME_COUNT){bullets[i].frame=0;};

      if (bullets[i].x > SCREEN_WIDTH) {
        bullets[i].active = false;
        if (current_min_bullets<current_max_bullets){current_max_bullets--;};
      }
    }
  }

// Move shockwave
 if (shockwaveactive>0){
   shockwaveX=shockwaveX+SHOCKWAVE_SPEED;
  for (int i = 0; i < MAX_TARGETS; i++) {
    if (!targets[i].active) continue;

    if ((targets[i].x >= shockwaveX) && (targets[i].x<=shockwaveX+SHOCKWAVE_SIZE))
        {
          targets[i].active = false;
          score++;
          spawnExplosion(targets[i].x,targets[i].y);
        };
  };
   if (shockwaveX>SCREEN_WIDTH){
       shockwaveX=0;
       shockwaveactive=0;
       };
 }


  /* ===== SPAWN TARGETS ===== */
  if (millis() - lastSpawn > spawnDelay()) {
    spawnTarget();
    lastSpawn = millis();
  }

  /* ===== MOVE TARGETS ===== */
  for (int i = 0; i < MAX_TARGETS; i++) {
    if (targets[i].active) {
      targets[i].x -= targetSpeed();
      targets[i].frame++;
          if (targets[i].frame>=RING_FRAME_COUNT){targets[i].frame=0;};

      if (targets[i].x < -TARGETSIZE) {
        targets[i].active = false;
        if (current_min_bullets<current_max_bullets){current_max_bullets--;};
      }
    }
  }


  /* ===== MOVE Explosions ===== */
  for (int i = 0; i < MAX_TARGETS; i++) {
    if (explosions[i].active) {
      explosions[i].y -= EXPLOSIONSPEED;
      explosions[i].lowery += EXPLOSIONSPEED;
      if ((explosions[i].y < -TARGETSIZE) &&(explosions[i].lowery> SCREEN_HEIGHT-TARGETSIZE))  {
          explosions[i].active = false;
      }
    }
  }


  /* ===== COLLISIONS ===== */
  for (int i = 0; i < MAX_TARGETS; i++) {
    if (!targets[i].active) continue;

    for (int b=0;b<MAX_BULLETS;b++){
    if (bullets[b].active &&
        bullets[b].x >= targets[i].x &&
        bullets[b].x <= targets[i].x + TARGETSIZE &&
        bullets[b].y >= targets[i].y &&
        bullets[b].y <= targets[i].y + TARGETSIZE) {

      bullets[b].active=false;
      targets[i].active = false;
      bulletActive = false;
      score++;
      if (current_max_bullets<MAX_BULLETS){current_max_bullets++;};
      shockwavePoints++;
      hitSound();
      spawnExplosion(targets[i].x,targets[i].y);
        }
    }

    if (targets[i].x <= SHIPSIZE &&
        targets[i].y + TARGETSIZE >= shipY &&
        targets[i].y <= shipY + SHIPSIZE) {

      gameOver = true;
    }
  }

  /* ===== DRAW ===== */
  gameFrame.fillScreen(FRAME_BLACK);
  gameFrame.setTextSize(2);
  gameFrame.setTextColor(FRAME_WHITE);

  gameFrame.setCursor(4, 4);
  gameFrame.print("Score:");
  gameFrame.print(score);
  gameFrame.print(" T:");
  gameFrame.print(elapsedSeconds());
  gameFrame.print(" B:");
  gameFrame.print(current_max_bullets);

  if (shockwavePoints >= POINTS_FOR_SHOCKWAVE) {
    gameFrame.fillRect(SCREEN_WIDTH - 40, 0, 40, 24, FRAME_DARKGREY);
    gameFrame.setCursor(SCREEN_WIDTH - 36, 4);
    gameFrame.print("SW");
  }

  if (shockwavePoints>0) {
    gameFrame.fillRect(max(SCREEN_WIDTH-POINTS_FOR_SHOCKWAVE,0), 26,
                       min(POINTS_FOR_SHOCKWAVE, shockwavePoints), 8, FRAME_WHITE);
  }

  if ((elapsedSeconds()/60)>current_min_bullets){current_min_bullets=elapsedSeconds()/60;};

  drawScaledBitmap(gameFrame, 0, shipY, shipBitmap, 5, 5, FRAME_WHITE);

  for (int i = 0; i < MAX_BULLETS; i++) {
    if (bullets[i].active) {
      gameFrame.drawBitmap(
        bullets[i].x - 1,
        bullets[i].y - 1,
        bulletFrames[bullets[i].frame],
        4,
        4,
        FRAME_WHITE
      );
    }
  }

  for (int i = 0; i < MAX_TARGETS; i++) {
    if (targets[i].active) {
      drawScaledBitmap(gameFrame, targets[i].x, targets[i].y,
                       ringFrames[targets[i].frame], 8, 8, FRAME_WHITE);
    }
  }

  for (int i = 0; i < MAX_TARGETS; i++) {
    if (explosions[i].active) {
      drawScaledBitmap(gameFrame, explosions[i].x, explosions[i].y,
                       EXPLODETOP, 8, 8, FRAME_WHITE);
      drawScaledBitmap(gameFrame, explosions[i].x, explosions[i].lowery,
                       EXPLODEBOT, 8, 8, FRAME_WHITE);
    }
  }

  if (shockwaveactive) {
    gameFrame.fillRect(max(shockwaveX, 0), 32, SHOCKWAVE_SIZE,
                       SCREEN_HEIGHT - 32, FRAME_WHITE);
  }

  presentGameFrame();
}
/* ===== LOOP ===== */
void loop() {
  looptimer=millis(); // record start of loop time
  updateTouch();

  switch (gamemode) {
    case GAMEMODE_SELECTOR:
      game_selector();
      break;  
    case GAMEMODE_BLASTER:
      blaster_game();
      break;
 
  }

  updateSound();

  while (millis()<looptimer+MINLOOPTIME)
    {
  delay(2);
  updateSound();
    };
}