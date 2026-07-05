// =============================================================================
// TANX - A classic tank artillery game inspired by Amiga games of the early 90s
// Built using the olc::PixelGameEngine (One Lone Coder)
// Two players take turns adjusting angle & power, then fire at each other
// across destructible terrain with wind physics.
// =============================================================================

#define OLC_PGE_APPLICATION
#include <cassert>
#include "olcPixelGameEngine.h"
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <cstring>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// --- Platform-specific socket headers ----------------------------------------
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET SocketHandle;
#  define INVALID_SOCK INVALID_SOCKET
#  define SOCK_ERR     SOCKET_ERROR
   static int NetErrno()        { return WSAGetLastError(); }
   static bool WouldBlock()     { return WSAGetLastError() == WSAEWOULDBLOCK; }
   static void CloseSocket(SocketHandle s) { closesocket(s); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int SocketHandle;
#  define INVALID_SOCK (-1)
#  define SOCK_ERR     (-1)
   static int  NetErrno()       { return errno; }
   static bool WouldBlock()     { return errno == EAGAIN || errno == EWOULDBLOCK; }
   static void CloseSocket(SocketHandle s) { close(s); }
#endif

static void SetNonBlocking(SocketHandle s) {
#ifdef _WIN32
    u_long m = 1; ioctlsocket(s, FIONBIO, &m);
#else
    int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

// Returns the best-guess LAN IP for this machine (IPv4)
static std::string GetLocalIP() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) return "?.?.?.?";
    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_INET;
    if (getaddrinfo(hostname, nullptr, &hints, &res) != 0) return "?.?.?.?";
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &((struct sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip));
    freeaddrinfo(res);
    return std::string(ip);
}

// --- Network message protocol -----------------------------------------------
constexpr uint16_t NET_PORT = 7890;

enum class NetMode { NONE, HOST, CLIENT };

enum class NetMsg : uint8_t {
    MATCH_START   = 1,  // host → client: GameSettings + player names
    ROUND_START   = 2,  // host → client: terrain array + positions + wind + gravity
    TURN_ACTION   = 3,  // acting → other: finalX, weapon, angle, power, flags
    TURN_RESULT   = 4,  // host → client: canonical HP/shields/ammo/pickup/wind
    DISCONNECT    = 5,
    PLAYER_NAME   = 6,  // client → host: the client's chosen display name
};

// Helper: write/read primitives into a byte buffer
struct NetBuf {
    std::vector<uint8_t> data;
    void writeU8(uint8_t  v) { data.push_back(v); }
    void writeI32(int32_t v) { uint32_t n = htonl((uint32_t)v); uint8_t b[4]; memcpy(b,&n,4); data.insert(data.end(),b,b+4); }
    void writeF32(float   v) { uint32_t n; memcpy(&n,&v,4); n=htonl(n); uint8_t b[4]; memcpy(b,&n,4); data.insert(data.end(),b,b+4); }
    void writeStr(const std::string& s) { writeU8((uint8_t)s.size()); data.insert(data.end(),s.begin(),s.end()); }
};
struct NetReader {
    const uint8_t* d; size_t pos = 0;
    uint8_t  readU8()  { return d[pos++]; }
    int32_t  readI32() { uint32_t n; memcpy(&n,d+pos,4); pos+=4; return (int32_t)ntohl(n); }
    float    readF32() { uint32_t n; memcpy(&n,d+pos,4); pos+=4; n=ntohl(n); float v; memcpy(&v,&n,4); return v; }
    std::string readStr() { uint8_t len=readU8(); std::string s((char*)d+pos,len); pos+=len; return s; }
};

// --- Game constants ---------------------------------------------------------

constexpr int SCREEN_W = 1024;
constexpr int SCREEN_H = 768;
constexpr int UI_HEIGHT = 180;
constexpr int GAME_TOP = UI_HEIGHT;
constexpr int GAME_HEIGHT = SCREEN_H - UI_HEIGHT;

constexpr float EXPLOSION_RADIUS = 30.0f;
constexpr int TANK_WIDTH = 24;
constexpr int TANK_HEIGHT = 12;
constexpr int BARREL_LENGTH = 16;
constexpr int MAX_POWER = 100;
// startingHP and moveBudget are now configurable in GameSettings below

// --- Game state machine -----------------------------------------------------
// TITLE -> MENU -> AIM <-> FIRING -> EXPLOSION -> NEXT_TURN -> AIM ...
// When a tank is destroyed: -> GAME_OVER -> MENU (match over) or AIM (next round)

enum class GameState {
    TITLE,
    MENU,       // Settings screen before the game starts
    LOBBY,      // Network: choose Host/Join, enter IP, wait for connection
    AIM,
    FIRING,
    EXPLOSION,
    LASER_FIRE, // Instant beam weapon animating
    NEXT_TURN,
    GAME_OVER
};

// --- Input mode for typed angle/power entry ---------------------------------

enum class InputMode {
    NONE,
    TYPING_ANGLE,
    TYPING_POWER
};

// --- Special weapons ----------------------------------------------------------
// NORMAL has infinite ammo. The others are scarce and selected from the HUD.

enum class WeaponType {
    NORMAL,
    HE,         // High Explosive: same shot, double explosion radius
    CLUSTER,    // Splits into 3 normal-explosion shells at the apex of flight
    LASER       // Instant beam along the chosen angle, cuts a trench through terrain
};

// --- Settings chosen on the menu screen -------------------------------------

struct GameSettings {
    std::string playerNames[2] = {"Megan", "Leia"};
    int windSetting = 4;       // 0=None, 1=Light, 2=Medium, 3=Strong, 4=Random
    int gravitySetting = 1;    // 0=Light, 1=Medium, 2=Strong, 3=Random
    int landscapeSetting = 2;  // 0=Mountains, 1=Foothills, 2=Random
    int roundsToWin = 3;       // 1, 3, 5, or 7
    bool nightMode = false;    // darkness + lightning + NVG pickups

    // Per-match gameplay values (configurable from the menu)
    int startingHP = 3;         // 1-5
    int moveBudget = 75;        // 20-100 step 10
    int startAmmoHE = 1;        // 0-5
    int startAmmoCluster = 2;   // 0-5
    int startAmmoLaser = 1;     // 0-5
    int startAmmoBallistics = 1;// 0-5
    int startAmmoShield = 1;    // 0-5
};

// --- Data structures --------------------------------------------------------

struct Tank {
    float x, y;
    int angle;
    int power;
    int hp;
    int movesLeft;
    int score;
    olc::Pixel colour;
    olc::Pixel barrelColour;

    // Special weapon ammo (match-wide, set in StartNewMatch)
    int ammoHE = 1;
    int ammoCluster = 2;
    int ammoLaser = 1;
    int ammoBallistics = 1;
    int ammoShield = 1;

    // Active shield: 0 = none, 1-3 = remaining protective layers
    int shieldCharges = 0;

    // Night Vision Goggles: turns remaining, 0 = inactive
    int nvgTurns = 0;
};

struct Projectile {
    float x, y;
    float vx, vy;
    bool active;
    WeaponType weapon = WeaponType::NORMAL;
    bool hasSplit = false;   // cluster shells split once at their apex
    std::vector<olc::vf2d> trail;
};

struct Explosion {
    float x, y;
    float radius;
    float maxRadius;
    float timer;
    bool active;
};

// --- Map pickups --------------------------------------------------------------
// At most one pickup exists on the map at a time, encouraging tanks to move
// toward it rather than camping. Driving over it (matching x position) collects it.

enum class PickupType {
    MYSTERY, // random ammo refill for one of the special weapons
    HEALTH,  // +1 HP, capped at settings.startingHP
    NVG      // night-vision goggles — full visibility for 5 turns (night rounds only)
};

struct Pickup {
    float x, y;
    PickupType type;
    bool active = false;
};

// =============================================================================
// Main game class
// =============================================================================

class Tanx : public olc::PixelGameEngine {
public:
    Tanx() { sAppName = "TANX"; }

private:
    // -- Terrain --
    std::vector<float> terrain;
    std::vector<olc::Pixel> terrainColour;

    // -- Game objects --
    Tank tanks[2];
    std::vector<Projectile> projectiles; // multiple at once to support cluster munitions
    std::vector<Explosion> explosions;

    // -- Special weapons --
    WeaponType selectedWeapon = WeaponType::NORMAL;
    bool reticleActive = false;       // ballistics computer target reticle, shown this turn
    std::vector<olc::vf2d> laserTrail; // points along the laser beam, for drawing

    // -- Map pickups --
    Pickup pickup;
    float pickupRespawnTimer = 0; // counts down after a pickup is collected
    std::string pickupMessage;
    float pickupMessageTimer = 0; // counts down while the "what was it" popup shows
    float worldTime = 0;          // unconditional clock for ambient animation (bobbing etc.)

    // -- Night mode --
    float lightningTimer = 0;              // counts down to next lightning strike
    float lightningFlash = 0;             // > 0 while the battlefield is lit up post-strike
    std::vector<olc::vf2d> lightningBolt; // jagged bolt geometry, regenerated each strike

    // -- Screen shake --
    float shakeTimer = 0;              // > 0 while shaking
    float shakeMag   = 0;              // starting magnitude in pixels
    int   shakeOffX  = 0;             // pixel offset applied this frame
    int   shakeOffY  = 0;
    olc::Sprite* pFrameBuf = nullptr; // full-screen render target; blit with offset

    // -- Easter egg: name yourself "Daddy" and triple-click during your turn
    // to toggle a perfect trajectory preview line for that tank --
    int cheatClickCount = 0;
    float cheatClickTimer = 0;
    bool trajectoryCheat[2] = {false, false};

    // -- Game state --
    GameState state;
    int currentPlayer;
    float wind;
    float stateTimer;
    int roundNumber;

    // -- Type-in input (during AIM phase) --
    InputMode inputMode;
    std::string inputBuffer;
    float cursorBlink;

    // -- Repeat timers for held keys/buttons --
    float repeatTimer;          // keyboard repeat timer
    float mouseRepeatTimer;     // mouse button repeat timer
    float repeatDelay = 0.3f;   // initial delay before repeating starts
    float repeatRate = 0.08f;   // seconds between repeats once going
    float frameTime;            // current frame's elapsed time (for use in DrawUI)

    // -- Sound (cross-platform via miniaudio) --
    ma_engine audioEngine;
    std::string explosionWavPath;
    std::string clickWavPath;
    std::string thunderWavPath;
    ma_sound whistleSound;
    bool whistleSoundLoaded = false; // true while whistleSound is initialised and may need stopping

    // -- Surrender --
    bool surrendered = false; // true while the GAME_OVER screen is showing a surrender result
    bool drawGame = false;    // true when both tanks die simultaneously or a round times out
    int turnsSinceHit = 0;    // stalemate counter: increments each turn; resets on any hit

    // -- Networking --
    NetMode       netMode       = NetMode::NONE;
    SocketHandle  listenSock    = INVALID_SOCK; // host: accept socket
    SocketHandle  remoteSock    = INVALID_SOCK; // connected peer
    int           localPlayer   = 0;            // which player index this machine controls
    bool          netConnected  = false;
    bool          waitForResult = false;        // client waits for TURN_RESULT after explosion
    bool          hasPendingResult = false;     // TURN_RESULT arrived mid-animation; apply when ready
    std::vector<uint8_t> pendingResultData;    // buffered TURN_RESULT payload
    float         nextWind = 0;               // HOST pre-computes next wind; CLIENT reads from TURN_RESULT
    std::string   localIP;
    std::string   netIPInput;                   // client: typed target IP
    std::string   netLobbyName;                 // name typed in the lobby before connecting
    bool          netEditingName = false;        // is the name field active?
    bool          netReadyToStart = false;       // host: true once client name received
    bool          netNameSent = false;           // client: has name been sent to host yet?
    float         netLobbyBlink = 0;
    std::vector<uint8_t> recvBuf;              // raw TCP stream buffer

    // -- Settings & menu --
    GameSettings settings;
    int menuEditingName;        // -1=none, 0=editing P1 name, 1=editing P2 name
    std::string menuNameBuffer;
    std::string menuNameBeforeEdit; // fallback if the field is left empty on commit
    float menuCursorBlink;

    // -- Active gameplay values (derived from settings each round) --
    float activeGravity;
    float activeWindMax;

    // =========================================================================
    // SOUND GENERATION
    // =========================================================================

    // Cross-platform temp file path (%TEMP% on Windows, /tmp or $TMPDIR elsewhere)
    std::string GetTempFilePath(const std::string& filename) {
        return (std::filesystem::temp_directory_path() / filename).string();
    }

    // Write mono 16-bit PCM samples to a WAV file
    void WriteWavFile(const std::string& path, const std::vector<int16_t>& samples, int sampleRate) {
        int dataSize = (int)samples.size() * 2;
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return;

        fwrite("RIFF", 1, 4, f);
        int32_t riffSize = 36 + dataSize; fwrite(&riffSize, 4, 1, f);
        fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f);
        int32_t fmtSize = 16; fwrite(&fmtSize, 4, 1, f);
        int16_t pcm = 1; fwrite(&pcm, 2, 1, f);
        int16_t mono = 1; fwrite(&mono, 2, 1, f);
        int32_t sr = sampleRate; fwrite(&sr, 4, 1, f);
        int32_t byteRate = sampleRate * 2; fwrite(&byteRate, 4, 1, f);
        int16_t blockAlign = 2; fwrite(&blockAlign, 2, 1, f);
        int16_t bps = 16; fwrite(&bps, 2, 1, f);
        fwrite("data", 1, 4, f);
        int32_t ds = dataSize; fwrite(&ds, 4, 1, f);
        fwrite(samples.data(), 2, samples.size(), f);
        fclose(f);
    }

    // Generate a short procedural explosion WAV: noise burst + low rumble with decay
    void GenerateExplosionWav(const std::string& path) {
        int sampleRate = 22050;
        int numSamples = (int)(sampleRate * 0.35f);
        std::vector<int16_t> samples(numSamples);

        for (int i = 0; i < numSamples; i++) {
            float t = (float)i / sampleRate;
            float decay = exp(-t * 12.0f);
            float noise = ((float)(rand() % 65536) - 32768.0f) / 32768.0f;
            float rumble = sin(t * 60.0f * 2.0f * 3.14159f);
            float sample = (noise * 0.6f + rumble * 0.4f) * decay;
            samples[i] = (int16_t)(sample * 28000);
        }

        WriteWavFile(path, samples, sampleRate);
    }

    // Generate a short mechanical "click/thunk" for the plunger fire button
    void GenerateClickWav(const std::string& path) {
        int sampleRate = 22050;
        int numSamples = (int)(sampleRate * 0.15f);
        std::vector<int16_t> samples(numSamples);

        for (int i = 0; i < numSamples; i++) {
            float t = (float)i / sampleRate;
            float decay = exp(-t * 35.0f);
            float click = (t < 0.008f) ? (((float)(rand() % 2000) - 1000.0f) / 1000.0f) : 0.0f;
            float thud = sin(t * 110.0f * 2.0f * 3.14159f) * decay;
            float sample = click * 0.6f + thud * 0.8f;
            samples[i] = (int16_t)(std::clamp(sample, -1.0f, 1.0f) * 26000);
        }

        WriteWavFile(path, samples, sampleRate);
    }

    // Generate a thunder crack + rolling rumble for lightning strikes (~1.8 s)
    void GenerateThunderWav(const std::string& path) {
        int sampleRate = 22050;
        int numSamples = (int)(sampleRate * 1.8f);
        std::vector<int16_t> samples(numSamples);

        for (int i = 0; i < numSamples; i++) {
            float t = (float)i / sampleRate;

            // Sharp initial crack — broadband noise, very fast decay
            float crackDecay = exp(-t * 60.0f);
            float crack = ((float)(rand() % 65536) - 32768.0f) / 32768.0f * crackDecay;

            // Rolling rumble — low-frequency noise, long slow decay
            float rumbleDecay = exp(-t * 1.8f);
            float rumble = sin(t * 55.0f  * 2.0f * 3.14159f) * 0.4f
                         + sin(t * 38.0f  * 2.0f * 3.14159f) * 0.3f
                         + sin(t * 22.0f  * 2.0f * 3.14159f) * 0.3f;
            rumble *= rumbleDecay;

            // Light noise texture over the rumble
            float noiseDecay = exp(-t * 2.5f);
            float noise = ((float)(rand() % 2000) - 1000.0f) / 1000.0f * noiseDecay * 0.2f;

            float sample = crack * 0.6f + rumble * 0.55f + noise;
            samples[i] = (int16_t)(std::clamp(sample, -1.0f, 1.0f) * 28000);
        }

        WriteWavFile(path, samples, sampleRate);
    }

    void PlayThunderSound() {
        ma_engine_play_sound(&audioEngine, thunderWavPath.c_str(), NULL);
    }

    // Generate a whistling shell WAV: pitch rises from launch to apex, then falls
    // back down until impact. Higher power raises the peak pitch.
    void GenerateWhistleWav(const std::string& path, float duration, float apexTime, int power) {
        int sampleRate = 22050;
        int numSamples = std::max(1, (int)(sampleRate * duration));
        std::vector<int16_t> samples(numSamples);

        float baseFreq = 200.0f;
        float peakFreq = 300.0f + power * 8.0f; // more power -> higher peak pitch

        float phase = 0;
        float fadeTime = 0.03f;
        for (int i = 0; i < numSamples; i++) {
            float t = (float)i / sampleRate;

            float freq;
            if (apexTime <= 0.01f) {
                // Flat shot: no ascent, whistle just falls from peak to base
                float frac = t / duration;
                freq = peakFreq - (peakFreq - baseFreq) * frac;
            } else if (t < apexTime) {
                float frac = t / apexTime;
                freq = baseFreq + (peakFreq - baseFreq) * frac;
            } else {
                float frac = (t - apexTime) / std::max(0.01f, duration - apexTime);
                freq = peakFreq - (peakFreq - baseFreq) * frac;
            }

            // Integrate phase rather than using t*freq directly, so the
            // frequency can change smoothly without clicks/discontinuities
            phase += 2.0f * 3.14159f * freq / sampleRate;
            if (phase > 2.0f * 3.14159f) phase -= 2.0f * 3.14159f;

            // Fade in/out to avoid clicks at the start and end of the clip
            float env = 1.0f;
            if (t < fadeTime) env = t / fadeTime;
            if (t > duration - fadeTime) env = (duration - t) / fadeTime;
            env = std::clamp(env, 0.0f, 1.0f);

            float tone = sin(phase);
            float breath = ((float)(rand() % 2000) - 1000.0f) / 1000.0f * 0.06f;
            float sample = (tone * 0.85f + breath) * env * 0.35f;

            samples[i] = (int16_t)(std::clamp(sample, -1.0f, 1.0f) * 28000);
        }

        WriteWavFile(path, samples, sampleRate);
    }

    // Fire-and-forget one-shots — miniaudio manages their lifetime internally
    void PlayExplosionSound() {
        ma_engine_play_sound(&audioEngine, explosionWavPath.c_str(), NULL);
    }

    void PlayPlungerSound() {
        ma_engine_play_sound(&audioEngine, clickWavPath.c_str(), NULL);
    }

    // Stop and free the current whistle sound, if any
    void StopWhistleSound() {
        if (whistleSoundLoaded) {
            ma_sound_stop(&whistleSound);
            ma_sound_uninit(&whistleSound);
            whistleSoundLoaded = false;
        }
    }

    // Run a lightweight forward simulation (same physics as the FIRING phase)
    // to estimate how long the shell will be in flight, for sizing the whistle.
    float SimulateFlightTime(float x, float y, float vx, float vy) {
        const float dt = 0.01f;
        int opponent = 1 - currentPlayer;
        float t = 0;
        for (int i = 0; i < 2000; i++) {
            vy += activeGravity * dt;
            vx += wind * 5.0f * dt;
            x += vx * dt;
            y += vy * dt;
            t += dt;

            if (CheckTankHit(opponent, x, y)) return t;
            int px = (int)x;
            if (px >= 0 && px < SCREEN_W && y >= terrain[px]) return t;
            if (x < -50 || x > SCREEN_W + 50 || y > SCREEN_H + 50) return t;
        }
        return t;
    }

    // --- Easter egg: "Daddy" trajectory cheat -------------------------------

    // Watch for the secret unlock: current player named "Daddy" (any case)
    // triple-clicking within a short window toggles the trajectory cheat for them.
    void UpdateCheatDetection(float fElapsedTime) {
        std::string name = settings.playerNames[currentPlayer];
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name != "daddy") {
            cheatClickCount = 0;
            return;
        }

        if (cheatClickCount > 0) {
            cheatClickTimer += fElapsedTime;
            if (cheatClickTimer > 1.0f) {
                cheatClickCount = 0;
                cheatClickTimer = 0;
            }
        }

        if (GetMouse(0).bPressed) {
            cheatClickCount++;
            cheatClickTimer = 0;
            if (cheatClickCount >= 3) {
                trajectoryCheat[currentPlayer] = !trajectoryCheat[currentPlayer];
                cheatClickCount = 0;
                ShowPickupMessage(trajectoryCheat[currentPlayer]
                    ? "DADDY MODE ACTIVATED!!!"
                    : "Daddy mode deactivated.");
            }
        }
    }

    // Forward-simulate the current angle/power/wind/gravity to trace exactly
    // where this shot will land, for the trajectory cheat line.
    std::vector<olc::vf2d> SimulateTrajectoryPoints() {
        std::vector<olc::vf2d> points;
        Tank& t = tanks[currentPlayer];
        float rad = GetBarrelRad(currentPlayer);
        float speed = t.power * 4.0f;
        float x = t.x + cos(rad) * BARREL_LENGTH;
        float y = t.y - TANK_HEIGHT - sin(rad) * BARREL_LENGTH;
        float vx = cos(rad) * speed;
        float vy = -sin(rad) * speed;

        int opponent = 1 - currentPlayer;
        const float dt = 0.02f;
        for (int i = 0; i < 500; i++) {
            vy += activeGravity * dt;
            vx += wind * 5.0f * dt;
            x += vx * dt;
            y += vy * dt;
            points.push_back({x, y});

            if (CheckTankHit(opponent, x, y)) break;
            int px = (int)x;
            if (px < 0 || px >= SCREEN_W) break;
            if (y >= terrain[px]) break;
            if (y > SCREEN_H + 50) break;
        }
        return points;
    }

    // Generate and start the whistle sound for the shell that was just fired
    void PlayWhistleSound() {
        StopWhistleSound();
        if (projectiles.empty()) return;

        Tank& t = tanks[currentPlayer];
        Projectile& p = projectiles[0];
        float duration = SimulateFlightTime(p.x, p.y, p.vx, p.vy);
        duration = std::clamp(duration * 1.15f, 0.2f, 4.0f); // small safety margin

        float apexTime = std::clamp(-p.vy / activeGravity, 0.0f, duration);

        std::string wavPath = GetTempFilePath("tanx_whistle.wav");
        GenerateWhistleWav(wavPath, duration, apexTime, t.power);

        ma_sound_init_from_file(&audioEngine, wavPath.c_str(), 0, NULL, NULL, &whistleSound);
        whistleSoundLoaded = true;
        ma_sound_start(&whistleSound);
    }

    // =========================================================================
    // NETWORKING
    // =========================================================================
    //
    // Architecture overview
    // ---------------------
    // One machine is the HOST (always Player 1) and one is the CLIENT (Player 2).
    // The HOST is authoritative: it runs the game simulation, resolves outcomes,
    // and sends the canonical state to the CLIENT after every turn.
    //
    // Roles:
    //   HOST  — generates terrain, resolves physics, sends TURN_RESULT
    //   CLIENT — receives state, mirrors simulation locally for smooth display
    //
    // Simultaneous fire (no lag on LAN)
    // -----------------------------------
    // When a player fires, TURN_ACTION is sent immediately. Both machines then
    // run the same physics simulation at the same time. Because angle, power,
    // wind and gravity are identical on both, the projectile follows the same
    // path on both screens without waiting for the network.
    // After the turn resolves, the HOST sends TURN_RESULT so the CLIENT snaps
    // to the authoritative state — correcting any tiny float drift.
    //
    // waitForResult flag
    // ------------------
    // After any turn ends the CLIENT sets waitForResult = true. This blocks
    // the state machine (but keeps rendering) until TURN_RESULT arrives and
    // clears the flag. This ensures the CLIENT never advances to the next turn
    // with stale ammo / HP / terrain.
    //
    // Terrain sync
    // ------------
    // The HOST generates terrain with rand() which would differ on another
    // platform/compiler with the same seed. Instead the HOST sends all 800
    // height floats (~3 KB) to the CLIENT at the start of every round. The
    // CLIENT overwrites its terrain array directly and never calls
    // GenerateTerrain() itself. The same approach is used for craters —
    // TURN_RESULT re-sends the full terrain array after each explosion.
    //
    // Wire format
    // -----------
    // TCP on port 7890. Each packet:
    //   [1 byte type | 3 bytes payload-length] [payload bytes...]
    // All multi-byte integers are sent in network byte order (big-endian).
    // =========================================================================

    // Frame and transmit one packet over the TCP connection.
    // Header packs the message type (top byte) and payload length (24 bits)
    // into a single uint32 sent in network byte order.
    void NetSend(NetMsg type, const std::vector<uint8_t>& payload) {
        if (remoteSock == INVALID_SOCK) return;
        uint32_t header = htonl(((uint32_t)type << 24) | (uint32_t)payload.size());
        send(remoteSock, (const char*)&header, 4, 0);
        if (!payload.empty()) send(remoteSock, (const char*)payload.data(), (int)payload.size(), 0);
    }

    // Called every frame during active gameplay.
    // Uses non-blocking I/O: recv() returns immediately whether or not data is
    // waiting. Incoming bytes accumulate in recvBuf; complete packets are
    // extracted and dispatched to NetHandleMessage() one at a time.
    void NetUpdate() {
        // HOST: check for a new incoming connection (lobby waiting phase)
        if (netMode == NetMode::HOST && listenSock != INVALID_SOCK && remoteSock == INVALID_SOCK) {
            sockaddr_in ca{}; socklen_t cl = sizeof(ca);
            SocketHandle c = accept(listenSock, (sockaddr*)&ca, &cl);
            if (c != INVALID_SOCK) {
                remoteSock = c;
                SetNonBlocking(remoteSock);
                netConnected = true;
            }
        }
        if (remoteSock == INVALID_SOCK) return;

        // CLIENT: first time connected, immediately tell the host our display name
        // so the host can include it in MATCH_START and both screens show both names
        if (netMode == NetMode::CLIENT && netConnected && !netNameSent) {
            NetBuf b; b.writeStr(settings.playerNames[1]);
            NetSend(NetMsg::PLAYER_NAME, b.data);
            netNameSent = true;
        }

        // Drain whatever bytes the OS has buffered into our stream buffer
        char chunk[4096];
        int n = recv(remoteSock, chunk, sizeof(chunk), 0);
        if (n > 0) {
            recvBuf.insert(recvBuf.end(), chunk, chunk + n);
        } else if (n == 0) {
            NetClose(); return;  // graceful disconnect
        } else if (n < 0 && !WouldBlock()) {
            NetClose(); return;  // error
        }

        // Extract all complete packets from the stream buffer.
        // TCP can deliver multiple packets in one recv() call, or split a single
        // packet across multiple calls — the buffer handles both cases.
        while (recvBuf.size() >= 4) {
            uint32_t hdr; memcpy(&hdr, recvBuf.data(), 4); hdr = ntohl(hdr);
            NetMsg type = (NetMsg)(hdr >> 24);
            uint32_t plen = hdr & 0xFFFFFF;
            if (recvBuf.size() < 4 + plen) break;  // packet not yet fully arrived
            std::vector<uint8_t> pkt(recvBuf.begin() + 4, recvBuf.begin() + 4 + plen);
            recvBuf.erase(recvBuf.begin(), recvBuf.begin() + 4 + plen);
            NetHandleMessage(type, pkt);
        }
    }

    void NetClose() {
        if (remoteSock != INVALID_SOCK) { CloseSocket(remoteSock); remoteSock = INVALID_SOCK; }
        if (listenSock != INVALID_SOCK) { CloseSocket(listenSock); listenSock = INVALID_SOCK; }
        netConnected = false;
    }

    // HOST: bind and listen on NET_PORT. Sets localPlayer = 0 (HOST is always P1).
    bool NetStartHost() {
        listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock == INVALID_SOCK) return false;
        int yes = 1; setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        SetNonBlocking(listenSock);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(NET_PORT); addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) < 0) { CloseSocket(listenSock); listenSock = INVALID_SOCK; return false; }
        listen(listenSock, 1);
        localPlayer = 0;
        localIP = GetLocalIP();
        return true;
    }

    // CLIENT: connect to the host's IP on NET_PORT. Sets localPlayer = 1 (CLIENT is always P2).
    bool NetConnectToHost(const std::string& ip) {
        remoteSock = socket(AF_INET, SOCK_STREAM, 0);
        if (remoteSock == INVALID_SOCK) return false;
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(NET_PORT);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        if (connect(remoteSock, (sockaddr*)&addr, sizeof(addr)) < 0 && !WouldBlock()) {
            CloseSocket(remoteSock); remoteSock = INVALID_SOCK; return false;
        }
        SetNonBlocking(remoteSock);
        localPlayer = 1;
        netConnected = true;
        return true;
    }

    // Returns true when it is this machine's player's turn to act
    bool IsLocalTurn() const { return netMode == NetMode::NONE || currentPlayer == localPlayer; }

    // Mark the CLIENT as ready to receive the HOST's canonical state.
    // If a TURN_RESULT arrived early (mid-animation) it was buffered — flush it now.
    void SetWaitForResult() {
        SetWaitForResult();
        if (hasPendingResult) {
            std::vector<uint8_t> saved = pendingResultData;
            hasPendingResult = false; pendingResultData.clear();
            NetHandleMessage(NetMsg::TURN_RESULT, saved);
        }
    }

    // ---- Message builders ---------------------------------------------------
    // Each function serialises a specific message payload into a NetBuf and
    // calls NetSend(). All values use network byte order (big-endian).

    // Sent once by HOST after both players have connected.
    // Contains all match configuration so the CLIENT uses identical settings.
    void NetSendMatchStart() {
        NetBuf b;
        b.writeStr(settings.playerNames[0]); b.writeStr(settings.playerNames[1]);
        b.writeU8((uint8_t)settings.windSetting); b.writeU8((uint8_t)settings.gravitySetting);
        b.writeU8((uint8_t)settings.landscapeSetting); b.writeU8((uint8_t)settings.roundsToWin);
        b.writeU8((uint8_t)settings.startingHP); b.writeI32(settings.moveBudget);
        b.writeU8((uint8_t)settings.startAmmoHE); b.writeU8((uint8_t)settings.startAmmoCluster);
        b.writeU8((uint8_t)settings.startAmmoLaser); b.writeU8((uint8_t)settings.startAmmoBallistics);
        b.writeU8((uint8_t)settings.startAmmoShield);
        b.writeU8(settings.nightMode ? 1 : 0);
        NetSend(NetMsg::MATCH_START, b.data);
    }

    // Sent by HOST at the start of every round.
    // The full terrain array (800 floats, ~3 KB) is sent rather than a seed
    // because rand() is not guaranteed to produce the same sequence on different
    // platforms/compilers, so both machines would diverge with the same seed.
    void NetSendRoundStart() {
        NetBuf b;
        b.writeI32(roundNumber);
        for (float h : terrain) b.writeF32(h);  // 800 × 4 bytes = 3,200 bytes
        for (int i = 0; i < 2; i++) { b.writeF32(tanks[i].x); b.writeF32(tanks[i].y); }
        b.writeF32(wind); b.writeF32(activeGravity);
        // Send initial pickup state so the CLIENT does NOT call SpawnPickup()
        // with its own rand() — that would produce a different position/type.
        b.writeU8(pickup.active ? 1 : 0);
        if (pickup.active) { b.writeF32(pickup.x); b.writeF32(pickup.y); b.writeU8((uint8_t)pickup.type); }
        NetSend(NetMsg::ROUND_START, b.data);
    }

    // Sent by the ACTING PLAYER when they commit their turn (fire / skip / surrender).
    // Received by the other machine which applies the same action locally so
    // both simulations start from exactly the same state at the same moment.
    void NetSendTurnAction(bool skip, bool surrender) {
        NetBuf b;
        Tank& t = tanks[currentPlayer];
        b.writeI32((int32_t)t.x);          // final tank X after all movement this turn
        b.writeU8((uint8_t)selectedWeapon); // weapon type (affects Fire() behaviour)
        b.writeI32(t.angle);               // barrel angle  (0 = toward enemy, 90 = up)
        b.writeI32(t.power);               // firing power  (5–100)
        b.writeU8(skip ? 1 : 0);
        b.writeU8(surrender ? 1 : 0);
        NetSend(NetMsg::TURN_ACTION, b.data);
    }

    // Sent by HOST at the end of EVERY turn (hit, miss, skip, laser).
    // This is the single authoritative sync point: the CLIENT applies this
    // snapshot and snaps to the HOST's ground truth before the next turn begins.
    // Terrain is re-sent in full after every explosion so crater damage stays
    // pixel-identical on both machines.
    void NetSendTurnResult() {
        // HOST pre-computes the wind for the NEXT turn here so the CLIENT uses
        // the exact same value. Without this both machines call rand() independently
        // in NEXT_TURN and the wind diverges, causing different projectile paths.
        nextWind = wind;
        if (activeWindMax > 0) {
            nextWind += ((rand() % 100) - 50) / 20.0f;
            nextWind = std::clamp(nextWind, -activeWindMax, activeWindMax);
        }

        NetBuf b;
        for (int i = 0; i < 2; i++) {
            b.writeI32(tanks[i].hp); b.writeI32(tanks[i].shieldCharges); b.writeI32(tanks[i].nvgTurns);
            b.writeI32(tanks[i].score);
            b.writeU8((uint8_t)tanks[i].ammoHE); b.writeU8((uint8_t)tanks[i].ammoCluster);
            b.writeU8((uint8_t)tanks[i].ammoLaser); b.writeU8((uint8_t)tanks[i].ammoBallistics);
            b.writeU8((uint8_t)tanks[i].ammoShield);
        }
        for (float h : terrain) b.writeF32(h);
        b.writeU8(pickup.active ? 1 : 0);
        if (pickup.active) { b.writeF32(pickup.x); b.writeF32(pickup.y); b.writeU8((uint8_t)pickup.type); }
        b.writeF32(nextWind);  // authoritative wind for the NEXT turn — CLIENT must use this
        NetSend(NetMsg::TURN_RESULT, b.data);
    }

    // ---- Message handlers ---------------------------------------------------
    // Dispatched from NetUpdate() whenever a complete packet is extracted from
    // the TCP stream buffer.

    void NetHandleMessage(NetMsg type, const std::vector<uint8_t>& pkt) {
        NetReader r{pkt.data()};
        switch (type) {

        // CLIENT receives: copy all settings from the HOST so both machines run
        // with identical configuration for the whole match.
        case NetMsg::MATCH_START: {
            settings.playerNames[0] = r.readStr(); settings.playerNames[1] = r.readStr();
            settings.windSetting = r.readU8(); settings.gravitySetting = r.readU8();
            settings.landscapeSetting = r.readU8(); settings.roundsToWin = r.readU8();
            settings.startingHP = r.readU8(); settings.moveBudget = r.readI32();
            settings.startAmmoHE = r.readU8(); settings.startAmmoCluster = r.readU8();
            settings.startAmmoLaser = r.readU8(); settings.startAmmoBallistics = r.readU8();
            settings.startAmmoShield = r.readU8();
            settings.nightMode = (r.readU8() != 0);
            tanks[0].score = 0; tanks[1].score = 0;
            break;
        }

        // CLIENT receives: overwrite local terrain with the HOST's exact array
        // (avoids platform rand() divergence), then initialise all round state.
        case NetMsg::ROUND_START: {
            roundNumber = r.readI32();
            terrain.resize(SCREEN_W);
            terrainColour.resize(SCREEN_W * GAME_HEIGHT);
            for (float& h : terrain) h = r.readF32();
            GenerateTerrainTexture();
            for (int i = 0; i < 2; i++) { tanks[i].x = r.readF32(); tanks[i].y = r.readF32(); }
            wind = r.readF32(); activeGravity = r.readF32();

            for (int i = 0; i < 2; i++) {
                tanks[i].hp = settings.startingHP;
                tanks[i].movesLeft = settings.moveBudget;
                tanks[i].ammoHE = settings.startAmmoHE;
                tanks[i].ammoCluster = settings.startAmmoCluster;
                tanks[i].ammoLaser = settings.startAmmoLaser;
                tanks[i].ammoBallistics = settings.startAmmoBallistics;
                tanks[i].ammoShield = settings.startAmmoShield;
                tanks[i].shieldCharges = 0;
                tanks[i].angle = 45; tanks[i].power = 50;
            }
            tanks[0].colour = olc::Pixel(60,140,60);  tanks[0].barrelColour = olc::Pixel(40,100,40);
            tanks[1].colour = olc::Pixel(160,60,60);  tanks[1].barrelColour = olc::Pixel(120,40,40);

            projectiles.clear(); explosions.clear(); laserTrail.clear();
            selectedWeapon = WeaponType::NORMAL; reticleActive = false;
            currentPlayer = 0; inputMode = InputMode::NONE; inputBuffer.clear();
            drawGame = false; turnsSinceHit = 0; surrendered = false;
            // Read the HOST's pickup state directly — do NOT call SpawnPickup()
            // here as it uses rand() and would produce a different position/type.
            pickup.active = r.readU8() != 0;
            if (pickup.active) { pickup.x = r.readF32(); pickup.y = r.readF32(); pickup.type = (PickupType)r.readU8(); pickup.y = terrain[(int)pickup.x]; }
            pickupRespawnTimer = 0; hasPendingResult = false; waitForResult = false;

            state = GameState::AIM; stateTimer = 0;
            break;
        }

        // REMOTE machine receives the acting player's committed action.
        // The remote applies the final position, angle, power and weapon then
        // calls Fire() (or handles skip/surrender). Both machines now run the
        // exact same physics from the same starting state — simultaneous fire.
        //
        // Guard: if we are not in AIM state the game is mid-simulation and we
        // cannot safely start a new shot. This can happen if the HOST fires
        // immediately after a very short NEXT_TURN pause and the packet races
        // the CLIENT's state transition. Drop and let the state machine catch up
        // — this case should not occur in normal play with the 0.5s pause.
        case NetMsg::TURN_ACTION: {
            if (state != GameState::AIM) break;

            int finalX = r.readI32();
            selectedWeapon = (WeaponType)r.readU8();
            int angle = r.readI32(); int power = r.readI32();
            bool skip = r.readU8() != 0; bool surrender = r.readU8() != 0;

            Tank& t = tanks[currentPlayer];
            t.x = (float)finalX; t.y = terrain[std::clamp(finalX, 0, SCREEN_W - 1)];
            t.angle = angle; t.power = power;

            if (surrender) { SurrenderMatch(); }
            else if (skip) {
                // HOST received skip from CLIENT: send canonical state back then advance
                if (netMode == NetMode::HOST) { NetSendTurnResult(); }
                state = GameState::NEXT_TURN; stateTimer = 0;
            }
            else { Fire(); }  // both machines now fire simultaneously
            break;
        }

        // CLIENT receives: apply the HOST's authoritative end-of-turn snapshot.
        // Overwrites HP, shields, ammo, scores, terrain, pickup and wind so any
        // tiny divergence accumulated during simulation is corrected before the
        // next turn. Clears waitForResult to unblock the CLIENT state machine.
        //
        // If TURN_RESULT arrives while the CLIENT's own animation is still
        // running (FIRING/EXPLOSION/LASER_FIRE) — which happens when the HOST
        // resolves the shot faster than the CLIENT's local sim — we buffer the
        // packet and re-process it once the CLIENT's animation completes
        // naturally. This avoids the "projectile frozen for a few frames" bug.
        case NetMsg::TURN_RESULT: {
            if (netMode == NetMode::CLIENT &&
                (state == GameState::FIRING || state == GameState::EXPLOSION
                 || state == GameState::LASER_FIRE) && !waitForResult) {
                hasPendingResult = true;
                pendingResultData = pkt;
                break;
            }
            // Apply the snapshot
            for (int i = 0; i < 2; i++) {
                tanks[i].hp = r.readI32(); tanks[i].shieldCharges = r.readI32(); tanks[i].nvgTurns = r.readI32();
                tanks[i].score = r.readI32();
                tanks[i].ammoHE = r.readU8(); tanks[i].ammoCluster = r.readU8();
                tanks[i].ammoLaser = r.readU8(); tanks[i].ammoBallistics = r.readU8();
                tanks[i].ammoShield = r.readU8();
            }
            for (float& h : terrain) h = r.readF32();
            GenerateTerrainTexture();
            SettleTanks(); SettlePickup();
            pickup.active = r.readU8() != 0;
            if (pickup.active) { pickup.x = r.readF32(); pickup.y = r.readF32(); pickup.type = (PickupType)r.readU8(); }
            nextWind = r.readF32();   // authoritative wind for the next turn
            wind = nextWind;
            // Clear any stale mid-flight projectiles / animations
            projectiles.clear(); explosions.clear(); laserTrail.clear();
            waitForResult = false; hasPendingResult = false;
            if (tanks[0].hp <= 0 || tanks[1].hp <= 0) {
                bool both = tanks[0].hp <= 0 && tanks[1].hp <= 0;
                if (both) drawGame = true;
                state = GameState::GAME_OVER; stateTimer = 0;
            } else {
                state = GameState::NEXT_TURN; stateTimer = 0;
            }
            break;
        }
        case NetMsg::PLAYER_NAME: {
            // Host receives the client's name, stores it, then launches the match
            NetReader nr{pkt.data()};
            settings.playerNames[1] = nr.readStr();
            netReadyToStart = true;
            break;
        }
        case NetMsg::DISCONNECT:
            NetClose();
            ShowPickupMessage("Opponent disconnected.");
            state = GameState::MENU; stateTimer = 0;
            break;
        default: break;
        }
    }

    // =========================================================================
    // INITIALISATION
    // =========================================================================

    bool OnUserCreate() override {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
        srand((unsigned)time(nullptr));
        roundNumber = 0;
        tanks[0].score = 0;
        tanks[1].score = 0;
        inputMode = InputMode::NONE;
        inputBuffer.clear();
        cursorBlink = 0;
        repeatTimer = 0;
        mouseRepeatTimer = 0;
        frameTime = 0;
        menuEditingName = -1;
        menuCursorBlink = 0;
        activeGravity = 180.0f;
        activeWindMax = 10.0f;
        whistleSoundLoaded = false;
        selectedWeapon = WeaponType::NORMAL;
        reticleActive = false;
        state = GameState::TITLE;
        stateTimer = 0;

        // Cross-platform audio engine (miniaudio) and pre-generated one-shot sounds
        pFrameBuf = new olc::Sprite(SCREEN_W, SCREEN_H);
        ma_engine_init(NULL, &audioEngine);

        explosionWavPath = GetTempFilePath("tanx_explosion.wav");
        GenerateExplosionWav(explosionWavPath);

        clickWavPath = GetTempFilePath("tanx_click.wav");
        GenerateClickWav(clickWavPath);

        thunderWavPath = GetTempFilePath("tanx_thunder.wav");
        GenerateThunderWav(thunderWavPath);

        return true;
    }

    // Reset for a new round (keeps scores and settings)
    void NewRound() {
        roundNumber++;

        // Apply gravity setting
        float gravities[] = {90.0f, 180.0f, 360.0f};
        if (settings.gravitySetting == 3)
            activeGravity = gravities[rand() % 3];
        else
            activeGravity = gravities[settings.gravitySetting];

        // Apply wind setting
        float windMaxes[] = {0.0f, 3.0f, 6.0f, 10.0f};
        if (settings.windSetting == 4) {
            activeWindMax = windMaxes[1 + rand() % 3];
        } else {
            activeWindMax = windMaxes[settings.windSetting];
        }

        GenerateTerrain();

        tanks[0].x = 40 + rand() % 60;
        tanks[1].x = SCREEN_W - 100 + rand() % 60;

        for (int i = 0; i < 2; i++) {
            tanks[i].y = terrain[(int)tanks[i].x];
            tanks[i].angle = 45;
            tanks[i].power = 50;
            tanks[i].hp = settings.startingHP;
            tanks[i].movesLeft = settings.moveBudget;
            tanks[i].shieldCharges = 0;
            tanks[i].nvgTurns = 0;
        }

        tanks[0].colour = olc::Pixel(60, 140, 60);
        tanks[0].barrelColour = olc::Pixel(40, 100, 40);
        tanks[1].colour = olc::Pixel(160, 60, 60);
        tanks[1].barrelColour = olc::Pixel(120, 40, 40);

        // Night mode: reset lightning timers
        lightningTimer = 5.0f + (rand() % 5);  // first strike after 5-10 s
        lightningFlash = 0;

        projectiles.clear();
        explosions.clear();
        laserTrail.clear();
        selectedWeapon = WeaponType::NORMAL;
        reticleActive = false;
        currentPlayer = 0;
        inputMode = InputMode::NONE;
        inputBuffer.clear();
        drawGame = false;
        turnsSinceHit = 0;

        pickup.active = false;
        pickupRespawnTimer = 0;
        pickupMessage.clear();
        pickupMessageTimer = 0;
        SpawnPickup();

        if (activeWindMax > 0)
            wind = ((rand() % 200) - 100) / 100.0f * activeWindMax;
        else
            wind = 0;

        state = GameState::AIM;
        stateTimer = 0;
    }

    void StartNewMatch() {
        roundNumber = 0;
        for (int i = 0; i < 2; i++) {
            tanks[i].score = 0;
            tanks[i].ammoHE = settings.startAmmoHE;
            tanks[i].ammoCluster = settings.startAmmoCluster;
            tanks[i].ammoLaser = settings.startAmmoLaser;
            tanks[i].ammoBallistics = settings.startAmmoBallistics;
            tanks[i].ammoShield = settings.startAmmoShield;
        }
        NewRound();
    }

    // =========================================================================
    // TERRAIN GENERATION
    // =========================================================================

    void GenerateTerrain() {
        terrain.resize(SCREEN_W);
        terrainColour.resize(SCREEN_W * GAME_HEIGHT);

        // Landscape setting controls terrain shape
        int landscape = settings.landscapeSetting;
        if (landscape == 2) landscape = rand() % 2; // Random picks mountains or foothills

        float minH, maxH, spread;
        int smoothPasses;

        if (landscape == 0) {
            // Mountains: dramatic peaks and valleys, kept low enough for open sky above
            minH = GAME_TOP + GAME_HEIGHT * 0.48f;
            maxH = GAME_TOP + GAME_HEIGHT * 0.88f;
            spread = (maxH - minH) * 0.7f;
            smoothPasses = 2;
        } else {
            // Foothills: gentle rolling terrain, sitting in the lower third
            minH = GAME_TOP + GAME_HEIGHT * 0.58f;
            maxH = GAME_TOP + GAME_HEIGHT * 0.80f;
            spread = (maxH - minH) * 0.4f;
            smoothPasses = 5;
        }

        terrain[0] = minH + (rand() % (int)(maxH - minH));
        terrain[SCREEN_W - 1] = minH + (rand() % (int)(maxH - minH));
        MidpointDisplace(0, SCREEN_W - 1, spread);

        for (int x = 0; x < SCREEN_W; x++) {
            terrain[x] = std::clamp(terrain[x], minH, maxH);
        }

        SmoothTerrain(smoothPasses);
        GenerateTerrainTexture();
    }

    void MidpointDisplace(int left, int right, float spread) {
        if (right - left < 2) return;
        int mid = (left + right) / 2;
        terrain[mid] = (terrain[left] + terrain[right]) / 2.0f
                       + ((rand() % 1000) / 500.0f - 1.0f) * spread;
        MidpointDisplace(left, mid, spread * 0.6f);
        MidpointDisplace(mid, right, spread * 0.6f);
    }

    void SmoothTerrain(int passes) {
        for (int p = 0; p < passes; p++) {
            std::vector<float> smoothed = terrain;
            for (int x = 1; x < SCREEN_W - 1; x++) {
                smoothed[x] = (terrain[x - 1] + terrain[x] + terrain[x + 1]) / 3.0f;
            }
            terrain = smoothed;
        }
    }

    // Three terrain layers: grass (top 3px), dirt (3-20px), rock (20px+)
    void GenerateTerrainTexture() {
        std::fill(terrainColour.begin(), terrainColour.end(), olc::BLANK);

        for (int x = 0; x < SCREEN_W; x++) {
            int groundTop = (int)terrain[x];
            for (int y = groundTop; y < SCREEN_H; y++) {
                int depth = y - groundTop;
                int idx = (y - GAME_TOP) * SCREEN_W + x;
                if (idx < 0 || idx >= (int)terrainColour.size()) continue;

                int noise = (rand() % 30) - 15;

                if (depth < 3) {
                    terrainColour[idx] = olc::Pixel(80 + noise, 160 + noise, 40 + noise);
                } else if (depth < 20) {
                    int g = 100 + noise - depth;
                    int r = 60 + noise + depth / 2;
                    terrainColour[idx] = olc::Pixel(
                        std::clamp(r, 20, 255),
                        std::clamp(g, 20, 255),
                        std::clamp(30 + noise, 20, 255));
                } else {
                    int shade = 80 + noise - std::min(depth / 3, 30);
                    terrainColour[idx] = olc::Pixel(
                        std::clamp(shade + 10, 20, 180),
                        std::clamp(shade - 5, 20, 140),
                        std::clamp(shade - 20, 10, 100));
                }
            }
        }
    }

    // =========================================================================
    // TERRAIN DESTRUCTION
    // =========================================================================

    // Carve a circular crater into the terrain heights only (no texture repaint —
    // callers that carve repeatedly, like the laser beam, repaint once at the end)
    void CarveCrater(float cx, float cy, float radius) {
        int left = std::max(0, (int)(cx - radius));
        int right = std::min(SCREEN_W - 1, (int)(cx + radius));

        for (int x = left; x <= right; x++) {
            float dx = x - cx;
            float halfChord = std::sqrt(radius * radius - dx * dx);
            float craterBottom = cy + halfChord;
            if (craterBottom > terrain[x]) {
                terrain[x] = std::min((float)SCREEN_H - 1, craterBottom);
            }
        }
    }

    void DestroyTerrain(float cx, float cy, float radius) {
        CarveCrater(cx, cy, radius);
        GenerateTerrainTexture();
    }

    void SettleTanks() {
        for (int i = 0; i < 2; i++) {
            int tx = (int)tanks[i].x;
            if (tx >= 0 && tx < SCREEN_W) {
                tanks[i].y = terrain[tx];
            }
        }
    }

    // Keep the pickup resting on the surface after terrain is reshaped
    void SettlePickup() {
        if (!pickup.active) return;
        int px = (int)pickup.x;
        if (px >= 0 && px < SCREEN_W) pickup.y = terrain[px];
    }

    // =========================================================================
    // MAP PICKUPS
    // =========================================================================

    void ShowPickupMessage(const std::string& msg) {
        pickupMessage = msg;
        pickupMessageTimer = 5.0f;
    }

    // Place a new pickup at a random spot on the terrain, away from both tanks
    void SpawnPickup() {
        int x = SCREEN_W / 2;
        for (int tries = 0; tries < 50; tries++) {
            int candidate = 60 + rand() % (SCREEN_W - 120);
            if (std::abs(candidate - (int)tanks[0].x) > 80 && std::abs(candidate - (int)tanks[1].x) > 80) {
                x = candidate;
                break;
            }
        }

        pickup.x = (float)x;
        pickup.y = terrain[x];

        // In night mode NVG goggles can also spawn (when neither tank already has them)
        bool tankCritical = (tanks[0].hp <= 1 || tanks[1].hp <= 1);
        bool nvgAvailable = settings.nightMode && tanks[0].nvgTurns == 0 && tanks[1].nvgTurns == 0;

        int roll = rand() % 10;
        if (nvgAvailable && roll < 3)           pickup.type = PickupType::NVG;
        else if (tankCritical && roll < 5)      pickup.type = PickupType::HEALTH;
        else                                    pickup.type = PickupType::MYSTERY;
        pickup.active = true;
    }

    // Mystery pickup grants +1 ammo to a randomly chosen special weapon
    void CollectMysteryPickup(int tankIdx) {
        Tank& t = tanks[tankIdx];
        std::string name;
        switch (rand() % 5) {
            case 0: t.ammoHE++; name = "HIGH EXPLOSIVE"; break;
            case 1: t.ammoCluster++; name = "CLUSTER"; break;
            case 2: t.ammoLaser++; name = "LASER"; break;
            case 3: t.ammoBallistics++; name = "BALLISTICS COMPUTER"; break;
            default: t.ammoShield++; name = "SHIELD"; break;
        }
        ShowPickupMessage(settings.playerNames[tankIdx] + " found +1 " + name + " AMMO!");
    }

    // NVG pickup grants 5 turns of full night-vision to the collecting tank
    void CollectNVGPickup(int tankIdx) {
        tanks[tankIdx].nvgTurns = 5;
        ShowPickupMessage(settings.playerNames[tankIdx] + " found NIGHT VISION GOGGLES! (5 turns)");
    }

    // Health pickup grants +1 HP, capped at settings.startingHP
    void CollectHealthPickup(int tankIdx) {
        Tank& t = tanks[tankIdx];
        if (t.hp < settings.startingHP) {
            t.hp++;
            ShowPickupMessage(settings.playerNames[tankIdx] + " found +1 HEALTH! ("
                + std::to_string(t.hp) + "/" + std::to_string(settings.startingHP) + ")");
        } else {
            ShowPickupMessage(settings.playerNames[tankIdx] + " found a Health Box (already full!)");
        }
    }

    // Check whether either tank is standing on the active pickup
    void CheckPickupCollisions() {
        if (!pickup.active) return;
        for (int i = 0; i < 2; i++) {
            if (tanks[i].hp <= 0) continue;
            if (std::abs(tanks[i].x - pickup.x) < TANK_WIDTH / 2.0f + 10.0f) {
                if (pickup.type == PickupType::MYSTERY)      CollectMysteryPickup(i);
                else if (pickup.type == PickupType::HEALTH)  CollectHealthPickup(i);
                else                                         CollectNVGPickup(i);

                pickup.active = false;
                pickupRespawnTimer = 6.0f + (float)(rand() % 8); // next one in 6-13s
                break;
            }
        }
    }

    // Destroy the pickup if an explosion (or laser cut) lands close enough to
    // it — lets players deny a pickup to the opponent by shooting it instead.
    void CheckPickupDestruction(float ex, float ey, float radius) {
        if (!pickup.active) return;
        float dx = ex - pickup.x;
        float dy = ey - pickup.y;
        if (std::sqrt(dx * dx + dy * dy) < radius + 12.0f) {
            pickup.active = false;
            pickupRespawnTimer = 6.0f + (float)(rand() % 8);
            ShowPickupMessage("The pickup was blown up!");
        }
    }

    // =========================================================================
    // INPUT HELPERS
    // =========================================================================

    // Throttle held actions: returns true on the first frame, then after an
    // initial delay, ticks at a steady rate. Call once per frame with 'held'
    // indicating whether the action is still active.
    bool RepeatTick(bool held, float fElapsedTime, float& timer) {
        if (!held) {
            timer = 0;
            return false;
        }
        if (timer == 0) {
            timer += fElapsedTime;
            return true;
        }
        timer += fElapsedTime;
        if (timer < repeatDelay) return false;
        float elapsed = timer - repeatDelay;
        float prev = elapsed - fElapsedTime;
        if (prev < 0) prev = 0;
        return (int)(elapsed / repeatRate) > (int)(prev / repeatRate);
    }

    // Returns 0-9 if a digit key was pressed this frame, or -1
    int GetDigitPressed() {
        olc::Key numKeys[] = {
            olc::Key::K0, olc::Key::K1, olc::Key::K2, olc::Key::K3, olc::Key::K4,
            olc::Key::K5, olc::Key::K6, olc::Key::K7, olc::Key::K8, olc::Key::K9
        };
        olc::Key npKeys[] = {
            olc::Key::NP0, olc::Key::NP1, olc::Key::NP2, olc::Key::NP3, olc::Key::NP4,
            olc::Key::NP5, olc::Key::NP6, olc::Key::NP7, olc::Key::NP8, olc::Key::NP9
        };
        for (int i = 0; i < 10; i++) {
            if (GetKey(numKeys[i]).bPressed || GetKey(npKeys[i]).bPressed)
                return i;
        }
        return -1;
    }

    // Returns an uppercase letter if a letter key was pressed, or '\0'
    char GetLetterPressed() {
        for (int k = (int)olc::Key::A; k <= (int)olc::Key::Z; k++) {
            if (GetKey((olc::Key)k).bPressed) {
                return 'A' + (k - (int)olc::Key::A);
            }
        }
        return '\0';
    }

    // Apply typed angle/power value, then advance to next field
    void CommitInput() {
        if (inputBuffer.empty()) {
            if (inputMode == InputMode::TYPING_ANGLE) {
                inputMode = InputMode::TYPING_POWER;
                inputBuffer.clear();
            } else {
                inputMode = InputMode::NONE;
            }
            return;
        }
        Tank& t = tanks[currentPlayer];
        int val = std::stoi(inputBuffer);
        if (inputMode == InputMode::TYPING_ANGLE) {
            t.angle = std::clamp(val, 0, 110);
            inputMode = InputMode::TYPING_POWER;
            inputBuffer.clear();
        } else if (inputMode == InputMode::TYPING_POWER) {
            t.power = std::clamp(val, 5, MAX_POWER);
            inputMode = InputMode::NONE;
            inputBuffer.clear();
        }
    }

    // =========================================================================
    // FIRING & COLLISION
    // =========================================================================

    // Convert a tank's relative angle (0=toward enemy, 90=up) to absolute radians.
    // P1 faces right: absolute = relative. P2 faces left: absolute = 180 - relative.
    float GetBarrelRad(int playerIdx) {
        float rel = (float)tanks[playerIdx].angle;
        float absDeg = (playerIdx == 0) ? rel : (180.0f - rel);
        return absDeg * 3.14159f / 180.0f;
    }

    void Fire() {
        // In network mode, send the action so the remote fires simultaneously
        if (netMode != NetMode::NONE && IsLocalTurn())
            NetSendTurnAction(false, false);

        if (selectedWeapon == WeaponType::LASER) {
            FireLaser();
            return;
        }

        Tank& t = tanks[currentPlayer];
        float rad = GetBarrelRad(currentPlayer);
        float speed = t.power * 4.0f;

        Projectile p;
        p.x = t.x + cos(rad) * BARREL_LENGTH;
        p.y = t.y - TANK_HEIGHT - sin(rad) * BARREL_LENGTH;
        p.vx = cos(rad) * speed;
        p.vy = -sin(rad) * speed;
        p.active = true;
        p.weapon = selectedWeapon;
        p.hasSplit = false;
        p.trail.clear();

        projectiles.clear();
        projectiles.push_back(p);

        if (selectedWeapon == WeaponType::HE) t.ammoHE--;
        if (selectedWeapon == WeaponType::CLUSTER) t.ammoCluster--;
        selectedWeapon = WeaponType::NORMAL;

        state = GameState::FIRING;
        PlayWhistleSound();
    }

    // Instant beam along the chosen angle: carves a trench through the terrain
    // and deals 1 HP damage if it hits the enemy tank directly.
    void FireLaser() {
        Tank& t = tanks[currentPlayer];
        float rad = GetBarrelRad(currentPlayer);
        float x = t.x + cos(rad) * BARREL_LENGTH;
        float y = t.y - TANK_HEIGHT - sin(rad) * BARREL_LENGTH;
        float dx = cos(rad), dy = -sin(rad);

        int opponent = 1 - currentPlayer;
        laserTrail.clear();

        for (int i = 0; i < 3000; i++) {
            x += dx * 2.0f;
            y += dy * 2.0f;
            laserTrail.push_back({x, y});

            if (CheckTankHit(opponent, x, y)) {
                ApplyHitDamage(opponent);
                break;
            }
            if (x < 0 || x >= SCREEN_W || y < 0 || y > SCREEN_H + 50) break;

            CheckPickupDestruction(x, y, 4.0f);

            int px = (int)x;
            if (px >= 0 && px < SCREEN_W && y >= terrain[px]) {
                CarveCrater(x, y, 6.0f);
            }
        }
        GenerateTerrainTexture();
        SettleTanks();
        SettlePickup();

        t.ammoLaser--;
        selectedWeapon = WeaponType::NORMAL;
        state = GameState::LASER_FIRE;
        stateTimer = 0;
    }

    // Numerically solves for the angle/power that would land a direct hit on
    // the enemy tank, ignoring wind (since it can't be reliably predicted).
    // Sets the current tank's angle/power to the best solution found.
    void UseBallisticsComputer() {
        Tank& t = tanks[currentPlayer];
        if (t.ammoBallistics <= 0) return;
        t.ammoBallistics--;

        int opponent = 1 - currentPlayer;
        Tank& enemy = tanks[opponent];

        int bestAngle = t.angle;
        int bestPower = t.power;
        float bestErr = 1e9f;
        bool found = false;

        for (int angle = 4; angle <= 106 && !found; angle += 2) {
            int lo = 5, hi = MAX_POWER;
            for (int iter = 0; iter < 12; iter++) {
                int power = (lo + hi) / 2;
                float absDeg = (currentPlayer == 0) ? (float)angle : (180.0f - angle);
                float rad = absDeg * 3.14159f / 180.0f;
                float speed = power * 4.0f;
                float x = t.x + cos(rad) * BARREL_LENGTH;
                float y = t.y - TANK_HEIGHT - sin(rad) * BARREL_LENGTH;
                float vx = cos(rad) * speed;
                float vy = -sin(rad) * speed;

                bool hit = false;
                float landX = x;
                const float simDt = 0.01f;
                for (int s = 0; s < 1200; s++) {
                    vy += activeGravity * simDt;
                    x += vx * simDt;
                    y += vy * simDt;
                    if (CheckTankHit(opponent, x, y)) { hit = true; landX = x; break; }
                    int px = (int)x;
                    if (px < 0 || px >= SCREEN_W) { landX = x; break; }
                    if (y >= terrain[px]) { landX = x; break; }
                    if (y > SCREEN_H + 50) { landX = x; break; }
                }

                float err = std::abs(landX - enemy.x);
                if (err < bestErr) { bestErr = err; bestAngle = angle; bestPower = power; }
                if (hit) { found = true; break; }

                bool overshoot = (currentPlayer == 0) ? (landX > enemy.x) : (landX < enemy.x);
                if (overshoot) hi = power; else lo = power;
            }
        }

        t.angle = bestAngle;
        t.power = bestPower;
        reticleActive = true;
    }

    // Activate a 3-layer shield on the current tank. Each layer absorbs one
    // direct hit instead of losing HP, degrading thick-blue -> orange -> red.
    void ActivateShield() {
        Tank& t = tanks[currentPlayer];
        if (t.ammoShield <= 0) return;
        t.ammoShield--;
        t.shieldCharges = 3;
        ShowPickupMessage(settings.playerNames[currentPlayer] + " raised a SHIELD!");
    }

    // Apply a direct hit to a tank: absorbed by an active shield if present,
    // otherwise reduces HP. Returns true if the shield absorbed the hit.
    // Trigger a camera shake — larger magnitude for HE hits, smaller for shield
    void TriggerShake(float magnitude = 8.0f, float duration = 0.35f) {
        shakeMag  = magnitude;
        shakeTimer = duration;
    }

    // Call once per frame to advance the shake and compute this frame's offset
    void UpdateShake(float dt) {
        if (shakeTimer <= 0) { shakeOffX = shakeOffY = 0; return; }
        shakeTimer -= dt;
        float intensity = (shakeTimer / 0.35f) * shakeMag;
        shakeOffX = (int)(((rand() % 2001) - 1000) / 1000.0f * intensity);
        shakeOffY = (int)(((rand() % 2001) - 1000) / 1000.0f * intensity * 0.6f);
        if (shakeTimer <= 0) { shakeOffX = shakeOffY = 0; }
    }

    bool ApplyHitDamage(int tankIdx) {
        Tank& t = tanks[tankIdx];
        if (t.shieldCharges > 0) {
            t.shieldCharges--;
            ShowPickupMessage(settings.playerNames[tankIdx] + "'s shield absorbs the hit!");
            TriggerShake(4.0f, 0.2f); // smaller shake for shield block
            return true;
        }
        t.hp--;
        turnsSinceHit = 0; // a hit happened — reset the stalemate counter
        TriggerShake(10.0f, 0.4f); // strong shake for a real hit
        return false;
    }

    // Concede the entire match: blow up your own tank and hand the opponent
    // an immediate match win, then drop back to the menu via GAME_OVER.
    void SurrenderMatch() {
        int opponent = 1 - currentPlayer;
        surrendered = true;
        tanks[opponent].score = settings.roundsToWin;
        tanks[currentPlayer].hp = 0;

        Tank& t = tanks[currentPlayer];
        explosions.push_back({t.x, t.y - TANK_HEIGHT / 2.0f, 1.0f, EXPLOSION_RADIUS * 1.5f, 0, true});
        projectiles.clear();
        StopWhistleSound();
        PlayExplosionSound();

        state = GameState::EXPLOSION;
        stateTimer = 0;
    }

    bool CheckTankHit(int tankIdx, float px, float py) {
        Tank& t = tanks[tankIdx];
        float dx = px - t.x;
        float dy = py - (t.y - TANK_HEIGHT / 2);
        return (std::abs(dx) < TANK_WIDTH && std::abs(dy) < TANK_HEIGHT);
    }

    // =========================================================================
    // DRAWING HELPERS
    // =========================================================================

    // Draw a bevelled wood-textured panel (used in the menu screen)
    void DrawWoodPanel(int x, int y, int w, int h) {
        FillRect(x, y, w, h, olc::Pixel(101, 67, 33));

        // Deterministic wood grain noise based on position
        for (int py = y; py < y + h; py++) {
            for (int px = x; px < x + w; px += 2) {
                int noise = ((px * 7 + py * 13 + px * py) % 20) - 10;
                if (noise > 3)
                    Draw(px, py, olc::Pixel(111 + noise, 77 + noise, 43 + noise));
            }
        }

        // Bevelled edges: light top-left, dark bottom-right
        DrawLine(x, y, x + w - 1, y, olc::Pixel(160, 120, 70));
        DrawLine(x, y, x, y + h - 1, olc::Pixel(160, 120, 70));
        DrawLine(x + 1, y + 1, x + w - 2, y + 1, olc::Pixel(140, 105, 60));
        DrawLine(x + 1, y + 1, x + 1, y + h - 2, olc::Pixel(140, 105, 60));
        DrawLine(x, y + h - 1, x + w - 1, y + h - 1, olc::Pixel(50, 30, 15));
        DrawLine(x + w - 1, y, x + w - 1, y + h - 1, olc::Pixel(50, 30, 15));
        DrawLine(x + 1, y + h - 2, x + w - 2, y + h - 2, olc::Pixel(65, 40, 20));
        DrawLine(x + w - 2, y + 1, x + w - 2, y + h - 2, olc::Pixel(65, 40, 20));
    }

    // Draw a small bevelled HUD button. Returns true if mouse is held over it.
    bool DrawHudButton(int x, int y, const std::string& label) {
        int w = 20, h = 16;
        bool hovering = GetMouseX() >= x && GetMouseX() < x + w
                     && GetMouseY() >= y && GetMouseY() < y + h;
        bool pressed = hovering && GetMouse(0).bHeld;

        if (pressed) {
            FillRect(x, y, w, h, olc::Pixel(40, 30, 15));
            DrawLine(x, y, x + w - 1, y, olc::Pixel(50, 30, 15));
            DrawLine(x, y, x, y + h - 1, olc::Pixel(50, 30, 15));
            DrawLine(x, y + h - 1, x + w - 1, y + h - 1, olc::Pixel(140, 110, 70));
            DrawLine(x + w - 1, y, x + w - 1, y + h - 1, olc::Pixel(140, 110, 70));
        } else {
            FillRect(x, y, w, h, olc::Pixel(80, 55, 30));
            DrawLine(x, y, x + w - 1, y, olc::Pixel(160, 120, 70));
            DrawLine(x, y, x, y + h - 1, olc::Pixel(160, 120, 70));
            DrawLine(x, y + h - 1, x + w - 1, y + h - 1, olc::Pixel(40, 25, 10));
            DrawLine(x + w - 1, y, x + w - 1, y + h - 1, olc::Pixel(40, 25, 10));
        }
        int textX = x + (w - (int)label.length() * 8) / 2;
        DrawString(textX, y + 4, label, olc::WHITE);

        return pressed;
    }

    // Draw a radio button: dark circle with red fill if selected
    void DrawRadioButton(int x, int y, bool selected) {
        FillCircle(x + 5, y + 5, 5, olc::Pixel(30, 30, 30));
        DrawCircle(x + 5, y + 5, 5, olc::Pixel(80, 80, 80));
        if (selected) {
            FillCircle(x + 5, y + 5, 3, olc::Pixel(200, 30, 30));
            // Shiny highlight
            Draw(x + 4, y + 3, olc::Pixel(255, 120, 120));
        }
    }

    // Draw a radio option row: label + radio button. Returns true if clicked.
    bool DrawMenuOption(int x, int y, int w, const std::string& label,
                        bool selected, olc::Pixel col = olc::WHITE) {
        DrawString(x, y + 2, label, col);
        int rbx = x + w - 16;
        DrawRadioButton(rbx, y, selected);

        // Check for mouse click on this row
        if (GetMouse(0).bPressed) {
            int mx = GetMouseX(), my = GetMouseY();
            if (mx >= x && mx < x + w && my >= y && my < y + 14)
                return true;
        }
        return false;
    }

    // Spinner row for the Game Settings panel: label on the left, [-] value [+]
    // on the right. Directly modifies 'value' in place on click. Returns true if changed.
    bool DrawMenuSpinner(int x, int y, int w, const std::string& label,
                         int& value, int minVal, int maxVal, olc::Pixel col = olc::WHITE) {
        DrawString(x + 4, y + 4, label, col);

        int btnW = 16, valW = 24, gap = 2;
        int rightEdge = x + w - 4;
        int plusX  = rightEdge - btnW;
        int valX   = plusX - gap - valW;
        int minusX = valX - gap - btnW;

        bool hovMinus = GetMouseX() >= minusX && GetMouseX() < minusX + btnW
                     && GetMouseY() >= y && GetMouseY() < y + 14;
        bool hovPlus  = GetMouseX() >= plusX  && GetMouseX() < plusX + btnW
                     && GetMouseY() >= y && GetMouseY() < y + 14;

        FillRect(minusX, y + 1, btnW, 12, hovMinus ? olc::Pixel(120,80,40) : olc::Pixel(80,55,30));
        DrawRect(minusX, y + 1, btnW, 12, olc::Pixel(160,120,70));
        DrawString(minusX + 4, y + 3, "-", olc::WHITE);

        FillRect(valX, y + 1, valW, 12, olc::Pixel(20,15,10));
        DrawRect(valX, y + 1, valW, 12, olc::Pixel(80,60,30));
        DrawString(valX + 4, y + 3, std::to_string(value), olc::YELLOW);

        FillRect(plusX, y + 1, btnW, 12, hovPlus ? olc::Pixel(120,80,40) : olc::Pixel(80,55,30));
        DrawRect(plusX, y + 1, btnW, 12, olc::Pixel(160,120,70));
        DrawString(plusX + 4, y + 3, "+", olc::WHITE);

        bool changed = false;
        if (GetMouse(0).bPressed) {
            if (hovMinus && value > minVal) { value--; changed = true; }
            if (hovPlus  && value < maxVal) { value++; changed = true; }
        }
        return changed;
    }

    // =========================================================================
    // DRAWING - MENU SCREEN
    // =========================================================================

    void DrawMenuScreen() {
        DrawWoodPanel(0, 0, SCREEN_W, SCREEN_H);

        // ── Layout constants ─────────────────────────────────────────────────
        const int margin  = 22;
        const int colGap  = 18;
        const int colW    = (SCREEN_W - margin * 2 - colGap * 2) / 3; // ~322 px each
        const int c1 = margin,            c1w = colW;
        const int c2 = c1 + colW + colGap, c2w = colW;
        const int c3 = c2 + colW + colGap, c3w = SCREEN_W - margin - c3; // remainder

        // Title
        DrawString(SCREEN_W / 2 - 112, 12, "T A N X", olc::Pixel(255, 200, 0), 4);

        // ── Player name panels (full width, side by side) ───────────────────
        int nameY = 62;
        int nameH = 54;
        int nameW = (SCREEN_W - margin * 2 - colGap) / 2;
        for (int p = 0; p < 2; p++) {
            int nx = margin + p * (nameW + colGap);
            DrawWoodPanel(nx, nameY, nameW, nameH);
            olc::Pixel lc = (p == 0) ? olc::Pixel(100,255,100) : olc::Pixel(255,100,100);
            DrawString(nx + 10, nameY + 8, (p == 0) ? "Player 1:" : "Player 2:", lc);

            int fx = nx + 100, fy = nameY + 26, fw = nameW - 112;
            FillRect(fx, fy, fw, 18, olc::Pixel(20,15,10));
            DrawRect(fx, fy, fw, 18, olc::Pixel(80,60,30));
            if (menuEditingName == p) {
                DrawRect(fx, fy, fw, 18, olc::Pixel(100,100,255));
                std::string d = menuNameBuffer;
                if (fmod(menuCursorBlink, 0.8f) < 0.4f) d += "_";
                DrawString(fx + 4, fy + 5, d, olc::Pixel(100,200,255));
            } else {
                DrawString(fx + 4, fy + 5, settings.playerNames[p], olc::YELLOW);
            }
            if (GetMouse(0).bPressed) {
                int mx = GetMouseX(), my = GetMouseY();
                if (mx >= fx && mx < fx+fw && my >= fy && my < fy+18) {
                    if (menuEditingName != -1 && menuEditingName != p) {
                        settings.playerNames[menuEditingName] = menuNameBuffer.empty()
                            ? menuNameBeforeEdit : menuNameBuffer;
                    }
                    menuEditingName = p;
                    menuNameBeforeEdit = settings.playerNames[p];
                    menuNameBuffer.clear();
                    menuCursorBlink = 0;
                }
            }
        }

        // ── Three-column settings area ───────────────────────────────────────
        const int settY = nameY + nameH + 12; // top of the three panels

        // ── Column 1: Wind / Landscape / Night Mode ──────────────────────────
        {
            int y = settY;
            // Wind
            int wH = 32 + 5 * 28;
            DrawWoodPanel(c1, y, c1w, wH);
            DrawString(c1+10, y+8, "Wind:", olc::Pixel(150,200,255), 2);
            const char* wo[] = {"None","Light","Medium","Strong","Random"};
            olc::Pixel wc[] = {olc::WHITE, olc::Pixel(100,255,100), olc::YELLOW,
                               olc::Pixel(255,100,100), olc::Pixel(200,150,255)};
            for (int i=0;i<5;i++) {
                int oy = y+30+i*28;
                if (DrawMenuOption(c1+8, oy, c1w-14, wo[i], settings.windSetting==i, wc[i]))
                    settings.windSetting = i;
            }
            y += wH + 10;

            // Landscape
            int lH = 32 + 3 * 28;
            DrawWoodPanel(c1, y, c1w, lH);
            DrawString(c1+10, y+8, "Landscape:", olc::Pixel(150,255,150), 2);
            const char* lo[] = {"Mountains","Foothills","Random"};
            for (int i=0;i<3;i++) {
                int oy = y+30+i*28;
                if (DrawMenuOption(c1+8, oy, c1w-14, lo[i], settings.landscapeSetting==i))
                    settings.landscapeSetting = i;
            }
            y += lH + 10;

            // Night mode toggle
            DrawWoodPanel(c1, y, c1w, 30);
            if (DrawMenuOption(c1+8, y+8, c1w-14, "Night Mode",
                    settings.nightMode, olc::Pixel(80,180,255)))
                settings.nightMode = !settings.nightMode;
        }

        // ── Column 2: Gravity / Rounds to Win ────────────────────────────────
        {
            int y = settY;
            int gH = 32 + 4 * 28;
            DrawWoodPanel(c2, y, c2w, gH);
            DrawString(c2+10, y+8, "Gravity:", olc::Pixel(255,200,100), 2);
            const char* go[] = {"Light","Medium","Strong","Random"};
            olc::Pixel gc[] = {olc::Pixel(100,255,100), olc::YELLOW,
                               olc::Pixel(255,100,100), olc::Pixel(200,150,255)};
            for (int i=0;i<4;i++) {
                int oy = y+30+i*28;
                if (DrawMenuOption(c2+8, oy, c2w-14, go[i], settings.gravitySetting==i, gc[i]))
                    settings.gravitySetting = i;
            }
            y += gH + 10;

            int rH = 32 + 4 * 28;
            DrawWoodPanel(c2, y, c2w, rH);
            DrawString(c2+10, y+8, "Rounds to Win:", olc::Pixel(255,255,100), 2);
            int ro[] = {1,3,5,7};
            for (int i=0;i<4;i++) {
                int oy = y+30+i*28;
                if (DrawMenuOption(c2+8, oy, c2w-14, std::to_string(ro[i]),
                        settings.roundsToWin==ro[i], olc::YELLOW))
                    settings.roundsToWin = ro[i];
            }
        }

        // ── Column 3: Game Settings (spinners) ───────────────────────────────
        {
            int gsH = 26 + 9 * 20;
            DrawWoodPanel(c3, settY, c3w, gsH);
            DrawString(c3+8, settY+6, "Game Settings:", olc::Pixel(255,200,100), 1);
            int r = settY + 22;
            DrawMenuSpinner(c3, r, c3w, "Tank HP",    settings.startingHP,         1, 5);             r+=20;
            DrawMenuSpinner(c3, r, c3w, "Movement",   settings.moveBudget,        10,100,olc::Pixel(150,200,255)); r+=20;
            DrawMenuSpinner(c3, r, c3w, "HI-EXPLO",   settings.startAmmoHE,        0, 9);             r+=20;
            DrawMenuSpinner(c3, r, c3w, "Cluster",    settings.startAmmoCluster,   0, 9);             r+=20;
            DrawMenuSpinner(c3, r, c3w, "Laser",      settings.startAmmoLaser,     0, 9);             r+=20;
            DrawMenuSpinner(c3, r, c3w, "Ballistic",  settings.startAmmoBallistics,0, 9);             r+=20;
            DrawMenuSpinner(c3, r, c3w, "Shield",     settings.startAmmoShield,    0, 9);             r+=20;
        }

        // ── Quick guide (full width) ─────────────────────────────────────────
        // Sits in the space between the panels and the buttons
        int guideY = SCREEN_H - 160;
        int guideW = SCREEN_W - margin * 2;
        DrawWoodPanel(margin, guideY, guideW, 88);
        DrawString(margin + 10, guideY + 6, "QUICK GUIDE", olc::Pixel(255, 220, 150), 1);

        struct GuideItem { const char* name; const char* desc; };
        GuideItem items[] = {
            {"HI-EXPLO","2X RADIUS"}, {"CLUSTER","SPLITS x3"}, {"LASER","BEAM SHOT"},
            {"BALLISTIC","AUTO-AIM"}, {"SHIELD","3 LAYERS"},
            {"MYSTERY","RND AMMO"}, {"HEALTH","+1 HP"}, {"NVG","5 TURN VISION"},
        };
        int numItems = 8;
        int itemW = guideW / numItems;
        int rowY = guideY + 24;
        for (int i = 0; i < numItems; i++) {
            int ix = margin + i * itemW;
            switch (i) {
                case 0: case 1: case 2: case 3: case 4:
                    DrawWeaponIcon(i+1, ix+14, rowY+10, false); break;
                case 5: // MYSTERY
                    FillCircle(ix+14, rowY+10, 6, olc::Pixel(255,200,0));
                    DrawString(ix+11, rowY+6, "?", olc::Pixel(120,60,0)); break;
                case 6: // HEALTH
                    FillRect(ix+8, rowY+4, 12,12, olc::Pixel(200,30,30));
                    DrawRect(ix+8, rowY+4, 12,12, olc::WHITE);
                    FillRect(ix+13,rowY+6, 2,8, olc::WHITE);
                    FillRect(ix+10,rowY+9, 8,2, olc::WHITE); break;
                case 7: // NVG
                    FillCircle(ix+10, rowY+10, 4, olc::Pixel(0,180,0));
                    FillCircle(ix+18, rowY+10, 4, olc::Pixel(0,180,0)); break;
            }
            DrawString(ix+28, rowY+3,  items[i].name, olc::WHITE);
            DrawString(ix+28, rowY+15, items[i].desc, olc::Pixel(170,170,170));
        }

        // ── PLAY + NETWORK buttons ───────────────────────────────────────────
        int btnY = SCREEN_H - 62;
        int btnH = 48;
        int btnW = 180;
        int btnX    = SCREEN_W / 2 - btnW - 16;
        int netBtnX = SCREEN_W / 2 + 16;
        float pulse = (sin(stateTimer * 3.0f) + 1.0f) * 0.5f;

        auto drawBtn = [&](int bx, int by, int bw, int bh,
                           olc::Pixel top, olc::Pixel bot, olc::Pixel border) {
            FillRect(bx+3, by+3, bw, bh, olc::Pixel(20,10,5));
            for (int y=0;y<bh;y++) {
                float t=(float)y/bh;
                DrawLine(bx,by+y,bx+bw-1,by+y,
                    olc::Pixel((int)(top.r+(bot.r-top.r)*t),
                               (int)(top.g+(bot.g-top.g)*t),
                               (int)(top.b+(bot.b-top.b)*t)));
            }
            DrawRect(bx,by,bw-1,bh-1,border);
        };
        drawBtn(btnX, btnY, btnW, btnH,
                olc::Pixel(180,50,20), olc::Pixel(110,25,10), olc::Pixel(220,100,60));
        DrawString(btnX+32, btnY+14, "PLAY!", olc::Pixel(255,(int)(200+55*pulse),(int)(50*pulse)), 3);

        drawBtn(netBtnX, btnY, btnW, btnH,
                olc::Pixel(30,80,160), olc::Pixel(15,45,100), olc::Pixel(60,140,220));
        DrawString(netBtnX+10, btnY+14, "NETWORK", olc::Pixel(150,220,255), 3);

        if (GetMouse(0).bPressed) {
            int mx=GetMouseX(), my=GetMouseY();
            auto commit = [&]() {
                if (menuEditingName >= 0) {
                    settings.playerNames[menuEditingName] = menuNameBuffer.empty()
                        ? menuNameBeforeEdit : menuNameBuffer;
                    menuEditingName = -1;
                }
            };
            if (mx>=btnX && mx<btnX+btnW && my>=btnY && my<btnY+btnH)
                { commit(); StartNewMatch(); }
            if (mx>=netBtnX && mx<netBtnX+btnW && my>=btnY && my<btnY+btnH)
                { commit(); netMode=NetMode::NONE; NetClose(); state=GameState::LOBBY; stateTimer=0; }
        }
    }

    // Handle keyboard input while on the menu screen
    void UpdateMenuInput(float fElapsedTime) {
        menuCursorBlink += fElapsedTime;

        if (menuEditingName >= 0) {
            // Typing a player name
            char letter = GetLetterPressed();
            if (letter != '\0' && menuNameBuffer.length() < 16) {
                menuNameBuffer += letter;
            }
            if (GetKey(olc::Key::SPACE).bPressed && menuNameBuffer.length() < 16) {
                menuNameBuffer += ' ';
            }
            int digit = GetDigitPressed();
            if (digit >= 0 && menuNameBuffer.length() < 16) {
                menuNameBuffer += std::to_string(digit);
            }
            if (GetKey(olc::Key::BACK).bPressed && !menuNameBuffer.empty()) {
                menuNameBuffer.pop_back();
            }
            if (GetKey(olc::Key::ENTER).bPressed || GetKey(olc::Key::RETURN).bPressed) {
                settings.playerNames[menuEditingName] = menuNameBuffer.empty()
                    ? menuNameBeforeEdit : menuNameBuffer;
                menuEditingName = -1;
            }
            if (GetKey(olc::Key::ESCAPE).bPressed) {
                menuEditingName = -1; // Cancel without saving
            }
        } else {
            // Not editing a name — Enter/Return starts the game
            if (GetKey(olc::Key::ENTER).bPressed || GetKey(olc::Key::RETURN).bPressed) {
                StartNewMatch();
            }
        }
    }

    // =========================================================================
    // DRAWING - TERRAIN & SKY
    // =========================================================================

    void DrawTerrain() {
        for (int x = 0; x < SCREEN_W; x++) {
            int groundTop = (int)terrain[x];
            for (int y = std::max(groundTop, GAME_TOP); y < SCREEN_H; y++) {
                int idx = (y - GAME_TOP) * SCREEN_W + x;
                if (idx >= 0 && idx < (int)terrainColour.size()) {
                    olc::Pixel c = terrainColour[idx];
                    if (c.a > 0) Draw(x, y, c);
                }
            }
        }
    }

    void DrawSky() {
        if (settings.nightMode) {
            // Dark night gradient with a subtle deep-blue tinge at the top
            for (int y = GAME_TOP; y < SCREEN_H; y++) {
                float t = (float)(y - GAME_TOP) / GAME_HEIGHT;
                DrawLine(0, y, SCREEN_W, y, olc::Pixel((int)(8+t*6), (int)(10+t*12), (int)(28+t*20)));
            }
            DrawStars();
            DrawBackgroundHills();
        } else {
            for (int y = GAME_TOP; y < SCREEN_H; y++) {
                float t = (float)(y - GAME_TOP) / GAME_HEIGHT;
                int r = (int)(135 + t * 50);
                int g = (int)(180 + t * 40);
                int b = (int)(255 - t * 30);
                DrawLine(0, y, SCREEN_W, y, olc::Pixel(r, g, b));
            }
            DrawSun();
            DrawClouds();
            DrawBackgroundHills();
        }
    }

    // Deterministic scattered stars for night mode
    void DrawStars() {
        for (int i = 0; i < 80; i++) {
            // Hash-based positions — same every frame, no persistent state
            int sx = (i * 173 + 37)  % SCREEN_W;
            int sy = GAME_TOP + (i * 211 + 19) % (GAME_HEIGHT / 2);
            uint8_t bright = 120 + (i * 97) % 136;
            float twinkle = (sin(worldTime * (0.5f + (i % 5) * 0.3f) + i) + 1.0f) * 0.5f;
            uint8_t a = (uint8_t)(bright * (0.6f + 0.4f * twinkle));
            Draw(sx, sy, olc::Pixel(a, a, a));
        }
    }

    // Fixed sun in the upper sky with a soft glow
    void DrawSun() {
        int sx = SCREEN_W - 110;
        int sy = GAME_TOP + 55;

        for (int r = 40; r > 18; r -= 4) {
            int alpha = (40 - r) * 4;
            DrawCircle(sx, sy, r, olc::Pixel(255, 240, 180, alpha));
        }
        FillCircle(sx, sy, 18, olc::Pixel(255, 225, 120));
        FillCircle(sx, sy, 14, olc::Pixel(255, 250, 210));
    }

    // A handful of puffy clouds drifting slowly left to right, each on its
    // own lane/speed and wrapping around once off the right edge
    void DrawClouds() {
        struct CloudDef { float yFrac; float speed; float seedOffset; int size; };
        static const CloudDef defs[5] = {
            {0.10f,  6.0f,   0.0f, 34},
            {0.18f,  4.0f, 260.0f, 24},
            {0.07f,  8.0f, 520.0f, 28},
            {0.24f,  5.0f, 120.0f, 20},
            {0.14f,  7.0f, 400.0f, 26},
        };

        for (auto& c : defs) {
            float cloudSpan = c.size * 3.0f;
            float x = fmod(worldTime * c.speed + c.seedOffset, SCREEN_W + cloudSpan) - cloudSpan * 0.5f;
            int cy = GAME_TOP + (int)(GAME_HEIGHT * c.yFrac);
            int cx = (int)x;

            olc::Pixel col(255, 255, 255, 200);
            FillCircle(cx, cy, c.size / 2, col);
            FillCircle(cx + c.size / 3, cy - c.size / 5, (int)(c.size * 0.4f), col);
            FillCircle(cx - c.size / 3, cy - c.size / 6, (int)(c.size * 0.35f), col);
            FillCircle(cx + (int)(c.size * 0.7f), cy + 2, (int)(c.size * 0.3f), col);
            FillCircle(cx - (int)(c.size * 0.6f), cy + 3, (int)(c.size * 0.28f), col);
        }
    }

    // Two layers of sine-wave hills for depth
    void DrawBackgroundHills() {
        for (int x = 0; x < SCREEN_W; x++) {
            float h1 = sin(x * 0.008f) * 30 + sin(x * 0.003f) * 45;
            int hilltop1 = (int)(GAME_TOP + GAME_HEIGHT * 0.40f + h1);
            for (int y = hilltop1; y < SCREEN_H; y++) {
                Draw(x, y, olc::Pixel(120, 150, 130, 80));
            }

            float h2 = sin(x * 0.012f + 2.0f) * 22 + sin(x * 0.005f + 1.0f) * 35;
            int hilltop2 = (int)(GAME_TOP + GAME_HEIGHT * 0.47f + h2);
            for (int y = hilltop2; y < SCREEN_H; y++) {
                Draw(x, y, olc::Pixel(90, 130, 100, 100));
            }
        }
    }

    // =========================================================================
    // DRAWING - TANKS
    // =========================================================================

    void DrawTank(const Tank& t, int playerIdx) {
        int tx = (int)t.x;
        int ty = (int)t.y;
        int bodyLeft = tx - TANK_WIDTH / 2;
        int bodyTop = ty - TANK_HEIGHT;

        // Body
        FillRect(bodyLeft, bodyTop + 4, TANK_WIDTH, TANK_HEIGHT - 4, t.colour);

        // Turret dome
        olc::Pixel turretCol = olc::Pixel(
            std::min(255, t.colour.r + 20),
            std::min(255, t.colour.g + 20),
            std::min(255, t.colour.b + 20));
        FillRect(tx - 6, bodyTop, 12, 6, turretCol);
        FillCircle(tx, bodyTop + 2, 5, turretCol);

        // Barrel (3 lines for thickness)
        float rad = GetBarrelRad(playerIdx);
        int bx = tx + (int)(cos(rad) * BARREL_LENGTH);
        int by = bodyTop - (int)(sin(rad) * BARREL_LENGTH) + 2;
        for (int w = -1; w <= 1; w++)
            DrawLine(tx + w, bodyTop + 2, bx + w, by, t.barrelColour);

        // Tracks and wheels
        FillRect(bodyLeft - 2, ty - 5, TANK_WIDTH + 4, 5, olc::Pixel(60, 60, 60));
        for (int i = 0; i < 4; i++) {
            int wx = bodyLeft + 2 + i * 6;
            FillCircle(wx + 2, ty - 3, 2, olc::Pixel(80, 80, 80));
        }

        // HP bar
        if (t.hp > 0) {
            int barWidth = TANK_WIDTH;
            int hpWidth = (int)(barWidth * ((float)t.hp / settings.startingHP));
            int barY = bodyTop - 8;
            FillRect(tx - barWidth / 2, barY, barWidth, 3, olc::Pixel(60, 0, 0));
            FillRect(tx - barWidth / 2, barY, hpWidth, 3, olc::Pixel(0, 200, 0));
        }

        // Player label
        std::string label = "P" + std::to_string(playerIdx + 1);
        DrawString(tx - 6, bodyTop - 18, label,
            playerIdx == 0 ? olc::Pixel(100, 200, 100) : olc::Pixel(200, 100, 100));
    }

    // Damage smoke rising from a tank that is on its last HP.
    // Uses worldTime to animate puffs cycling upward — no extra state needed.
    // =========================================================================
    // NIGHT MODE
    // =========================================================================

    // Tick lightning and return true on the frame a strike starts
    // Generate a jagged lightning bolt from sky to terrain at a random x position
    void GenerateLightningBolt() {
        lightningBolt.clear();
        float bx = (float)(SCREEN_W / 4 + rand() % (SCREEN_W / 2));
        float by = (float)GAME_TOP;
        lightningBolt.push_back({bx, by});

        while (by < SCREEN_H) {
            bx += (float)((rand() % 60) - 30); // jag left or right
            bx  = std::clamp(bx, 20.0f, (float)(SCREEN_W - 20));
            by += 18.0f + (float)(rand() % 20);
            lightningBolt.push_back({bx, by});

            // Occasionally branch a short secondary bolt
            if (rand() % 4 == 0 && lightningBolt.size() > 2) {
                float ex = bx + (float)((rand() % 80) - 40);
                float ey = by + 30.0f + (float)(rand() % 40);
                lightningBolt.push_back({bx, by});   // branch start
                lightningBolt.push_back({ex, ey});   // branch end
                lightningBolt.push_back({bx, by});   // back to main path
            }
        }
    }

    bool UpdateLightning(float dt) {
        if (!settings.nightMode) return false;
        if (lightningFlash > 0) {
            lightningFlash -= dt;
            return false;
        }
        lightningTimer -= dt;
        if (lightningTimer <= 0) {
            lightningFlash = 1.5f;
            lightningTimer = 5.0f + (float)(rand() % 7); // next strike in 5-12 s
            GenerateLightningBolt();
            return true; // this is the strike frame — caller plays thunder sound
        }
        return false;
    }

    // Darkness overlay drawn AFTER all normal rendering.
    // Each player sees only a circular area around their own tank.
    // During a lightning flash the entire battlefield is revealed.
    // If the local player has NVG active, a green tint replaces the darkness.
    void DrawNightOverlay(int localPlayerIdx) {
        if (!settings.nightMode) return;

        // Lightning or NVG: full visibility
        bool nvgActive = (tanks[localPlayerIdx].nvgTurns > 0);

        if (lightningFlash > 0) {
            // Full-screen white flash in the very first frames of the strike
            float flashFrac = lightningFlash / 1.5f; // 1=just struck, 0=fading out
            if (flashFrac > 0.9f) {
                uint8_t a = (uint8_t)(200 * (flashFrac - 0.9f) / 0.1f);
                FillRect(0, GAME_TOP, SCREEN_W, GAME_HEIGHT, olc::Pixel(230, 235, 255, a));
            }

            // Draw the bolt for the first 0.7 s of the flash, with flicker
            if (lightningFlash > 0.8f && !lightningBolt.empty()) {
                float boltFrac = (lightningFlash - 0.8f) / 0.7f; // 1→0 over 0.7s
                float flicker  = (sin(lightningFlash * 40.0f) + 1.0f) * 0.5f;
                uint8_t coreA  = (uint8_t)(255 * boltFrac * (0.6f + 0.4f * flicker));
                uint8_t glowA  = (uint8_t)(80  * boltFrac);

                for (size_t i = 1; i < lightningBolt.size(); i++) {
                    int x0 = (int)lightningBolt[i-1].x, y0 = (int)lightningBolt[i-1].y;
                    int x1 = (int)lightningBolt[i].x,   y1 = (int)lightningBolt[i].y;
                    // Wide blue-white glow
                    for (int o = -2; o <= 2; o++) {
                        DrawLine(x0+o, y0, x1+o, y1, olc::Pixel(180, 200, 255, glowA));
                    }
                    // Bright white core
                    DrawLine(x0, y0, x1, y1, olc::Pixel(255, 255, 255, coreA));
                }
            }
            return; // battlefield visible — no darkness overlay during flash
        }

        if (nvgActive) {
            // Green night-vision tint over the entire battlefield
            FillRect(0, GAME_TOP, SCREEN_W, GAME_HEIGHT, olc::Pixel(0, 60, 0, 160));
            return;
        }

        // Normal darkness: render line by line, leaving a clear circle around own tank
        const int visRadius = 90;
        int cx = (int)tanks[localPlayerIdx].x;
        int cy = (int)(tanks[localPlayerIdx].y - TANK_HEIGHT / 2);

        for (int y = GAME_TOP; y < SCREEN_H; y++) {
            int dy = y - cy;
            int clearHalf = 0;
            if (std::abs(dy) < visRadius)
                clearHalf = (int)std::sqrt((float)(visRadius * visRadius - dy * dy));

            // Left of the clear zone
            int leftEdge = cx - clearHalf;
            if (leftEdge > 0)
                FillRect(0, y, leftEdge, 1, olc::Pixel(0, 0, 0, 230));
            // Right of the clear zone
            int rightEdge = cx + clearHalf;
            if (rightEdge < SCREEN_W)
                FillRect(rightEdge, y, SCREEN_W - rightEdge, 1, olc::Pixel(0, 0, 0, 230));
            // Rows entirely outside the circle
            if (clearHalf == 0)
                FillRect(0, y, SCREEN_W, 1, olc::Pixel(0, 0, 0, 230));
        }

        // Soft glow edge around the visibility circle
        for (int i = 0; i < 12; i++) {
            float angle = i * 3.14159f / 6.0f;
            int ex = cx + (int)(cos(angle) * (visRadius - 1));
            int ey = cy + (int)(sin(angle) * (visRadius - 1));
            if (ey >= GAME_TOP && ey < SCREEN_H)
                Draw(ex, ey, olc::Pixel(40, 40, 40, 120));
        }
    }

    void DrawSmokeEffect(const Tank& t) {
        if (t.hp != 1) return;

        int cx = (int)t.x;
        int baseY = (int)(t.y - TANK_HEIGHT - 2); // just above the hull

        const int numPuffs = 6;
        const float cycle = 1.4f; // seconds for one puff to travel full height

        for (int i = 0; i < numPuffs; i++) {
            // Stagger puffs evenly across the cycle
            float phase = fmod(worldTime + i * (cycle / numPuffs), cycle) / cycle;

            float rise   = phase * 44.0f;                         // pixels risen
            float drift  = sin(worldTime * 1.8f + i * 1.7f) * 5.0f  // gentle wobble
                         + sin(i * 2.3f) * 3.0f;
            float radius = 3.0f + phase * 7.0f;                   // grows as it rises
            uint8_t alpha = (uint8_t)((1.0f - phase) * 180.0f);   // fades out

            int px = cx + (int)drift;
            int py = baseY - (int)rise;

            // Keep smoke in the game area
            if (py < GAME_TOP) continue;

            olc::Pixel col(160, 150, 140, alpha); // warm grey smoke
            FillCircle(px, py, (int)radius, col);
        }
    }

    // Draws one arc of the shield dome (upper semicircle only) at a given radius
    void DrawShieldArc(int cx, int cy, float radius, olc::Pixel col) {
        int steps = 20;
        int prevX = 0, prevY = 0;
        for (int i = 0; i <= steps; i++) {
            float theta = 3.14159f * i / steps; // sweeps 0..pi = the upper half
            int x = cx + (int)(cos(theta) * radius);
            int y = cy - (int)(sin(theta) * radius);
            if (i > 0) DrawLine(prevX, prevY, x, y, col);
            prevX = x; prevY = y;
        }
    }

    // Shield dome over a tank. Degrades thick blue (3 charges) -> orange (2) -> red (1).
    void DrawShield(const Tank& t) {
        if (t.shieldCharges <= 0) return;

        int cx = (int)t.x;
        int cy = (int)(t.y - TANK_HEIGHT - 8);

        olc::Pixel col;
        int lines;
        if (t.shieldCharges >= 3)      { col = olc::Pixel(80, 200, 255); lines = 3; }
        else if (t.shieldCharges == 2) { col = olc::Pixel(255, 150, 30); lines = 2; }
        else                            { col = olc::Pixel(255, 40, 40);  lines = 1; }

        float baseRadius = 18.0f;
        for (int i = 0; i < lines; i++)
            DrawShieldArc(cx, cy, baseRadius + i * 3.0f, col);
    }

    // =========================================================================
    // DRAWING - PROJECTILE & EXPLOSION
    // =========================================================================

    void DrawProjectile() {
        for (auto& p : projectiles) {
            if (!p.active) continue;

            for (size_t i = 1; i < p.trail.size(); i++) {
                float alpha = (float)i / p.trail.size();
                olc::Pixel trailCol = olc::Pixel(255, (int)(200 * alpha), 0, (int)(255 * alpha));
                DrawLine(
                    (int)p.trail[i - 1].x, (int)p.trail[i - 1].y,
                    (int)p.trail[i].x, (int)p.trail[i].y, trailCol);
            }

            olc::Pixel core = (p.weapon == WeaponType::HE) ? olc::Pixel(255, 120, 60) : olc::WHITE;
            FillCircle((int)p.x, (int)p.y, 3, core);
            FillCircle((int)p.x, (int)p.y, 2, olc::YELLOW);
        }
    }

    void DrawExplosion() {
        for (auto& ex : explosions) {
            if (!ex.active) continue;

            float t = ex.radius / ex.maxRadius;
            int r = 255;
            int g = (int)(255 * (1.0f - t * 0.7f));
            int b = (int)(100 * (1.0f - t));

            for (float ring = ex.radius; ring > 0; ring -= 2) {
                float rt = ring / ex.radius;
                DrawCircle((int)ex.x, (int)ex.y, (int)ring,
                    olc::Pixel(r, (int)(g * rt), (int)(b * rt)));
            }
            FillCircle((int)ex.x, (int)ex.y, (int)(ex.radius * 0.4f),
                olc::Pixel(255, 255, 200));
        }
    }

    // =========================================================================
    // DRAWING - PICKUPS
    // =========================================================================

    void DrawPickup() {
        if (!pickup.active) return;

        float bob = sin(worldTime * 3.0f) * 2.0f;
        int cx = (int)pickup.x;
        int cy = (int)(pickup.y - 10 + bob);

        if (pickup.type == PickupType::MYSTERY) {
            float pulse = (sin(worldTime * 4.0f) + 1.0f) * 0.5f;
            DrawCircle(cx, cy, 11 + (int)(pulse * 2), olc::Pixel(255, 220, 100, 90));
            FillCircle(cx, cy, 9, olc::Pixel(255, 200, 0));
            DrawCircle(cx, cy, 9, olc::Pixel(150, 100, 0));
            DrawCircle(cx, cy, 7, olc::Pixel(255, 230, 100));
            DrawString(cx - 4, cy - 5, "?", olc::Pixel(120, 60, 0));
        } else if (pickup.type == PickupType::HEALTH) {
            FillRect(cx - 7, cy - 7, 14, 14, olc::Pixel(200, 30, 30));
            DrawRect(cx - 7, cy - 7, 14, 14, olc::WHITE);
            FillRect(cx - 1, cy - 5, 2, 10, olc::WHITE);
            FillRect(cx - 5, cy - 1, 10, 2, olc::WHITE);
        } else { // NVG goggles — two green circles
            float pulse = (sin(worldTime * 5.0f) + 1.0f) * 0.5f;
            DrawCircle(cx, cy, 10 + (int)(pulse), olc::Pixel(0, 200, 0, 80));
            FillCircle(cx - 4, cy, 4, olc::Pixel(0, 180, 0));
            FillCircle(cx + 4, cy, 4, olc::Pixel(0, 180, 0));
            FillRect(cx - 1, cy - 1, 2, 2, olc::Pixel(0, 255, 0));
            DrawString(cx - 12, cy + 7, "NVG", olc::Pixel(0, 220, 0));
        }
    }

    // Banner across the top of the game area explaining what was just collected
    void DrawPickupMessage() {
        if (pickupMessageTimer <= 0) return;

        int boxW = (int)pickupMessage.length() * 8 + 32;
        int boxX = SCREEN_W / 2 - boxW / 2;
        int boxY = GAME_TOP + 10;

        int a = (pickupMessageTimer < 1.0f) ? (int)(220 * pickupMessageTimer) : 220;
        FillRect(boxX, boxY, boxW, 26, olc::Pixel(20, 20, 20, a));
        DrawRect(boxX, boxY, boxW, 26, olc::Pixel(255, 200, 0, a));
        DrawString(boxX + 16, boxY + 9, pickupMessage, olc::Pixel(255, 230, 150, a));
    }

    // =========================================================================
    // DRAWING - HUD
    // =========================================================================

    void DrawUI() {
        // Wood background with noise
        FillRect(0, 0, SCREEN_W, UI_HEIGHT, olc::Pixel(101, 67, 33));
        for (int y = 0; y < UI_HEIGHT; y++) {
            for (int x = 0; x < SCREEN_W; x += 3) {
                int noise = rand() % 20 - 10;
                if (noise > 5)
                    Draw(x, y, olc::Pixel(111 + noise, 77 + noise, 43 + noise));
            }
        }

        // Bevelled border
        DrawRect(0, 0, SCREEN_W - 1, UI_HEIGHT - 1, olc::Pixel(60, 40, 20));
        DrawRect(1, 1, SCREEN_W - 3, UI_HEIGHT - 3, olc::Pixel(140, 100, 60));
        DrawLine(0, UI_HEIGHT, SCREEN_W, UI_HEIGHT, olc::Pixel(40, 25, 10));
        DrawLine(0, UI_HEIGHT + 1, SCREEN_W, UI_HEIGHT + 1, olc::Pixel(40, 25, 10));

        Tank& t = tanks[currentPlayer];
        olc::Pixel textCol = olc::WHITE;
        olc::Pixel valCol = olc::YELLOW;
        int scale = 2;

        // Player name header (uses settings name instead of just "P1")
        std::string playerStr = settings.playerNames[currentPlayer] + ", YOUR SHOT";
        int headerX = SCREEN_W / 2 - (int)(playerStr.length() * 4 * scale);
        headerX = std::max(4, headerX);
        DrawString(headerX, 8, playerStr,
            currentPlayer == 0 ? olc::Pixel(100, 255, 100) : olc::Pixel(255, 100, 100), scale);

        int leftCol = 20;
        int midCol = SCREEN_W / 2 - 80;
        int rightCol = SCREEN_W - 220;
        int row1 = 40;
        int row2 = 65;
        int row3 = 90;
        int row4 = 115;
        int row5 = 144;

        // Angle (with +/- buttons and type-in support)
        bool amHeld = false, apHeld = false, pmHeld = false, ppHeld = false;
        bool mlHeld = false, mrHeld = false; // movement left/right click buttons

        DrawString(leftCol, row1, "ANGLE:", textCol);
        if (inputMode == InputMode::TYPING_ANGLE) {
            FillRect(leftCol + 56, row1 - 2, 60, 12, olc::Pixel(20, 20, 60));
            DrawRect(leftCol + 56, row1 - 2, 60, 12, olc::Pixel(100, 100, 255));
            std::string display = inputBuffer;
            if (fmod(cursorBlink, 0.8f) < 0.4f) display += "_";
            DrawString(leftCol + 58, row1, display, olc::Pixel(100, 200, 255));
        } else {
            amHeld = DrawHudButton(leftCol + 56, row1 - 3, "-");
            FillRect(leftCol + 78, row1 - 2, 30, 14, olc::Pixel(20, 15, 10));
            DrawString(leftCol + 80, row1 + 1, std::to_string(t.angle), valCol);
            apHeld = DrawHudButton(leftCol + 110, row1 - 3, "+");
            DrawString(leftCol + 134, row1, "deg", olc::Pixel(150, 150, 150));
        }

        // Power (with +/- buttons and type-in support)
        DrawString(leftCol, row2, "POWER:", textCol);
        if (inputMode == InputMode::TYPING_POWER) {
            FillRect(leftCol + 56, row2 - 2, 60, 12, olc::Pixel(20, 20, 60));
            DrawRect(leftCol + 56, row2 - 2, 60, 12, olc::Pixel(100, 100, 255));
            std::string display = inputBuffer;
            if (fmod(cursorBlink, 0.8f) < 0.4f) display += "_";
            DrawString(leftCol + 58, row2, display, olc::Pixel(100, 200, 255));
        } else {
            pmHeld = DrawHudButton(leftCol + 56, row2 - 3, "-");
            FillRect(leftCol + 78, row2 - 2, 30, 14, olc::Pixel(20, 15, 10));
            DrawString(leftCol + 80, row2 + 1, std::to_string(t.power), valCol);
            ppHeld = DrawHudButton(leftCol + 110, row2 - 3, "+");
        }

        // Moves — [<] remaining [>] click buttons, greyed when budget is exhausted
        // Drawn BEFORE RepeatTick so mlHeld/mrHeld are captured in time this frame
        DrawString(leftCol, row3, "MOVES:", textCol);
        bool moveDisabled = (t.movesLeft <= 0 || state != GameState::AIM || inputMode != InputMode::NONE);
        if (!moveDisabled) {
            mlHeld = DrawHudButton(leftCol + 56, row3 - 3, "<");
            FillRect(leftCol + 78, row3 - 2, 30, 14, olc::Pixel(20, 15, 10));
            DrawString(leftCol + 80, row3 + 1, std::to_string(t.movesLeft), valCol);
            mrHeld = DrawHudButton(leftCol + 110, row3 - 3, ">");
        } else {
            FillRect(leftCol + 56, row3 - 2, 73, 14, olc::Pixel(30, 20, 10));
            DrawString(leftCol + 80, row3 + 1, std::to_string(t.movesLeft), olc::Pixel(100,100,100));
        }

        // Apply all mouse button clicks with repeat throttling
        if (state == GameState::AIM && inputMode == InputMode::NONE) {
            bool anyBtnHeld = amHeld || apHeld || pmHeld || ppHeld || mlHeld || mrHeld;
            if (RepeatTick(anyBtnHeld, frameTime, mouseRepeatTimer)) {
                if (amHeld) t.angle = std::max(0, t.angle - 1);
                if (apHeld) t.angle = std::min(110, t.angle + 1);
                if (pmHeld) t.power = std::max(5, t.power - 1);
                if (ppHeld) t.power = std::min(MAX_POWER, t.power + 1);
                if (mlHeld && t.movesLeft > 0) {
                    int newX = (int)t.x - 1;
                    if (newX > TANK_WIDTH / 2) { t.x = (float)newX; t.y = terrain[newX]; t.movesLeft--; }
                }
                if (mrHeld && t.movesLeft > 0) {
                    int newX = (int)t.x + 1;
                    if (newX < SCREEN_W - TANK_WIDTH / 2) { t.x = (float)newX; t.y = terrain[newX]; t.movesLeft--; }
                }
            }
        }

        // Power bar
        int barX = leftCol + 136;
        int barW = 100;
        FillRect(barX, row2 - 1, barW, 9, olc::Pixel(40, 40, 40));
        FillRect(barX, row2 - 1, (int)(barW * t.power / 100.0f), 9,
            olc::Pixel(200, 50 + t.power, 50));
        DrawRect(barX, row2 - 1, barW, 9, olc::Pixel(150, 150, 150));

        // Wind
        DrawString(midCol, row1, "WIND:", textCol);
        std::string windStr;
        if (wind > 0.5f) windStr = ">>> " + std::to_string((int)wind);
        else if (wind < -0.5f) windStr = std::to_string((int)wind) + " <<<";
        else windStr = "CALM";
        DrawString(midCol + 48, row1, windStr, WindColour(std::abs(wind)));

        // Round
        DrawString(midCol, row2, "ROUND:", textCol);
        DrawString(midCol + 56, row2, std::to_string(roundNumber), valCol);

        // Player panels: name, round-win score, and a segmented HP bar
        for (int p = 0; p < 2; p++) {
            int panelY = (p == 0) ? 38 : 63;
            int panelX = rightCol;
            int panelW = SCREEN_W - rightCol - 8;

            // Dark background
            FillRect(panelX, panelY, panelW, 22, olc::Pixel(20, 15, 10));
            DrawRect(panelX, panelY, panelW - 1, 21,
                p == 0 ? olc::Pixel(60, 100, 60) : olc::Pixel(100, 60, 60));

            // Name (up to 7 chars)
            std::string name = settings.playerNames[p].substr(0, 7);
            olc::Pixel nameCol = (p == 0) ? olc::Pixel(100, 255, 100) : olc::Pixel(255, 100, 100);
            DrawString(panelX + 4, panelY + 4, name, nameCol);

            // Round wins (top-right of panel)
            std::string wins = std::to_string(tanks[p].score) + "W";
            DrawString(panelX + panelW - (int)wins.length() * 8 - 4, panelY + 4, wins, valCol);

            // Segmented HP bar — one pip per HP point
            int maxHP = settings.startingHP;
            int curHP = tanks[p].hp;
            int pipTotalW = panelW - 8;
            int pipW = (pipTotalW - (maxHP - 1) * 2) / maxHP; // pip width
            int pipH = 7;
            int pipY = panelY + 13;
            for (int h = 0; h < maxHP; h++) {
                int pipX = panelX + 4 + h * (pipW + 2);
                bool filled = (h < curHP);

                olc::Pixel fillCol;
                if (!filled) {
                    fillCol = olc::Pixel(40, 30, 20); // empty
                } else if (curHP == 1) {
                    fillCol = olc::Pixel(220, 40, 40);  // critical — red
                } else if (curHP <= maxHP / 2) {
                    fillCol = olc::Pixel(220, 140, 30); // half — orange
                } else {
                    fillCol = (p == 0) ? olc::Pixel(60, 200, 60) : olc::Pixel(200, 60, 60); // healthy
                }

                FillRect(pipX, pipY, pipW, pipH, fillCol);
                DrawRect(pipX, pipY, pipW, pipH, olc::Pixel(60, 50, 40));
            }
        }

        // Controls help
        if (inputMode != InputMode::NONE) {
            std::string fieldName = (inputMode == InputMode::TYPING_ANGLE) ? "ANGLE" : "POWER";
            DrawString(leftCol, row4, "Type " + fieldName + ": [0-9] Enter  [TAB] Next  [ESC] Cancel",
                olc::Pixel(100, 200, 255));
        } else {
            DrawString(leftCol, row4, "[0-9] Type  [A/D] Angle  [W/S] Power  [</>] Move  [SPACE] Fire",
                olc::Pixel(180, 180, 180));
        }

        // Weapon selector — pick a special for this shot (NORMAL has infinite ammo)
        // 6 weapon boxes spread across the available width
        int numBoxes = 6, gap = 6;
        int boxW = (rightCol - leftCol - (numBoxes - 1) * gap) / numBoxes;
        int boxH = 24;
        bool canSelect = (state == GameState::AIM && inputMode == InputMode::NONE);

        if (DrawWeaponBox(leftCol + 0 * (boxW + gap), row5, boxW, boxH,
                "NORMAL", 0, true, selectedWeapon == WeaponType::NORMAL, 0) && canSelect)
            selectedWeapon = WeaponType::NORMAL;

        if (DrawWeaponBox(leftCol + 1 * (boxW + gap), row5, boxW, boxH,
                "HI-EXPLO", t.ammoHE, false, selectedWeapon == WeaponType::HE, 1) && canSelect && t.ammoHE > 0)
            selectedWeapon = WeaponType::HE;

        if (DrawWeaponBox(leftCol + 2 * (boxW + gap), row5, boxW, boxH,
                "CLUSTER", t.ammoCluster, false, selectedWeapon == WeaponType::CLUSTER, 2) && canSelect && t.ammoCluster > 0)
            selectedWeapon = WeaponType::CLUSTER;

        if (DrawWeaponBox(leftCol + 3 * (boxW + gap), row5, boxW, boxH,
                "LASER", t.ammoLaser, false, selectedWeapon == WeaponType::LASER, 3) && canSelect && t.ammoLaser > 0)
            selectedWeapon = WeaponType::LASER;

        if (DrawWeaponBox(leftCol + 4 * (boxW + gap), row5, boxW, boxH,
                "BALLISTIC", t.ammoBallistics, false, false, 4) && canSelect && t.ammoBallistics > 0)
            UseBallisticsComputer();

        if (DrawWeaponBox(leftCol + 5 * (boxW + gap), row5, boxW, boxH,
                "SHIELD", t.ammoShield, false, false, 5) && canSelect && t.ammoShield > 0)
            ActivateShield();

        // Plunger fire button and surrender flag — sit in the otherwise empty
        // space above the weapon row, to the right of the score readout
        if (DrawPlungerButton(rightCol, 86, 95, 50) && canSelect) {
            PlayPlungerSound();
            Fire();
        }

        if (DrawFlagButton(rightCol + 100, 86, 95, 50) && canSelect) {
            SurrenderMatch();
        }
    }

    // =========================================================================
    // DRAWING - WIND & AIM INDICATORS
    // =========================================================================

    // Colour grades green → yellow → orange → red as wind strength increases
    olc::Pixel WindColour(float strength) {
        float t = std::clamp(strength / 10.0f, 0.0f, 1.0f);
        int r = (int)(60  + 195 * std::min(1.0f, t * 2.0f));
        int g = (int)(220 - 160 * std::min(1.0f, t * 1.6f));
        return olc::Pixel(r, g, 40);
    }

    void DrawWindIndicator() {
        int cx = SCREEN_W / 2;
        int cy = GAME_TOP + 18;

        // Dark pill for contrast — arrow sits on top of this
        FillRect(cx - 72, cy - 9, 144, 18, olc::Pixel(0, 0, 0, 160));
        DrawRect(cx - 72, cy - 9, 143, 17, olc::Pixel(70, 70, 70, 130));

        if (std::abs(wind) <= 0.5f) {
            DrawString(cx - 16, cy - 4, "CALM", olc::Pixel(180, 220, 180));
            return;
        }

        float absWind  = std::abs(wind);
        int   dir      = wind > 0 ? 1 : -1;
        int   arrowLen = (int)(absWind * 6);   // up to 60 px at max wind
        int   headLen  = 9;
        int   headW    = 6;
        int   shaftEnd = cx + arrowLen * dir;
        olc::Pixel col = WindColour(absWind);

        // Thick shaft — 3 pixels tall
        for (int dy = -1; dy <= 1; dy++)
            DrawLine(cx, cy + dy, shaftEnd, cy + dy, col);

        // Filled triangular arrowhead
        for (int i = -headW; i <= headW; i++) {
            float frac   = 1.0f - (float)std::abs(i) / headW;
            int   xStart = shaftEnd - (int)(headLen * frac) * dir;
            DrawLine(xStart, cy + i, shaftEnd, cy + i, col);
        }

        // Small origin tick so the arrow has a visible base
        DrawLine(cx, cy - 4, cx, cy + 4, col);
    }

    void DrawAimGuide() {
        Tank& t = tanks[currentPlayer];
        float rad = GetBarrelRad(currentPlayer);
        int bx = (int)(t.x + cos(rad) * (BARREL_LENGTH + 5));
        int by = (int)(t.y - TANK_HEIGHT - sin(rad) * (BARREL_LENGTH + 5) + 2);

        if (selectedWeapon == WeaponType::LASER) {
            // Long pulsing preview line showing exactly where the beam will travel
            float dx = cos(rad), dy = -sin(rad);
            float pulse = (sin(stateTimer * 6.0f) + 1.0f) * 0.5f;
            olc::Pixel col(255, (int)(80 + 100 * pulse), (int)(80 + 100 * pulse), 220);
            for (int i = 0; i < 60; i++) {
                int px = bx + (int)(dx * i * 6);
                int py = by + (int)(dy * i * 6);
                if (px < 0 || px >= SCREEN_W || py < 0 || py > SCREEN_H) break;
                FillCircle(px, py, 2, col);
            }
            return;
        }

        for (int i = 0; i < 5; i++) {
            float dist = 20 + i * 8.0f;
            int dx = bx + (int)(cos(rad) * dist * (i % 2 == 0 ? 1 : 0.5f));
            int dy = by - (int)(sin(rad) * dist * (i % 2 == 0 ? 1 : 0.5f));
            FillCircle(dx, dy, 3, olc::Pixel(255, 255, 255, 230));
            DrawCircle(dx, dy, 3, olc::Pixel(0, 0, 0, 120));
        }
    }

    // Dashed magenta trajectory line + landing marker for the "Daddy" cheat
    void DrawTrajectoryCheat() {
        if (!trajectoryCheat[currentPlayer]) return;

        std::vector<olc::vf2d> pts = SimulateTrajectoryPoints();
        olc::Pixel col(255, 0, 220, 220);
        for (size_t i = 1; i < pts.size(); i++) {
            if ((i / 4) % 2 == 0)
                DrawLine((int)pts[i - 1].x, (int)pts[i - 1].y, (int)pts[i].x, (int)pts[i].y, col);
        }
        if (!pts.empty()) {
            olc::vf2d land = pts.back();
            DrawCircle((int)land.x, (int)land.y, 6, col);
            DrawCircle((int)land.x, (int)land.y, 3, col);
        }
    }

    // Crosshair reticle shown on the enemy tank after using the ballistics computer
    void DrawReticle(int playerIdx) {
        Tank& t = tanks[playerIdx];
        int cx = (int)t.x, cy = (int)(t.y - TANK_HEIGHT / 2);
        float pulse = (sin(stateTimer * 4.0f) + 1.0f) * 0.5f;
        olc::Pixel col(255, (int)(50 + 50 * pulse), (int)(50 + 50 * pulse));
        DrawCircle(cx, cy, 14, col);
        DrawCircle(cx, cy, 8, col);
        DrawLine(cx - 18, cy, cx - 10, cy, col);
        DrawLine(cx + 10, cy, cx + 18, cy, col);
        DrawLine(cx, cy - 18, cx, cy - 10, col);
        DrawLine(cx, cy + 10, cx, cy + 18, col);
    }

    // The laser beam itself: draws progressively further along the path as
    // stateTimer advances, giving the beam a brief "zap" travel animation
    void DrawLaserBeam() {
        if (laserTrail.empty()) return;
        float t = std::clamp(stateTimer / 0.4f, 0.0f, 1.0f);
        int showCount = (int)(laserTrail.size() * std::min(1.0f, t * 3.0f));

        olc::Pixel glowCol(255, 160, 160, 120);
        olc::Pixel beamCol(255, 60, 60);

        for (int i = 1; i < showCount && i < (int)laserTrail.size(); i++) {
            DrawLine((int)laserTrail[i - 1].x, (int)laserTrail[i - 1].y,
                     (int)laserTrail[i].x, (int)laserTrail[i].y, glowCol);
            DrawLine((int)laserTrail[i - 1].x + 1, (int)laserTrail[i - 1].y,
                     (int)laserTrail[i].x + 1, (int)laserTrail[i].y, beamCol);
        }
    }

    // Draw one weapon selector box in the HUD. Returns true if clicked.
    // Small icon matching the menu's "Quick Guide" art, drawn at (cx,cy).
    // icon: 0=Normal 1=HI-EXPLO 2=CLUSTER 3=LASER 4=BALLISTIC
    void DrawWeaponIcon(int icon, int cx, int cy, bool disabled) {
        if (disabled) {
            DrawCircle(cx, cy, 6, olc::Pixel(90, 90, 90));
            return;
        }
        switch (icon) {
            case 0: // NORMAL — plain shell
                FillCircle(cx, cy, 5, olc::WHITE);
                FillCircle(cx, cy, 3, olc::YELLOW);
                break;
            case 1: // HI-EXPLO — orange shell
                FillCircle(cx, cy, 6, olc::Pixel(255, 120, 60));
                DrawCircle(cx, cy, 6, olc::Pixel(180, 60, 20));
                break;
            case 2: // CLUSTER — three shells
                FillCircle(cx - 4, cy + 2, 2, olc::YELLOW);
                FillCircle(cx, cy - 3, 2, olc::YELLOW);
                FillCircle(cx + 4, cy + 2, 2, olc::YELLOW);
                break;
            case 3: // LASER — red beam
                DrawLine(cx - 6, cy + 6, cx + 6, cy - 6, olc::Pixel(255, 60, 60));
                DrawLine(cx - 5, cy + 6, cx + 7, cy - 6, olc::Pixel(255, 150, 150));
                break;
            case 4: // BALLISTIC — mini reticle
                DrawCircle(cx, cy, 5, olc::Pixel(255, 80, 80));
                DrawLine(cx, cy - 7, cx, cy - 4, olc::Pixel(255, 80, 80));
                DrawLine(cx, cy + 4, cx, cy + 7, olc::Pixel(255, 80, 80));
                break;
            case 5: // SHIELD — mini dome arcs
                DrawShieldArc(cx, cy + 4, 8, olc::Pixel(80, 200, 255));
                DrawShieldArc(cx, cy + 4, 5, olc::Pixel(80, 200, 255));
                break;
        }
    }

    bool DrawWeaponBox(int x, int y, int w, int h, const std::string& label,
                        int ammo, bool infinite, bool selected, int icon) {
        bool disabled = !infinite && ammo <= 0;
        bool hovering = GetMouseX() >= x && GetMouseX() < x + w
                     && GetMouseY() >= y && GetMouseY() < y + h;

        olc::Pixel bg = disabled ? olc::Pixel(40, 40, 40)
                       : selected ? olc::Pixel(120, 40, 20)
                       : olc::Pixel(80, 55, 30);
        FillRect(x, y, w, h, bg);
        olc::Pixel borderCol = selected ? olc::Pixel(255, 180, 80) : olc::Pixel(160, 120, 70);
        DrawRect(x, y, w - 1, h - 1, borderCol);

        DrawWeaponIcon(icon, x + 13, y + h / 2, disabled);

        olc::Pixel textCol = disabled ? olc::Pixel(100, 100, 100) : olc::WHITE;
        DrawString(x + 26, y + 3, label, textCol);
        if (!infinite) {
            std::string ammoStr = "x" + std::to_string(ammo);
            DrawString(x + w - (int)ammoStr.length() * 8 - 4, y + 3, ammoStr,
                disabled ? olc::Pixel(100, 100, 100) : olc::YELLOW);
        }

        return !disabled && hovering && GetMouse(0).bPressed;
    }

    // Detonator-style plunger button — pushes down visually while held,
    // fires the current weapon on release. Returns true on click.
    bool DrawPlungerButton(int x, int y, int w, int h) {
        bool hovering = GetMouseX() >= x && GetMouseX() < x + w
                     && GetMouseY() >= y && GetMouseY() < y + h;
        bool held = hovering && GetMouse(0).bHeld;

        FillRect(x, y, w, h, olc::Pixel(80, 55, 30));
        DrawRect(x, y, w - 1, h - 1, olc::Pixel(160, 120, 70));
        DrawString(x + 4, y + 2, "FIRE", olc::Pixel(220, 200, 180));

        int cx = x + w / 2;
        int baseY = y + h - 10;
        FillCircle(cx, baseY, 11, olc::Pixel(170, 170, 170));
        DrawCircle(cx, baseY, 11, olc::Pixel(90, 90, 90));

        int handleY = held ? baseY - 6 : baseY - 16;
        DrawLine(cx, baseY - 2, cx, handleY, olc::Pixel(50, 50, 50));
        DrawLine(cx + 1, baseY - 2, cx + 1, handleY, olc::Pixel(50, 50, 50));
        FillCircle(cx, handleY, 9, olc::Pixel(180, 20, 20));
        FillCircle(cx - 2, handleY - 2, 4, olc::Pixel(230, 60, 60));

        return hovering && GetMouse(0).bPressed;
    }

    // White flag surrender button. Returns true on click.
    bool DrawFlagButton(int x, int y, int w, int h) {
        bool hovering = GetMouseX() >= x && GetMouseX() < x + w
                     && GetMouseY() >= y && GetMouseY() < y + h;

        FillRect(x, y, w, h, hovering ? olc::Pixel(100, 70, 40) : olc::Pixel(80, 55, 30));
        DrawRect(x, y, w - 1, h - 1, olc::Pixel(160, 120, 70));
        DrawString(x + 4, y + 2, "SURRENDER", olc::Pixel(220, 200, 180));

        int poleX = x + w / 2 - 4;
        int poleTop = y + 14;
        int poleBottom = y + h - 6;
        DrawLine(poleX, poleTop, poleX, poleBottom, olc::Pixel(90, 60, 30));
        DrawLine(poleX + 1, poleTop, poleX + 1, poleBottom, olc::Pixel(90, 60, 30));

        FillRect(poleX + 2, poleTop, 20, 13, olc::WHITE);
        DrawRect(poleX + 2, poleTop, 20, 13, olc::Pixel(180, 180, 180));

        return hovering && GetMouse(0).bPressed;
    }

    // =========================================================================
    // DRAWING - TITLE & GAME OVER SCREENS
    // =========================================================================

    // =========================================================================
    // DRAWING - LOBBY SCREEN
    // =========================================================================

    void DrawLobbyScreen() {
        DrawWoodPanel(0, 0, SCREEN_W, SCREEN_H);
        DrawString(SCREEN_W / 2 - 112, 18, "T A N X", olc::Pixel(255, 200, 0), 4);
        DrawString(SCREEN_W / 2 - 60, 70, "NETWORK GAME", olc::WHITE, 2);

        float pulse = (sin(stateTimer * 3.0f) + 1.0f) * 0.5f;
        netLobbyBlink += frameTime;

        // --- Name field (always shown, both host and client set their own name) ---
        DrawString(SCREEN_W/2 - 80, 120, "Your name:", olc::WHITE);
        int nameX = SCREEN_W/2 - 80, nameY = 135, nameW = 260;
        bool nameHovered = GetMouseX() >= nameX && GetMouseX() < nameX + nameW
                        && GetMouseY() >= nameY && GetMouseY() < nameY + 22;

        FillRect(nameX, nameY, nameW, 22, olc::Pixel(20,15,10));
        DrawRect(nameX, nameY, nameW, 22, netEditingName ? olc::Pixel(255,200,80) : olc::Pixel(80,60,30));
        std::string nameDisplay = netLobbyName + (netEditingName && fmod(netLobbyBlink, 0.8f) < 0.4f ? "_" : "");
        DrawString(nameX + 6, nameY + 7, nameDisplay.empty() ? "(click to set your name)" : nameDisplay,
            netLobbyName.empty() ? olc::Pixel(120,120,120) : olc::YELLOW);

        if (GetMouse(0).bPressed && nameHovered) netEditingName = true;
        if (GetMouse(0).bPressed && !nameHovered) netEditingName = false;

        // --- Mode panels ---
        if (netMode == NetMode::NONE) {
            DrawWoodPanel(100, 200, 240, 80);
            DrawString(150, 225, "HOST GAME", olc::Pixel(100, 255, 100), 2);
            DrawString(120, 255, "Share your IP with other player", olc::Pixel(180,180,180));

            DrawWoodPanel(460, 200, 240, 80);
            DrawString(510, 225, "JOIN GAME", olc::Pixel(100, 200, 255), 2);
            DrawString(480, 255, "Type the host's IP address", olc::Pixel(180,180,180));

            DrawString(SCREEN_W/2 - 40, 500, "ESC = back", olc::Pixel(140,140,140));

            if (GetMouse(0).bPressed && !nameHovered) {
                int mx = GetMouseX(), my = GetMouseY();
                if (mx >= 100 && mx < 340 && my >= 200 && my < 280) {
                    settings.playerNames[0] = netLobbyName.empty() ? "Player 1" : netLobbyName;
                    if (NetStartHost()) { netMode = NetMode::HOST; netReadyToStart = false; }
                }
                if (mx >= 460 && mx < 700 && my >= 200 && my < 280) {
                    settings.playerNames[1] = netLobbyName.empty() ? "Player 2" : netLobbyName;
                    netMode = NetMode::CLIENT;
                    netIPInput.clear();
                    netNameSent = false;
                }
            }
        }
        else if (netMode == NetMode::HOST) {
            DrawString(SCREEN_W/2 - 40, 200, "HOST MODE", olc::Pixel(100,255,100), 2);
            DrawString(SCREEN_W/2 - 80, 250, "Your IP address:", olc::WHITE);
            DrawString(SCREEN_W/2 - (int)(localIP.length()*12), 275, localIP, olc::YELLOW, 3);
            DrawString(SCREEN_W/2 - 100, 345, "Share this with the other player.", olc::Pixel(180,180,180));

            if (!netConnected) {
                std::string waiting = "Waiting for connection...";
                DrawString(SCREEN_W/2 - (int)(waiting.length()*4), 400, waiting,
                    olc::Pixel(100,200,255,(int)(200+55*pulse)));
            } else if (!netReadyToStart) {
                DrawString(SCREEN_W/2 - 88, 400, "Connected! Waiting for", olc::Pixel(100,255,100));
                DrawString(SCREEN_W/2 - 64, 418, "player name...", olc::Pixel(100,255,100));
            } else {
                // Both connected and name received — start the match (runs once)
                DrawString(SCREEN_W/2 - 72, 400, "Starting match...", olc::WHITE);
                StartNewMatch();
                NetSendMatchStart();
                NetSendRoundStart();
            }
            DrawString(SCREEN_W/2 - 40, 500, "ESC = cancel", olc::Pixel(140,140,140));
        }
        else if (netMode == NetMode::CLIENT) {
            DrawString(SCREEN_W/2 - 40, 200, "JOIN GAME", olc::Pixel(100,200,255), 2);
            DrawString(SCREEN_W/2 - 100, 255, "Enter host IP address:", olc::WHITE);

            int fieldX = SCREEN_W/2 - 140, fieldY = 275, fieldW = 280;
            bool ipHovered = GetMouseX() >= fieldX && GetMouseX() < fieldX+fieldW
                          && GetMouseY() >= fieldY && GetMouseY() < fieldY+24;
            FillRect(fieldX, fieldY, fieldW, 24, olc::Pixel(20,15,10));
            DrawRect(fieldX, fieldY, fieldW, 24, (!netEditingName) ? olc::Pixel(100,100,255) : olc::Pixel(80,60,30));
            std::string ipDisplay = netIPInput + ((!netEditingName) && fmod(netLobbyBlink,0.8f)<0.4f ? "_" : "");
            DrawString(fieldX + 6, fieldY + 8, ipDisplay, olc::Pixel(100,200,255));
            if (GetMouse(0).bPressed && ipHovered) netEditingName = false;

            DrawString(SCREEN_W/2 - 60, 315, "ENTER to connect", olc::Pixel(200,200,200));
            DrawString(SCREEN_W/2 - 40, 500, "ESC = cancel", olc::Pixel(140,140,140));

            if (!netConnected) {
                if (!netIPInput.empty())
                    DrawString(SCREEN_W/2 - 60, 370, "Connecting...", olc::YELLOW);
            } else {
                DrawString(SCREEN_W/2 - 72, 370, "Connected!", olc::Pixel(100,255,100));
                DrawString(SCREEN_W/2 - 100, 390, "Waiting for host to start...", olc::Pixel(180,180,180));
            }
        }
    }

    // Lobby keyboard input — handles both the name field and the IP field
    void UpdateLobbyInput() {
        netLobbyBlink += frameTime;

        if (netEditingName) {
            // Name field is active
            char letter = GetLetterPressed();
            if (letter != '\0' && netLobbyName.length() < 16) netLobbyName += letter;
            if (GetKey(olc::Key::SPACE).bPressed && netLobbyName.length() < 16) netLobbyName += ' ';
            if (GetKey(olc::Key::BACK).bPressed && !netLobbyName.empty()) netLobbyName.pop_back();
            if (GetKey(olc::Key::ENTER).bPressed || GetKey(olc::Key::RETURN).bPressed) netEditingName = false;
        } else if (netMode == NetMode::CLIENT && !netConnected) {
            // IP field is active
            int digit = GetDigitPressed();
            if (digit >= 0 && netIPInput.length() < 15) netIPInput += std::to_string(digit);
            if (GetKey(olc::Key::PERIOD).bPressed && netIPInput.length() < 15) netIPInput += '.';
            if (GetKey(olc::Key::BACK).bPressed && !netIPInput.empty()) netIPInput.pop_back();
            if ((GetKey(olc::Key::ENTER).bPressed || GetKey(olc::Key::RETURN).bPressed) && !netIPInput.empty())
                NetConnectToHost(netIPInput);
        }

        if (GetKey(olc::Key::ESCAPE).bPressed) {
            if (netEditingName) { netEditingName = false; return; }
            NetClose();
            netReadyToStart = false; netNameSent = false;
            if (netMode == NetMode::NONE) { state = GameState::MENU; stateTimer = 0; }
            netMode = NetMode::NONE;
        }
    }

    void DrawTitleScreen() {
        Clear(olc::BLACK);

        DrawString(SCREEN_W / 2 - 80, 150, "T A N X", olc::Pixel(255, 200, 0), 5);
        DrawString(SCREEN_W / 2 - 140, 250, "A Classic Artillery Game", olc::Pixel(180, 180, 180), 2);

        float pulse = (sin(stateTimer * 3.0f) + 1.0f) * 0.5f;
        olc::Pixel startCol = olc::Pixel(
            (int)(100 + 155 * pulse), (int)(200 * pulse), (int)(50 + 50 * pulse));
        DrawString(SCREEN_W / 2 - 120, 350, "PRESS SPACE TO START", startCol, 2);

        DrawString(SCREEN_W / 2 - 100, 450, "Inspired by TANX (1991)", olc::Pixel(120, 120, 120));
        DrawString(SCREEN_W / 2 - 68, 470, "by Gary Roberts", olc::Pixel(120, 120, 120));
    }

    void DrawGameOverScreen() {
        // Determine winner safely — if both are dead it's a draw
        int winner = (tanks[0].hp <= 0 && tanks[1].hp > 0) ? 1 :
                     (tanks[1].hp <= 0 && tanks[0].hp > 0) ? 0 : -1;
        bool matchOver = (winner >= 0 && tanks[winner].score >= settings.roundsToWin);

        FillRect(SCREEN_W / 2 - 200, SCREEN_H / 2 - 80, 400, 160, olc::Pixel(0, 0, 0, 200));
        DrawRect(SCREEN_W / 2 - 200, SCREEN_H / 2 - 80, 400, 160, olc::YELLOW);

        if (drawGame) {
            DrawString(SCREEN_W / 2 - 72, SCREEN_H / 2 - 60, "IT'S A DRAW!", olc::Pixel(200, 200, 100), 2);
            DrawString(SCREEN_W / 2 - 80, SCREEN_H / 2 - 30,
                turnsSinceHit >= 30 ? "30 turns without a hit" : "Both tanks destroyed!",
                olc::Pixel(180, 180, 180));
        } else if (surrendered) {
            std::string loseText = settings.playerNames[1 - winner] + " SURRENDERED";
            DrawString(SCREEN_W / 2 - (int)(loseText.length() * 4), SCREEN_H / 2 - 60, loseText,
                olc::Pixel(200, 200, 200));
            std::string winText = settings.playerNames[winner];
            DrawString(SCREEN_W / 2 - (int)(winText.length() * 4 * 2), SCREEN_H / 2 - 38, winText,
                winner == 0 ? olc::Pixel(100, 255, 100) : olc::Pixel(255, 100, 100), 2);
            DrawString(SCREEN_W / 2 - 88, SCREEN_H / 2 - 14, "WINS THE MATCH!", olc::YELLOW, 2);
        } else if (matchOver) {
            // Match winner
            std::string winText = settings.playerNames[winner];
            DrawString(SCREEN_W / 2 - (int)(winText.length() * 4 * 2), SCREEN_H / 2 - 60, winText,
                winner == 0 ? olc::Pixel(100, 255, 100) : olc::Pixel(255, 100, 100), 2);
            DrawString(SCREEN_W / 2 - 88, SCREEN_H / 2 - 35, "WINS THE MATCH!", olc::YELLOW, 2);
        } else {
            // Round winner
            std::string winText = settings.playerNames[winner] + " wins the round!";
            int tx = SCREEN_W / 2 - (int)(winText.length() * 4);
            DrawString(std::max(10, tx), SCREEN_H / 2 - 50, winText,
                winner == 0 ? olc::Pixel(100, 255, 100) : olc::Pixel(255, 100, 100));
        }

        std::string scoreText = std::to_string(tanks[0].score) + " - " + std::to_string(tanks[1].score);
        DrawString(SCREEN_W / 2 - (int)(scoreText.length() * 4 * 2), SCREEN_H / 2, scoreText, olc::WHITE, 2);

        float pulse = (sin(stateTimer * 3.0f) + 1.0f) * 0.5f;
        std::string prompt = (drawGame || !matchOver) ? "SPACE for next round" : "SPACE for menu";
        DrawString(SCREEN_W / 2 - (int)(prompt.length() * 4), SCREEN_H / 2 + 40, prompt,
            olc::Pixel((int)(255 * pulse), (int)(255 * pulse), 0));
    }

    // =========================================================================
    // MAIN UPDATE LOOP
    // =========================================================================

    bool OnUserUpdate(float fElapsedTime) override {
        stateTimer += fElapsedTime;
        frameTime = fElapsedTime;
        UpdateShake(fElapsedTime);

        // --- TITLE / MENU / LOBBY: draw directly, no shake needed ---
        if (state == GameState::TITLE) {
            SetDrawTarget(nullptr);
            DrawTitleScreen();
            if (GetKey(olc::Key::SPACE).bPressed) { state = GameState::MENU; stateTimer = 0; }
            return true;
        }
        if (state == GameState::MENU) {
            SetDrawTarget(nullptr);
            UpdateMenuInput(fElapsedTime);
            DrawMenuScreen();
            return true;
        }
        if (state == GameState::LOBBY) {
            SetDrawTarget(nullptr);
            NetUpdate();
            UpdateLobbyInput();
            DrawLobbyScreen();
            return true;
        }

        // --- PICKUPS: ambient clock, respawn timer, message fade, collisions ---
        worldTime += fElapsedTime;
        if (UpdateLightning(fElapsedTime)) PlayThunderSound();
        if (pickupMessageTimer > 0) pickupMessageTimer -= fElapsedTime;
        if (!pickup.active && pickupRespawnTimer > 0) {
            pickupRespawnTimer -= fElapsedTime;
            if (pickupRespawnTimer <= 0) SpawnPickup();
        }
        // Only the acting player's machine runs pickup collection.
        // CollectMysteryPickup() calls rand() — if both machines ran it they
        // would grant different weapons. The HOST's canonical result is what matters.
        if (IsLocalTurn()) CheckPickupCollisions();

        // Poll for incoming network messages every frame during active gameplay
        if (netMode != NetMode::NONE) NetUpdate();

        // While waiting for the host's TURN_RESULT we skip all state-machine
        // logic but still fall through to the render section below, so the
        // screen shows a clean frame (explosions already cleared from the
        // vector) instead of freezing on the last rendered frame.
        Tank& t = tanks[currentPlayer];

        if (!waitForResult) { // --- STATE MACHINE (skipped while awaiting TURN_RESULT) ---

        // --- AIM PHASE ---
        if (state == GameState::AIM) {
            cursorBlink += fElapsedTime;
            UpdateCheatDetection(fElapsedTime);

            if (IsLocalTurn()) {  // all input handling is inside this guard

            if (inputMode != InputMode::NONE) {
                int digit = GetDigitPressed();
                if (digit >= 0 && inputBuffer.length() < 3)
                    inputBuffer += std::to_string(digit);
                if (GetKey(olc::Key::BACK).bPressed && !inputBuffer.empty())
                    inputBuffer.pop_back();
                if (GetKey(olc::Key::ENTER).bPressed || GetKey(olc::Key::RETURN).bPressed)
                    CommitInput();
                if (GetKey(olc::Key::ESCAPE).bPressed) {
                    inputMode = InputMode::NONE;
                    inputBuffer.clear();
                }
                if (GetKey(olc::Key::TAB).bPressed)
                    CommitInput();
            } else {
                int digit = GetDigitPressed();
                if (digit >= 0) {
                    inputMode = InputMode::TYPING_ANGLE;
                    inputBuffer = std::to_string(digit);
                    cursorBlink = 0;
                }

                // Check if any held action is active for repeat throttling
                bool anyHeld = GetKey(olc::Key::A).bHeld || GetKey(olc::Key::D).bHeld
                            || GetKey(olc::Key::W).bHeld || GetKey(olc::Key::S).bHeld
                            || GetKey(olc::Key::LEFT).bHeld || GetKey(olc::Key::RIGHT).bHeld;

                if (RepeatTick(anyHeld, fElapsedTime, repeatTimer)) {
                    if (GetKey(olc::Key::A).bHeld) t.angle = std::min(110, t.angle + 1);
                    if (GetKey(olc::Key::D).bHeld) t.angle = std::max(0, t.angle - 1);
                    if (GetKey(olc::Key::W).bHeld) t.power = std::min(MAX_POWER, t.power + 1);
                    if (GetKey(olc::Key::S).bHeld) t.power = std::max(5, t.power - 1);

                    if (GetKey(olc::Key::LEFT).bHeld && t.movesLeft > 0) {
                        int newX = (int)t.x - 1;
                        if (newX > TANK_WIDTH / 2) {
                            t.x = (float)newX;
                            t.y = terrain[newX];
                            t.movesLeft--;
                        }
                    }
                    if (GetKey(olc::Key::RIGHT).bHeld && t.movesLeft > 0) {
                        int newX = (int)t.x + 1;
                        if (newX < SCREEN_W - TANK_WIDTH / 2) {
                            t.x = (float)newX;
                            t.y = terrain[newX];
                            t.movesLeft--;
                        }
                    }
                }

                if (GetKey(olc::Key::SPACE).bPressed)
                    Fire();  // Fire() sends TURN_ACTION in net mode (see below)
                if (GetKey(olc::Key::ENTER).bPressed) {
                    if (netMode == NetMode::NONE) {
                        state = GameState::NEXT_TURN; stateTimer = 0;
                    } else if (netMode == NetMode::HOST) {
                        NetSendTurnAction(true, false);
                        NetSendTurnResult();
                        state = GameState::NEXT_TURN; stateTimer = 0;
                    } else { // CLIENT acting: tell host, then wait for result
                        NetSendTurnAction(true, false);
                        SetWaitForResult();
                    }
                }
            }
            } // end IsLocalTurn() guard
        }

        // --- FIRING PHASE: projectile(s) in flight ---
        if (state == GameState::FIRING) {
            float dt = fElapsedTime;
            int steps = 4;
            float subDt = dt / steps;
            int opponent = 1 - currentPlayer;

            for (int s = 0; s < steps; s++) {
                std::vector<Projectile> newSpawns;

                for (auto& p : projectiles) {
                    if (!p.active) continue;

                    float prevVy = p.vy;
                    p.vy += activeGravity * subDt;  // Uses settings-derived gravity
                    p.vx += wind * 5.0f * subDt;
                    p.x += p.vx * subDt;
                    p.y += p.vy * subDt;

                    p.trail.push_back({p.x, p.y});
                    if (p.trail.size() > 200)
                        p.trail.erase(p.trail.begin());

                    // Cluster munitions split into 3 normal shells at the apex of flight
                    if (p.weapon == WeaponType::CLUSTER && !p.hasSplit && prevVy < 0 && p.vy >= 0) {
                        p.hasSplit = true;
                        Projectile side1 = p, side2 = p;
                        side1.vx -= 70.0f; side1.weapon = WeaponType::NORMAL; side1.trail.clear();
                        side2.vx += 70.0f; side2.weapon = WeaponType::NORMAL; side2.trail.clear();
                        p.weapon = WeaponType::NORMAL;
                        newSpawns.push_back(side1);
                        newSpawns.push_back(side2);
                    }

                    float radius = (p.weapon == WeaponType::HE) ? EXPLOSION_RADIUS * 2 : EXPLOSION_RADIUS;

                    if (CheckTankHit(opponent, p.x, p.y)) {
                        ApplyHitDamage(opponent);
                        explosions.push_back({p.x, p.y, 1.0f, radius, 0, true});
                        p.active = false;
                        StopWhistleSound();
                        PlayExplosionSound();
                        continue;
                    }

                    int px = (int)p.x;
                    if (px >= 0 && px < SCREEN_W && p.y >= terrain[px]) {
                        explosions.push_back({p.x, p.y, 1.0f, radius, 0, true});
                        p.active = false;
                        StopWhistleSound();
                        PlayExplosionSound();
                        continue;
                    }

                    if (p.x < -50 || p.x > SCREEN_W + 50 || p.y > SCREEN_H + 50) {
                        p.active = false;
                        StopWhistleSound();
                    }
                }

                for (auto& np : newSpawns) projectiles.push_back(np);
            }

            bool anyActive = false;
            for (auto& p : projectiles) if (p.active) anyActive = true;

            if (!anyActive) {
                if (!explosions.empty()) {
                    state = GameState::EXPLOSION; stateTimer = 0;
                } else {
                    // Miss — no explosion to trigger TURN_RESULT, sync ammo manually
                    if (netMode == NetMode::HOST) { NetSendTurnResult(); state = GameState::NEXT_TURN; stateTimer = 0; }
                    else if (netMode == NetMode::CLIENT) { SetWaitForResult(); }
                    else { state = GameState::NEXT_TURN; stateTimer = 0; }
                }
            }
        }

        // --- EXPLOSION PHASE ---
        if (state == GameState::EXPLOSION) {
            bool allDone = true;
            for (auto& ex : explosions) {
                if (!ex.active) continue;
                ex.timer += fElapsedTime;
                ex.radius = ex.maxRadius * std::min(1.0f, ex.timer * 4.0f);
                if (ex.timer <= 0.5f) allDone = false;
            }

            if (allDone) {
                for (auto& ex : explosions) {
                    CarveCrater(ex.x, ex.y, ex.maxRadius);
                    CheckPickupDestruction(ex.x, ex.y, ex.maxRadius);
                }
                GenerateTerrainTexture();
                SettleTanks();
                SettlePickup();
                explosions.clear();

                if (netMode == NetMode::HOST) {
                    // Host is authoritative: apply outcome, send canonical state to client
                    if (tanks[0].hp <= 0 || tanks[1].hp <= 0) {
                        bool bothDead = tanks[0].hp <= 0 && tanks[1].hp <= 0;
                        if (bothDead) drawGame = true;
                        else if (!surrendered) { int w = (tanks[0].hp <= 0) ? 1 : 0; tanks[w].score++; }
                    }
                    NetSendTurnResult();
                    // Host transitions state immediately; client will transition upon receiving result
                    if (tanks[0].hp <= 0 || tanks[1].hp <= 0) { state = GameState::GAME_OVER; }
                    else { state = GameState::NEXT_TURN; }
                    stateTimer = 0;
                } else if (netMode == NetMode::CLIENT) {
                    // Client waits for host's TURN_RESULT to get canonical state
                    SetWaitForResult();
                } else {
                    // Local play — existing logic
                    if (tanks[0].hp <= 0 || tanks[1].hp <= 0) {
                        bool bothDead = (tanks[0].hp <= 0 && tanks[1].hp <= 0);
                        if (bothDead) { drawGame = true; }
                        else if (!surrendered) { int winner = (tanks[0].hp <= 0) ? 1 : 0; tanks[winner].score++; }
                        state = GameState::GAME_OVER;
                        stateTimer = 0;
                    } else {
                        state = GameState::NEXT_TURN;
                        stateTimer = 0;
                    }
                }
            }
        }

        // --- LASER FIRE PHASE: brief beam animation ---
        if (state == GameState::LASER_FIRE) {
            if (stateTimer > 0.5f) {
                laserTrail.clear();
                int opponent = 1 - currentPlayer;
                bool killed = (tanks[opponent].hp <= 0);
                if (killed && !surrendered) tanks[currentPlayer].score++;

                if (netMode == NetMode::HOST) {
                    NetSendTurnResult();
                    state = killed ? GameState::GAME_OVER : GameState::NEXT_TURN;
                    stateTimer = 0;
                } else if (netMode == NetMode::CLIENT) {
                    SetWaitForResult(); // TURN_RESULT will set state
                } else {
                    state = killed ? GameState::GAME_OVER : GameState::NEXT_TURN;
                    stateTimer = 0;
                }
            }
        }

        // --- NEXT TURN ---
        if (state == GameState::NEXT_TURN) {
            if (stateTimer > 0.5f) {
                currentPlayer = 1 - currentPlayer;
                tanks[currentPlayer].movesLeft = settings.moveBudget;
                inputMode = InputMode::NONE;
                inputBuffer.clear();
                selectedWeapon = WeaponType::NORMAL;
                reticleActive = false;

                // Shield fades by one layer every time it cycles back to its
                // owner's turn, on top of degrading from direct hits
                Tank& starting = tanks[currentPlayer];
                if (starting.shieldCharges > 0) {
                    starting.shieldCharges--;
                    if (starting.shieldCharges > 0)
                        ShowPickupMessage(settings.playerNames[currentPlayer] + "'s shield weakens! ("
                            + std::to_string(starting.shieldCharges) + " left)");
                    else
                        ShowPickupMessage(settings.playerNames[currentPlayer] + "'s shield fades away!");
                }
                // NVG goggles: count down one turn for the player whose turn is starting
                if (tanks[currentPlayer].nvgTurns > 0) {
                    tanks[currentPlayer].nvgTurns--;
                    if (tanks[currentPlayer].nvgTurns == 0)
                        ShowPickupMessage(settings.playerNames[currentPlayer] + "'s night vision fades!");
                }

                // Wind for the next turn:
                // HOST already computed nextWind in NetSendTurnResult() and the CLIENT
                // received it via TURN_RESULT — both machines use the same value.
                // In network mode the local rand() call is skipped entirely.
                if (netMode == NetMode::NONE) {
                    if (activeWindMax > 0) {
                        wind += ((rand() % 100) - 50) / 20.0f;
                        wind = std::clamp(wind, -activeWindMax, activeWindMax);
                    }
                } else {
                    wind = nextWind; // apply the synced value from TURN_RESULT
                }

                // Stalemate check: if 30 turns pass with no hit, declare a draw
                turnsSinceHit++;
                if (turnsSinceHit >= 30) {
                    drawGame = true;
                    state = GameState::GAME_OVER;
                    stateTimer = 0;
                    return true;
                }

                state = GameState::AIM;
                stateTimer = 0;
            }
        }

        } // end if (!waitForResult)

        // --- RENDER ---
        // Redirect all drawing into the frame buffer sprite so we can blit it
        // back to screen with a pixel offset — that is how screen shake works.
        SetDrawTarget(pFrameBuf);
        Clear(olc::BLACK);
        DrawSky();
        DrawTerrain();

        DrawPickup();

        for (int i = 0; i < 2; i++) {
            if (tanks[i].hp > 0) {
                DrawTank(tanks[i], i);
                DrawShield(tanks[i]);
                DrawSmokeEffect(tanks[i]);
            }
        }

        if (state == GameState::AIM) {
            DrawAimGuide();
            if (reticleActive) DrawReticle(1 - currentPlayer);
            DrawTrajectoryCheat();
        }
        DrawProjectile();
        DrawExplosion();
        DrawLaserBeam();
        DrawWindIndicator();
        // Night overlay drawn last so it sits on top of everything
        DrawNightOverlay(netMode == NetMode::NONE ? currentPlayer : localPlayer);
        DrawPickupMessage();
        DrawUI();

        // Network: overlay "Waiting for..." when it's the remote player's turn or we're awaiting result
        if (netMode != NetMode::NONE && (!IsLocalTurn() || waitForResult)) {
            std::string msg = waitForResult
                ? "Resolving turn..."
                : "Waiting for " + settings.playerNames[currentPlayer] + "...";
            float p2 = (sin(stateTimer * 3.0f) + 1.0f) * 0.5f;
            int bw = (int)msg.length() * 8 + 24;
            int bx = SCREEN_W/2 - bw/2, by = GAME_TOP + 10;
            FillRect(bx, by, bw, 20, olc::Pixel(0,0,0,180));
            DrawString(bx + 12, by + 6, msg, olc::Pixel(200,200,255,(int)(180+75*p2)));
        }

        if (state == GameState::GAME_OVER) {
            DrawGameOverScreen();
            if (GetKey(olc::Key::SPACE).bPressed && stateTimer > 1.0f) {
                // Determine if the match is truly over (a winner has enough round wins)
                int winner = (tanks[0].hp > 0 && tanks[1].hp <= 0) ? 0 :
                             (tanks[1].hp > 0 && tanks[0].hp <= 0) ? 1 : -1;
                bool matchOver = (winner >= 0 && tanks[winner].score >= settings.roundsToWin);

                if (!drawGame && matchOver) {
                    // Match is decided — return to menu
                    if (netMode != NetMode::NONE) { NetSend(NetMsg::DISCONNECT, {}); NetClose(); netMode = NetMode::NONE; }
                    state = GameState::MENU;
                    menuEditingName = -1;
                    surrendered = false;
                    stateTimer = 0;
                } else {
                    // Draw or round win — play the next round
                    NewRound();
                    // Host syncs the new round to the client
                    if (netMode == NetMode::HOST) NetSendRoundStart();
                }
            }
        }

        // Restore default draw target then blit the frame buffer to screen.
        // The shake offset is applied here as a pixel translation so all
        // draw calls above are shifted uniformly — the correct way to shake.
        SetDrawTarget(nullptr);
        Clear(olc::BLACK);
        DrawSprite(shakeOffX, shakeOffY, pFrameBuf);

        return true;
    }
};

// =============================================================================
// ENTRY POINT
// =============================================================================

int main() {
    Tanx game;
    if (game.Construct(SCREEN_W, SCREEN_H, 1, 1))
        game.Start();
    return 0;
}
