#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <random>
#include <string>
#include <vector>
using std::min;
using std::max;
#include <gdiplus.h>
#include "Aseprite.h"

namespace
{
    struct Column { int ceil; int floor; int drip; int ceilDrip; };

    const UINT_PTR TIMER_ID = 1;
    const COLORREF BG = RGB(10, 6, 16);     // cave interior: near-black with a purple tint
    const COLORREF FG = RGB(88, 40, 120);   // cave walls: dark alien purple
    const int SHIP_W = 96, SHIP_H = 96, SHIP_SPEED = 8;

    // A looping set of frames rotated to face right; pixels are owned here because
    // Gdiplus::Bitmap built from scan0 does not copy them.
    struct Animation
    {
        std::vector<Gdiplus::Bitmap*> frames;
        std::vector<std::vector<uint8_t>> pixels;
        std::vector<int> durations;
        size_t current = 0;
        ULONGLONG frameStart = 0;
        int spriteW = 0, spriteH = 0;          // frame size after rotation
        int left = 0, top = 0, right = 0, bottom = 0; // opaque bounds of frame 0 (for hit boxes)

        bool Load(const std::wstring& dir, const std::wstring& file, const std::vector<std::string>& skip, bool counterClockwise = false, bool upright = false)
        {
            AsepriteImage ase;
            if (!ase.Load(dir + file, skip) && !ase.Load(file, skip))
                return false;
            // Rotate 90 degrees clockwise (sprites point up) unless upright, and convert to the
            // BGRA byte order GDI+ Format32bppArgb expects.
            const int w = ase.width, h = ase.height;
            spriteW = upright ? w : h;
            spriteH = upright ? h : w;
            for (const AsepriteImage::Frame& frame : ase.frames)
            {
                std::vector<uint8_t> px(frame.rgba.size());
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const uint8_t* s = &frame.rgba[(y * w + x) * 4];
                        uint8_t* d = upright ? &px[(y * w + x) * 4]
                                   : counterClockwise ? &px[((w - 1 - x) * h + y) * 4]
                                                      : &px[(x * h + (h - 1 - y)) * 4];
                        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                    }
                pixels.push_back(std::move(px));
                durations.push_back(std::max(20, frame.durationMs));
            }
            left = spriteW; top = spriteH; right = 0; bottom = 0;
            for (int y = 0; y < spriteH; ++y)
                for (int x = 0; x < spriteW; ++x)
                    if (pixels[0][(y * spriteW + x) * 4 + 3])
                    {
                        left = std::min(left, x); right = std::max(right, x + 1);
                        top = std::min(top, y); bottom = std::max(bottom, y + 1);
                    }
            for (std::vector<uint8_t>& px : pixels)
                frames.push_back(new Gdiplus::Bitmap(spriteW, spriteH, spriteW * 4, PixelFormat32bppARGB, px.data()));
            frameStart = GetTickCount64();
            return true;
        }

        ULONGLONG TotalMs() const
        {
            ULONGLONG total = 0;
            for (int d : durations) total += d;
            return total;
        }

        // Frame shown after elapsedMs; -1 once a non-looping animation has finished.
        int IndexAt(ULONGLONG elapsedMs, bool loop) const
        {
            ULONGLONG total = TotalMs();
            if (total == 0) return -1;
            if (loop) elapsedMs %= total;
            else if (elapsedMs >= total) return -1;
            for (size_t i = 0; i < durations.size(); ++i)
            {
                if (elapsedMs < static_cast<ULONGLONG>(durations[i])) return static_cast<int>(i);
                elapsedMs -= durations[i];
            }
            return -1;
        }

        void DrawFrame(Gdiplus::Graphics& g, int index, int x, int y, int w, int h) const
        {
            if (index >= 0 && index < static_cast<int>(frames.size()))
                g.DrawImage(frames[index], x, y, w, h);
        }

        void Restart(size_t first = 0) { current = first; frameStart = GetTickCount64(); }

        // Loops through frames [first, end).
        void Advance(size_t first = 0)
        {
            if (frames.empty()) return;
            ULONGLONG now = GetTickCount64();
            while (now - frameStart >= static_cast<ULONGLONG>(durations[current]))
            {
                frameStart += durations[current];
                current = current + 1 < frames.size() ? current + 1 : first;
            }
        }

        void Draw(Gdiplus::Graphics& g, int x, int y, int w, int h) const
        {
            if (!frames.empty())
                g.DrawImage(frames[current], x, y, w, h);
        }

        void Free()
        {
            for (Gdiplus::Bitmap* bmp : frames) delete bmp;
            frames.clear();
        }
    };

    Animation shipSprite;      // frame 0 only: the file's frames are alternate ship designs
    Animation engineIdle;
    Animation enginePowering;
    bool thrusting = false;
    bool firing = false;
    ULONGLONG lastShot = 0;

    enum BulletKind { BK_SHOT, BK_ROCKET, BK_BIG };
    struct Bullet
    {
        float x, y, vy = 0;
        BulletKind kind = BK_SHOT;
        int dmg = 1;
        float w = 22, h = 6;       // hit box: [x, x+w] by [y-h/2, y+h/2]
        float speed = 22, ang = 0; // ang is only used by homing rockets
        std::vector<unsigned> hitIds; // piercing shots: enemies already hit
    };

    // Game state: score, health, energy, power-ups.
    enum DropKind { DROP_HEALTH, DROP_WEAPON, DROP_SHIELD, DROP_ENGINE, DROP_ROCKET, DROP_BIGGUN, DROP_ZAPPER, DROP_KIND_COUNT };
    struct Drop { float x, y; DropKind kind; int variant; };
    enum WeaponType { WPN_CANNON, WPN_ROCKET, WPN_BIGGUN, WPN_ZAPPER };
    std::vector<Drop> drops;
    int score = 0, health = 100;
    const int MAX_HEALTH = 100;
    int weaponLevel = 1;                 // autocannon: 1 single, 2 dual, 3 triple, 4 quad
    WeaponType weaponType = WPN_CANNON;  // specials are only offered once the cannon is maxed
    int engineLevel = 0;                 // 0..3, faster energy recovery
    const float MAX_ENERGY = 100.0f;
    const float ENERGY_REGEN[4] = { 6.0f, 11.0f, 17.0f, 25.0f };   // per second
    float energy = MAX_ENERGY;
    const ULONGLONG EMPTY_INTERVAL_MS = 1300;  // fire rate with too little energy for the weapon
    const float ZAPPER_DRAIN = 40.0f;          // energy per second while the beam is on
    ULONGLONG lastTick = 0;
    bool zapperExhausted = false;
    bool beamOn = false;
    float beamAngle = 0;
    int lastShipY = 0;
    ULONGLONG lastBeamHit = 0;
    ULONGLONG shieldUntil = 0, hurtUntil = 0;
    bool gameOver = false;
    const int DROP_SIZE = 40;
    Animation dropSprites[DROP_KIND_COUNT]; // the health slot has no art and stays empty
    Animation engineIcons[3];
    Animation weaponAnim[4];             // indexed by WeaponType
    Animation& cannon = weaponAnim[WPN_CANNON];
    bool rocketSide = false;
    std::vector<Bullet> bullets;
    // Muzzle positions inside the 96x96 ship sprite (scaled from the 48x48 art, rotated to face right).
    const float MUZZLE_X = 68.0f, MUZZLE_Y[2] = { 29.0f, 67.0f };
    ULONG_PTR gdiToken = 0;

    // Background planets: at most two on screen, drifting slowly right to left.
    struct Planet { Gdiplus::Bitmap* bmp; float x, y; int size; };
    std::vector<Planet> planets;
    std::wstring assetDir;
    const float PLANET_SPEED = 0.7f;
    int planetCount = 0;     // number of planet_N.png files found
    int lastPlanet = -1;

    void FreePlanets()
    {
        for (Planet& p : planets) delete p.bmp;
        planets.clear();
    }

    void SpawnPlanet(const RECT& rc);
    void UpdatePlanets(const RECT& rc);
    int shipX = 40, shipY = 0;
    bool shipPlaced = false;

    // Enemy ships are drawn at 1.5x their art size, flying right to left.
    struct EnemyType
    {
        const wchar_t* name;
        int hp;
        float speed;   // pixels per tick
        int weight;    // relative spawn chance
        int power;     // size class: drives score, drops, damage and fire rate
        int faction = 0;  // 0 = Nairan (roundNo 1), 1 = Kla'ed (roundNo 2)
        Animation base, engine, destruction;
    };
    struct Enemy
    {
        size_t type;
        float x, y;    // top-left of the drawn sprite
        int hp;
        ULONGLONG born;
        bool dying;
        ULONGLONG diedAt;
        float ox, oy;  // formation slot relative to the formation's lead
        ULONGLONG nextShot;
        ULONGLONG nextAbility = 0;
        unsigned id = 0;   // assigned on first collision pass
    };
    unsigned nextEnemyId = 1;
    struct EnemyBullet { float x, y, vx, vy; int dmg; };
    std::vector<EnemyBullet> enemyBullets;
    const float ENEMY_BULLET_SPEED = 9.0f;
    std::vector<EnemyType> enemyTypes = {
        { L"Scout",         1, 7.0f, 5, 1 },
        { L"Fighter",       2, 6.0f, 5, 2 },
        { L"Bomber",        3, 4.0f, 4, 3 },
        { L"Support Ship",  3, 4.0f, 3, 3 },
        { L"Torpedo Ship",  3, 5.0f, 3, 3 },
        { L"Frigate",       10, 3.0f, 2, 6 },
        { L"Battlecruiser", 30, 2.0f, 1, 12 },
        { L"Dreadnought",   60, 2.0f, 1, 18 },
    };
    std::vector<Enemy> enemies;
    int wave = 0;                    // number of waves started so far
    int roundNo = 0;                   // 0 = Nairan fleet, 1 = Kla'ed fleet (tougher)
    ULONGLONG roundBannerUntil = 0;
    bool waveActive = false, victory = false;
    ULONGLONG nextWaveAt = 0, nextChunk = 0, bannerUntil = 0;
    struct WaveEntry { const wchar_t* type; int count; };
    std::vector<WaveEntry> spawnQueue;
    ULONGLONG nextSpawn = 0;
    const int ENEMY_SCALE_NUM = 3, ENEMY_SCALE_DEN = 2;

    int EnemyDrawSize(const EnemyType& t) { return t.base.spriteW * ENEMY_SCALE_NUM / ENEMY_SCALE_DEN; }

    void ResetGame()
    {
        score = 0; health = MAX_HEALTH; weaponLevel = 1;
        weaponType = WPN_CANNON; engineLevel = 0; energy = MAX_ENERGY;
        zapperExhausted = beamOn = false; beamAngle = 0;
        shieldUntil = hurtUntil = 0; gameOver = false;
        enemies.clear(); enemyBullets.clear(); bullets.clear(); drops.clear();
        nextSpawn = 0;
        wave = 0; roundNo = 0; roundBannerUntil = 0; waveActive = victory = false;
        nextWaveAt = nextChunk = bannerUntil = 0;
        spawnQueue.clear();
    }

    bool splash = true;
    int highScore = 0;

    std::wstring HighScorePath() { return assetDir + L"highscore.txt"; }

    void LoadHighScore()
    {
        HANDLE f = CreateFileW(HighScorePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        char buf[16] = {};
        DWORD n = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
        CloseHandle(f);
        highScore = std::max(0, atoi(buf));
    }

    void UpdateHighScore()
    {
        if (score <= highScore) return;
        highScore = score;
        HANDLE f = CreateFileW(HighScorePath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        const std::string s = std::to_string(highScore);
        DWORD n = 0;
        WriteFile(f, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
        CloseHandle(f);
    }

    std::mt19937 rng{ std::random_device{}() };
    std::vector<Column> cave;
    int gridCols = 0, gridRows = 0;
    int cellW = 12, cellH = 20;
    HFONT font = nullptr;

    int Rand(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

    void SpawnPlanet(const RECT& rc)
    {
        if (planetCount <= 0) return;
        int n = Rand(0, planetCount - 1);
        if (n == lastPlanet && planetCount > 1) n = (n + 1) % planetCount;
        lastPlanet = n;
        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile((assetDir + L"Planets\\planet_" + std::to_wstring(n) + L".png").c_str());
        if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return; }
        int size = Rand(220, 440);
        // Allow planets to sit partly off the top or bottom edge.
        float y = static_cast<float>(Rand(-size / 4, std::max(-size / 4, static_cast<int>(rc.bottom) - size * 3 / 4)));
        planets.push_back({ bmp, static_cast<float>(rc.right), y, size });
    }

    void UpdatePlanets(const RECT& rc)
    {
        for (Planet& p : planets) p.x -= PLANET_SPEED;
        for (size_t i = planets.size(); i-- > 0;)
            if (planets[i].x + planets[i].size < 0)
            {
                delete planets[i].bmp;
                planets.erase(planets.begin() + i);
            }
        // A second planet only enters once the first is well into the screen.
        bool room = planets.empty() || (planets.size() < 2 &&
            planets.back().x + planets.back().size < rc.right - 150 && planets.back().x < rc.right * 0.45f);
        if (room && Rand(0, 100) < 2) SpawnPlanet(rc);
    }

    Column NextColumn(const Column* prev)
    {
        // Wall thickness is ~75% of the previous 4..40% (ceiling) and 3..40% (floor) ranges.
        int minCeil = 3, maxCeil = std::max(minCeil, gridRows * 3 / 10);
        int minFloor = gridRows * 7 / 10, maxFloor = gridRows - 2;
        Column c;
        c.ceil = prev ? prev->ceil : Rand(minCeil, maxCeil);
        c.floor = prev ? prev->floor : Rand(minFloor, maxFloor);
        if (Rand(0, 2) == 0) c.ceil += Rand(-1, 1);
        if (Rand(0, 2) == 0) c.floor += Rand(-1, 1);
        if (Rand(0, 12) == 0) c.ceil += Rand(-3, 3);
        if (Rand(0, 12) == 0) c.floor += Rand(-3, 3);
        c.ceil = std::clamp(c.ceil, minCeil, maxCeil);
        c.floor = std::clamp(c.floor, std::max(minFloor, c.ceil + 7), std::max(maxFloor, minFloor));
        c.drip = Rand(0, 5) == 0 ? Rand(1, 3) : 0;
        c.ceilDrip = Rand(0, 5) == 0 ? Rand(1, 3) : 0;
        return c;
    }

    void ResizeGrid(HWND hwnd)
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        gridCols = std::max(1, static_cast<int>(rc.right) / cellW + 1);
        gridRows = std::max(12, static_cast<int>(rc.bottom) / cellH + 1);
        // Keep the newest columns on the right edge when resizing.
        if (static_cast<int>(cave.size()) > gridCols)
            cave.erase(cave.begin(), cave.end() - gridCols);
        while (static_cast<int>(cave.size()) < gridCols)
            cave.insert(cave.begin(), NextColumn(cave.empty() ? nullptr : &cave.front()));
    }

    wchar_t Shade(int level)
    {
        switch (level)
        {
        case 0: return L' ';
        case 1: return L'\u2591';
        case 2: return L'\u2592';
        case 3: return L'\u2593';
        default: return L'\u2588';
        }
    }

    wchar_t CellChar(const Column& c, int row)
    {
        if (row >= c.ceil + c.ceilDrip && row <= c.floor - c.drip)
            return L' ';
        if (row < c.ceil + c.ceilDrip)
            return Shade(c.ceil + c.ceilDrip - row); // mirrors the floor shading
        return Shade(row - c.floor + c.drip);
    }

    void Opening(int x, int height, int& top, int& bottom);

    // Wave script from Space_Shooter_Level_Design.txt. "*" is a random small ship.
    struct WaveDef { std::vector<WaveEntry> entries; int level; };
    const std::vector<WaveDef> waves = {
        { { { L"Scout", 10 } }, 0 }, { { { L"Scout", 10 } }, 0 },
        { { { L"Scout", 8 }, { L"Fighter", 2 } }, 0 }, { { { L"Scout", 6 }, { L"Fighter", 4 } }, 0 },
        { { { L"Fighter", 10 } }, 0 }, { { { L"Fighter", 8 }, { L"Bomber", 2 } }, 0 },
        { { { L"Fighter", 6 }, { L"Bomber", 4 } }, 0 }, { { { L"Bomber", 10 } }, 0 },
        { { { L"Bomber", 8 }, { L"Support Ship", 2 } }, 0 }, { { { L"Bomber", 6 }, { L"Support Ship", 4 } }, 0 },
        { { { L"Support Ship", 10 } }, 0 }, { { { L"Frigate", 1 } }, 0 },
        { { { L"Fighter", 6 }, { L"Support Ship", 4 } }, 1 }, { { { L"Fighter", 8 }, { L"Torpedo Ship", 2 } }, 1 },
        { { { L"Fighter", 6 }, { L"Torpedo Ship", 4 } }, 1 }, { { { L"Torpedo Ship", 10 } }, 1 },
        { { { L"Bomber", 4 }, { L"Support Ship", 4 }, { L"Torpedo Ship", 2 } }, 1 },
        { { { L"Bomber", 8 }, { L"Torpedo Ship", 4 } }, 1 }, { { { L"*", 12 } }, 1 }, { { { L"Frigate", 2 } }, 1 },
        { { { L"Fighter", 12 } }, 2 }, { { { L"Fighter", 8 }, { L"Bomber", 4 } }, 2 },
        { { { L"Bomber", 6 }, { L"Support Ship", 4 } }, 2 }, { { { L"Torpedo Ship", 6 }, { L"Fighter", 6 } }, 2 },
        { { { L"Frigate", 4 }, { L"Fighter", 8 } }, 2 },
        { { { L"Frigate", 2 }, { L"Bomber", 6 }, { L"Fighter", 6 } }, 2 },
        { { { L"Battlecruiser", 1 }, { L"Fighter", 8 } }, 2 },
        { { { L"Battlecruiser", 2 } }, 3 }, { { { L"Battlecruiser", 1 }, { L"Frigate", 2 }, { L"Fighter", 8 } }, 3 },
        { { { L"Battlecruiser", 2 }, { L"Torpedo Ship", 4 } }, 3 }, { { { L"Frigate", 3 }, { L"Support Ship", 6 } }, 3 },
        { { { L"Battlecruiser", 1 }, { L"*", 12 } }, 3 }, { { { L"Battlecruiser", 3 } }, 3 },
        { { { L"Battlecruiser", 2 }, { L"Frigate", 4 } }, 4 }, { { { L"Frigate", 4 }, { L"Torpedo Ship", 8 } }, 4 },
        { { { L"Battlecruiser", 1 }, { L"Frigate", 2 }, { L"Fighter", 12 } }, 4 },
        { { { L"Battlecruiser", 2 }, { L"*", 12 } }, 4 },
        { { { L"Dreadnought", 1 } }, 4 }, { { { L"Dreadnought", 1 } }, 4 }, { { { L"Dreadnought", 1 } }, 4 },
    };
    const std::vector<const wchar_t*> levelNames = { L"FIRST CONTACT", L"ESCALATION", L"FLEET ACTION",
                                                      L"CAPITAL SHIP OFFENSIVE", L"FINAL ASSAULT" };
    int FindType(const wchar_t* name)
    {
        for (size_t i = 0; i < enemyTypes.size(); ++i)
            if (wcscmp(enemyTypes[i].name, name) == 0 && enemyTypes[i].faction == roundNo) return static_cast<int>(i);
        return -1;
    }

    bool HasFaction(int faction)
    {
        for (const EnemyType& t : enemyTypes)
            if (t.faction == faction) return true;
        return false;
    }

    int RandomSmallType()
    {
        std::vector<int> smallTypes;
        for (size_t i = 0; i < enemyTypes.size(); ++i)
            if (enemyTypes[i].power <= 3 && enemyTypes[i].faction == roundNo) smallTypes.push_back(static_cast<int>(i));
        return smallTypes.empty() ? 0 : smallTypes[Rand(0, static_cast<int>(smallTypes.size()) - 1)];
    }

    void SpawnFormation(const RECT& rc, ULONGLONG now, size_t type, int count)
    {
        const EnemyType& t = enemyTypes[type];
        int size = EnemyDrawSize(t);
        int pattern = Rand(0, 2); // 0 = V wedge, 1 = vertical line, 2 = trailing column
        int top, bottom;
        Opening(rc.right - 1, rc.bottom, top, bottom);
        float lead = static_cast<float>(Rand(top, std::max(top, bottom - size)));
        for (int i = 0; i < count; ++i)
        {
            float ox = 0, oy = 0;
            int rank = (i + 1) / 2, side = (i % 2) ? -1 : 1;
            if (pattern == 0) { ox = rank * size * 0.9f; oy = side * rank * size * 0.8f; }
            else if (pattern == 1) { oy = (i - (count - 1) / 2.0f) * size * 1.2f; }
            else { ox = i * size * 1.3f; }
            float y = std::clamp(lead + oy, static_cast<float>(top), static_cast<float>(std::max(top, bottom - size)));
            const int hp = t.power >= 18 ? t.hp * std::max(1, wave - 37) : t.hp;
            enemies.push_back({ type, rc.right + ox, y, hp, now, false, 0, ox, oy,
                                now + Rand(800, 2500) + i * 300 });
        }
    }

    void UpdateWaves(const RECT& rc, ULONGLONG now)
    {
        if (!waveActive)
        {
            if (nextWaveAt == 0) nextWaveAt = now + 2000;
            if (now < nextWaveAt) return;
            if (wave >= static_cast<int>(waves.size())) return;
            spawnQueue = waves[wave].entries;
            ++wave;
            waveActive = true;
            nextChunk = now;
            bannerUntil = now + 2500;
            return;
        }
        if (!spawnQueue.empty() && now >= nextChunk)
        {
            WaveEntry& e = spawnQueue.front();
            int type = wcscmp(e.type, L"*") == 0 ? RandomSmallType() : FindType(e.type);
            if (type < 0) { spawnQueue.erase(spawnQueue.begin()); return; }
            const int power = enemyTypes[type].power;
            int n = std::min(e.count, power <= 3 ? 4 : (power <= 6 ? 2 : 1));
            SpawnFormation(rc, now, static_cast<size_t>(type), n);
            e.count -= n;
            if (e.count <= 0) spawnQueue.erase(spawnQueue.begin());
            nextChunk = now + 1800;
        }
        else if (spawnQueue.empty() && enemies.empty())
        {
            waveActive = false;
            if (wave >= static_cast<int>(waves.size()))
            {
                if (roundNo == 0 && HasFaction(1))
                {
                    // Round 2: the Kla'ed fleet attacks; the player keeps their upgrades and gets some repairs.
                    roundNo = 1;
                    wave = 0;
                    health = std::min(MAX_HEALTH, health + 50);
                    nextWaveAt = now + 4000;
                    roundBannerUntil = now + 3500;
                }
                else
                {
                    victory = gameOver = true;
                    UpdateHighScore();
                }
            }
            else nextWaveAt = now + 2000;
        }
    }

    // Support ships repair nearby allies; the Dreadnought launches escorts and, in phase 3, fires its super weapon.
    void UpdateAbilities(const RECT& rc, ULONGLONG now)
    {
        const size_t count = enemies.size();
        for (size_t i = 0; i < count; ++i)
        {
            if (enemies[i].dying || now < enemies[i].nextAbility) continue;
            const EnemyType& t = enemyTypes[enemies[i].type];
            const int size = EnemyDrawSize(t);
            if (wcscmp(t.name, L"Support Ship") == 0)
            {
                enemies[i].nextAbility = now + 2000;
                for (Enemy& o : enemies)
                {
                    if (o.dying || &o == &enemies[i]) continue;
                    if (std::fabs(o.x - enemies[i].x) < 300 && std::fabs(o.y - enemies[i].y) < 300)
                        o.hp = std::min(enemyTypes[o.type].hp, o.hp + 1);
                }
            }
            else if (t.power >= 18 && wave >= 39 && enemies[i].x < rc.right)
            {
                enemies[i].nextAbility = now + 4500;
                const int minion = FindType(wave >= 40 ? L"Bomber" : L"Fighter");
                if (minion >= 0)
                {
                    const float y = std::clamp(enemies[i].y + size / 2.0f, 0.0f, static_cast<float>(rc.bottom));
                    enemies.push_back({ static_cast<size_t>(minion), enemies[i].x, y, enemyTypes[minion].hp, now,
                                        false, 0, 0, 0, now + 1000 });
                }
                if (wave >= 40)
                {
                    const float sx = enemies[i].x + size * 0.15f, sy = enemies[i].y + size / 2.0f;
                    for (int k = -4; k <= 4; ++k)
                        enemyBullets.push_back({ sx, sy, -ENEMY_BULLET_SPEED, k * 2.0f, 12 });
                }
            }
            else enemies[i].nextAbility = now + 1000;
        }
    }

    // Open (cave-free) vertical range in pixels at screen x.
    void Opening(int x, int height, int& top, int& bottom)
    {
        top = 0; bottom = height;
        if (cave.empty()) return;
        size_t col = static_cast<size_t>(std::clamp(x / cellW, 0, static_cast<int>(cave.size()) - 1));
        top = (cave[col].ceil + cave[col].ceilDrip) * cellH;
        bottom = (cave[col].floor - cave[col].drip) * cellH;
    }

    struct FRect { float left, top, right, bottom; };

    FRect EnemyHitBox(const Enemy& e)
    {
        const EnemyType& t = enemyTypes[e.type];
        float s = static_cast<float>(ENEMY_SCALE_NUM) / ENEMY_SCALE_DEN;
        return { e.x + t.base.left * s, e.y + t.base.top * s, e.x + t.base.right * s, e.y + t.base.bottom * s };
    }

    DropKind PickDrop()
    {
        // Weapon pickups follow the progression: cannon levels first, then the special weapons.
        struct Choice { DropKind kind; int weight; };
        std::vector<Choice> choices = { { DROP_HEALTH, 30 }, { DROP_SHIELD, 10 } };
        if (weaponLevel < 4 && weaponType == WPN_CANNON) choices.push_back({ DROP_WEAPON, 28 });
        else { choices.push_back({ DROP_ROCKET, 9 }); choices.push_back({ DROP_BIGGUN, 9 }); choices.push_back({ DROP_ZAPPER, 7 }); }
        if (engineLevel < 3) choices.push_back({ DROP_ENGINE, 14 });
        int total = 0;
        for (const Choice& c : choices) total += c.weight;
        int pick = Rand(0, total - 1);
        for (const Choice& c : choices)
        {
            if (pick < c.weight) return c.kind;
            pick -= c.weight;
        }
        return DROP_HEALTH;
    }

    void DamageEnemy(Enemy& e, int dmg, ULONGLONG now)
    {
        if (e.dying) return;
        e.hp -= dmg;
        if (e.hp > 0) return;
        e.dying = true;
        e.diedAt = now;
        const EnemyType& t = enemyTypes[e.type];
        score += t.power * 100;
        // Bigger ships are more likely to drop something.
        if (Rand(0, 99) < std::min(70, 10 + t.power * 3))
        {
            int size = EnemyDrawSize(t);
            drops.push_back({ e.x + size / 2.0f - DROP_SIZE / 2.0f, e.y + size / 2.0f - DROP_SIZE / 2.0f,
                              PickDrop(), engineLevel });
        }
    }

    void DrawSplash(HDC mem, const RECT& rc)
    {
        struct Entry { DropKind kind; const wchar_t* name; const wchar_t* desc; };
        static const Entry entries[] = {
            { DROP_HEALTH, L"Repair Kit",        L"Restores 30 HP" },
            { DROP_SHIELD, L"Shield Generator",  L"8 seconds of invincibility" },
            { DROP_WEAPON, L"Autocannon",        L"Adds a barrel, up to quad shot" },
            { DROP_ENGINE, L"Engine",            L"Faster energy recovery (3 levels)" },
            { DROP_ROCKET, L"Homing Rockets",    L"Missiles that seek out enemies" },
            { DROP_BIGGUN, L"Big Space Gun",     L"Slow, heavy, powerful shots" },
            { DROP_ZAPPER, L"Zapper",            L"Continuous beam, drains energy" },
        };
        const int count = static_cast<int>(sizeof(entries) / sizeof(entries[0]));
        const int rowH = 52, cx = rc.right / 2;
        const int top = std::max(10, static_cast<int>(rc.bottom - (140 + count * rowH + 90)) / 2);

        auto center = [&](int y, const std::wstring& s, COLORREF c)
        {
            SetTextColor(mem, c);
            SetTextAlign(mem, TA_CENTER);
            TextOutW(mem, cx, y, s.c_str(), static_cast<int>(s.size()));
            SetTextAlign(mem, TA_LEFT);
        };

        HFONT big = CreateFontW(-64, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        HGDIOBJ prev = SelectObject(mem, big);
        center(top, L"CAVE DASH", RGB(190, 120, 255));
        SelectObject(mem, prev);
        DeleteObject(big);

        center(top + 80, L"POWER-UPS", RGB(255, 190, 70));

        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const ULONGLONG now = GetTickCount64();
        const int iconX = std::max(16, cx - 260);
        for (int i = 0; i < count; ++i)
        {
            const Entry& e = entries[i];
            const int y = top + 120 + i * rowH;
            if (e.kind == DROP_HEALTH)
            {
                Gdiplus::SolidBrush cross(Gdiplus::Color(255, 60, 220, 90));
                const int t = DROP_SIZE / 3;
                g.FillRectangle(&cross, iconX, y + t, DROP_SIZE, t);
                g.FillRectangle(&cross, iconX + t, y, t, DROP_SIZE);
            }
            else
            {
                const Animation& a = e.kind == DROP_ENGINE ? engineIcons[0] : dropSprites[e.kind];
                if (!a.frames.empty()) a.DrawFrame(g, a.IndexAt(now, true), iconX, y, DROP_SIZE, DROP_SIZE);
            }
            SetTextColor(mem, RGB(255, 255, 255));
            TextOutW(mem, iconX + DROP_SIZE + 16, y - 2, e.name, static_cast<int>(wcslen(e.name)));
            SetTextColor(mem, RGB(170, 170, 200));
            TextOutW(mem, iconX + DROP_SIZE + 16, y + 20, e.desc, static_cast<int>(wcslen(e.desc)));
        }

        const int by = top + 120 + count * rowH + 10;
        center(by, L"HIGH SCORE " + std::to_wstring(highScore), RGB(255, 220, 90));
        if ((now / 500) % 2 == 0)
            center(by + 36, L"Press Enter to start", RGB(255, 255, 255));
    }

    void Paint(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(BG);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        {
            Gdiplus::Graphics pg(mem);
            pg.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
            // Dim the planets so they stay behind the action.
            Gdiplus::ColorMatrix dim = { 0.55f, 0, 0, 0, 0,  0, 0.55f, 0, 0, 0,  0, 0, 0.55f, 0, 0,  0, 0, 0, 1, 0,  0, 0, 0, 0, 1 };
            Gdiplus::ImageAttributes attr;
            attr.SetColorMatrix(&dim);
            for (const Planet& p : planets)
                pg.DrawImage(p.bmp, Gdiplus::Rect(static_cast<int>(p.x), static_cast<int>(p.y), p.size, p.size),
                    0, 0, p.bmp->GetWidth(), p.bmp->GetHeight(), Gdiplus::UnitPixel, &attr);
        }
        HGDIOBJ oldFont = SelectObject(mem, font);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, FG);

        std::wstring line;
        for (int r = 0; r < gridRows; ++r)
        {
            line.clear();
            for (const Column& c : cave)
                line += CellChar(c, r);
            TextOutW(mem, 0, r * cellH, line.c_str(), static_cast<int>(line.size()));
        }

        if (splash)
        {
            DrawSplash(mem, rc);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldFont);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return;
        }

        {
            Gdiplus::Graphics g(mem);
            g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

            ULONGLONG now = GetTickCount64();
            for (const Enemy& e : enemies)
            {
                const EnemyType& t = enemyTypes[e.type];
                int size = EnemyDrawSize(t);
                int ex = static_cast<int>(e.x), ey = static_cast<int>(e.y);
                if (e.dying)
                {
                    t.destruction.DrawFrame(g, t.destruction.IndexAt(now - e.diedAt, false), ex, ey, size, size);
                }
                else
                {
                    // Engine files for the larger ships include the hull, which matches the base art.
                    t.engine.DrawFrame(g, t.engine.IndexAt(now - e.born, true), ex, ey, size, size);
                    t.base.DrawFrame(g, 0, ex, ey, size, size);
                }
            }
            const bool blink = now < hurtUntil && (now / 80) % 2 == 0;
            if (!gameOver && !blink)
            {
                (thrusting ? enginePowering : engineIdle).Draw(g, shipX, shipY, SHIP_W, SHIP_H);
                shipSprite.Draw(g, shipX, shipY, SHIP_W, SHIP_H);
                if (weaponType != WPN_CANNON || weaponLevel >= 2) // the single shot has no mounted gun
                    weaponAnim[weaponType].Draw(g, shipX, shipY, SHIP_W, SHIP_H);
            }
            if (!gameOver && now < shieldUntil)
            {
                Gdiplus::Pen shieldPen(Gdiplus::Color(200, 90, 200, 255), 3.0f);
                Gdiplus::SolidBrush shieldFill(Gdiplus::Color(40, 90, 200, 255));
                g.FillEllipse(&shieldFill, shipX - 8, shipY - 8, SHIP_W + 16, SHIP_H + 16);
                g.DrawEllipse(&shieldPen, shipX - 8, shipY - 8, SHIP_W + 16, SHIP_H + 16);
            }

            for (const Drop& d : drops)
            {
                const int dx = static_cast<int>(d.x), dy = static_cast<int>(d.y);
                const Animation& a = d.kind == DROP_ENGINE ? engineIcons[std::clamp(d.variant, 0, 2)] : dropSprites[d.kind];
                if (!a.frames.empty())
                {
                    a.DrawFrame(g, a.IndexAt(now, true), dx, dy, DROP_SIZE, DROP_SIZE);
                    continue;
                }
                // No health icon in the pickup pack: draw a green cross.
                Gdiplus::SolidBrush fill(Gdiplus::Color(255, 60, 220, 90));
                Gdiplus::Pen rim(Gdiplus::Color(255, 255, 255, 255), 2.0f);
                const int t = DROP_SIZE / 3;
                Gdiplus::Rect bars[2] = { { dx, dy + t, DROP_SIZE, t }, { dx + t, dy, t, DROP_SIZE } };
                for (const Gdiplus::Rect& r : bars) g.FillRectangle(&fill, r);
                for (const Gdiplus::Rect& r : bars) g.DrawRectangle(&rim, r);
            }

            Gdiplus::SolidBrush glow(Gdiplus::Color(255, 255, 214, 64));
            Gdiplus::SolidBrush core(Gdiplus::Color(255, 255, 255, 230));
            for (const Bullet& b : bullets)
            {
                if (b.kind == BK_SHOT)
                {
                    g.FillRectangle(&glow, static_cast<int>(b.x), static_cast<int>(b.y) - 3, 22, 6);
                    g.FillRectangle(&core, static_cast<int>(b.x) + 4, static_cast<int>(b.y) - 1, 16, 2);
                }
                else if (b.kind == BK_BIG)
                {
                    Gdiplus::SolidBrush bigGlow(Gdiplus::Color(160, 80, 200, 255));
                    Gdiplus::SolidBrush bigMid(Gdiplus::Color(255, 140, 230, 255));
                    g.FillEllipse(&bigGlow, static_cast<int>(b.x) - 6, static_cast<int>(b.y - b.h / 2) - 6, static_cast<int>(b.w) + 12, static_cast<int>(b.h) + 12);
                    g.FillEllipse(&bigMid, static_cast<int>(b.x), static_cast<int>(b.y - b.h / 2), static_cast<int>(b.w), static_cast<int>(b.h));
                    g.FillEllipse(&core, static_cast<int>(b.x) + 10, static_cast<int>(b.y) - 6, static_cast<int>(b.w) - 20, 12);
                }
                else
                {
                    Gdiplus::SolidBrush body(Gdiplus::Color(255, 210, 215, 225));
                    Gdiplus::SolidBrush flame(Gdiplus::Color(255, 255, 140, 30));
                    Gdiplus::SolidBrush noseBrush(Gdiplus::Color(255, 235, 70, 60));
                    Gdiplus::Point nose[3] = { { 8, -4 }, { 15, 0 }, { 8, 4 } };
                    g.TranslateTransform(b.x + b.w / 2, b.y);
                    g.RotateTransform(b.ang * 57.2958f);
                    g.FillRectangle(&flame, -17, -2, 8, 4);
                    g.FillRectangle(&body, -9, -4, 17, 8);
                    g.FillPolygon(&noseBrush, nose, 3);
                    g.ResetTransform();
                }
            }
            if (beamOn)
            {
                const float ox = shipX + SHIP_W - 10.0f, oy = shipY + SHIP_H / 2.0f;
                const float ca = std::cos(beamAngle), sa = std::sin(beamAngle), len = static_cast<float>(rc.right) * 1.5f;
                Gdiplus::PointF pts[40];
                int n = 0;
                for (float t = 0; t < len && n < 40; t += 40.0f, ++n)
                {
                    float jitter = n == 0 ? 0.0f : static_cast<float>((now / 35 + n * 37) % 9) - 4.0f;
                    pts[n] = { ox + ca * t - sa * jitter, oy + sa * t + ca * jitter };
                }
                Gdiplus::Pen outer(Gdiplus::Color(70, 90, 255, 200), 16.0f);
                Gdiplus::Pen mid(Gdiplus::Color(170, 120, 255, 230), 8.0f);
                Gdiplus::Pen inner(Gdiplus::Color(255, 255, 255, 255), 2.0f);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.DrawLines(&outer, pts, n);
                g.DrawLines(&mid, pts, n);
                g.DrawLines(&inner, pts, n);
                g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
            }
            Gdiplus::SolidBrush eGlow(Gdiplus::Color(255, 255, 60, 140));
            Gdiplus::SolidBrush eCore(Gdiplus::Color(255, 255, 220, 240));
            for (const EnemyBullet& b : enemyBullets)
            {
                g.FillEllipse(&eGlow, static_cast<int>(b.x) - 7, static_cast<int>(b.y) - 7, 14, 14);
                g.FillEllipse(&eCore, static_cast<int>(b.x) - 3, static_cast<int>(b.y) - 3, 6, 6);
            }
        }

        // HUD: GDI text and health bar on top of everything.
        {
            const ULONGLONG hudNow = GetTickCount64();
            auto text = [&](int x, int y, const std::wstring& s, COLORREF c)
            {
                SetTextColor(mem, c);
                TextOutW(mem, x, y, s.c_str(), static_cast<int>(s.size()));
            };
            text(16, 10, L"SCORE " + std::to_wstring(score), RGB(255, 255, 255));
            RECT bar = { 16, 40, 16 + 220, 56 };
            HBRUSH back = CreateSolidBrush(RGB(50, 20, 20));
            FillRect(mem, &bar, back);
            DeleteObject(back);
            bar.right = bar.left + 220 * std::max(0, health) / MAX_HEALTH;
            HBRUSH hp = CreateSolidBrush(health > 30 ? RGB(60, 220, 90) : RGB(235, 50, 50));
            FillRect(mem, &bar, hp);
            DeleteObject(hp);
            text(244, 36, L"HP " + std::to_wstring(std::max(0, health)), RGB(255, 255, 255));

            RECT ebar = { 16, 62, 16 + 220, 74 };
            HBRUSH eback = CreateSolidBrush(RGB(16, 24, 50));
            FillRect(mem, &ebar, eback);
            DeleteObject(eback);
            ebar.right = ebar.left + static_cast<int>(220 * std::max(0.0f, energy) / MAX_ENERGY);
            HBRUSH efill = CreateSolidBrush(energy < 12 ? RGB(230, 150, 40) : RGB(70, 160, 255));
            FillRect(mem, &ebar, efill);
            DeleteObject(efill);
            text(244, 56, L"ENERGY", RGB(150, 190, 255));

            static const wchar_t* cannonNames[4] = { L"SINGLE SHOT", L"DUAL AUTOCANNON", L"TRIPLE SHOT", L"QUAD SHOT" };
            std::wstring status = weaponType == WPN_ROCKET ? L"HOMING ROCKETS"
                : weaponType == WPN_BIGGUN ? L"BIG SPACE GUN"
                : weaponType == WPN_ZAPPER ? L"ZAPPER BEAM"
                : cannonNames[std::clamp(weaponLevel, 1, 4) - 1];
            status += L"   ENGINE LV " + std::to_wstring(engineLevel + 1);
            if (hudNow < shieldUntil) status += L"   SHIELD " + std::to_wstring((shieldUntil - hudNow) / 1000 + 1) + L"s";
            text(16, 82, status, RGB(255, 190, 70));
            if (waveActive && hudNow < bannerUntil)
            {
                SetTextAlign(mem, TA_CENTER);
                const std::wstring w = L"WAVE " + std::to_wstring(wave);
                text(rc.right / 2, 90, w, RGB(255, 255, 255));
                text(rc.right / 2, 90 + cellH + 4, levelNames[waves[wave - 1].level], RGB(190, 120, 255));
                SetTextAlign(mem, TA_LEFT);
            }
            else if (!waveActive && wave > 0 && !gameOver)
            {
                SetTextAlign(mem, TA_CENTER);
                text(rc.right / 2, 90, L"WAVE " + std::to_wstring(wave) + L" COMPLETE", RGB(120, 255, 160));
                SetTextAlign(mem, TA_LEFT);
            }
            {
                const std::wstring w = L"ROUND " + std::to_wstring(roundNo + 1) + L"  WAVE " + std::to_wstring(wave) + L"/" + std::to_wstring(waves.size());
                SetTextAlign(mem, TA_RIGHT);
                text(rc.right - 16, 10, w, RGB(200, 200, 255));
                SetTextAlign(mem, TA_LEFT);
            }
            if (hudNow < roundBannerUntil)
            {
                SetTextAlign(mem, TA_CENTER);
                text(rc.right / 2, rc.bottom / 2 - 40, L"ROUND 2", RGB(255, 80, 80));
                text(rc.right / 2, rc.bottom / 2 - 40 + cellH + 4, L"THE KLA'ED FLEET ARRIVES - STRONGER ENEMIES", RGB(255, 255, 255));
                SetTextAlign(mem, TA_LEFT);
            }
            if (gameOver)
            {
                const wchar_t* msg1 = victory ? L"VICTORY!" : L"GAME OVER";
                const wchar_t* msg2 = L"Press Enter to restart";
                SetTextAlign(mem, TA_CENTER);
                text(rc.right / 2, rc.bottom / 2 - 30, msg1, RGB(255, 80, 80));
                text(rc.right / 2, rc.bottom / 2 + 4, msg2, RGB(255, 255, 255));
                SetTextAlign(mem, TA_LEFT);
            }
        }

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        HDC hdc = GetDC(hwnd);
        HGDIOBJ old = SelectObject(hdc, font);
        TEXTMETRICW tm;
        GetTextMetricsW(hdc, &tm);
        cellW = tm.tmAveCharWidth;
        cellH = tm.tmHeight;
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
        ResizeGrid(hwnd);
        {
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&gdiToken, &input, nullptr);
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring dir(path);
            dir.erase(dir.find_last_of(L"\\/") + 1);
            assetDir = dir;
            LoadHighScore();
            while (GetFileAttributesW((dir + L"Planets\\planet_" + std::to_wstring(planetCount) + L".png").c_str()) != INVALID_FILE_ATTRIBUTES)
                ++planetCount;
            const std::wstring engineDir = L"Main Ship - Engine Effects\\Aseprite\\";
            const std::wstring engineName = L"Main Ship - Engines - Base Engine - ";
            shipSprite.Load(dir, L"Main Ship - Fighter Design.aseprite", { "Background", "48 48 measures" });
            if (shipSprite.frames.size() > 1) // keep only the base design
            {
                for (size_t i = 1; i < shipSprite.frames.size(); ++i) delete shipSprite.frames[i];
                shipSprite.frames.resize(1);
            }
            engineIdle.Load(dir, engineDir + engineName + L"Idle.aseprite", {});
            // Layers 0 and 5 are hidden in the file, so the loader skips them already.
            cannon.Load(dir, L"Main Ship - Weapons\\Aseprite\\Main Ship - Weapons - Auto Cannon.aseprite", {});
            dropSprites[DROP_WEAPON].Load(dir, L"PickupsPack\\Weapons\\Aseprite\\Pickup Icon - Weapons - Auto Cannons.aseprite", {}, false, true);
            dropSprites[DROP_ROCKET].Load(dir, L"PickupsPack\\Weapons\\Aseprite\\Pickup Icon - Weapons - Rocket.aseprite", {}, false, true);
            dropSprites[DROP_BIGGUN].Load(dir, L"PickupsPack\\Weapons\\Aseprite\\Pickup Icon - Weapons - Big Space Gun 2000.aseprite", {}, false, true);
            dropSprites[DROP_ZAPPER].Load(dir, L"PickupsPack\\Weapons\\Aseprite\\Pickup Icon - Weapons - Zapper.aseprite", {}, false, true);
            static const wchar_t* engineIconNames[3] = { L"Big Pulse Engine", L"Burst Engine", L"Supercharged Engine" };
            for (int i = 0; i < 3; ++i)
                engineIcons[i].Load(dir, std::wstring(L"PickupsPack\\Engines\\Aseprite\\Pickup Icon - Engines - ") + engineIconNames[i] + L".aseprite", {}, false, true);
            weaponAnim[WPN_ROCKET].Load(dir, L"Main Ship - Weapons\\Aseprite\\Main Ship - Weapons - Rockets.aseprite", {});
            weaponAnim[WPN_BIGGUN].Load(dir, L"Main Ship - Weapons\\Aseprite\\Main Ship - Weapons - Big Space Gun.aseprite", { "Layer 3" });
            weaponAnim[WPN_ZAPPER].Load(dir, L"Main Ship - Weapons\\Aseprite\\Main Ship - Weapons - Zapper.aseprite", {});
            dropSprites[DROP_SHIELD].Load(dir, L"PickupsPack\\Shield Generators\\Aseprite\\Pickup Icon - Shield Generator - Invincibility Shield.aseprite", {}, false, true);
            // Round 2: the Kla'ed fleet mirrors the Nairan roster with tougher, faster ships.
            const size_t nairanCount = enemyTypes.size();
            for (size_t i = 0; i < nairanCount; ++i)
            {
                EnemyType k = enemyTypes[i];
                k.faction = 1;
                k.hp = (k.hp * 3 + 1) / 2;
                k.speed *= 1.1f;
                enemyTypes.push_back(std::move(k));
            }
            for (EnemyType& t : enemyTypes)
            {
                const bool kla = t.faction == 1;
                const std::wstring root = kla ? L"Enemy Ships\\Kla'ed\\" : L"Enemy Ships\\Nairan\\";
                const std::wstring n = t.name;
                if (kla)
                {
                    const std::wstring kn = L"Kla'ed - " + (n == L"Support Ship" ? std::wstring(L"Support ship") : n);
                    t.base.Load(dir, root + L"Base\\Aseprite\\" + kn + L" - Base.aseprite", { "Background", "Layer 5" }, true);
                    t.engine.Load(dir, root + L"Engine\\Aseprite\\" + kn + L" - Engine.aseprite", { "Background", "Layer 5" }, true);
                    t.destruction.Load(dir, root + L"Destruction\\Aseprite\\" + kn + L" - Destruction.aseprite", { "Background", "Layer 5" }, true);
                    continue;
                }
                t.base.Load(dir, root + L"Designs - Base\\Aseprite\\Nairan - " + n + L" - Base.aseprite", {}, true);
                t.engine.Load(dir, root + L"Engine Effects\\Aseprite\\Nairan - " + n + L" - Engine.aseprite",
                    { "Background", "Layer 5", "Layer 6" }, true); // backdrop and label/reference layers
                // The Battlecruiser file name has an extra space.
                const std::wstring gap = n == L"Battlecruiser" ? L"  -  " : L" -  ";
                t.destruction.Load(dir, root + L"Destruction\\Aseprite\\Nairan - " + n + gap + L"Destruction.aseprite",
                    { "Background", "Layer 5" }, true);
            }
            // Drop enemies whose sprites failed to load.
            enemyTypes.erase(std::remove_if(enemyTypes.begin(), enemyTypes.end(),
                [](const EnemyType& t) { return t.base.frames.empty() || t.destruction.frames.empty(); }),
                enemyTypes.end());
            // The Powering file also contains a reference ship on these layers.
            enginePowering.Load(dir, engineDir + engineName + L"Powering.aseprite",
                { "Engine Effects", "Layer 1", "Layer 2" });
        }
        SetTimer(hwnd, TIMER_ID, 40, nullptr);
        return 0;
    }
    case WM_SIZE:
        ResizeGrid(hwnd);
        return 0;
    case WM_TIMER:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (!shipPlaced)
        {
            shipY = (rc.bottom - SHIP_H) / 2;
            shipPlaced = true;
            lastShipY = shipY;
        }
        auto held = [](int a, int b) { return ((GetAsyncKeyState(a) | GetAsyncKeyState(b)) & 0x8000) != 0; };
        bool left = held(VK_LEFT, 'A'), right = held(VK_RIGHT, 'D');
        bool up = held(VK_UP, 'W'), down = held(VK_DOWN, 'S');
        if (gameOver) left = right = up = down = false;
        if (left) shipX -= SHIP_SPEED;
        if (right) shipX += SHIP_SPEED;
        if (up) shipY -= SHIP_SPEED;
        if (down) shipY += SHIP_SPEED;
        bool nowThrusting = left || right || up || down;
        if (nowThrusting != thrusting)
        {
            thrusting = nowThrusting;
            (thrusting ? enginePowering : engineIdle).Restart();
        }
        shipX = std::clamp(shipX, 0, std::max(0, static_cast<int>(rc.right) - SHIP_W));
        shipY = std::clamp(shipY, 0, std::max(0, static_cast<int>(rc.bottom) - SHIP_H));
        if (!cave.empty())
        {
            Column next = NextColumn(&cave.back());
            cave.erase(cave.begin());
            cave.push_back(next);
        }
        (thrusting ? enginePowering : engineIdle).Advance();
        UpdatePlanets(rc);
        if (splash)
        {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        const ULONGLONG now = GetTickCount64();
        const float dt = lastTick ? std::min(0.1f, (now - lastTick) / 1000.0f) : 0.04f;
        lastTick = now;
        energy = std::min(MAX_ENERGY, energy + ENERGY_REGEN[engineLevel] * dt);

        bool fireKey = !gameOver && (GetAsyncKeyState(VK_SPACE) & 0x8000);
        if (fireKey && !firing)
        {
            weaponAnim[weaponType].Restart(1);
            lastShot = 0;
        }
        else if (!fireKey && firing)
        {
            weaponAnim[weaponType].Restart(0); // back to the idle frame
        }
        firing = fireKey;

        // The zapper beam sweeps opposite to vertical movement, then swings back.
        beamAngle = std::clamp((beamAngle + (shipY - lastShipY) * 0.008f) * 0.9f, -0.7f, 0.7f);
        lastShipY = shipY;
        beamOn = false;
        if (zapperExhausted && energy >= 15.0f) zapperExhausted = false;

        if (firing)
        {
            weaponAnim[weaponType].Advance(1);
            float cost = 0;
            ULONGLONG interval = 0;
            switch (weaponType)
            {
            case WPN_CANNON:
            {
                static const ULONGLONG intervals[4] = { 450, 400, 360, 330 }; // modest speed-ups per level
                static const float costs[4] = { 4, 6, 8, 10 };
                interval = intervals[weaponLevel - 1];
                cost = costs[weaponLevel - 1];
                break;
            }
            case WPN_ROCKET: interval = 750; cost = 16; break;
            case WPN_BIGGUN: interval = 1000; cost = 30; break;
            case WPN_ZAPPER: break;
            }

            const float mx = shipX + MUZZLE_X, midY = shipY + SHIP_H / 2.0f;
            if (weaponType == WPN_ZAPPER && !zapperExhausted && energy > 0)
            {
                beamOn = true;
                energy -= ZAPPER_DRAIN * dt;
                if (energy <= 0) { energy = 0; zapperExhausted = true; }
            }
            else if (weaponType != WPN_ZAPPER && energy >= cost)
            {
                if (now - lastShot >= interval)
                {
                    lastShot = now;
                    energy -= cost;
                    if (weaponType == WPN_CANNON)
                    {
                        if (weaponLevel == 1) bullets.push_back({ mx, midY });
                        else
                        {
                            bullets.push_back({ mx, shipY + MUZZLE_Y[0] });
                            bullets.push_back({ mx, shipY + MUZZLE_Y[1] });
                            if (weaponLevel == 3) bullets.push_back({ mx, midY });
                            if (weaponLevel >= 4) // diagonal side shots
                            {
                                bullets.push_back({ mx, shipY + MUZZLE_Y[0], -3.0f });
                                bullets.push_back({ mx, shipY + MUZZLE_Y[1], 3.0f });
                            }
                        }
                    }
                    else if (weaponType == WPN_ROCKET)
                    {
                        rocketSide = !rocketSide;
                        Bullet r{ mx, shipY + MUZZLE_Y[rocketSide ? 1 : 0] };
                        r.kind = BK_ROCKET; r.dmg = 6; r.w = r.h = 18; r.speed = 5.0f;
                        r.ang = rocketSide ? 0.5f : -0.5f; // launch outward, then home in
                        bullets.push_back(r);
                    }
                    else
                    {
                        Bullet big{ mx, midY };
                        big.kind = BK_BIG; big.dmg = 12; big.w = 56; big.h = 30; big.speed = 14.0f;
                        bullets.push_back(big);
                    }
                }
            }
            else if (now - lastShot >= EMPTY_INTERVAL_MS)
            {
                lastShot = now; // out of energy: a free, very slow single shot
                bullets.push_back({ mx, midY });
            }
        }
        for (Bullet& b : bullets)
        {
            if (b.kind != BK_ROCKET)
            {
                b.x += b.speed;
                b.y += b.vy;
                continue;
            }
            // Home in on the nearest living enemy that is still on screen and not behind the rocket.
            float tx = 0, ty = 0, bestD = 1e12f;
            bool found = false;
            for (const Enemy& e : enemies)
            {
                if (e.dying) continue;
                const FRect hb = EnemyHitBox(e);
                const float cx = (hb.left + hb.right) / 2, cy = (hb.top + hb.bottom) / 2;
                if (cx < b.x - 30 || cx > rc.right) continue;
                const float d = (cx - b.x) * (cx - b.x) + (cy - b.y) * (cy - b.y);
                if (d < bestD) { bestD = d; tx = cx; ty = cy; found = true; }
            }
            float diff = (found ? std::atan2(ty - b.y, tx - b.x) : 0.0f) - b.ang;
            while (diff > 3.14159f) diff -= 6.28318f;
            while (diff < -3.14159f) diff += 6.28318f;
            b.ang += std::clamp(diff, -0.16f, 0.16f);
            b.speed = std::min(b.speed + 0.7f, 15.0f);
            b.x += std::cos(b.ang) * b.speed;
            b.y += std::sin(b.ang) * b.speed;
        }
        bullets.erase(std::remove_if(bullets.begin(), bullets.end(),
            [&](const Bullet& b) { return b.x > rc.right || b.x < -60 || b.y < 0 || b.y > rc.bottom; }), bullets.end());

        if (!enemyTypes.empty() && !gameOver)
        {
            UpdateWaves(rc, now);
            UpdateAbilities(rc, now);
        }
        const float playerCx = shipX + SHIP_W / 2.0f, playerCy = shipY + SHIP_H / 2.0f;
        for (Enemy& e : enemies)
        {
            const EnemyType& t = enemyTypes[e.type];
            if (e.dying) { e.x -= 2.0f; continue; }
            // Close in on the player, then hold back and drift while shooting.
            float holdX = rc.right * 0.55f + e.ox;
            e.x -= e.x > holdX ? t.speed : t.speed / 4;

            // Steer toward the player's height, keeping the formation slot and staying in the cave.
            int size = EnemyDrawSize(t), top, bottom;
            Opening(static_cast<int>(e.x + size / 2), rc.bottom, top, bottom);
            float target = std::clamp(playerCy - size / 2.0f + e.oy,
                static_cast<float>(top), static_cast<float>(std::max(top, bottom - size)));
            e.y += std::clamp(target - e.y, -2.5f, 2.5f);

            if (now >= e.nextShot && e.x < rc.right - size && e.x > playerCx)
            {
                float sx = e.x + size * 0.15f, sy = e.y + size / 2.0f;
                float dx = playerCx - sx, dy = playerCy - sy;
                float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
                float vx = dx / len * ENEMY_BULLET_SPEED, vy = dy / len * ENEMY_BULLET_SPEED;
                int dmg = 5 + t.power / 3 + roundNo;
                float speedScale = roundNo ? 1.15f : 1.0f;
                if (wcscmp(t.name, L"Bomber") == 0) dmg += 3;                                // explosive shots
                else if (wcscmp(t.name, L"Torpedo Ship") == 0) { dmg += 5; speedScale = 0.75f; } // heavy torpedoes
                vx *= speedScale; vy *= speedScale;
                enemyBullets.push_back({ sx, sy, vx, vy, dmg });
                if (t.power >= 12) // capital ships fire a three-shot spread
                {
                    enemyBullets.push_back({ sx, sy, vx, vy - 3.0f, dmg });
                    enemyBullets.push_back({ sx, sy, vx, vy + 3.0f, dmg });
                }
                // Bigger ships fire faster.
                int base = std::max(450, 2000 - t.power * 90) * (roundNo ? 80 : 100) / 100;
                e.nextShot = now + Rand(base, base * 2);
            }
        }
        for (EnemyBullet& b : enemyBullets) { b.x += b.vx; b.y += b.vy; }
        enemyBullets.erase(std::remove_if(enemyBullets.begin(), enemyBullets.end(), [&](const EnemyBullet& b)
        {
            return b.x < -20 || b.x > rc.right + 20 || b.y < -20 || b.y > rc.bottom + 20;
        }), enemyBullets.end());

        // Player damage, pickups.
        const FRect pb = { shipX + 14.0f, shipY + 14.0f, shipX + SHIP_W - 8.0f, shipY + SHIP_H - 14.0f };
        auto hurt = [&](int dmg) -> bool
        {
            if (gameOver || now < shieldUntil || now < hurtUntil) return false;
            health -= dmg;
            hurtUntil = now + 700;
            if (dmg >= 10) // a heavy hit knocks out the special weapon, or one cannon level
            {
                if (weaponType != WPN_CANNON) { weaponType = WPN_CANNON; weaponAnim[WPN_CANNON].Restart(firing ? 1 : 0); }
                else weaponLevel = std::max(1, weaponLevel - 1);
            }
            if (health <= 0) { health = 0; gameOver = true; UpdateHighScore(); }
            return true;
        };
        if (!gameOver)
        {
            enemyBullets.erase(std::remove_if(enemyBullets.begin(), enemyBullets.end(), [&](const EnemyBullet& b)
            {
                bool hit = b.x > pb.left && b.x < pb.right && b.y > pb.top && b.y < pb.bottom;
                if (hit) hurt(b.dmg);
                return hit;
            }), enemyBullets.end());
            for (Enemy& e : enemies)
            {
                if (e.dying) continue;
                FRect hb = EnemyHitBox(e);
                if (hb.right > pb.left && hb.left < pb.right && hb.bottom > pb.top && hb.top < pb.bottom)
                {
                    if (now < shieldUntil) DamageEnemy(e, 3, now); // ramming under the shield
                    else if (hurt(20)) DamageEnemy(e, 6, now);
                }
            }
            drops.erase(std::remove_if(drops.begin(), drops.end(), [&](const Drop& d)
            {
                bool got = d.x + DROP_SIZE > pb.left && d.x < pb.right && d.y + DROP_SIZE > pb.top && d.y < pb.bottom;
                if (!got) return false;
                score += 100;
                auto equip = [&](WeaponType w)
                {
                    weaponType = w;
                    weaponLevel = 4;
                    weaponAnim[w].Restart(firing ? 1 : 0);
                };
                switch (d.kind)
                {
                case DROP_HEALTH: health = std::min(MAX_HEALTH, health + 30); break;
                case DROP_WEAPON: weaponLevel = std::min(4, weaponLevel + 1); break;
                case DROP_SHIELD: shieldUntil = now + 8000; break;
                case DROP_ENGINE: engineLevel = std::min(3, engineLevel + 1); break;
                case DROP_ROCKET: equip(WPN_ROCKET); break;
                case DROP_BIGGUN: equip(WPN_BIGGUN); break;
                case DROP_ZAPPER: equip(WPN_ZAPPER); break;
                default: break;
                }
                return true;
            }), drops.end());
        }
        for (Drop& d : drops) d.x -= 3.0f;
        drops.erase(std::remove_if(drops.begin(), drops.end(),
            [&](const Drop& d) { return d.x + DROP_SIZE < 0; }), drops.end());

        for (Enemy& e : enemies)
        {
            if (!e.id) e.id = nextEnemyId++;
            if (e.dying) continue;
            FRect hb = EnemyHitBox(e);
            bullets.erase(std::remove_if(bullets.begin(), bullets.end(), [&](Bullet& b)
            {
                bool hit = !e.dying && b.x + b.w > hb.left && b.x < hb.right && b.y + b.h / 2 > hb.top && b.y - b.h / 2 < hb.bottom;
                if (!hit) return false;
                if (b.kind == BK_BIG) // pierces: damages each enemy once and keeps flying
                {
                    if (std::find(b.hitIds.begin(), b.hitIds.end(), e.id) != b.hitIds.end()) return false;
                    b.hitIds.push_back(e.id);
                    DamageEnemy(e, b.dmg, now);
                    return false;
                }
                DamageEnemy(e, b.dmg, now);
                return true;
            }), bullets.end());
        }

        // The zapper beam damages everything along its line, in ticks.
        if (beamOn && now - lastBeamHit >= 100)
        {
            lastBeamHit = now;
            const float ox = shipX + SHIP_W - 10.0f, oy = shipY + SHIP_H / 2.0f;
            const float ca = std::cos(beamAngle), sa = std::sin(beamAngle);
            for (Enemy& e : enemies)
            {
                if (e.dying) continue;
                const FRect hb = EnemyHitBox(e);
                for (float t = 0; t < rc.right * 1.5f; t += 8.0f)
                {
                    const float px = ox + ca * t, py = oy + sa * t;
                    if (px > hb.left - 6 && px < hb.right + 6 && py > hb.top - 6 && py < hb.bottom + 6)
                    {
                        DamageEnemy(e, 2, now);
                        break;
                    }
                }
            }
        }
        enemies.erase(std::remove_if(enemies.begin(), enemies.end(), [&](const Enemy& e)
        {
            const EnemyType& t = enemyTypes[e.type];
            return e.x + EnemyDrawSize(t) < 0 || (e.dying && now - e.diedAt >= t.destruction.TotalMs());
        }), enemies.end());
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(hwnd);
        else if (wParam == VK_RETURN && splash)
        {
            splash = false;
            ResetGame();
        }
        else if (wParam == VK_RETURN && gameOver)
        {
            ResetGame();
            splash = true;
        }
        return 0;
    case WM_DESTROY:
        UpdateHighScore();
        KillTimer(hwnd, TIMER_ID);
        DeleteObject(font);
        shipSprite.Free();
        engineIdle.Free();
        enginePowering.Free();
        cannon.Free();
        for (Animation& a : weaponAnim) a.Free();
        for (Animation& a : engineIcons) a.Free();
        FreePlanets();
        for (Animation& a : dropSprites) a.Free();
        for (EnemyType& t : enemyTypes)
        {
            t.base.Free();
            t.engine.Free();
            t.destruction.Free();
        }
        Gdiplus::GdiplusShutdown(gdiToken);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    const wchar_t className[] = L"CaveDashWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, className, L"CaveDash - press Esc to exit",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
