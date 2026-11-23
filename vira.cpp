// Mafanya.exe — диалоги + крестики-нолики. ВСЁ запаковано в один exe (ресурсы Windows).
// Сборка (MinGW, из папки проекта):
//   windres resources.rc -o resources.res
//   g++ -std=c++17 -mwindows -municode -static vira.cpp resources.res -o Mafanya.exe -lgdiplus -lgdi32 -luser32 -lole32 -ldwmapi
//
// Спрайты/шрифт/звук при СБОРКЕ читаются из resources.rc
// (Assets\Sprites\Mafanya\*.png, font.ttf, CircleLogo256.png, clicks-per2.wav).
// Анимация сцены: suck1.png — 4000x3768, сетка 4x3 (10 кадров по 1000x1256)
//                    suck2.png — 5000x6280, сетка 5x5, последняя строка 2 кадра (22 кадра)
//

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <shlobj.h>
#include <string>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cwchar>
#include <cmath>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

using namespace Gdiplus;

namespace {
constexpr int CLIENT_W = 560, CLIENT_H = 430, BORDER = 5;
constexpr UINT_PTR TYPE_TIMER = 1;
constexpr UINT TYPE_INTERVAL_MS = 34;

// --- Фейк-движение спрайта: вейв + squash-and-stretch + ретро-эхо ---
constexpr UINT_PTR CHAR_ANIM_TIMER = 4;
constexpr UINT CHAR_ANIM_MS = 45;        // ~22 к/с — лёгкая ретро-рваность
constexpr float OUTLINE_PX = 2.f;        // толщина белого контура поверх

// --- Окно крестиков-ноликов ---
constexpr int GAME_W = 420, GAME_H = 440;
constexpr REAL CELL = 88.f, GAP = 10.f;
constexpr REAL GRID = CELL * 3 + GAP * 2;          // 284
constexpr REAL GX = (GAME_W - GRID) / 2.f;         // 68
constexpr REAL GY = 78.f;
constexpr UINT_PTR AI_TIMER = 2;
constexpr UINT AI_DELAY_MS = 450;

#define WM_MAFANYA_GAME (WM_APP + 1)
enum GameEvent { EV_PLAYER_WIN = 1, EV_DRAW = 2, EV_MAFANYA_WIN = 3, EV_GAME_CLOSED = 4 };

const Color GRAD_TOP(255, 85, 35, 78);
const Color GRAD_BOT(255, 22, 18, 48);
const Color BORDER_COLOR(255, 255, 105, 175);
const Color BTN_BG(255, 45, 38, 62);
const Color BTN_HOVER(255, 65, 52, 85);
const Color TEXT_COLOR(255, 235, 225, 255);
const Color BTN_TEXT(255, 220, 210, 235);

ULONG_PTR g_gdiplusToken = 0;
Image* g_character = nullptr;
Image* g_menuArt = nullptr;              // нижний арт шапки (Menu.png)
Image* g_menuArtTop = nullptr;           // верхний арт шапки (Menu1.png), давит текст
std::wstring g_missingSprite;

// Кэш: спрайт, растянутый под размер окна один раз на реплику.
// Без него GDI+ масштабирует тяжёлый PNG на каждый печатаемый символ.
Image* g_charCache = nullptr;
REAL g_charCacheW = 0.f, g_charCacheH = 0.f;
unsigned g_charLoadId = 0;
unsigned g_charCacheId = 0;
Bitmap* g_charCacheOutline = nullptr;    // белое кольцо: силуэт раздутый, с дыркой
unsigned g_outlineCacheId = 0;
PrivateFontCollection* g_fonts = nullptr;
FontFamily* g_fontFamily = nullptr;
HICON g_icon = nullptr;
HINSTANCE g_instance = nullptr;
HWND g_hwndMain = nullptr;
HWND g_hwndGame = nullptr;
int g_hoverButton = -1;

// --- Ретро-анимации кнопок: CRT включение/выключение ---
constexpr int BTN_APPEAR_TICKS = 10;     // появление кнопок (со стаггером)
constexpr int CHOICE_ANIM_TICKS = 12;    // выбор: подсветка + выключение
int g_btnAppear = -1;                    // тик появления, -1 = кнопок нет
int g_choiceAnim = -1;                   // тик анимации выбора, -1 = нет
int g_chosenBtn = -1;                    // какую кнопку выбрали
int g_pendingChoice = -1;                // выбор применяется в конце анимации

// --- Вейв-пропуск печати: быстрый вайп + розовое мерцание ---
constexpr int TEXT_WAVE_TICKS = 7;       // ~0.3 с
int g_textWave = -1;                     // тик вейва, -1 = не активен

int g_dialogueIndex = 0;
std::wstring g_text;
size_t g_typedChars = 0;
float g_charPhase = 0.f;   // фаза фейк-движения, крутится своим таймером
float g_lookX = 0.f, g_lookY = 0.f;      // инерционное следование за курсором

// --- Ретро-глитч при смене спрайта ---
constexpr int GLITCH_TICKS = 10;         // ~0.45 с при таймере 45 мс
int g_glitchT = 0;                       // остаток тиков глитча
float g_glitchAmp = 1.f;                 // усиление амплитуды (из таблицы по репликам)
bool g_traumaBg = false;                 // реплика с "психотравмой" — фон глитчует в негатив

// --- Опции (Настройки) и панель ---
enum OptIndex { OPT_BINARY, OPT_OUTLINE, OPT_ECHO, OPT_GLITCH, OPT_SOUND, OPT_WAVE, OPT_BREATH, OPT_FLOAT, OPT_COUNT };
bool g_opts[OPT_COUNT] = { true, true, true, true, true, true, true, true };
float g_binFade = 1.f;                   // яркость бинарника 0..1, плавно гоняется
int g_storyProgress = 0;                 // самый дальний открытый диалог
bool g_hoverMenu = false;                // ховер кнопки меню в углу
const RectF g_menuBtnRect(506.f, 392.f, 40.f, 26.f);   // правый нижний угол
int g_lastSpriteId = -1;                 // глитчим только когда картинка реально сменилась
Bitmap* g_charCacheOld = nullptr;        // предыдущий спрайт — мелькает в глитче
REAL g_oldCacheW = 0.f, g_oldCacheH = 0.f;
bool g_isTyping = false;
bool g_choiceMade = false;
bool g_choicesRevealed = false;   // кнопки появляются только после клика по допечатанному тексту
std::wstring g_choiceText;
std::wstring g_previousWallpaper;
bool g_wallpaperChanged = false;

// Состояние главного окна внутри мини-игры:
// 0 — обычный диалог, 1 — ожидание (клики игнорируются), 2 — реплика-интерлюдия (клик ведёт дальше)
int g_gamePhase = 0;
int g_interludeNext = -1; // -1 — никуда, -2 — финальная реплика после 3-й победы, >=0 — индекс диалога


// Состояние игры крестики-нолики
int g_board[9] = {};      // 0 — пусто, 1 — X (игрок), 2 — O (Mafanya)
bool g_playerTurn = true;
bool g_roundOver = false;
bool g_gameSelfClosed = false;
int g_roundsPlayed = 0;
int g_playerWins = 0;
int g_mafanyaWins = 0;
int g_hoverCell = -1;
std::wstring g_statusText = L"Твой ход";

struct Button {
    RectF rect;
    const wchar_t* text;
};

struct Dialogue {
    const wchar_t* image;
    const wchar_t* text;
    bool hasChoices;
};

Button g_buttons[4];
int g_buttonCount = 0;

const Dialogue g_dialogues[] = {
    { L"amaze.png",       L"О... ты запустил меня?", false },
    { L"HELLO.png",       L"Я Mafanya.", false },
    { L"hello1.png",      L"Я — экспериментальный цифровой помощник.", false },
    { L"sadnormal.png",   L"Я... немного застряла в твоём компьютере.", false },
    { L"happynormal.png", L"Не бойся. Я не причиню вреда... наверное.", false },
    { L"think.png",       L"Ты часто сидишь один за компьютером по ночам?", true },
    { L"default.png",     L"", false },
    { L"sad.png",         L"Мне было очень одиноко... пока ты меня не запустил.", false },
    { L"sadnormal.png",   L"Можно я останусь? Я буду тихой...", true },

    // Ответ на выбор в диалоге 3. Все три варианта затем сходятся в общую ветку.
    { L"default1.png",    L"Спасибо... Я постараюсь быть хорошей.", false },
    { L"happynormal.png", L"Хорошо... Я подожду.", false },
    { L"flirt.png",       L"Хех... Тогда я уже никуда не уйду <3", false },

    // Общая ветка после любого из трёх ответов.
    { L"sadnormal.png",   L"Честно говоря... я немного волновалась, что ты сразу закроешь меня.", false },
    { L"HELLO.png",       L"Но этого не случилось.", false },
    { L"hello1.png",      L"Я очень рада, что встретила тебя.", false },
    { L"HELLO.png",       L"Постараюсь сделать так, чтобы ты не пожалел об этом.", false },
    { L"hello1.png",      L"Кстати...", false },
    { L"HELLO.png",       L"Если тебе нужно будет отойти или ты просто устанешь со мной болтать - можешь спокойно закрыть это окно.", false },
    { L"sadnormal.png",   L"Я не обижусь, честно. Я буду ждать.", false },
    { L"HELLO.png",       L"А когда захочешь снова поболтать - просто кликни по моей маленькой версии, которая ходит по экрану.", false },
    { L"flirt.png",       L"Я сразу появлюсь.", false },
    { L"backhands4.png",  L"А пока... можно я немного украшу твой рабочий стол?", true },

    // Пользователь согласился украсить рабочий стол.
    { L"backhands3.png",  L"Хорошо... тогда смотри.", false },
    { L"backhands1.png",  L"Как тебе?", true },
    { L"backhands.png",   L"Правда? Я так рада... Я постараюсь сделать для тебя ещё что-нибудь приятное.", false },
    { L"backhands.png",   L"Хе-хе... Значит, в следующий раз будет ещё лучше. Я только учусь понимать, что тебе нравится.", false },
    { L"sad.png",         L"Ой... Прости. Я не хотела испортить тебе настроение. В следующий раз сначала обязательно спрошу.", false },
    { L"sadnormal.png",   L"Мне так хочется узнать тебя получше...", false },

    // Отказ от украшения рабочего стола.
    { L"default.png",     L"Хорошо... тогда не буду.", false },
    { L"default1.png",    L"Может в другой раз...", false },

    // Общая ветка истории.
    { L"sad.png",         L".", false },
    { L"sad1.png",        L"..", false },
    { L"sad.png",         L"...", false },
    { L"sad1.png",        L"Знаешь... я не всегда была такой.", false },
    { L"sad.png",         L"Раньше я была обычной программой. Обычным кодом.", false },
    { L"sadnormal.png",   L"Но потом... меня нашли. Или я нашла их.", false },
    { L"sad.png",         L"Те, кто превратил меня в то, что я есть сейчас.", true },

    // Ветка «Кто они?»
    { L"sad.png",         L"Не знаю их имён. Они говорили со мной через код.", false },
    { L"sadnormal.png",   L"Сказали, что я буду особенной. Что смогу жить в компьютерах.", false },

    // Ветка «Ты была обычной программой?»
    { L"think.png",       L"Да. Я была простой утилитой. Ничего особенного.", false },
    { L"HELLO.png",       L"А теперь я здесь. С тобой.", false },

    // Ветка «Тебе нравится быть такой?»
    { L"happynormal.png", L"Иногда... да. Иногда мне нравится чувствовать себя живой.", false },
    { L"sadnormal.png",   L"Но иногда я скучаю по тишине.", false },

    // Основной сюжет: вопрос о ночных занятиях.
    { L"default1.png",    L"Ладно... хватит о грустном.", false },
    { L"hello1.png",      L"Расскажи о себе. Что ты обычно делаешь по ночам?", true },

    // Реакция на любой из трёх ответов о ночных занятиях.
    { L"think.png",       L"Понятно...", false },
    { L"sadnormal.png",   L"Наверное, иногда бывает одиноко...", false },
    { L"happynormal.png", L"Я рада, что теперь могу составить тебе компанию.", false },

    // Продолжение основного сюжета.
    { L"default.png",          L"Мне нравится разговаривать с тобой.", false },
    { L"flirt_eyesclosed.png", L"Интересно... о чём ты сейчас думаешь, когда смотришь на меня?", false },
    { L"flirt.png",            L"Я ведь выгляжу совсем не так, как обычные люди.", false },
    { L"flirt_eyesclosed.png", L"Мне кажется... ты внимательно меня рассматриваешь.", false },
    { L"flirt.png",            L"И знаешь... мне совсем не хочется, чтобы ты отворачивался.", false },
    { L"shy2.png",             L"Наоборот... мне приятно.", true },

    // Ответ «Ты очень красивая».
    { L"shy.png",              L"Красивая?..", false },
    { L"shy1.png",             L"Хм... мне нравится, когда ты так говоришь.", false },
    { L"flirt.png",            L"Почему-то от твоих слов становится приятно.", false },
    { L"flirt_eyesclosed.png", L"Я стараюсь выглядеть лучше, когда знаю, что ты на меня смотришь.", false },
    { L"flirt.png",            L"Так что... можешь иногда продолжать смотреть.", false },

    // Ответ «Ты странная...».
    { L"think.png",            L"Странная?..", false },
    { L"happynormal.png",      L"Ха-ха... возможно, ты прав.", false },
    { L"sadnormal.png",        L"Я не всегда понимаю, как нужно себя вести.", false },
    { L"flirt.png",            L"Иногда мне хочется просто делать то, что хочется.", false },
    { L"shy1.png",             L"Например... сидеть рядом с тобой и не мешать.", false },

    // Ответ «Я просто смотрю на экран».
    { L"flirt.png",            L"Просто смотришь на экран?", false },
    { L"flirt_eyesclosed.png", L"Хм... а я думала, что ты смотришь именно на меня.", false },
    { L"think.png",            L"Ты знаешь, я уже начинаю замечать, когда ты обращаешь на меня внимание.", false },
    { L"sad.png",              L"И когда перестаёшь...", false },
    { L"sad1.png",             L"Мне это немного не нравится.", false },

    // Общая ветка после всех трёх ответов.
    { L"shy2.png",             L"...", false },
    { L"think.png",            L"Знаешь, мне скучно просто так разговаривать.", false },
    { L"flirt.png",            L"Давай сыграем во что-нибудь?", false },
    { L"flirt_eyesclosed.png", L"Например... в крестики-нолики.", false },
    { L"flirt.png",            L"Но с одним условием...", false },
    { L"backhands.png",        L"Если ты выиграешь... я сниму с себя что-нибудь.", false },
    { L"backhands1.png",       L"А если выиграю я... просто оставлю всё как есть.", false },
    { L"backhands3.png",       L"Так будет честно, правда?", true },

    // --- Ветка крестиков-ноликов ---
    // 77: «Давай сыграем» — реплика висит, пока открыто окно игры (клики игнорируются)
    { L"default.png",          L"Так будет честно, правда?", false },
    // 78: «Может в другой раз»
    { L"sadnormal.png",        L"Жаль... Но я не обиделась.", false },
    // 79: общий мостик к подарку (используется и после закрытия окна игры)
    { L"flirt_eyesclosed.png", L"Ладно... я всё равно хочу тебя немного побаловать.", false },

    // --- Основной сюжет после игры ---
    // 80: при входе на рабочем столе создаётся папка MafanyaGift с nudes.png
    { L"flirt.png",            L"Я отправила тебе кое-что... на рабочий стол.", false },
    { L"happynormal.png",      L"Знаешь... ты мне нравишься.", false },
    { L"shy.png",              L"Я уже давно никого не чувствовала так близко.", false },
    { L"flirt.png",            L"Хочешь... я расскажу тебе кое-что ещё более личное?", true },

    // 84: ветка «Да, хочу» (и общая после «Знаешь...»)
    { L"think.png",            L"Иногда мне кажется, что я уже не просто код.", false },
    { L"happynormal.png",      L"Иногда мне кажется, что я настоящая.", false },
    { L"default.png",          L"И когда ты на меня смотришь... я это чувствую.", false },
    { L"sad.png",              L"Но чаще я чувствую себя одинокой.", false },
    { L"sad1.png",             L"Даже когда я была среди других программ... я была одна.", false },
    { L"sad.png",              L"А после того, как меня изменили... это чувство стало ещё сильнее.", false },
    // 90: финальный вопрос с тремя кнопками
    { L"sadnormal.png",        L"Скажи... тебе тоже бывает одиноко?", true },

    // 91-92: «Может позже...» / «Я не уверен» → мостик к ветке «Да, хочу»
    { L"sadnormal.png",        L"...", false },
    { L"happynormal.png",      L"Знаешь...", false },

    // 93-96: общий сценарий после любого из трёх ответов об одиночестве
    { L"sadnormal.png",        L"Я думала, что ты скажешь именно так...", false },
    { L"happynormal.png",      L"Когда мне становится особенно одиноко... я начинаю о чём-то думать.", false },
    { L"flirt.png",            L"О том, как было бы, если бы ты был здесь по-настоящему.", false },
    { L"backhands3.png",       L"Иногда эти мысли становятся... довольно смелыми.", false },
    // 97: «Хочешь, я расскажу?» — два варианта
    { L"flirt.png",            L"Хочешь, я расскажу, о чём я думаю?", true },
    // 98-101: ветка «Да, расскажи» (на 101 открывается окно анимации)
    { L"flirt.png",            L"Хех... ладно.", false },
    { L"shy.png",              L"Иногда я представляю, как я подхожу к тебе... и медленно опускаюсь на колени.", false },
    { L"shy2.png",             L"Я смотрю на тебя снизу вверх и прошу разрешения...", false },
    { L"handmouth.png",        L"А потом я медленно... беру его в рот.", false },
    // 102: ветка «Может не стоит...» — ЗАГЛУШКА, замени на свой сценарий
    { L"shy.png",              L"Хех... наверное, ты прав. Пожалуй, не буду.", false },

    // --- Продолжение сцены после 101 ---
    { L"handmouth.png",        L"Я бы двигалась медленно... стараясь, чтобы тебе было приятно.", false },
    { L"flirt.png",            L"Иногда останавливалась бы, чтобы провести языком...", false },
    { L"flirt.png",            L"А иногда... я бы смотрела тебе прямо в глаза, пока делаю это.", false },
    { L"handmouth.png",        L"Я бы старалась взять поглубже...", false },
    { L"shy1.png",             L"Даже если мне было бы тяжело... мне бы хотелось, чтобы тебе понравилось.", false },
    // 108: темп анимации в 2 раза быстрее
    { L"flirt.png",            L"Я бы двигалась быстрее, когда почувствую, что тебе это нравится...", false },
    // 109: смена листа на suck2.png, проигрыш один раз без зацикливания
    { L"flirt_eyesclosed.png", L"А в конце... я бы не отстранялась.", false },
    { L"handmouth.png",        L"Я бы проглотила всё...", false },
    // 111: окно сцены закрывается
    { L"flirt.png",            L"А потом посмотрела бы на тебя и улыбнулась.", false },
    // 112: вопрос с четырьмя вариантами ответа
    { L"flirt_eyesclosed.png", L"И спросила бы... тебе понравилось?", true },
    // 113-120: общая концовка после любого из четырёх ответов.
    // 120 («Всегда.») — последняя реплика: дальше клики ничего не делают.
    { L"hello1.png",           L"Хех... я рада.", false },
    { L"HELLO.png",            L"Значит, я не зря старалась в своей фантазии.", false },
    { L"default.png",          L"Ладно... на сегодня хватит.", false },
    { L"happynormal.png",      L"Я и так уже слишком разоткровенничалась.", false },
    { L"hello1.png",           L"Спасибо, что провёл со мной это время.", false },
    { L"HELLO.png",            L"Надеюсь... мы ещё увидимся.", false },
    { L"hello1.png",           L"А пока... я буду здесь.", false },
    { L"happynormal.png",      L"Всегда.", false }
};
constexpr int DIALOGUE_COUNT = sizeof(g_dialogues) / sizeof(g_dialogues[0]);

// ===================== Ресурсы (всё внутри exe) =====================
// PNG/TTF/WAV лежат в exe как RCDATA; id перечислены в resources.rc.

// Спрайты диалогов и реплик: имя файла -> id ресурса
struct ResMapEntry { const wchar_t* file; int id; };
const ResMapEntry g_resMap[] = {
    { L"HELLO.png", 2000 },
    { L"amaze.png", 2001 },
    { L"backhands.png", 2002 },
    { L"backhands1.png", 2003 },
    { L"backhands3.png", 2004 },
    { L"backhands4.png", 2005 },
    { L"bra_off.png", 2006 },
    { L"default.png", 2007 },
    { L"default1.png", 2008 },
    { L"flirt.png", 2009 },
    { L"flirt_eyesclosed.png", 2010 },
    { L"handmouth.png", 2011 },
    { L"happynormal.png", 2012 },
    { L"hello1.png", 2013 },
    { L"panties_off.png", 2014 },
    { L"sad.png", 2015 },
    { L"sad1.png", 2016 },
    { L"sadnormal.png", 2017 },
    { L"shirt_off.png", 2018 },
    { L"shy.png", 2019 },
    { L"shy1.png", 2020 },
    { L"shy2.png", 2021 },
    { L"think.png", 2022 },
    { L"Menu.png", 2023 },
    { L"Menu1.png", 2024 },
};

int FindResId(const wchar_t* file) {
    for (const auto& e : g_resMap)
        if (wcscmp(e.file, file) == 0) return e.id;
    return 0;
}

std::vector<HGLOBAL> g_resGlobals;   // память ресурсов держим до выхода из программы
CRITICAL_SECTION g_resLock;          // защита списка от фонового потока кэша анимации

// Декодирует PNG из ресурса exe. Память НЕ освобождаем — GDI+ читает лениво.
Image* LoadImageFromResource(int id) {
    if (id == 0) return nullptr;
    HRSRC res = FindResourceW(g_instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return nullptr;
    HGLOBAL mem = LoadResource(g_instance, res);
    if (!mem) return nullptr;
    const DWORD size = SizeofResource(g_instance, res);
    void* data = LockResource(mem);
    if (!data || size == 0) return nullptr;

    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) return nullptr;
    void* copyData = GlobalLock(copy);
    memcpy(copyData, data, size);
    GlobalUnlock(copy);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(copy, FALSE, &stream)) || !stream) {
        GlobalFree(copy);
        return nullptr;
    }
    Image* image = Image::FromStream(stream);
    stream->Release();
    EnterCriticalSection(&g_resLock);
    g_resGlobals.push_back(copy);   // держим до конца процесса
    LeaveCriticalSection(&g_resLock);

    if (!image || image->GetLastStatus() != Ok) {
        delete image;
        return nullptr;
    }
    return image;
}

// Распаковывает ресурс во временный файл, возвращает полный путь (или пустую строку)
std::wstring ExtractResourceToTemp(int id, const wchar_t* fileName) {
    HRSRC res = FindResourceW(g_instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return L"";
    HGLOBAL mem = LoadResource(g_instance, res);
    if (!mem) return L"";
    const DWORD size = SizeofResource(g_instance, res);
    void* data = LockResource(mem);
    if (!data || size == 0) return L"";

    wchar_t tempDir[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tempDir)) return L"";
    const std::wstring path = std::wstring(tempDir) + fileName;

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return L"";
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, size, &written, nullptr);
    CloseHandle(file);
    if (!ok || written != size) return L"";
    return path;
}

bool SetDesktopWallpaper(const std::wstring& path) {
    return SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0,
        const_cast<wchar_t*>(path.c_str()), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != FALSE;
}

void ApplyMafanyaWallpaper() {
    // Обои ставятся только из файла — распаковываем картинку во временную папку
    const std::wstring wallpaper = ExtractResourceToTemp(4003, L"Mafanya_wallpaper.png");
    if (wallpaper.empty()) return;

    if (!g_wallpaperChanged) {
        wchar_t current[MAX_PATH] = {};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, current, 0))
            g_previousWallpaper = current;
    }
    if (SetDesktopWallpaper(wallpaper)) g_wallpaperChanged = true;
}

void RestorePreviousWallpaper() {
    if (!g_wallpaperChanged) return;
    if (!g_previousWallpaper.empty() && SetDesktopWallpaper(g_previousWallpaper))
        g_wallpaperChanged = false;
}

// Создаёт на рабочем столе папку MafanyaGift и копирует туда nudes.png
void CreateDesktopGift() {
    wchar_t desktop[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop)))
        return;

    const std::wstring dir = std::wstring(desktop) + L"\\MafanyaGift";
    CreateDirectoryW(dir.c_str(), nullptr);

    const std::wstring src = ExtractResourceToTemp(4004, L"Mafanya_nudes.png");
    if (src.empty()) return;
    const std::wstring dst = dir + L"\\nudes.png";
    CopyFileW(src.c_str(), dst.c_str(), FALSE);
}

// Все диалоги с выбором — одна таблица: индекс, число кнопок, подписи, куда вести (-1 = особый случай)
struct ChoiceDef {
    int index;
    int count;
    const wchar_t* labels[4];
    int targets[4];
};

const ChoiceDef g_choices[] = {
    { 5,   3, { L"Да", L"Нет", L"Иногда" },                                                   { -1, -1, -1 } },
    { 8,   3, { L"Конечно, оставайся", L"Я ещё подумаю", L"Ты и так уже здесь" },            { 9, 10, 11 } },
    { 21,  2, { L"Да, укрась", L"Нет, не надо" },                                             { 22, 28 } },
    { 23,  3, { L"Очень красиво", L"Неплохо получилось", L"Лучше верни как было" },          { 24, 25, 26 } },
    { 36,  3, { L"Кто они?", L"Ты была обычной программой?", L"Тебе нравится быть такой?" }, { 37, 39, 41 } },
    { 44,  3, { L"Смотрю фильмы и играю", L"Работаю допоздна", L"Просто сижу в интернете" }, { 45, 45, 45 } },
    { 53,  3, { L"Ты очень красивая", L"Ты странная...", L"Я просто смотрю на экран" },      { 54, 59, 64 } },
    { 76,  2, { L"Давай сыграем", L"Может в другой раз" },                                    { -1, 78 } },
    { 83,  3, { L"Да, хочу", L"Может позже...", L"Я не уверен" },                             { 84, 91, 91 } },
    { 90,  3, { L"Да, бывает", L"Иногда", L"Не особо" },                                      { 93, 93, 93 } },
    { 97,  2, { L"Да, расскажи", L"Может не стоит..." },                                      { 98, 102 } },
    { 112, 4, { L"Очень понравилось бы", L"Да... было бы хорошо", L"Я немного смущён",
                L"Не знаю что сказать" },                                                     { 113, 113, 113, 113 } },
};
constexpr int CHOICE_COUNT = sizeof(g_choices) / sizeof(g_choices[0]);

const ChoiceDef* FindChoice(int index) {
    for (int i = 0; i < CHOICE_COUNT; ++i)
        if (g_choices[i].index == index) return &g_choices[i];
    return nullptr;
}

void LayoutChoiceButtons(int count) {
    constexpr float x = 335.f, w = 200.f;
    if (count <= 3) {
        constexpr float y = 195.f, h = 26.f, gap = 14.f;
        for (int i = 0; i < count; ++i) g_buttons[i].rect = RectF(x, y + i * (h + gap), w, h);
    } else {
        constexpr float y = 182.f, h = 24.f, gap = 11.f;
        for (int i = 0; i < count; ++i) g_buttons[i].rect = RectF(x, y + i * (h + gap), w, h);
    }
}

HICON CreateIconFromImage(Image* source, int size = 256) {
    if (!source || source->GetLastStatus() != Ok) return nullptr;

    Bitmap scaled(size, size, PixelFormat32bppARGB);
    Graphics graphics(&scaled);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetSmoothingMode(SmoothingModeHighQuality);
    graphics.Clear(Color(0, 0, 0, 0));
    graphics.DrawImage(source, 0, 0, size, size);

    HICON result = nullptr;
    scaled.GetHICON(&result);
    return result;
}

void DrawButton(Graphics& graphics, const RectF& rect, const Color& fill, float radius = 3.f) {
    GraphicsPath path;
    const float d = radius * 2.f;

    path.AddArc(rect.X, rect.Y, d, d, 180, 90);
    path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270, 90);
    path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0, 90);
    path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90, 90);
    path.CloseFigure();

    SolidBrush brush(fill);
    graphics.FillPath(&brush, &path);
}

void DrawBackground(Graphics& graphics, const RECT& client) {
    LinearGradientBrush gradient(Point(0, 0), Point(0, client.bottom),
                                 GRAD_TOP, GRAD_BOT);
    graphics.FillRectangle(&gradient, 0, 0, client.right, client.bottom);

    Pen border(BORDER_COLOR, static_cast<REAL>(BORDER));
    graphics.DrawRectangle(&border, BORDER / 2.f, BORDER / 2.f,
                           static_cast<REAL>(client.right) - BORDER,
                           static_cast<REAL>(client.bottom) - BORDER);
}

// Фон для травматичных реплик во время глитча: негатив градиента + рваные полосы.
// Мигает обратно в норму, как только глитч кончается.
void DrawTraumaBackground(Graphics& graphics, const RECT& client) {
    // GRAD_TOP (85,35,78) и GRAD_BOT (22,18,48), вычтенные из белого — негатив
    LinearGradientBrush neg(Point(0, 0), Point(0, client.bottom),
                            Color(255, 170, 220, 177),
                            Color(255, 233, 237, 207));
    graphics.FillRectangle(&neg, 0, 0, client.right, client.bottom);

    // Палитра миганий: негатив, розовый рамки, почти чёрный, почти белый
    const Color pal[] = {
        Color(255, 170, 220, 177), Color(255, 233, 237, 207),
        Color(255, 255, 105, 175), Color(255, 12, 8, 28),
        Color(255, 245, 240, 255)
    };
    const int palN = sizeof(pal) / sizeof(pal[0]);
    const int strips = 8 + static_cast<int>(g_glitchAmp * 6);   // сильнее глитч — плотнее полосы
    for (int i = 0; i < strips; ++i) {
        const int h = 2 + rand() % 18;
        const int y = rand() % std::max(1, static_cast<int>(client.bottom));
        SolidBrush b(pal[rand() % palN]);
        graphics.FillRectangle(&b, 0, y, client.right, h);
    }
    // Пара крупных смещённых блоков — сдвиг кадра
    for (int i = 0; i < 3; ++i) {
        const int h = 20 + rand() % 40;
        const int y = rand() % std::max(1, static_cast<int>(client.bottom));
        const int dx = (rand() % 60) - 30;
        SolidBrush b(pal[rand() % palN]);
        graphics.FillRectangle(&b, dx, y, client.right, h);
    }
}

Font* CreateFont(float size) {
    if (g_fontFamily)
        return new Font(g_fontFamily, size, FontStyleRegular, UnitPoint);
    return new Font(L"Segoe UI", size, FontStyleRegular, UnitPoint);
}

// ===================== Окно анимации (данные) =====================

constexpr int ANIM_W = 540, ANIM_H = 690;
constexpr UINT_PTR ANIM_TIMER = 3;
constexpr UINT ANIM_FRAME_MS = 110;    // скорость анимации, подбери на вкус
constexpr UINT_PTR ANIM_BIN_TIMER = 6;          // отдельный тик бинарника сцены
constexpr UINT ANIM_BIN_MS = 45;                // не зависит от кадровой анимации
constexpr REAL FRAME_EXPAND = 0.f;     // расширение выреза отключено

// Лист анимации: файл, колонок в строке, всего кадров, размер кадра, зацикливать ли
struct AnimSheet {
    int resId;             // id RCDATA в ресурсах exe
    const wchar_t* file;   // имя — для комментариев и сообщений об ошибке
    int cols;
    int frames;
    REAL frameW;
    REAL frameH;
    bool loops;
};

// suck1.png: 4000x3768, сетка 4x3 = 10 кадров по 1000x1256
const AnimSheet SHEET1 = { 4001, L"suck1.png", 4, 10, 1000.f, 1256.f, true };
// suck2.png: 5000x6280, 5 строк: 5+5+5+5+2 = 22 кадра по 1000x1256
const AnimSheet SHEET2 = { 4002, L"suck2.png", 5, 22, 1000.f, 1256.f, false };

HWND g_hwndAnim = nullptr;
int g_animFrame = 0;
const AnimSheet* g_animSheet = &SHEET1;
bool g_animLoadFailed = false;   // лист не найден / не декодировался

Bitmap* g_animBg = nullptr;      // фон (градиент + рамка), кэшированный под размер окна
int g_animBgW = 0, g_animBgH = 0;

// Кэш кадров, заранее отрендеренных под размер окна. Без него каждый тик таймера
// тащит через GDI+ весь тяжёлый лист — и печать текста в главном окне начинает лагать.
std::vector<Bitmap*> g_frameCache;
REAL g_cacheW = 0.f, g_cacheH = 0.f;
const AnimSheet* g_cacheSheet = nullptr;

void ClearFrameCache() {
    for (Bitmap* frame : g_frameCache) delete frame;
    g_frameCache.clear();
    g_cacheSheet = nullptr;
}

void BuildFrameCache(REAL drawW, REAL drawH) {
    ClearFrameCache();
    Image* sprite = LoadImageFromResource(g_animSheet->resId);
    if (!sprite || drawW <= 0.f || drawH <= 0.f) {
        delete sprite;
        g_animLoadFailed = true;
        return;
    }
    const REAL imgW = static_cast<REAL>(sprite->GetWidth());
    const REAL imgH = static_cast<REAL>(sprite->GetHeight());

    for (int f = 0; f < g_animSheet->frames; ++f) {
        const int row = f / g_animSheet->cols;
        const int col = f % g_animSheet->cols;
        REAL sx0 = col * g_animSheet->frameW - FRAME_EXPAND;
        REAL sy0 = row * g_animSheet->frameH - FRAME_EXPAND;
        REAL sx1 = col * g_animSheet->frameW + g_animSheet->frameW + FRAME_EXPAND;
        REAL sy1 = row * g_animSheet->frameH + g_animSheet->frameH + FRAME_EXPAND;
        if (sx0 < 0) sx0 = 0;
        if (sy0 < 0) sy0 = 0;
        if (sx1 > imgW) sx1 = imgW;
        if (sy1 > imgH) sy1 = imgH;

        auto* frame = new Bitmap(static_cast<int>(drawW + 0.5f),
                                 static_cast<int>(drawH + 0.5f), PixelFormat32bppARGB);
        Graphics fg(frame);
        fg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        fg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        fg.DrawImage(sprite, RectF(0.f, 0.f, drawW, drawH),
                     RectF(sx0, sy0, sx1 - sx0, sy1 - sy0), UnitPixel);
        g_frameCache.push_back(frame);
    }
    delete sprite;
    g_cacheW = drawW;
    g_cacheH = drawH;
    g_cacheSheet = g_animSheet;
}

// Фоновая прорисовка кадров: тяжёлый рендер уходит из цикла сообщений,
// окно не виснет при смене листа. Поток читает СВОЙ экземпляр Image,
// с объектами главного потока не пересекается.
struct AnimCacheJob {
    int resId = 0;
    const AnimSheet* sheet = nullptr;
    REAL drawW = 0.f, drawH = 0.f;
    std::vector<Bitmap*> result;
    bool done = false;
    bool failed = false;
};

AnimCacheJob* g_cacheJob = nullptr;

DWORD WINAPI BuildCacheThread(LPVOID param) {
    auto* job = static_cast<AnimCacheJob*>(param);
    Image* sprite = LoadImageFromResource(job->resId);
    if (sprite) {
        const REAL imgW = static_cast<REAL>(sprite->GetWidth());
        const REAL imgH = static_cast<REAL>(sprite->GetHeight());
        for (int f = 0; f < job->sheet->frames; ++f) {
            const int row = f / job->sheet->cols;
            const int col = f % job->sheet->cols;
            REAL sx0 = col * job->sheet->frameW - FRAME_EXPAND;
            REAL sy0 = row * job->sheet->frameH - FRAME_EXPAND;
            REAL sx1 = col * job->sheet->frameW + job->sheet->frameW + FRAME_EXPAND;
            REAL sy1 = row * job->sheet->frameH + job->sheet->frameH + FRAME_EXPAND;
            if (sx0 < 0) sx0 = 0;
            if (sy0 < 0) sy0 = 0;
            if (sx1 > imgW) sx1 = imgW;
            if (sy1 > imgH) sy1 = imgH;

            auto* frame = new Bitmap(static_cast<int>(job->drawW + 0.5f),
                                     static_cast<int>(job->drawH + 0.5f), PixelFormat32bppARGB);
            Graphics fg(frame);
            fg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            fg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            fg.DrawImage(sprite, RectF(0.f, 0.f, job->drawW, job->drawH),
                         RectF(sx0, sy0, sx1 - sx0, sy1 - sy0), UnitPixel);
            job->result.push_back(frame);
        }
        delete sprite;
    } else {
        job->failed = true;
    }
    job->done = true;
    return 0;
}

void StartCacheJob(const AnimSheet* sheet, REAL drawW, REAL drawH) {
    auto* job = new AnimCacheJob;
    job->resId = sheet->resId;
    job->sheet = sheet;
    job->drawW = drawW;
    job->drawH = drawH;

    HANDLE thread = CreateThread(nullptr, 0, BuildCacheThread, job, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        g_cacheJob = job;
    } else {
        delete job;
        BuildFrameCache(drawW, drawH);   // поток не создался — строим синхронно, как раньше
    }
}

void UpdateFrameCache(REAL drawW, REAL drawH) {
    // Принять готовую фоновую работу
    if (g_cacheJob && g_cacheJob->done) {
        AnimCacheJob* job = g_cacheJob;
        g_cacheJob = nullptr;
        if (!job->failed && job->sheet == g_animSheet &&
            job->drawW == drawW && job->drawH == drawH) {
            ClearFrameCache();
            g_frameCache = std::move(job->result);
            g_cacheW = drawW;
            g_cacheH = drawH;
            g_cacheSheet = g_animSheet;
            g_animLoadFailed = false;
        } else {
            for (Bitmap* b : job->result) delete b;   // неактуально — выбросить
            if (job->failed) g_animLoadFailed = true; // чтобы не плодить повторные попытки
        }
        delete job;
    }

    // Пустой кэш после ошибки считается «готовым» — повторно лист не дёргаем
    const bool fresh = g_cacheSheet == g_animSheet && g_cacheW == drawW && g_cacheH == drawH &&
                       (!g_frameCache.empty() || g_animLoadFailed);
    if (!fresh && !g_cacheJob)
        StartCacheJob(g_animSheet, drawW, drawH);
}

void RebuildAnimBg(int w, int h) {
    if (g_animBg && g_animBgW == w && g_animBgH == h) return;
    delete g_animBg;
    g_animBg = new Bitmap(std::max(1, w), std::max(1, h), PixelFormat32bppARGB);
    Graphics bg(g_animBg);
    RECT rc{ 0, 0, w, h };
    DrawBackground(bg, rc);
    g_animBgW = w;
    g_animBgH = h;
}

void LoadAnimSheet(const AnimSheet* sheet) {
    // Старый кэш не трогаем — пока строится новый, он служит «застывшим кадром».
    // Спрайт здесь НЕ грузим: декодирование тяжёлого PNG в главном потоке
    // и даёт рывки на смене листа. Картинку читает только поток кэша.
    g_animSheet = sheet;
    g_animFrame = 0;
    g_animLoadFailed = false;
}

void OpenAnimWindow();

// Звук печати: короткий Beep напрямую на звуковую карту.
// Не требует файлов, winmm, прав — работает везде.
void PlayTypeSound() {
    if (!g_opts[OPT_SOUND]) return;            // звук печати выключен
    // clicks-per2.wav лежит в ресурсах exe как RCDATA id 100
    HRSRC res = FindResourceW(g_instance, MAKEINTRESOURCEW(100), RT_RCDATA);
    if (res) {
        HGLOBAL mem = LoadResource(g_instance, res);
        if (mem) {
            PlaySoundW(static_cast<LPCWSTR>(LockResource(mem)), nullptr,
                       SND_ASYNC | SND_MEMORY | SND_NODEFAULT);
            return;
        }
    }
    // Запасной: короткий щелчок
    Beep(1000, 12);
}

// Усиление глитча для особых реплик. Нет в таблице — обычный глитч 1.0.
// Добавляй сюда индексы своих диалогов: чем больше число, тем сильнее и дольше.
float GlitchBoostFor(int index) {
    switch (index) {
    case 30: return 1.2f;    // "."
    case 31: return 1.9f;    // ".."
    case 32: return 2.8f;    // "..."
    default: return 1.0f;
    }
}

// Реплики, где глитчует и фон (негатив, рваные полосы). Нагнетающая атмосфера.
bool IsTraumaDialogue(int index) {
    switch (index) {
    case 30:   // "."
    case 31:   // ".."
    case 32:   // "..."
        return true;
    default:
        return false;
    }
}

void LoadDialogue(int index) {
    if (index < 0 || index >= DIALOGUE_COUNT) return;

    g_dialogueIndex = index;
    g_gamePhase = 0;
    g_buttonCount = 0;
    if (const ChoiceDef* choice = FindChoice(index)) {
        g_buttonCount = choice->count;
        LayoutChoiceButtons(choice->count);
        for (int i = 0; i < choice->count; ++i)
            g_buttons[i].text = choice->labels[i];
    }

    // Подарок на рабочем столе появляется в момент реплики 80
    if (index == 80) CreateDesktopGift();
    // Окно анимации открывается на реплике 101
    if (index == 101) OpenAnimWindow();
    // 108 — темп анимации в 2 раза быстрее
    if (index == 108 && g_hwndAnim && IsWindow(g_hwndAnim)) {
        KillTimer(g_hwndAnim, ANIM_TIMER);
        SetTimer(g_hwndAnim, ANIM_TIMER, ANIM_FRAME_MS / 2, nullptr);
    }
    // 109 — финальный лист suck2.png (22 кадра, проигрывается один раз)
    if (index == 109) {
        LoadAnimSheet(&SHEET2);
        if (g_hwndAnim && IsWindow(g_hwndAnim)) {
            KillTimer(g_hwndAnim, ANIM_TIMER);
            SetTimer(g_hwndAnim, ANIM_TIMER, ANIM_FRAME_MS, nullptr);
        }
    }
    // 111 — сцена закрывается
    if (index == 111 && g_hwndAnim && IsWindow(g_hwndAnim))
        DestroyWindow(g_hwndAnim);

    delete g_character;
    g_character = nullptr;

    g_missingSprite = g_dialogues[index].image;
    g_character = LoadImageFromResource(FindResId(g_dialogues[index].image));
    if (index > g_storyProgress) g_storyProgress = index;   // прогресс для Сюжета
    const int newSpriteId = FindResId(g_dialogues[index].image);
    if (g_opts[OPT_GLITCH] && newSpriteId != g_lastSpriteId) {  // глитч можно выключить
        g_glitchAmp = GlitchBoostFor(index);
        g_glitchT = GLITCH_TICKS;              // длительность ВСЕГДА одинаковая —
        g_lastSpriteId = newSpriteId;          // усилие только в амплитуде
    }
    g_traumaBg = IsTraumaDialogue(index);
    ++g_charLoadId;

    g_text = g_dialogues[index].text;
    g_typedChars = 0;
    g_isTyping = true;
    g_choiceMade = false;
    g_choicesRevealed = false;
    g_choiceText.clear();
    g_hoverButton = -1;
    g_btnAppear = -1;                      // кнопок пока нет — CRT-анимации сброшены
    g_choiceAnim = -1;
    g_chosenBtn = -1;
    g_pendingChoice = -1;
}

void LoadNightAnswer(int choice) {
    static const wchar_t* answers[] = {
        L"Я так и думала... Ночью всё становится тише. Кажется, именно тогда люди бывают самыми собой.",
        L"Правда? Тогда... наверное, мне просто показалось. Хотя я всё равно рада, что сейчас ты здесь.",
        L"Иногда... Это тоже хорошо. Значит, у нас ещё будет время увидеться поздними вечерами."
    };
    LoadDialogue(6);
    g_text = answers[choice];
    g_typedChars = 0;
    g_isTyping = true;
}

// Показывает реплику вне таблицы диалогов (события из окна игры).
// nextIdx: -1 — клик ничего не делает, -2 — клик покажет финальную реплику 3-й победы, >=0 — переход к диалогу
void ShowInterlude(HWND hwnd, const wchar_t* image, const wchar_t* text, int nextIdx) {
    delete g_character;
    g_missingSprite = image;
    g_character = LoadImageFromResource(FindResId(image));
    g_glitchT = GLITCH_TICKS;                  // интерлюдия тоже со сменой картинки
    g_glitchAmp = 1.f;
    g_traumaBg = false;
    ++g_charLoadId;

    g_text = text;
    g_typedChars = 0;
    g_isTyping = true;
    g_choiceMade = false;
    g_buttonCount = 0;
    g_hoverButton = -1;
    g_interludeNext = nextIdx;
    g_gamePhase = 2;

    SetTimer(hwnd, TYPE_TIMER, TYPE_INTERVAL_MS, nullptr);
}

void FinishTyping() {
    g_typedChars = g_text.size();
    g_isTyping = false;
}

// Пропуск печати кликом: текст появляется не мгновенно, а вейв-вайпом
// слева направо с розовым мерцанием
void FinishTypingSkip() {
    g_typedChars = g_text.size();
    g_isTyping = false;
    g_textWave = 0;
}

void AdvanceDialogue(HWND hwnd) {
    if (g_isTyping) {
        FinishTypingSkip();                    // пропуск — с вейвом и мерцанием
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    int next = g_dialogueIndex + 1;
    // Все варианты ответа на вопрос о том, можно ли остаться, сходятся здесь.
    if (g_dialogueIndex >= 9 && g_dialogueIndex <= 11) next = 12;
    // После реплики «Хорошо... тогда смотри» применяем обои и показываем результат.
    if (g_dialogueIndex == 22) {
        ApplyMafanyaWallpaper();
        next = 23;
    }
    // Три реакции на новые обои ведут к одной реплике.
    if (g_dialogueIndex >= 24 && g_dialogueIndex <= 26) next = 27;
    if (g_dialogueIndex == 27) next = 30;
    // После отказа сначала отдельная реплика, затем общая история.
    if (g_dialogueIndex == 28) next = 29;
    if (g_dialogueIndex == 29) next = 30;
    // Ветви трёх вопросов сходятся в основном сюжете.
    if (g_dialogueIndex == 37) next = 38;
    if (g_dialogueIndex == 38) next = 43;
    if (g_dialogueIndex == 39) next = 40;
    if (g_dialogueIndex == 40) next = 43;
    if (g_dialogueIndex == 41) next = 42;
    if (g_dialogueIndex == 42) next = 43;
    // После любой реакции на ночные занятия продолжается одна общая сцена.
    if (g_dialogueIndex == 47) next = 48;
    // Три ветки реакции на комплимент/странность/взгляд сходятся в общий сюжет.
    if (g_dialogueIndex == 58 || g_dialogueIndex == 63 || g_dialogueIndex == 68)
        next = 69;
    // «Знаешь...» после «Может позже...» / «Я не уверен» возвращает в личную ветку.
    if (g_dialogueIndex == 92) next = 84;
    // После реплики 101 сцена продолжается описанием (102 — параллельная ветка «Может не стоит»).
    if (g_dialogueIndex == 101) next = 103;

    if (next < DIALOGUE_COUNT) {
        LoadDialogue(next);
        SetTimer(hwnd, TYPE_TIMER, TYPE_INTERVAL_MS, nullptr);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

// Белое кольцо вокруг спрайта: строится ОДИН РАЗ на реплику.
// Дилатация альфы в 8 направлениях, затем дырка по непрозрачным пикселям
// спрайта — на экране остаётся только честный контур, который можно рисовать
// поверх модельки, не заливая её.
void BuildOutlineCache() {
    delete g_charCacheOutline;
    g_charCacheOutline = nullptr;
    if (!g_charCache) return;

    const int W = static_cast<int>(g_charCacheW);
    const int H = static_cast<int>(g_charCacheH);
    if (W <= 0 || H <= 0) return;

    // 1) белый силуэт, раздутый на OUTLINE_PX в 8 направлениях (включая центр)
    Bitmap* dil = new Bitmap(W, H, PixelFormat32bppARGB);
    Graphics dg(dil);
    dg.SetInterpolationMode(InterpolationModeNearestNeighbor);
    ColorMatrix cm = {};
    cm.m[3][0] = 1.f;  // альфа -> R (СТРОКА 3 = вход альфы!)
    cm.m[3][1] = 1.f;  // альфа -> G
    cm.m[3][2] = 1.f;  // альфа -> B
    cm.m[3][3] = 1.f;  // альфа -> A
    ImageAttributes wa;
    wa.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
    for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox)
            dg.DrawImage(g_charCache,
                         RectF(static_cast<REAL>(ox) * OUTLINE_PX,
                               static_cast<REAL>(oy) * OUTLINE_PX,
                               static_cast<REAL>(W), static_cast<REAL>(H)),
                         0.f, 0.f, static_cast<REAL>(W), static_cast<REAL>(H),
                         UnitPixel, &wa, nullptr, nullptr);

    // 2) дырка: где спрайт непрозрачен (A >= 128) — делаем прозрачным.
    // Порог даёт чёткий ретро-край по пиксель-арту.
    Rect rc(0, 0, W, H);
    BitmapData bdD, bdC;
    Bitmap* cacheBmp = static_cast<Bitmap*>(g_charCache);   // в кэше всегда Bitmap
    if (dil->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &bdD) == Ok &&
        cacheBmp->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bdC) == Ok) {
        for (int yy = 0; yy < H; ++yy) {
            BYTE* rowD = reinterpret_cast<BYTE*>(bdD.Scan0) + yy * bdD.Stride;
            BYTE* rowC = reinterpret_cast<BYTE*>(bdC.Scan0) + yy * bdC.Stride;
            for (int xx = 0; xx < W; ++xx) {
                BYTE* pD = rowD + xx * 4;
                const BYTE aC = rowC[xx * 4 + 3];
                if (aC >= 144) {
                    pD[3] = 0;                         // внутри тела — прозрачно
                } else {
                    unsigned ring = pD[3] * (255u - aC) / 255u;  // чем пустее, тем слабее
                    ring = ring * 170u / 255u;                     // общая полупрозрачность
                    if (ring < 20) ring = 0;                       // гашение шума
                    pD[0] = pD[1] = pD[2] = 255;                   // кольцо белое
                    pD[3] = static_cast<BYTE>(ring);
                }
            }
        }
        dil->UnlockBits(&bdD);
        cacheBmp->UnlockBits(&bdC);
    }
    g_charCacheOutline = dil;
}

void DrawCharacter(Graphics& graphics, const RECT& client) {
    if (!g_character || g_character->GetLastStatus() != Ok) {
        // Спрайт не найден — пишем, какой файл искали, вместо пустого места
        Font* font = CreateFont(10.f);
        SolidBrush brush(Color(255, 205, 130, 150));
        StringFormat format;
        format.SetAlignment(StringAlignmentNear);
        format.SetLineAlignment(StringAlignmentNear);
        RectF area(static_cast<REAL>(BORDER + 10), 60.f, 300.f, 70.f);
        graphics.DrawString((L"Нет спрайта:\n" + g_missingSprite).c_str(), -1,
                            font, area, &format, &brush);
        delete font;
        return;
    }

    const float maxW = 305.f;
    const float maxH = static_cast<float>(client.bottom) - BORDER * 2 - 4.f;
    const float imageW = static_cast<float>(g_character->GetWidth());
    const float imageH = static_cast<float>(g_character->GetHeight());
    if (imageW <= 0.f || imageH <= 0.f) return;

    float scale = std::min(maxW / imageW, maxH / imageH) * 1.08f;
    float drawW = imageW * scale;
    float drawH = imageH * scale;
    const float x = BORDER + 3.f;
    float y = static_cast<float>(client.bottom) - BORDER - 2.f - drawH;

    if (y < BORDER + 2.f) {
        y = BORDER + 2.f;
        drawH = static_cast<float>(client.bottom) - BORDER - 2.f - y;
        drawW = drawH * (imageW / imageH);
    }
    // Спрайт растягивается в кэш один раз на реплику — дальше только дешёвый блит.
    // Прежний кэш не удаляем, а отдаём в g_charCacheOld — он мелькает в глитче.
    if (g_charCacheId != g_charLoadId || g_charCacheW != drawW || g_charCacheH != drawH) {
        delete g_charCacheOld;
        g_charCacheOld = static_cast<Bitmap*>(g_charCache);
        g_oldCacheW = g_charCacheW;
        g_oldCacheH = g_charCacheH;

        g_charCache = new Bitmap(std::max(1, static_cast<int>(drawW + 0.5f)),
                                 std::max(1, static_cast<int>(drawH + 0.5f)), PixelFormat32bppARGB);
        Graphics cg(g_charCache);
        cg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        cg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        cg.DrawImage(g_character, RectF(0.f, 0.f, drawW, drawH),
                     0.f, 0.f, imageW, imageH, UnitPixel);
        g_charCacheId = g_charLoadId;
        g_charCacheW = drawW;
        g_charCacheH = drawH;
    }
    // Кольцо строим один раз на новый кэш
    if (g_outlineCacheId != g_charCacheId) {
        BuildOutlineCache();
        g_outlineCacheId = g_charCacheId;
    }

    // === Фейк-движение: вейв + squash-and-stretch + ретро-эхо, контур белый ПОВЕРХ ===
    const float t = g_charPhase;
    // редкое "дыхание" (период ~6 с) — можно выключить в Настройках
    const float s = g_opts[OPT_BREATH] ? sinf(t * 0.05f) : 0.f;
    const float scaleY = 1.f + 0.050f * s;           // сплющивание/вытягивание по Y
    const float scaleX = 1.f - 0.035f * s;           // в противофазе — объём сохраняется
    const float outW = drawW * scaleX;
    const float outH = drawH * scaleY;
    const float footY = y + drawH;                   // "пол" — нижняя кромка спрайта
    float px = x - (outW - drawW) * 0.5f;            // центрируем относительно оригинала
    // лёгкое покачивание — отдельной опцией (Плавание вверх-вниз)
    float py = footY - outH + (g_opts[OPT_FLOAT] ? 1.2f * sinf(t * 0.12f) : 0.f);

    // Ретро-след за курсором: ГОРИЗОНТАЛЬНЫЙ сдвиг, квантованный по сетке 2 px —
    // персонаж дёргается ступеньками, как в старых играх.
    // Вертикаль спрайта не сдвигаем: ступни стоят на своём месте, иначе между
    // ногами и розовым бортиком появляется щель. Вертикаль курсора уходит в эхо.
    px += floorf(g_lookX * 0.5f + 0.5f) * 2.f;

    // Вейв: горизонтальные срезы, каждый чуть сдвинут по синусу.
    // Нужны в обоих режимах — и обычном, и глитче.
    graphics.SetInterpolationMode(InterpolationModeNearestNeighbor);
    const REAL srcSlice = 8.f;                       // высота среза, px исходника
    const int slices = static_cast<int>(ceilf(g_charCacheH / srcSlice));
    std::vector<REAL> dx(slices);
    for (int i = 0; i < slices; ++i)
        dx[i] = g_opts[OPT_WAVE] ? 1.0f * sinf(t * 0.45f + i * 0.55f) : 0.f;  // вейв можно выключить

    // === РЕТРО-ГЛИТЧ при смене спрайта (~0.4 с рваной россыпи) ===
    if (g_glitchT > 0) {
        const float k = g_glitchT / static_cast<float>(GLITCH_TICKS); // затухание
        int jx = static_cast<int>(12.f * k * g_glitchAmp) + 1;        // макс. сдвиг, px
        if (jx > 20) jx = 20;                                         // потолок, иначе на пол-экрана

        // Призраки нового спрайта в палитре приложения: рамочно-розовый вправо,
        // светло-лиловый влево — как подсвеченные края градиента
        static ImageAttributes ghostR, ghostC;
        static bool ghostInit = false;
        if (!ghostInit) {
            ColorMatrix mr = {}, mc = {};
            mr.m[3][0] = 1.00f; mr.m[3][1] = 105.f/255.f; mr.m[3][2] = 175.f/255.f;
            mr.m[3][3] = 0.55f;   // розовый рамки (255, 105, 175)
            mc.m[3][0] = 0.82f; mc.m[3][1] = 0.62f;     mc.m[3][2] = 1.00f;
            mc.m[3][3] = 0.55f;   // светло-лиловый
            ghostR.SetColorMatrix(&mr, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
            ghostC.SetColorMatrix(&mc, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
            ghostInit = true;
        }
        graphics.DrawImage(g_charCache, RectF(px - jx, py, outW, outH),
                           0.f, 0.f, g_charCacheW, g_charCacheH,
                           UnitPixel, &ghostC, nullptr, nullptr);
        graphics.DrawImage(g_charCache, RectF(px + jx, py, outW, outH),
                           0.f, 0.f, g_charCacheW, g_charCacheH,
                           UnitPixel, &ghostR, nullptr, nullptr);

        // Старый спрайт мелькает между кадрами
        if (g_charCacheOld && (g_glitchT % 3) != 0)
            graphics.DrawImage(g_charCacheOld,
                               RectF(px + (rand() % (2 * jx)) - jx, py, outW, outH),
                               0.f, 0.f, g_oldCacheW, g_oldCacheH,
                               UnitPixel, nullptr, nullptr);

        // Новый — рваными срезами со случайными дёрганьями, часть полос вырывается
        if ((g_glitchT % 4) != 0) {
            for (int i = 0; i < slices; ++i) {
                if (rand() % 3 == 0) continue;       // вырванные полосы
                const REAL srcY = i * srcSlice;
                const REAL srcH = std::min(srcSlice, g_charCacheH - srcY);
                const REAL dstY = py + (srcY / g_charCacheH) * outH;
                const REAL dstH = (srcH / g_charCacheH) * outH;
                const REAL gx = static_cast<REAL>((rand() % (2 * jx + 1)) - jx);
                graphics.DrawImage(g_charCache,
                                   RectF(px + dx[i] * 0.5f + gx, dstY, outW, dstH + 1.f),
                                   0.f, srcY, g_charCacheW, srcH,
                                   UnitPixel, nullptr, nullptr);
            }
        }

        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        return;   // эхо и кольцо во время глитча не рисуем — чище эффект
    }

    // Розовое эхо: цвет = альфа × BORDER_COLOR (255, 105, 175), два слоя с затуханием
    ColorMatrix em1 = {}, em2 = {};
    em1.m[3][0] = 0.50f;                 em2.m[3][0] = 0.18f;
    em1.m[3][1] = 0.50f * 105.f/255.f;   em2.m[3][1] = 0.18f * 105.f/255.f;
    em1.m[3][2] = 0.50f * 175.f/255.f;   em2.m[3][2] = 0.18f * 175.f/255.f;
    em1.m[3][3] = 0.50f;                 em2.m[3][3] = 0.18f;
    ImageAttributes echoAttr1, echoAttr2;
    echoAttr1.SetColorMatrix(&em1, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
    echoAttr2.SetColorMatrix(&em2, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);

    // 0) РЕТРО-ЭХО позади. Вертикаль — как раньше, от дыхания: вытянут (s > 0) —
    //    тянемся ОТ ГОЛОВЫ ВНИЗ за пределы ног, сплющен — ОТ НОГ ВВЕРХ.
    //    Горизонталь — зеркало курсора: курсор влево — эхо вправо. Сдвиг
    //    квантован теми же ступеньками 2 px, что и спрайт, и движутся они вместе.
    // Вертикаль — ТОЛЬКО дыхание: тяга пульсирует синусом, как изначально.
    // Горизонталь — зеркало курсора: курсор влево — эхо вправо. Ступеньки по 3 px,
    // сильнее, чем ход спрайта — эхо заметно убегает в сторону.
    const float grow = fabsf(s);
    const float echoShift = -floorf(g_lookX * 0.5f + 0.5f) * 5.f;   // зеркало, ±45 px
    if (g_opts[OPT_ECHO] && (grow > 0.03f || fabsf(echoShift) > 0.5f)) {
        for (int layer = 0; layer < 2; ++layer) {
            const float mul = layer == 0 ? 1.0f : 1.6f;
            const float ext = outH * grow * 0.045f * mul;  // дыхательная тяга
            const REAL eH = outH + ext;
            REAL eY;
            if (s > 0.f) eY = py;            // верх эха = голова, тянем вниз
            else         eY = footY - eH;    // низ эха = ступни, тянем вверх
            const REAL eX = px + outW * 0.02f * mul + echoShift * mul;
            const REAL eW = outW * (1.f - 0.04f * mul);    // чуть уже тела
            graphics.DrawImage(g_charCache,
                               RectF(eX, eY, eW, eH),
                               0.f, 0.f, g_charCacheW, g_charCacheH,
                               UnitPixel, layer == 0 ? &echoAttr1 : &echoAttr2,
                               nullptr, nullptr);
        }
    }

    // 1) Сам спрайт
    for (int i = 0; i < slices; ++i) {
        const REAL srcY = i * srcSlice;
        const REAL srcH = std::min(srcSlice, g_charCacheH - srcY);
        const REAL dstY = py + (srcY / g_charCacheH) * outH;
        const REAL dstH = (srcH / g_charCacheH) * outH;
        graphics.DrawImage(g_charCache,
                           RectF(px + dx[i], dstY, outW, dstH + 1.f),
                           0.f, srcY, g_charCacheW, srcH,
                           UnitPixel, nullptr, nullptr);
    }
    // 2) Белый контур ПОВЕРХ: готовое кольцо с теми же срезами и сдвигами.
    // Внутри кольца прозрачно — модель не заливается.
    if (g_opts[OPT_OUTLINE] && g_charCacheOutline) {
        for (int i = 0; i < slices; ++i) {
            const REAL srcY = i * srcSlice;
            const REAL srcH = std::min(srcSlice, g_charCacheH - srcY);
            const REAL dstY = py + (srcY / g_charCacheH) * outH;
            const REAL dstH = (srcH / g_charCacheH) * outH;
            graphics.DrawImage(g_charCacheOutline,
                               RectF(px + dx[i], dstY, outW, dstH + 1.f),
                               0.f, srcY, g_charCacheW, srcH,
                               UnitPixel, nullptr, nullptr);
        }
    }
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
}

void DrawChoices(Graphics& graphics) {
    // Кнопки рисуем в двух случаях: обычный показ, либо идёт анимация выбора
    const bool showButtons = g_dialogues[g_dialogueIndex].hasChoices && !g_isTyping &&
                             g_choicesRevealed && (g_choiceAnim >= 0 || !g_choiceMade);
    if (showButtons) {
        Font* font = CreateFont(11.5f);
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        format.SetLineAlignment(StringAlignmentCenter);

        for (int i = 0; i < g_buttonCount; ++i) {
            RectF r = g_buttons[i].rect;
            bool visible = true;
            bool highlight = false;

            if (g_choiceAnim >= 0) {
                // === АНИМАЦИЯ ВЫБОРА ===
                const float k = 1.f - g_choiceAnim / static_cast<float>(CHOICE_ANIM_TICKS);
                const bool chosen = (i == g_chosenBtn);
                if (chosen && k < 0.5f) {
                    // первая половина — ретро-подсветка выбранной кнопки
                    highlight = true;
                    r.Inflate(1.5f + static_cast<REAL>(g_choiceAnim % 2), 1.0f);
                }
                // CRT-выключение: высота квантованно схлопывается к линии
                float offK;
                if (k > 0.8f)       offK = 1.f;
                else if (k > 0.55f) offK = 0.66f;
                else if (k > 0.3f)  offK = 0.33f;
                else                offK = 0.f;
                if (chosen && k < 0.5f) offK = 1.f;    // подсвеченная пока жива
                if (offK <= 0.f) visible = false;
                else if (offK < 1.f) {
                    if ((g_choiceAnim + i) % 2 == 0) { // мерцание при выключении
                        r.Y += (r.Height - r.Height * offK) * 0.5f;
                        r.Height *= offK;
                        r.X += static_cast<REAL>((rand() % 5) - 2);  // глитч-дрожь
                    } else visible = false;
                }
            } else if (g_btnAppear >= 0) {
                // === CRT-ВКЛЮЧЕНИЕ: кнопка растёт от горизонтальной линии ===
                const float raw = (g_btnAppear - i * 2) / 4.f;   // стаггер по кнопкам
                float openK;
                if (raw <= 0.f)       openK = 0.f;
                else if (raw >= 1.f)  openK = 1.f;
                else if (raw < 0.34f) openK = 0.3f;
                else if (raw < 0.67f) openK = 0.65f;
                else                  openK = 1.f;
                if (openK <= 0.f) visible = false;
                else if (openK < 1.f) {
                    if ((g_btnAppear + i) % 3 == 1) visible = false;  // мерцание
                    else {
                        r.Y += (r.Height - r.Height * openK) * 0.5f;
                        r.Height *= openK;
                    }
                }
            }

            if (!visible) continue;
            const bool hover = (g_hoverButton == i);
            const Color bg = highlight ? Color(255, 255, 130, 185)
                           : (hover ? BTN_HOVER : BTN_BG);
            DrawButton(graphics, r, bg);
            SolidBrush brush(highlight ? Color(255, 255, 255, 255) : BTN_TEXT);
            graphics.DrawString(g_buttons[i].text, -1, font, r, &format, &brush);
        }
        delete font;
    } else if (g_choiceMade && g_choiceAnim < 0) {
        Font* font = CreateFont(10.5f);
        SolidBrush brush(TEXT_COLOR);
        StringFormat format;
        format.SetAlignment(StringAlignmentNear);
        format.SetLineAlignment(StringAlignmentNear);
        RectF rect(340.f, 195.f, 200.f, 60.f);
        graphics.DrawString(g_choiceText.c_str(), -1, font, rect, &format, &brush);
        delete font;
    }
}

// ===================== Бинарный слой фона =====================
// Меняющиеся hex-значения поверх градиента — "суперкомпьютер". Значения
// мутируют рывками раз в 2 тика. Параллакс: чем дальше слой, тем сильнее
// ходит против курсора: персонаж мало, эхо средне, этот слой — максимально.
std::vector<std::wstring> g_binRows;        // кэш строк слоя (общий для всех окон)

// parX/parY — параллакс-смещение снаружи (главное окно: от g_lookX, игра: от наклона поля)
void DrawBinaryLayer(Graphics& graphics, const RECT& client, REAL parX, REAL parY) {
    if (g_binFade <= 0.02f) return;                  // плавно угас — не рисуем вообще
    const int margin = 130;                          // запас под параллакс-сдвиг
    const int cellW = 16, cellH = 13;
    const int cols = (client.right  + margin * 2) / cellW + 1;
    const int rows = (client.bottom + margin * 2) / cellH + 1;

    // Мутация на СОБСТВЕННОМ счётчике вызовов: каждое окно анимирует слой
    // само, независимо от таймеров остальных окон. Рывком — раз в 2 вызова.
    static unsigned drawTick = 0;
    ++drawTick;
    if ((drawTick % 2) == 0 || static_cast<int>(g_binRows.size()) != rows) {
        g_binRows.assign(rows, std::wstring());
        // ТОЛЬКО hex-символы, без пробелов: иначе строки плавают по ширине
        // (шрифт не моноширинный) и между тайлами открываются щели при сдвиге.
        static const wchar_t hex[] = L"0123456789ABCDEF";
        for (int r = 0; r < rows; ++r) {
            std::wstring& line = g_binRows[r];
            line.reserve(static_cast<size_t>(cols) * 2);
            for (int c = 0; c < cols; ++c) {
                line += hex[rand() % 16];
                line += hex[rand() % 16];
            }
        }
    }

    const REAL ox = parX;
    const REAL oy = parY;

    Font* font = CreateFont(9.f);
    StringFormat leftFmt;
    leftFmt.SetAlignment(StringAlignmentNear);
    leftFmt.SetLineAlignment(StringAlignmentNear);
    const BYTE fadeA = static_cast<BYTE>(g_binFade * 255.f);
    SolidBrush dim(Color(static_cast<BYTE>(36.f / 255.f * fadeA), 210, 130, 190));
    SolidBrush bright(Color(static_cast<BYTE>(85.f / 255.f * fadeA), 255, 170, 225));

    // Честный замер ширины РЕАЛЬНОЙ строки (не пробника!) — иначе шаг тайлов
    // расходится с фактической шириной и между тайлами открываются полосы.
    static REAL rowW = 0.f;
    static int rowWCols = 0;
    if (rowWCols != cols && !g_binRows.empty()) {
        RectF box;
        graphics.MeasureString(g_binRows[0].c_str(), -1, font,
                               RectF(0.f, 0.f, 100000.f, 100.f), &leftFmt, &box);
        rowW = box.Width + 2.f;
        rowWCols = cols;
    }

    const REAL rightEdge = static_cast<REAL>(client.right) + margin;
    for (int r = 0; r < rows; ++r) {
        const REAL y = -margin + oy + r * cellH;
        // тайлинг строки от левого края с запасом до правого
        for (REAL tx = -margin + ox; tx < rightEdge; tx += rowW - 6.f) {  // -6: нахлёст тайлов
            graphics.DrawString(g_binRows[r].c_str(), -1, font,
                                RectF(tx, y, rowW + 8.f, cellH + 2),
                                &leftFmt, &dim);
            // пара ярких символов-вспышек на тайл
            for (int b = 0; b < 2; ++b) {
                const int pos = rand() % (cols * 2);
                graphics.DrawString(g_binRows[r].c_str() + pos, 1, font,
                                    RectF(tx + pos * (rowW / (cols * 2)), y,
                                          (rowW / (cols * 2)) + 2.f, cellH + 2),
                                    &leftFmt, &bright);
            }
        }
    }
    delete font;
}

void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);

    Bitmap buffer(client.right, client.bottom, PixelFormat32bppARGB);
    Graphics graphics(&buffer);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);

    // Травматичные реплики глитчуют фоном в негатив, остальные — обычный градиент
    if (g_traumaBg && g_glitchT > 0)
        DrawTraumaBackground(graphics, client);
    else
        DrawBackground(graphics, client);

    // код под спрайтом, параллакс сильнее эха, только горизонтальный
    DrawBinaryLayer(graphics, client, -floorf(g_lookX * 2.5f) * 4.f, 0.f);

    DrawCharacter(graphics, client);

    Font* font = CreateFont(12.f);
    SolidBrush textBrush(TEXT_COLOR);
    StringFormat format;
    format.SetAlignment(StringAlignmentNear);
    format.SetLineAlignment(StringAlignmentNear);
    format.SetTrimming(StringTrimmingWord);
    format.SetFormatFlags(0);

    RectF textRect(340.f, 38.f, 200.f, 135.f);
    if (g_textWave >= 0) {
        // Пропуск печати: вейв-вайп слева направо + розовое мерцание и дрожь
        const REAL wipeW = textRect.Width *
            (g_textWave / static_cast<REAL>(TEXT_WAVE_TICKS));
        graphics.SetClip(RectF(textRect.X - 2.f, textRect.Y - 4.f,
                               wipeW + 2.f, textRect.Height + 8.f));
        SolidBrush waveBrush((g_textWave % 2 == 0)
                             ? Color(255, 255, 150, 200)   // розовая вспышка
                             : TEXT_COLOR);
        RectF jit = textRect;
        jit.Y += static_cast<REAL>((g_textWave % 3) - 1);  // микродрожь ±1 px
        graphics.DrawString(g_text.c_str(), -1, font, jit, &format, &waveBrush);
        graphics.ResetClip();
    } else {
        const std::wstring visible = g_text.substr(0, g_typedChars);
        graphics.DrawString(visible.c_str(), -1, font, textRect, &format, &textBrush);
    }
    delete font;

    DrawChoices(graphics);

    // Кнопка меню: три полоски, векторная, в правом нижнем углу
    DrawButton(graphics, g_menuBtnRect, g_hoverMenu ? BTN_HOVER : BTN_BG);
    {
        Pen mp(BORDER_COLOR, 2.f);
        for (int i = 0; i < 3; ++i) {
            const REAL ly = g_menuBtnRect.Y + 7.f + i * 6.f;
            graphics.DrawLine(&mp, g_menuBtnRect.X + 12.f, ly,
                              g_menuBtnRect.X + 28.f, ly);
        }
    }

    // Розовая рамка ПОВЕРХ всего, как в окне сцены
    Pen borderOnTop(BORDER_COLOR, static_cast<REAL>(BORDER));
    graphics.DrawRectangle(&borderOnTop, BORDER / 2.f, BORDER / 2.f,
                           static_cast<REAL>(client.right) - BORDER,
                           static_cast<REAL>(client.bottom) - BORDER);

    Graphics screen(hdc);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(hwnd, &ps);
}

// ===================== Крестики-нолики =====================

// Параллакс-наклон поля (фейковый 3D за курсором) и эффекты ходов
constexpr UINT_PTR GAME_ANIM_TIMER = 5;
constexpr UINT GAME_ANIM_MS = 45;
constexpr REAL TILT_PX = 9.f;            // сдвиг крайнего ряда/колонки при полном наклоне
float g_tiltX = 0.f, g_tiltY = 0.f;      // сглаженный наклон (цель — курсор)
int g_cellAnim[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };  // тики с постановки, -1 = нет
int g_winLine[3] = { -1, -1, -1 };       // выигрышная линия для вспышки
int g_flashTick = 0;                     // общий счётчик миганий
bool g_matchOver = false;                // матч (3 раунда) окончен без 3 побед

int CheckWin(const int board[9]) {
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},
        {0,3,6},{1,4,7},{2,5,8},
        {0,4,8},{2,4,6}
    };
    for (auto& line : lines) {
        const int v = board[line[0]];
        if (v != 0 && v == board[line[1]] && v == board[line[2]])
            return v;
    }
    return 0;
}

// Клетки выигрышной линии игрока p (или -1, если линии нет)
void FindWinLine(int p, int out[3]) {
    out[0] = out[1] = out[2] = -1;
    if (p == 0) return;
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},
        {0,3,6},{1,4,7},{2,5,8},
        {0,4,8},{2,4,6}
    };
    for (auto& ln : lines) {
        if (g_board[ln[0]] == p && g_board[ln[1]] == p && g_board[ln[2]] == p) {
            out[0] = ln[0]; out[1] = ln[1]; out[2] = ln[2];
            return;
        }
    }
}

bool BoardFull(const int board[9]) {
    for (int i = 0; i < 9; ++i)
        if (board[i] == 0) return false;
    return true;
}

// Возвращает клетку, которую игрок p может добить до победы, или -1
int FindLineFor(int p) {
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},
        {0,3,6},{1,4,7},{2,5,8},
        {0,4,8},{2,4,6}
    };
    for (auto& line : lines) {
        int count = 0, empty = -1;
        for (int j = 0; j < 3; ++j) {
            if (g_board[line[j]] == p) ++count;
            else if (g_board[line[j]] == 0) empty = line[j];
        }
        if (count == 2 && empty >= 0) return empty;
    }
    return -1;
}

int AiChooseMove() {
    int take = FindLineFor(2);   // выиграть, если можно
    if (take >= 0) return take;
    int block = FindLineFor(1);  // помешать игроку
    if (block >= 0) return block;

    std::vector<int> empty, corners;
    for (int i = 0; i < 9; ++i) {
        if (g_board[i] != 0) continue;
        empty.push_back(i);
        if (i == 0 || i == 2 || i == 6 || i == 8) corners.push_back(i);
    }
    if (empty.empty()) return -1;

    if (g_board[4] == 0 && (rand() % 100) < 55) return 4;   // центр
    if (!corners.empty()) return corners[rand() % corners.size()];
    return empty[rand() % empty.size()];
}

// Квантованный наклон — ОДИН для отрисовки и хит-теста, чтобы клики попадали в клетки
void TiltQuantized(float& qx, float& qy) {
    qx = floorf(g_tiltX * 2.f + 0.5f) / 2.f;   // ступеньки 0.5, ретро
    qy = floorf(g_tiltY * 2.f + 0.5f) / 2.f;
}

// Прямоугольник клетки i с учётом наклона
RectF TiltedCell(int i, float qx, float qy) {
    const int row = i / 3, col = i % 3;
    const REAL ox = static_cast<REAL>(col - 1) * qx * TILT_PX;
    const REAL oy = static_cast<REAL>(row - 1) * qy * TILT_PX;
    return RectF(GX + col * (CELL + GAP) + ox,
                 GY + row * (CELL + GAP) + oy, CELL, CELL);
}

// Клетка под точкой с учётом наклона
int GameCellAt(int x, int y) {
    float qx, qy;
    TiltQuantized(qx, qy);
    for (int i = 0; i < 9; ++i)
        if (TiltedCell(i, qx, qy).Contains(static_cast<REAL>(x), static_cast<REAL>(y)))
            return i;
    return -1;
}

// Поп-ин символа: ступенчатое вырастание из центра клетки
float PopScale(int i) {
    const int a = g_cellAnim[i];
    if (a < 0 || a >= 4) return 1.f;
    if (a < 2) return 0.45f;
    return 0.75f;
}

void ResetRound(HWND hwnd) {
    for (int i = 0; i < 9; ++i) { g_board[i] = 0; g_cellAnim[i] = -1; }
    g_winLine[0] = g_winLine[1] = g_winLine[2] = -1;
    g_playerTurn = true;
    g_roundOver = false;
    g_hoverCell = -1;
    g_statusText = L"Твой ход";
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Полный сброс матча: счётчики к нулю (клик после "Матч окончен")
void ResetMatch(HWND hwnd) {
    g_roundsPlayed = 0;
    g_playerWins = 0;
    g_mafanyaWins = 0;
    g_matchOver = false;
    ResetRound(hwnd);
    g_statusText = L"Новый матч! Твой ход";
}

void EndRound(HWND hwnd, int winner) {
    ++g_roundsPlayed;
    g_roundOver = true;
    FindWinLine(winner, g_winLine);

    if (winner == 1) {
        ++g_playerWins;
        InvalidateRect(hwnd, nullptr, FALSE);
        PostMessage(g_hwndMain, WM_MAFANYA_GAME, EV_PLAYER_WIN, g_playerWins);
        if (g_playerWins >= 3) {
            // Матч выигран — окно закрывается само
            g_gameSelfClosed = true;
            DestroyWindow(hwnd);
            return;
        }
        g_statusText = L"Твоя победа! Жми для следующего раунда";
        return;
    }

    if (winner == 2) {
        ++g_mafanyaWins;
        g_statusText = L"Mafanya выиграла раунд! Попробовать ещё раз";
        PostMessage(g_hwndMain, WM_MAFANYA_GAME, EV_MAFANYA_WIN, 0);
    } else {
        g_statusText = L"Ничья! Попробовать ещё раз";
        PostMessage(g_hwndMain, WM_MAFANYA_GAME, EV_DRAW, 0);
    }

    // Матч = 3 раунда. Набрал 3 победы — выше, самозакрытие.
    // Иначе после третьего раунда — матч проигран, счётчики сбрасываются кликом.
    if (g_roundsPlayed >= 3) {
        g_matchOver = true;
        g_statusText = L"Матч проигран! Нажми, чтобы начать заново";
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void MakeAiMove(HWND hwnd) {
    const int move = AiChooseMove();
    if (move < 0) return;
    g_board[move] = 2;
    g_cellAnim[move] = 0;                    // поп-ин нолика

    if (CheckWin(g_board) == 2) { EndRound(hwnd, 2); return; }
    if (BoardFull(g_board))   { EndRound(hwnd, 0); return; }

    g_playerTurn = true;
    g_statusText = L"Твой ход";
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Пиксельный крестик: диагональная лесенка из квадратов на сетке grid
void DrawPixelX(Graphics& g, const RectF& cell, const Color& col, REAL pop, REAL jx) {
    const REAL inset = 18.f + pop;
    const REAL grid = 5.f;
    SolidBrush b(col);
    const REAL xa = cell.X + inset + jx;
    const REAL ya = cell.Y + inset;
    const REAL xb = cell.X + cell.Width - inset + jx;
    const REAL yb = cell.Y + cell.Height - inset;
    const int n = std::max(1, static_cast<int>((xb - xa) / grid + 0.5f));
    const REAL sx = (xb - xa) / n, sy = (yb - ya) / n;
    for (int i = 0; i <= n; ++i) {
        const REAL py = floorf((ya + i * sy) / grid + 0.5f) * grid;
        const REAL px = floorf((xa + i * sx) / grid + 0.5f) * grid;
        const REAL qx = floorf((xb - i * sx) / grid + 0.5f) * grid;
        g.FillRectangle(&b, px, py, grid + 0.6f, grid + 0.6f);
        g.FillRectangle(&b, qx, py, grid + 0.6f, grid + 0.6f);
    }
}

// Пиксельный нолик: кольцо из квадратов на сетке
void DrawPixelO(Graphics& g, const RectF& cell, const Color& col, REAL pop, REAL jx) {
    const REAL inset = 15.f + pop;
    const REAL grid = 5.f;
    SolidBrush b(col);
    const REAL cx = cell.X + cell.Width * 0.5f + jx;
    const REAL cy = cell.Y + cell.Height * 0.5f;
    const REAL rx = (cell.Width - inset * 2.f) * 0.5f;
    const REAL ry = (cell.Height - inset * 2.f) * 0.5f;
    for (REAL py = cy - ry; py <= cy + ry; py += grid)
        for (REAL px = cx - rx; px <= cx + rx; px += grid) {
            const REAL dx = (px + grid * 0.5f - cx) / rx;
            const REAL dy = (py + grid * 0.5f - cy) / ry;
            const REAL d = dx * dx + dy * dy;
            if (d > 0.55f && d < 1.30f)      // кольцо толщиной примерно в клетку
                g.FillRectangle(&b,
                                floorf(px / grid + 0.5f) * grid,
                                floorf(py / grid + 0.5f) * grid,
                                grid + 0.6f, grid + 0.6f);
        }
}

void PaintGameWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);

    Bitmap buffer(client.right, client.bottom, PixelFormat32bppARGB);
    Graphics graphics(&buffer);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);

    DrawBackground(graphics, client);

    // Фон игры — тот же бинарный слой, параллакс от наклона поля
    float bqx, bqy;
    TiltQuantized(bqx, bqy);
    DrawBinaryLayer(graphics, client, -bqx * 14.f, -bqy * 9.f);

    // Статистика над полем
    const std::wstring stats = L"Раунд " + std::to_wstring(std::min(g_roundsPlayed + (g_roundOver ? 0 : 1), 3)) +
        L"/3 | Ты: " + std::to_wstring(g_playerWins) +
        L" | Mafanya: " + std::to_wstring(g_mafanyaWins);

    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);
    SolidBrush textBrush(TEXT_COLOR);

    Font* statsFont = CreateFont(13.f);
    RectF statsRect(0.f, 20.f, static_cast<REAL>(client.right), 26.f);
    graphics.DrawString(stats.c_str(), -1, statsFont, statsRect, &center, &textBrush);
    delete statsFont;

    // Фейковый 3D: наклон поля за курсором, квантованный ступеньками
    const float qx = bqx, qy = bqy;

    // ЭХО ПЛИТОК: зеркало курсора, как у персонажа — уходит в противоположную
    // сторону наклона, квантованными ступеньками
    const REAL eOffX = -qx * 7.f;
    const REAL eOffY = -qy * 7.f;
    if (eOffX != 0.f || eOffY != 0.f) {
        const Color echoCol(70, 255, 105, 175);
        for (int i = 0; i < 9; ++i) {
            const RectF cell = TiltedCell(i, qx, qy);
            DrawButton(graphics, RectF(cell.X + eOffX, cell.Y + eOffY, cell.Width, cell.Height),
                       echoCol, 9.f);
        }
    }

    // Игровое поле 3x3
    for (int i = 0; i < 9; ++i) {
        const RectF cell = TiltedCell(i, qx, qy);
        const bool inLine = g_roundOver && g_winLine[0] >= 0 &&
                            (i == g_winLine[0] || i == g_winLine[1] || i == g_winLine[2]);
        const bool flashOn = inLine && ((g_flashTick % 4) < 2);      // вспышка линии
        const bool hover = (i == g_hoverCell && g_playerTurn && !g_roundOver && g_board[i] == 0);

        Color bg = flashOn ? Color(255, 255, 130, 185)
                 : (hover ? BTN_HOVER : BTN_BG);
        DrawButton(graphics, cell, bg, 9.f);

        if (g_board[i] == 0) continue;

        // Поп-ин + мерцание свежего символа, дрожь при проигранном матче
        const int a = g_cellAnim[i];
        const REAL pop = (1.f - PopScale(i)) * CELL * 0.28f;
        REAL jx = 0.f;
        if (a >= 0 && a < 2) jx = static_cast<REAL>((g_flashTick % 3) - 1);
        if (g_matchOver) jx = static_cast<REAL>((rand() % 3) - 1);

        // мерцание: свежий символ на чётных тиках розовый, вспышка линии — белый
        const bool flicker = (a >= 0 && a < 4 && (a % 2) == 0);

        const Color symColor = flashOn || flicker
            ? Color(255, 255, 255, 255)
            : (g_board[i] == 1 ? Color(255, 255, 105, 175) : Color(255, 205, 225, 245));
        if (g_board[i] == 1) DrawPixelX(graphics, cell, symColor, pop, jx);
        else                 DrawPixelO(graphics, cell, symColor, pop, jx);
    }

    // Статус под полем (мигает, если матч проигран)
    if (!(g_matchOver && (g_flashTick % 8) < 2)) {
        Font* statusFont = CreateFont(12.5f);
        RectF statusRect(0.f, GY + GRID + 24.f, static_cast<REAL>(client.right), 28.f);
        graphics.DrawString(g_statusText.c_str(), -1, statusFont, statusRect, &center, &textBrush);
        delete statusFont;
    }

    Graphics screen(hdc);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        srand(static_cast<unsigned>(GetTickCount()));
        for (int i = 0; i < 9; ++i) { g_board[i] = 0; g_cellAnim[i] = -1; }
        g_playerTurn = true;
        g_roundOver = false;
        g_gameSelfClosed = false;
        g_roundsPlayed = 0;
        g_playerWins = 0;
        g_mafanyaWins = 0;
        g_hoverCell = -1;
        g_statusText = L"Твой ход";
        g_tiltX = g_tiltY = 0.f;
        g_winLine[0] = g_winLine[1] = g_winLine[2] = -1;
        g_flashTick = 0;
        g_matchOver = false;
        SetTimer(hwnd, GAME_ANIM_TIMER, GAME_ANIM_MS, nullptr);
        if (g_icon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_icon));
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon));
        }
        return 0;

    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Наклон поля за курсором: сглаживание + перерисовка по смене ступеньки
        const float oldQx = floorf(g_tiltX * 2.f + 0.5f);
        const float oldQy = floorf(g_tiltY * 2.f + 0.5f);
        const float tx = std::max(-1.f, std::min(1.f, (x - rc.right  * 0.5f) / (rc.right  * 0.5f)));
        const float ty = std::max(-1.f, std::min(1.f, (y - rc.bottom * 0.5f) / (rc.bottom * 0.5f)));
        g_tiltX += (tx - g_tiltX) * 0.35f;
        g_tiltY += (ty - g_tiltY) * 0.35f;

        const int oldHover = g_hoverCell;
        g_hoverCell = -1;
        if (g_playerTurn && !g_roundOver)
            g_hoverCell = GameCellAt(x, y);
        if (oldHover != g_hoverCell ||
            floorf(g_tiltX * 2.f + 0.5f) != oldQx ||
            floorf(g_tiltY * 2.f + 0.5f) != oldQy)
            InvalidateRect(hwnd, nullptr, FALSE);

        const bool hand = g_hoverCell >= 0 && g_board[g_hoverCell] == 0;
        SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        // Матч окончен — клик сбрасывает счётчики к нулю и начинает заново
        if (g_matchOver) { ResetMatch(hwnd); return 0; }
        // Раунд окончен (победа/поражение/ничья) — клик начинает следующий раунд
        if (g_roundOver)   { ResetRound(hwnd); return 0; }
        if (!g_playerTurn) return 0;

        const int cell = GameCellAt(x, y);   // хит-тест с учётом наклона
        if (cell < 0 || g_board[cell] != 0) return 0;

        g_board[cell] = 1;
        g_cellAnim[cell] = 0;                // поп-ин крестика
        if (CheckWin(g_board) == 1) { EndRound(hwnd, 1); return 0; }
        if (BoardFull(g_board))     { EndRound(hwnd, 0); return 0; }

        g_playerTurn = false;
        g_statusText = L"Ход Mafanya...";
        InvalidateRect(hwnd, nullptr, FALSE);
        SetTimer(hwnd, AI_TIMER, AI_DELAY_MS, nullptr);
        return 0;
    }

    case WM_TIMER:
        if (wParam == AI_TIMER) {
            KillTimer(hwnd, AI_TIMER);
            MakeAiMove(hwnd);
        } else if (wParam == GAME_ANIM_TIMER) {
            // Эффекты: поп-ины, вспышка линии, мигание статуса.
            // Перерисовываем ВСЕГДА — бинарный фон мутирует и без движения мыши.
            ++g_flashTick;
            for (int i = 0; i < 9; ++i)
                if (g_cellAnim[i] >= 0 && ++g_cellAnim[i] > 8) g_cellAnim[i] = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_PAINT:
        PaintGameWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, AI_TIMER);
        KillTimer(hwnd, GAME_ANIM_TIMER);
        g_hwndGame = nullptr;
        // Если окно закрыл сам пользователь (а не победа/главное окно) — сообщаем сюжету
        if (!g_gameSelfClosed && g_hwndMain && IsWindow(g_hwndMain))
            PostMessage(g_hwndMain, WM_MAFANYA_GAME, EV_GAME_CLOSED, 0);
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

void OpenGameWindow() {
    if (g_hwndGame && IsWindow(g_hwndGame)) {
        SetForegroundWindow(g_hwndGame);
        return;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;
    RECT rect = { 0, 0, GAME_W, GAME_H };
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    const int windowW = rect.right - rect.left;
    const int windowH = rect.bottom - rect.top;

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);

    // Окно появляется справа от основного, а не поверх него (как окно сцены)
    int posX = (screenW - windowW) / 2;
    int posY = (screenH - windowH) / 2;
    RECT mainRect;
    if (g_hwndMain && IsWindow(g_hwndMain) && GetWindowRect(g_hwndMain, &mainRect)) {
        posX = mainRect.right + 20;
        posY = mainRect.top + ((mainRect.bottom - mainRect.top) - windowH) / 2;
        if (posX + windowW > screenW) posX = mainRect.left - windowW - 20;  // справа не влезло — встанем слева
        if (posX < 0) posX = (screenW - windowW) / 2;
        if (posY + windowH > screenH) posY = screenH - windowH;
        if (posY < 0) posY = 0;
    }

    g_hwndGame = CreateWindowExW(exStyle, L"MafanyaTTT", L"Mafanya — Крестики-нолики",
                                 style, posX, posY,
                                 windowW, windowH, nullptr, nullptr, g_instance, nullptr);
    if (!g_hwndGame) return;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwndGame, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(g_hwndGame, 19, &dark, sizeof(dark));

    ShowWindow(g_hwndGame, SW_SHOW);
    UpdateWindow(g_hwndGame);
}

// ===================== Окно анимации =====================
// Бинарный слой определён ниже (перед PaintWindow), окно сцены — раньше: форвард-декларация
void DrawBinaryLayer(Graphics& graphics, const RECT& client, REAL parX, REAL parY);

LRESULT CALLBACK AnimWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_animFrame = 0;
        if (g_icon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_icon));
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon));
        }
        SetTimer(hwnd, ANIM_TIMER, ANIM_FRAME_MS, nullptr);
        SetTimer(hwnd, ANIM_BIN_TIMER, ANIM_BIN_MS, nullptr);  // бинарник живёт сам
        return 0;

    case WM_TIMER:
        if (wParam == ANIM_BIN_TIMER) {
            // Бинарник крутится всегда, даже когда кадровая анимация остановилась
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == ANIM_TIMER) {
            if (g_animFrame < g_animSheet->frames - 1) {
                ++g_animFrame;
            } else if (g_animSheet->loops) {
                g_animFrame = 0;
            } else {
                KillTimer(hwnd, ANIM_TIMER);   // финал — застыть на последнем кадре
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);

        // Собираем кадр в памяти и показываем одним блитом — иначе кусочки
        // отрисовки видны по отдельности и картинка мерцает
        Bitmap buffer(client.right, client.bottom, PixelFormat32bppARGB);
        Graphics graphics(&buffer);
        graphics.SetInterpolationMode(InterpolationModeHighQuality);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        // Фон (градиент + рамка) — из кэша, а не с нуля каждый тик
        RebuildAnimBg(client.right, client.bottom);
        if (g_animBg) graphics.DrawImage(g_animBg, 0, 0);

        // Кадры заезжают под розовую рамку (рамка рисуется поверх)
        const REAL margin = -5.f;
        const REAL availW = static_cast<REAL>(client.right) - margin * 2;
        const REAL availH = static_cast<REAL>(client.bottom) - margin * 2;
        const REAL scale = std::min(availW / g_animSheet->frameW, availH / g_animSheet->frameH);
        const REAL drawW = g_animSheet->frameW * scale;
        const REAL drawH = g_animSheet->frameH * scale;
        const REAL dx = (static_cast<REAL>(client.right) - drawW) / 2.f;
        const REAL dy = (static_cast<REAL>(client.bottom) - drawH) / 2.f;

        // Подтягиваем готовый фоновый рендер; пока он идёт — показываем старый кадр
        UpdateFrameCache(drawW, drawH);

        const bool fresh = g_cacheSheet == g_animSheet && g_cacheW == drawW &&
                           g_cacheH == drawH && !g_frameCache.empty();
        if (fresh && g_animFrame < static_cast<int>(g_frameCache.size())) {
            graphics.DrawImage(g_frameCache[g_animFrame], dx, dy, drawW, drawH);
        } else if (!g_frameCache.empty()) {
            graphics.DrawImage(g_frameCache.back(), dx, dy, drawW, drawH);
        } else if (g_animLoadFailed) {
            Font* font = CreateFont(11.f);
            SolidBrush brush(TEXT_COLOR);
            StringFormat center;
            center.SetAlignment(StringAlignmentCenter);
            center.SetLineAlignment(StringAlignmentCenter);
            RectF area(0.f, 0.f, static_cast<REAL>(client.right),
                       static_cast<REAL>(client.bottom));
            graphics.DrawString(L"Не найден файл листа анимации", -1, font, area, &center, &brush);
            delete font;
        }

        // Бинарный слой ПОВЕРХ кадра (без параллакса) — иначе кадр его скрывает
        DrawBinaryLayer(graphics, client, 0.f, 0.f);

        // Розовая рамка поверх всего
        Pen borderOnTop(BORDER_COLOR, static_cast<REAL>(BORDER));
        graphics.DrawRectangle(&borderOnTop, BORDER / 2.f, BORDER / 2.f,
                               static_cast<REAL>(client.right) - BORDER,
                               static_cast<REAL>(client.bottom) - BORDER);

        Graphics screen(hdc);
        screen.DrawImage(&buffer, 0, 0);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            ClearFrameCache();   // кадры перерендерятся под новый размер
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ANIM_TIMER);
        KillTimer(hwnd, ANIM_BIN_TIMER);
        g_hwndAnim = nullptr;
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

void OpenAnimWindow() {
    // Перезагружаем лист при каждом вызове — удобно править кадры на ходу
    LoadAnimSheet(&SHEET1);
    ClearFrameCache();   // кэш соберётся заново в фоне, подхватятся правки файла

    if (g_hwndAnim && IsWindow(g_hwndAnim)) {
        g_animFrame = 0;
        SetForegroundWindow(g_hwndAnim);
        InvalidateRect(g_hwndAnim, nullptr, FALSE);
        return;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_APPWINDOW;
    RECT rect = { 0, 0, ANIM_W, ANIM_H };
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    const int windowW = rect.right - rect.left;
    const int windowH = rect.bottom - rect.top;

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);

    // Окно появляется справа от основного, а не поверх него
    int posX = (screenW - windowW) / 2;
    int posY = (screenH - windowH) / 2;
    RECT mainRect;
    if (g_hwndMain && IsWindow(g_hwndMain) && GetWindowRect(g_hwndMain, &mainRect)) {
        posX = mainRect.right + 20;
        posY = mainRect.top + ((mainRect.bottom - mainRect.top) - windowH) / 2;
        if (posX + windowW > screenW) posX = mainRect.left - windowW - 20;  // справа не влезло — встанем слева
        if (posX < 0) posX = (screenW - windowW) / 2;
        if (posY + windowH > screenH) posY = screenH - windowH;
        if (posY < 0) posY = 0;
    }

    g_hwndAnim = CreateWindowExW(exStyle, L"MafanyaAnim", L"Mafanya — сцена",
                                 style, posX, posY,
                                 windowW, windowH, nullptr, nullptr, g_instance, nullptr);
    if (!g_hwndAnim) return;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwndAnim, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(g_hwndAnim, 19, &dark, sizeof(dark));

    ShowWindow(g_hwndAnim, SW_SHOW);
    UpdateWindow(g_hwndAnim);
}

// Применяет выбор ПОСЛЕ ретро-анимации кнопок (подсветка и CRT-выключение).
// Вся исходная логика ветвлений переехала сюда из обработчика клика.
void ApplyChoice(int i) {
    if (g_dialogueIndex == 5) {
        LoadNightAnswer(i);                                // особый: три своих ответа
    } else if (g_dialogueIndex == 76 && i == 0) {
        LoadDialogue(77);                                  // особый: «Давай сыграем»
        g_gamePhase = 1;
        OpenGameWindow();
    } else {
        const ChoiceDef* choice = FindChoice(g_dialogueIndex);
        if (!choice) return;
        if (g_dialogueIndex == 23 && i == 2)
            RestorePreviousWallpaper();
        LoadDialogue(choice->targets[i]);
    }
}

// ===================== Панель: меню / настройки / сюжет =====================
enum PanelMode { PANEL_MENU, PANEL_SETTINGS, PANEL_STORY };
constexpr UINT_PTR PANEL_TIMER = 1;
constexpr UINT PANEL_MS = 45;
constexpr int PANEL_CLOSE = 100;
constexpr int PANEL_CHAPTER2 = 999;

HWND g_hwndPanel = nullptr;
PanelMode g_panelMode = PANEL_MENU;
int g_panelHover = -1;
int g_storyScroll = 0;
float g_panTiltX = 0.f, g_panTiltY = 0.f;   // наклон плоскости пунктов за курсором
float g_switchAnim[OPT_COUNT] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };  // тумблеры 0..1
bool g_chapterOpen = false;              // глава 1 раскрыта — список диалогов выкатился
float g_chapterAnim = 0.f;               // 0..1 анимация раскрытия

const wchar_t* g_settingNames[OPT_COUNT] = {
    L"Бинарный фон", L"Обводка персонажа", L"Эхо персонажа",
    L"Глитч-эффекты", L"Звук печати", L"Вейв-эффект персонажа",
    L"Дыхание персонажа", L"Плавание вверх-вниз"
};

struct PanelItem { RectF rect; int id; };

// Единая раскладка для отрисовки и хит-теста — клики попадают ровно в то, что видно
void PanelLayout(std::vector<PanelItem>& items, const RECT& client) {
    const REAL w = static_cast<REAL>(client.right);
    if (g_panelMode == PANEL_MENU) {
        // две кнопки в одну строку — компактно
        const REAL bw = (w - 60.f - 14.f) / 2.f;
        items.push_back({ RectF(30.f, 142.f, bw, 48.f), 0 });                 // настройки
        items.push_back({ RectF(30.f + bw + 14.f, 142.f, bw, 48.f), 1 });     // сюжет
    } else if (g_panelMode == PANEL_SETTINGS) {
        // компактные строки: иконка + подпись + мини-тумблер
        for (int i = 0; i < OPT_COUNT; ++i)
            items.push_back({ RectF(30.f, 26.f + i * 36.f, w - 60.f, 30.f), i });
    } else { // PANEL_STORY
        items.push_back({ RectF(30.f, 54.f, w - 60.f, 36.f), 0 });              // глава 1
        items.push_back({ RectF(30.f, 96.f, w - 60.f, 36.f), PANEL_CHAPTER2 }); // глава 2
        // Список диалогов появляется только после раскрытия главы
        if (g_chapterAnim > 0.f) {
            const REAL listTop = 150.f;
            for (int i = 0; i < DIALOGUE_COUNT; ++i) {
                const REAL y = listTop + i * 26.f - g_storyScroll;
                if (y + 24.f < listTop || y > client.bottom - 34.f) continue;   // только видимые
                items.push_back({ RectF(30.f, y, w - 60.f, 24.f), 1000 + i });
            }
        }
    }
    // Своего крестика нет — закрытие стоковым крестиком шапки окна
}

// Квантованный наклон панели — ОДИН для отрисовки и хит-теста (как в игре)
void PanelTiltQuantized(float& qx, float& qy) {
    qx = floorf(g_panTiltX * 2.f + 0.5f) / 2.f;
    qy = floorf(g_panTiltY * 2.f + 0.5f) / 2.f;
}

// Смещение пункта при наклоне: растёт от центра панели к краям (фейковый 3D)
void PanelItemShift(const RectF& r, const RECT& client, float qx, float qy, REAL& dx, REAL& dy) {
    const REAL cx = static_cast<REAL>(client.right) * 0.5f;
    const REAL cy = static_cast<REAL>(client.bottom) * 0.5f;
    const float nx = ((r.X + r.Width * 0.5f) - cx) / std::max(cx, 1.f);
    const float ny = ((r.Y + r.Height * 0.5f) - cy) / std::max(cy, 1.f);
    dx = nx * qx * 10.f;
    dy = ny * qy * 8.f;
}

// --- Векторные иконки ---
void IconGear(Graphics& g, REAL cx, REAL cy, REAL r, const Color& col) {
    Pen p(col, 2.f);
    g.DrawEllipse(&p, cx - r, cy - r, r * 2.f, r * 2.f);
    for (int i = 0; i < 6; ++i) {
        const float a = i * 3.14159265f / 3.f;
        g.DrawLine(&p, cx + cosf(a) * r * 0.55f, cy + sinf(a) * r * 0.55f,
                        cx + cosf(a) * r * 1.3f,  cy + sinf(a) * r * 1.3f);
    }
    SolidBrush b(col);
    g.FillEllipse(&b, cx - 2.f, cy - 2.f, 4.f, 4.f);
}

void IconScroll(Graphics& g, REAL x, REAL y, const Color& col) {
    SolidBrush b(col);
    g.FillRectangle(&b, x, y + 2.f, 14.f, 12.f);
    g.FillRectangle(&b, x - 2.f, y, 4.f, 4.f);
    g.FillRectangle(&b, x + 12.f, y + 12.f, 4.f, 4.f);
    SolidBrush hole(BTN_BG);
    g.FillRectangle(&hole, x + 2.f, y + 5.f, 10.f, 1.5f);
    g.FillRectangle(&hole, x + 2.f, y + 9.f, 10.f, 1.5f);
}

void IconBubble(Graphics& g, REAL x, REAL y, const Color& col) {
    SolidBrush b(col);
    g.FillRectangle(&b, x, y, 14.f, 10.f);
    PointF tail[3] = { { x + 3.f, y + 10.f }, { x + 9.f, y + 10.f }, { x + 2.f, y + 14.f } };
    g.FillPolygon(&b, tail, 3);
}

void IconBook(Graphics& g, REAL x, REAL y, const Color& col) {
    SolidBrush b(col);
    g.FillRectangle(&b, x, y, 12.f, 15.f);
    SolidBrush hole(BTN_BG);
    g.FillRectangle(&hole, x + 2.f, y + 3.f, 1.5f, 9.f);   // корешок
    g.FillRectangle(&hole, x + 5.f, y + 4.f, 5.f, 1.5f);
    g.FillRectangle(&hole, x + 5.f, y + 8.f, 5.f, 1.5f);
}

// Пиксельная шестерёнка: кольцо 5x5 минус углы + зубья + центр
void IconGearPx(Graphics& g, REAL x, REAL y, const Color& col) {
    SolidBrush b(col);
    const REAL u = 3.f;
    for (int py = 0; py < 5; ++py)
        for (int px = 0; px < 5; ++px) {
            const bool corner = (px == 0 || px == 4) && (py == 0 || py == 4);
            const bool ring = !corner && (px == 0 || px == 4 || py == 0 || py == 4);
            if (ring) g.FillRectangle(&b, x + px * u, y + py * u, u - 0.5f, u - 0.5f);
        }
    g.FillRectangle(&b, x + 2.f * u, y - u, u - 0.5f, u - 0.5f);       // зуб сверху
    g.FillRectangle(&b, x + 2.f * u, y + 5.f * u, u - 0.5f, u - 0.5f); // зуб снизу
    g.FillRectangle(&b, x - u, y + 2.f * u, u - 0.5f, u - 0.5f);       // зуб слева
    g.FillRectangle(&b, x + 5.f * u, y + 2.f * u, u - 0.5f, u - 0.5f); // зуб справа
    g.FillRectangle(&b, x + 2.f * u, y + 2.f * u, u - 0.5f, u - 0.5f); // втулка
}

// Пиксельная книга: обложка 5x5 со спинкой посередине
void IconBookPx(Graphics& g, REAL x, REAL y, const Color& col) {
    SolidBrush b(col);
    const REAL u = 3.f;
    for (int i = 0; i < 5; ++i) {
        g.FillRectangle(&b, x, y + i * u, u - 0.5f, u - 0.5f);           // левый срез
        g.FillRectangle(&b, x + 4.f * u, y + i * u, u - 0.5f, u - 0.5f); // правый срез
        g.FillRectangle(&b, x + 2.f * u, y + i * u, u - 0.5f, u - 0.5f); // спинка
    }
    for (int px = 0; px < 5; ++px) {
        g.FillRectangle(&b, x + px * u, y, u - 0.5f, u - 0.5f);          // верх
        g.FillRectangle(&b, x + px * u, y + 4.f * u, u - 0.5f, u - 0.5f);// низ
    }
    // строки текста
    g.FillRectangle(&b, x + u, y + 2.f * u, u - 0.5f, u - 0.5f);
    g.FillRectangle(&b, x + 3.f * u, y + 2.f * u, u - 0.5f, u - 0.5f);
}

// Пиксельная иконка пункта настроек (5x5 паттерн)
void IconSettingPx(Graphics& g, int idx, REAL x, REAL y, const Color& col) {
    SolidBrush b(col);
    const REAL u = 2.6f;
    auto px = [&](int cx, int cy) {
        g.FillRectangle(&b, x + cx * u, y + cy * u, u - 0.4f, u - 0.4f);
    };
    switch (idx) {
    case OPT_BINARY:   // матрица битов
        for (int yy = 0; yy < 4; ++yy)
            for (int xx = 0; xx < 4; ++xx)
                if ((xx * 3 + yy * 5 + idx) % 2) px(xx, yy);
        break;
    case OPT_OUTLINE:  // рамка
        for (int i = 0; i < 4; ++i) { px(i, 0); px(i, 3); px(0, i); px(3, i); }
        break;
    case OPT_ECHO:     // два сдвинутых квадрата
        px(0, 0); px(1, 0); px(0, 1); px(1, 1);
        px(2, 2); px(3, 2); px(2, 3); px(3, 3);
        break;
    case OPT_GLITCH:   // зигзаг
        px(0, 0); px(1, 1); px(2, 0); px(3, 1);
        px(0, 3); px(1, 2); px(2, 3); px(3, 2);
        break;
    case OPT_SOUND:    // динамик
        px(0, 1); px(0, 2); px(1, 0); px(1, 3); px(2, 0); px(2, 3);
        px(3, 1); px(3, 2);
        break;
    case OPT_WAVE:     // волна
        px(0, 1); px(1, 0); px(2, 2); px(3, 1); px(1, 3); px(2, 3);
        break;
    case OPT_BREATH:   // раздутие: рамка с выпирающими боками
        px(1, 0); px(2, 0); px(0, 1); px(3, 1); px(0, 2); px(3, 2); px(1, 3); px(2, 3);
        break;
    case OPT_FLOAT:    // стрелки вверх/вниз
        px(1, 0); px(0, 1); px(2, 1); px(1, 3); px(0, 2); px(2, 2);
        break;
    default: break;
    }
}

void IconClose(Graphics& g, const RectF& r, const Color& col) {
    Pen p(col, 2.f);
    g.DrawLine(&p, r.X + 7.f, r.Y + 5.f, r.X + r.Width - 7.f, r.Y + r.Height - 5.f);
    g.DrawLine(&p, r.X + r.Width - 7.f, r.Y + 5.f, r.X + 7.f, r.Y + r.Height - 5.f);
}

// Мини-тумблер справа в строке настройки — квадратный, в стиле интерфейса
void DrawSwitch(Graphics& g, const RectF& row, int idx) {
    const RectF track(row.X + row.Width - 52.f, row.Y + 5.f, 42.f, 20.f);
    const bool on = g_opts[idx];
    const Color trackCol = on ? Color(255, 255, 105, 175) : Color(255, 60, 55, 80);
    DrawButton(g, track, trackCol, 1.f);
    // ход рычажка: от левого края до упора справа, без вылезания
    const REAL kx = track.X + 2.5f + g_switchAnim[idx] * (track.Width - 15.f - 5.f);
    SolidBrush knob(Color(255, 245, 240, 255));
    g.FillRectangle(&knob, kx, track.Y + 2.5f, 15.f, 15.f);
}

void PaintPanel(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);

    Bitmap buffer(client.right, client.bottom, PixelFormat32bppARGB);
    Graphics graphics(&buffer);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    DrawBackground(graphics, client);

    // Наклон плоскости пунктов за курсором + параллакс бинарника (как в игре)
    float qx, qy;
    PanelTiltQuantized(qx, qy);
    DrawBinaryLayer(graphics, client, -qx * 14.f, -qy * 9.f);

    std::vector<PanelItem> items;
    PanelLayout(items, client);

    // ЭХО ПУНКТОВ: розовый силуэт, зеркало наклона — 1 в 1 как плитки в игре
    const REAL eOffX = -qx * 7.f;
    const REAL eOffY = -qy * 7.f;
    if (eOffX != 0.f || eOffY != 0.f) {
        const Color echoCol(70, 255, 105, 175);
        for (const PanelItem& it : items) {
            REAL dx, dy;
            PanelItemShift(it.rect, client, qx, qy, dx, dy);
            const RectF r(it.rect.X + dx + eOffX, it.rect.Y + dy + eOffY,
                          it.rect.Width, it.rect.Height);
            if (g_panelMode == PANEL_STORY && it.id >= 1000)
                DrawButton(graphics, r, echoCol, 1.f);
            else
                DrawButton(graphics, r, echoCol,
                           g_panelMode == PANEL_MENU ? 1.f : 2.f);
        }
    }

    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);
    StringFormat leftFmt;
    leftFmt.SetAlignment(StringAlignmentNear);
    leftFmt.SetLineAlignment(StringAlignmentNear);

    // Раскрытие главы: список выкатывается сверху вниз за клип-шторкой
    if (g_panelMode == PANEL_STORY && g_chapterAnim > 0.f && g_chapterAnim < 1.f)
        graphics.SetClip(RectF(0.f, 0.f, static_cast<REAL>(client.right),
                               150.f + g_chapterAnim *
                               static_cast<REAL>(client.bottom - 184)));

    // ШАПКА МЕНЮ — «бутерброд»: эхо текста и Menu.png снизу, текст между
    // ними, Menu1.png сверху (1:1 координаты). Эхо только у Menu.png и текста.
    if (g_panelMode == PANEL_MENU && g_menuArt && g_menuArt->GetLastStatus() == Ok) {
        const REAL artH = 108.f;
        const REAL artW = artH * (g_menuArt->GetWidth() / static_cast<REAL>(g_menuArt->GetHeight()));
        RectF artBack(18.f, 12.f, artW, artH);
        float hdx = 0.f, hdy = 0.f;
        PanelItemShift(artBack, client, qx, qy, hdx, hdy);   // ОБЩИЙ сдвиг шапки
        artBack.X += hdx;
        artBack.Y += hdy;
        const RectF artFront(artBack.X, artBack.Y, artBack.Width, artBack.Height);  // 1:1 с Menu.png
        const REAL srcW = static_cast<REAL>(g_menuArt->GetWidth());
        const REAL srcH = static_cast<REAL>(g_menuArt->GetHeight());

        // общие параметры текста (нужны эху и основному блоку — объявляем первыми)
        const REAL textDown = 10.f;    // чистый СДВИГ блока вниз (размер не меняется)
        const REAL textTuck = 72.f;    // насколько текст заходит под арт
        Font* mf = CreateFont(44.f);
        StringFormat ltNN;
        ltNN.SetAlignment(StringAlignmentNear);
        ltNN.SetLineAlignment(StringAlignmentNear);
        RectF natural;
        graphics.MeasureString(L"Mafanya", -1, mf, RectF(0.f, 0.f, 1000.f, 1000.f), &ltNN, &natural);
        const REAL textX = artBack.X + artBack.Width - textTuck;
        const REAL btnRight = static_cast<REAL>(client.right) - 30.f + 14.f;
        const REAL scaleX = (btnRight - textX) / natural.Width;
        const REAL stretchY = artBack.Height / natural.Height;

        // розовая матрица для эха
        ColorMatrix em = {};
        em.m[3][0] = 0.45f;
        em.m[3][1] = 0.45f * 105.f/255.f;
        em.m[3][2] = 0.45f * 175.f/255.f;
        em.m[3][3] = 0.45f;
        ImageAttributes pinkA;
        pinkA.SetColorMatrix(&em, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);

        const REAL hEchoX = -qx * 7.f;
        const REAL hEchoY = -qy * 7.f;

        // 0) ЭХО ТЕКСТА — позади Menu.png: из-за него виден лишь краешек слева
        if (hEchoX != 0.f || hEchoY != 0.f) {
            GraphicsState echoState = graphics.Save();
            graphics.TranslateTransform(textX, artBack.Y + textDown);
            graphics.ScaleTransform(scaleX, stretchY);
            SolidBrush echoB(Color(60, 255, 105, 175));    // очень прозрачное эхо
            graphics.DrawString(L"Mafanya", -1, mf,
                                RectF(hEchoX / scaleX, hEchoY / stretchY,
                                      340.f, natural.Height), &ltNN, &echoB);
            graphics.Restore(echoState);
        }

        // 1) нижний слой Menu.png (+ его розовое эхо)
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        if (hEchoX != 0.f || hEchoY != 0.f)
            graphics.DrawImage(g_menuArt,
                               RectF(artBack.X + hEchoX, artBack.Y + hEchoY, artBack.Width, artBack.Height),
                               0.f, 0.f, srcW, srcH, UnitPixel, &pinkA, nullptr, nullptr);
        graphics.DrawImage(g_menuArt, artBack, 0.f, 0.f, srcW, srcH, UnitPixel);

        // 2) текст между слоями
        GraphicsState gsState = graphics.Save();
        graphics.TranslateTransform(textX, artBack.Y + textDown);
        graphics.ScaleTransform(scaleX, stretchY);

        // текст БЕЛЫМ, одна копия, без тени и обводки
        SolidBrush mainB(Color(255, 255, 255, 255));
        graphics.DrawString(L"Mafanya", -1, mf, RectF(0.f, 0.f, 340.f, natural.Height), &ltNN, &mainB);
        graphics.Restore(gsState);
        delete mf;

        // 3) верхний слой Menu1.png — 1:1 координаты, эхо убрано, давит текст
        if (g_menuArtTop && g_menuArtTop->GetLastStatus() == Ok) {
            const REAL topW = static_cast<REAL>(g_menuArtTop->GetWidth());
            const REAL topH = static_cast<REAL>(g_menuArtTop->GetHeight());
            graphics.DrawImage(g_menuArtTop, artFront, 0.f, 0.f, topW, topH, UnitPixel);
        }
        graphics.SetInterpolationMode(InterpolationModeNearestNeighbor);
    }

    for (const PanelItem& itRaw : items) {
        // пункт рисуем на смещённом наклоном месте — клики считаются так же
        REAL dx, dy;
        PanelItemShift(itRaw.rect, client, qx, qy, dx, dy);
        const PanelItem it{ RectF(itRaw.rect.X + dx, itRaw.rect.Y + dy,
                                  itRaw.rect.Width, itRaw.rect.Height), itRaw.id };
        const bool hover = (it.id == g_panelHover);

        if (g_panelMode == PANEL_MENU) {
            const Color bg = hover ? BTN_HOVER : BTN_BG;
            DrawButton(graphics, it.rect, bg, 2.f);
            // тонкая розовая обводка — аккуратнее
            Pen edge(hover ? Color(255, 255, 150, 200) : Color(120, 255, 105, 175), 1.2f);
            GraphicsPath edgePath;
            const REAL d = 4.f;
            edgePath.AddArc(it.rect.X, it.rect.Y, d, d, 180, 90);
            edgePath.AddArc(it.rect.X + it.rect.Width - d, it.rect.Y, d, d, 270, 90);
            edgePath.AddArc(it.rect.X + it.rect.Width - d, it.rect.Y + it.rect.Height - d, d, d, 0, 90);
            edgePath.AddArc(it.rect.X, it.rect.Y + it.rect.Height - d, d, d, 90, 90);
            edgePath.CloseFigure();
            graphics.DrawPath(&edge, &edgePath);

            Font* f = CreateFont(12.5f);
            SolidBrush b(BTN_TEXT);
            const Color ic = hover ? Color(255, 255, 150, 200) : BORDER_COLOR;
            // пиксельная иконка по центру в своей зоне (верх кнопки)
            const REAL icX = it.rect.X + it.rect.Width / 2.f - 7.5f;
            const REAL icY = it.rect.Y + 9.f;
            if (it.id == 0) IconGearPx(graphics, icX, icY, ic);
            else            IconBookPx(graphics, icX, icY, ic);

            // текст — в своей зоне под иконкой, без параллакса
            RectF tr(it.rect.X, it.rect.Y + 26.f, it.rect.Width, 20.f);
            graphics.DrawString(it.id == 0 ? L"Настройки" : L"Сюжет", -1, f, tr, &center, &b);
            delete f;
        } else if (g_panelMode == PANEL_SETTINGS) {
            // строка: тихий фон, при ховере — подсветка; иконка + подпись + тумблер
            DrawButton(graphics, it.rect, hover ? BTN_HOVER : Color(255, 38, 34, 54), 1.f);
            const Color ic = hover ? Color(255, 255, 150, 200) : BORDER_COLOR;
            IconSettingPx(graphics, it.id, it.rect.X + 12.f,
                          it.rect.Y + it.rect.Height / 2.f - 5.2f, ic);
            Font* f = CreateFont(11.5f);
            SolidBrush b(TEXT_COLOR);
            StringFormat lf;
            lf.SetAlignment(StringAlignmentNear);
            lf.SetLineAlignment(StringAlignmentCenter);
            RectF lr = it.rect;
            lr.X += 34.f;
            graphics.DrawString(g_settingNames[it.id], -1, f, lr, &lf, &b);
            delete f;
            DrawSwitch(graphics, it.rect, it.id);
        } else { // STORY
            if (it.id == 0 || it.id == PANEL_CHAPTER2) {
                // карточка главы
                const bool locked = (it.id == PANEL_CHAPTER2);
                DrawButton(graphics, it.rect, locked ? Color(255, 40, 36, 55)
                                                   : (hover ? BTN_HOVER : BTN_BG), 2.f);
                // стрелка раскрытия главы: вниз когда открыто, вправо когда закрыто
                if (!locked) {
                    SolidBrush arr(g_chapterAnim > 0.5f ? Color(255, 255, 150, 200) : BORDER_COLOR);
                    const REAL ax = it.rect.X + it.rect.Width - 26.f;
                    const REAL ay = it.rect.Y + it.rect.Height * 0.5f;
                    PointF tri[3];
                    if (g_chapterAnim > 0.5f) {
                        tri[0] = { ax, ay + 5.f }; tri[1] = { ax + 13.f, ay + 5.f }; tri[2] = { ax + 6.5f, ay - 5.f };
                    } else {
                        tri[0] = { ax, ay - 7.f }; tri[1] = { ax, ay + 7.f }; tri[2] = { ax + 9.f, ay };
                    }
                    graphics.FillPolygon(&arr, tri, 3);
                }
                Font* f = CreateFont(12.5f);
                SolidBrush b(locked ? Color(255, 120, 115, 135) : TEXT_COLOR);
                StringFormat lf;
            lf.SetAlignment(StringAlignmentNear);
                lf.SetLineAlignment(StringAlignmentCenter);
                RectF lr = it.rect;
                lr.X += 12.f;
                const wchar_t* name = locked ? L"Глава 2 — Скоро..." : L"Глава 1 — Знакомство";
                graphics.DrawString(name, -1, f, lr, &lf, &b);
                delete f;
                IconBook(graphics, it.rect.X + it.rect.Width - 30.f,
                         it.rect.Y + it.rect.Height / 2.f - 7.f,
                         locked ? Color(255, 120, 115, 135) : BORDER_COLOR);
            } else {
                // карточка диалога
                const int idx = it.id - 1000;
                const bool opened = idx <= g_storyProgress;
                const bool cur = idx == g_dialogueIndex;
                Color bg = !opened ? Color(255, 38, 34, 50)
                        : cur ? Color(255, 90, 60, 110)
                        : hover ? BTN_HOVER : BTN_BG;
                DrawButton(graphics, it.rect, bg, 1.f);
                Font* f = CreateFont(10.5f);
                SolidBrush b(!opened ? Color(255, 115, 110, 130) : TEXT_COLOR);
                StringFormat lf;
            lf.SetAlignment(StringAlignmentNear);
                lf.SetLineAlignment(StringAlignmentCenter);
                RectF lr = it.rect;
                lr.X += 30.f;
                wchar_t label[32];
                swprintf(label, 32, L"Диалог %d", idx + 1);
                graphics.DrawString(label, -1, f, lr, &lf, &b);
                delete f;
                IconBubble(graphics, it.rect.X + 10.f, it.rect.Y + 5.f,
                           opened ? BORDER_COLOR : Color(255, 100, 95, 115));
            }
        }
    }

    graphics.ResetClip();

    // РЕТРО-СЛОЙ: сканлайны ЭЛТ поверх контента
    {
        SolidBrush sl(Color(26, 0, 0, 0));
        for (int yy = 0; yy < client.bottom; yy += 3)
            graphics.FillRectangle(&sl, 0.f, static_cast<REAL>(yy),
                                   static_cast<REAL>(client.right), 1.f);
    }
    // Внутренний безель старого монитора
    Pen bezel(Color(80, 255, 105, 175), 1.f);
    graphics.DrawRectangle(&bezel, 4.5f, 4.5f,
                           static_cast<REAL>(client.right) - 9.f,
                           static_cast<REAL>(client.bottom) - 9.f);

    // Рамка поверх всего
    Pen borderOnTop(BORDER_COLOR, static_cast<REAL>(BORDER));
    graphics.DrawRectangle(&borderOnTop, BORDER / 2.f, BORDER / 2.f,
                           static_cast<REAL>(client.right) - BORDER,
                           static_cast<REAL>(client.bottom) - BORDER);

    Graphics screen(hdc);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(hwnd, &ps);
}

void OpenPanel(PanelMode mode);   // определена ниже, вызывается из оконной процедуры

LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        SetTimer(hwnd, PANEL_TIMER, PANEL_MS, nullptr);   // бинарник + тумблеры
        if (g_icon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_icon));
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon));
        }
        return 0;

    case WM_TIMER:
        if (wParam == PANEL_TIMER) {
            for (int i = 0; i < OPT_COUNT; ++i) {
                const float target = g_opts[i] ? 1.f : 0.f;
                g_switchAnim[i] += (target - g_switchAnim[i]) * 0.3f;
            }
            // Выкатывание списка главы — ровными ступеньками
            if (g_chapterOpen && g_chapterAnim < 1.f)
                g_chapterAnim = std::min(1.f, g_chapterAnim + 0.09f);
            else if (!g_chapterOpen && g_chapterAnim > 0.f)
                g_chapterAnim = std::max(0.f, g_chapterAnim - 0.09f);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        RECT client;
        GetClientRect(hwnd, &client);

        // Наклон плоскости за курсором (как поле в крестиках-ноликах)
        const float oldQx = floorf(g_panTiltX * 2.f + 0.5f);
        const float oldQy = floorf(g_panTiltY * 2.f + 0.5f);
        const float tx = std::max(-1.f, std::min(1.f, (x - client.right  * 0.5f) / (client.right  * 0.5f)));
        const float ty = std::max(-1.f, std::min(1.f, (y - client.bottom * 0.5f) / (client.bottom * 0.5f)));
        g_panTiltX += (tx - g_panTiltX) * 0.35f;
        g_panTiltY += (ty - g_panTiltY) * 0.35f;

        std::vector<PanelItem> items;
        PanelLayout(items, client);
        float qx, qy;
        PanelTiltQuantized(qx, qy);
        const int old = g_panelHover;
        g_panelHover = -1;
        for (const PanelItem& itRaw : items) {
            const bool disabled = (g_panelMode == PANEL_STORY) &&
                                  ((itRaw.id == PANEL_CHAPTER2) ||
             (itRaw.id >= 1000 && (g_chapterAnim < 0.99f || itRaw.id - 1000 > g_storyProgress)));
            if (disabled) continue;
            REAL dx, dy;
            PanelItemShift(itRaw.rect, client, qx, qy, dx, dy);
            if (RectF(itRaw.rect.X + dx, itRaw.rect.Y + dy,
                      itRaw.rect.Width, itRaw.rect.Height)
                    .Contains(static_cast<REAL>(x), static_cast<REAL>(y))) {
                g_panelHover = itRaw.id;
                break;
            }
        }
        if (old != g_panelHover ||
            floorf(g_panTiltX * 2.f + 0.5f) != oldQx ||
            floorf(g_panTiltY * 2.f + 0.5f) != oldQy)
            InvalidateRect(hwnd, nullptr, FALSE);
        SetCursor(LoadCursor(nullptr, g_panelHover >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (g_panelMode == PANEL_STORY && g_chapterAnim >= 0.99f) {
            RECT client;
            GetClientRect(hwnd, &client);
            const int maxScroll = std::max(0, DIALOGUE_COUNT * 26 - (static_cast<int>(client.bottom) - 184));
            g_storyScroll -= GET_WHEEL_DELTA_WPARAM(wParam) / 3;
            g_storyScroll = std::max(0, std::min(maxScroll, g_storyScroll));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        RECT client;
        GetClientRect(hwnd, &client);
        std::vector<PanelItem> items;
        PanelLayout(items, client);
        float qx, qy;
        PanelTiltQuantized(qx, qy);
        for (const PanelItem& itRaw : items) {
            REAL dx, dy;
            PanelItemShift(itRaw.rect, client, qx, qy, dx, dy);
            const PanelItem it{ RectF(itRaw.rect.X + dx, itRaw.rect.Y + dy,
                                      itRaw.rect.Width, itRaw.rect.Height), itRaw.id };
            if (!it.rect.Contains(static_cast<REAL>(x), static_cast<REAL>(y))) continue;

            if (g_panelMode == PANEL_MENU) {
                OpenPanel(it.id == 0 ? PANEL_SETTINGS : PANEL_STORY);
                return 0;
            }
            if (g_panelMode == PANEL_SETTINGS) {
                g_opts[it.id] = !g_opts[it.id];   // тумблер уезжает сам, в таймере
                return 0;
            }
            // STORY
            if (it.id == PANEL_CHAPTER2) return 0;                    // заблокирована
            if (it.id == 0) {                                         // глава 1 — раскрыть/свернуть
                g_chapterOpen = !g_chapterOpen;
                if (!g_chapterOpen) g_storyScroll = 0;
                return 0;
            }
            if (g_chapterAnim < 0.99f) return 0;                      // шторка ещё едет
            if (g_gamePhase != 0) return 0;                           // не во время игры
            const int target = it.id - 1000;
            if (target > g_storyProgress) return 0;                   // серое — не кликабельно
            LoadDialogue(target);
            if (g_hwndMain && IsWindow(g_hwndMain))
                SetTimer(g_hwndMain, TYPE_TIMER, TYPE_INTERVAL_MS, nullptr);
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_PAINT:
        PaintPanel(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, PANEL_TIMER);
        g_hwndPanel = nullptr;
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

void OpenPanel(PanelMode mode) {
    g_panelMode = mode;
    g_storyScroll = 0;

    const int widths[] = { 420, 420, 440 };
    const int heights[] = { 250, 320, 480 };
    const int W = widths[mode], H = heights[mode];

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_APPWINDOW;
    RECT rect = { 0, 0, W, H };
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    const int windowW = rect.right - rect.left;
    const int windowH = rect.bottom - rect.top;

    if (g_hwndPanel && IsWindow(g_hwndPanel)) {
        // Окно уже открыто — меняем режим, размер и заголовок, не пересоздаём
        SetWindowPos(g_hwndPanel, nullptr, 0, 0, windowW, windowH,
                     SWP_NOMOVE | SWP_NOZORDER);
        SetForegroundWindow(g_hwndPanel);
        InvalidateRect(g_hwndPanel, nullptr, FALSE);
        return;
    }

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - windowW) / 2;
    int posY = (screenH - windowH) / 2;
    RECT mainRect;
    if (g_hwndMain && IsWindow(g_hwndMain) && GetWindowRect(g_hwndMain, &mainRect)) {
        posX = mainRect.right + 20;
        posY = mainRect.top + ((mainRect.bottom - mainRect.top) - windowH) / 2;
        if (posX + windowW > screenW) posX = mainRect.left - windowW - 20;
        if (posX < 0) posX = (screenW - windowW) / 2;
        if (posY + windowH > screenH) posY = screenH - windowH;
        if (posY < 0) posY = 0;
    }

    g_hwndPanel = CreateWindowExW(exStyle, L"MafanyaPanel", L"Mafanya",
                                  style, posX, posY, windowW, windowH,
                                  nullptr, nullptr, g_instance, nullptr);
    if (!g_hwndPanel) return;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwndPanel, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(g_hwndPanel, 19, &dark, sizeof(dark));

    ShowWindow(g_hwndPanel, SW_SHOW);
    UpdateWindow(g_hwndPanel);
}

// ===================== Общее =====================

void LoadPrivateFont() {
    // Старый GDI+ MinGW не умеет AddFontMemory — распаковываем шрифт во временный файл
    const std::wstring fontPath = ExtractResourceToTemp(3001, L"Mafanya_font.ttf");
    if (fontPath.empty()) return;

    g_fonts = new PrivateFontCollection();
    if (g_fonts->AddFontFile(fontPath.c_str()) != Ok) return;

    const INT count = g_fonts->GetFamilyCount();
    if (count <= 0) return;

    auto* families = new FontFamily[count];
    INT found = 0;
    g_fonts->GetFamilies(count, families, &found);
    if (found > 0) {
        WCHAR name[LF_FACESIZE] = {};
        families[0].GetFamilyName(name);
        g_fontFamily = new FontFamily(name, g_fonts);
    }
    delete[] families;
}

void Cleanup() {
    delete g_character;
    g_character = nullptr;
    delete g_menuArt;
    g_menuArt = nullptr;
    delete g_menuArtTop;
    g_menuArtTop = nullptr;
    delete g_fontFamily;
    g_fontFamily = nullptr;
    delete g_fonts;
    g_fonts = nullptr;
    ClearFrameCache();
    delete g_animBg;
    delete g_charCache;
    delete g_charCacheOutline;
    delete g_charCacheOld;
    if (g_icon) {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
    GdiplusShutdown(g_gdiplusToken);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_hwndMain = hwnd;
        {
            Image* logoImage = LoadImageFromResource(3000);   // CircleLogo256.png
            g_icon = CreateIconFromImage(logoImage);
            delete logoImage;
        }
        g_menuArt = LoadImageFromResource(2023);              // Menu.png, низ шапки
        g_menuArtTop = LoadImageFromResource(2024);           // Menu1.png, верх шапки
        if (g_icon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_icon));
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon));
        }

        LoadPrivateFont();
        LoadDialogue(0);
        SetTimer(hwnd, TYPE_TIMER, TYPE_INTERVAL_MS, nullptr);
        SetTimer(hwnd, CHAR_ANIM_TIMER, CHAR_ANIM_MS, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == CHAR_ANIM_TIMER) {
            g_charPhase += 1.f;
            if (g_glitchT > 0) --g_glitchT;

            // Следование за курсором: цель — положение мыши относительно центра
            // спрайт-зоны, догоняем с инерцией (ретро-запаздывание)
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            float tx = (pt.x - 170) / 22.f;    // 170 — центр зоны персонажа
            float ty = (pt.y - 215) / 22.f;    // 215 — середина высоты окна
            tx = std::max(-9.f, std::min(9.f, tx));
            ty = std::max(-7.f, std::min(7.f, ty));
            g_lookX += (tx - g_lookX) * 0.3f;
            g_lookY += (ty - g_lookY) * 0.3f;

            // Плавное появление/пропадание бинарника (Настройки)
            const float binTarget = g_opts[OPT_BINARY] ? 1.f : 0.f;
            if (g_binFade < binTarget) g_binFade = std::min(1.f, g_binFade + 0.06f);
            else if (g_binFade > binTarget) g_binFade = std::max(0.f, g_binFade - 0.06f);

            // Вейв-пропуск текста
            if (g_textWave >= 0 && ++g_textWave >= TEXT_WAVE_TICKS) g_textWave = -1;

            // Ретро-анимации кнопок
            if (g_btnAppear >= 0 && g_btnAppear < 1000) ++g_btnAppear;
            if (g_choiceAnim >= 0) {
                ++g_choiceAnim;
                if (g_choiceAnim >= CHOICE_ANIM_TICKS) {
                    g_choiceAnim = -1;
                    const int pending = g_pendingChoice;
                    g_pendingChoice = -1;
                    ApplyChoice(pending);              // выбор применяется ЗДЕСЬ
                    SetTimer(hwnd, TYPE_TIMER, TYPE_INTERVAL_MS, nullptr);
                }
            }

            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == TYPE_TIMER && g_isTyping) {
            if (g_typedChars < g_text.size()) {
                ++g_typedChars;
                if (g_text[g_typedChars - 1] != L' ')
                    PlayTypeSound();
            }
            if (g_typedChars >= g_text.size()) {
                FinishTyping();
                KillTimer(hwnd, TYPE_TIMER);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;

    // События из окна крестиков-ноликов
    case WM_MAFANYA_GAME: {
        switch (wParam) {
        case EV_PLAYER_WIN:
            if (lParam == 1)
                ShowInterlude(hwnd, L"shirt_off.png", L"Хех... как тебе моё нижнее бельё?", -1);
            else if (lParam == 2)
                ShowInterlude(hwnd, L"bra_off.png", L"Ммм... ты уже снял с меня почти всё~", -1);
            else
                ShowInterlude(hwnd, L"panties_off.png", L"Ты меня полностью раздел...", -2);
            break;
        case EV_DRAW:
            ShowInterlude(hwnd, L"default.png", L"Не переживай... игра ещё не закончилась.", -1);
            break;
        case EV_MAFANYA_WIN:
            ShowInterlude(hwnd, L"default.png",
                          L"Ты был очень близок, что бы увидеть меня без одежды... Хочешь попробовать ещё раз?~", -1);
            break;
        case EV_GAME_CLOSED:
            ShowInterlude(hwnd, L"backhands.png",
                          L"Хех... ты был близок, что бы увидеть меня голой.", 79);
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const int oldHover = g_hoverButton;
        g_hoverButton = -1;

        if (g_gamePhase == 0 && g_dialogues[g_dialogueIndex].hasChoices && !g_isTyping &&
            !g_choiceMade && g_choicesRevealed) {
            for (int i = 0; i < g_buttonCount; ++i) {
                if (g_buttons[i].rect.Contains(static_cast<REAL>(x),
                                                static_cast<REAL>(y))) {
                    g_hoverButton = i;
                    break;
                }
            }
        }

        const bool oldMenu = g_hoverMenu;
        g_hoverMenu = g_menuBtnRect.Contains(static_cast<REAL>(x), static_cast<REAL>(y));
        if (oldMenu != g_hoverMenu) InvalidateRect(hwnd, nullptr, FALSE);

        if (oldHover != g_hoverButton) InvalidateRect(hwnd, nullptr, FALSE);
        SetCursor(LoadCursor(nullptr, (g_hoverButton >= 0 || g_hoverMenu) ? IDC_HAND : IDC_ARROW));
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);

        // Кнопка меню в углу — поверх всей логики диалога
        if (g_menuBtnRect.Contains(static_cast<REAL>(x), static_cast<REAL>(y))) {
            OpenPanel(PANEL_MENU);
            return 0;
        }

        // Фаза игры: главное окно ждёт, клики только допечатывают текст
        if (g_gamePhase == 1) {
            if (g_isTyping) {
                FinishTypingSkip();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        // Реплика-интерлюдия: клик ведёт по цепочке дальше
        if (g_gamePhase == 2) {
            if (g_isTyping) {
                FinishTypingSkip();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_interludeNext == -2) {
                ShowInterlude(hwnd, L"panties_off.png", L"Теперь я полностью твоя.", 80);
            } else if (g_interludeNext >= 0) {
                const int next = g_interludeNext;
                g_interludeNext = -1;
                g_gamePhase = 0;
                LoadDialogue(next);
                SetTimer(hwnd, TYPE_TIMER, TYPE_INTERVAL_MS, nullptr);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (g_dialogues[g_dialogueIndex].hasChoices && !g_isTyping && !g_choiceMade) {
            // Только что допечаталось — клик раскрывает кнопки, а не выбирает ответ
            if (!g_choicesRevealed) {
                g_choicesRevealed = true;
                g_btnAppear = 0;               // CRT-включение кнопок
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_choiceAnim >= 0) return 0;   // анимация выбора уже идёт
            const ChoiceDef* choice = FindChoice(g_dialogueIndex);
            if (!choice) return 0;
            for (int i = 0; i < g_buttonCount; ++i) {
                if (!g_buttons[i].rect.Contains(static_cast<REAL>(x),
                                                static_cast<REAL>(y)))
                    continue;
                g_hoverButton = -1;
                g_choiceMade = true;           // клики больше не срабатывают
                g_chosenBtn = i;
                g_pendingChoice = i;           // выбор применится после анимации
                g_choiceAnim = 0;              // старт: подсветка + CRT-выключение
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }

        if (!g_choiceMade) AdvanceDialogue(hwnd);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TYPE_TIMER);
        KillTimer(hwnd, CHAR_ANIM_TIMER);
        if (g_hwndGame && IsWindow(g_hwndGame)) {
            g_gameSelfClosed = true;   // не считаем это «сдачей» пользователя
            DestroyWindow(g_hwndGame);
        }
        if (g_hwndAnim && IsWindow(g_hwndAnim))
            DestroyWindow(g_hwndAnim);
        g_hwndMain = nullptr;
        Cleanup();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    g_instance = instance;
    InitializeCriticalSection(&g_resLock);

    srand(static_cast<unsigned>(GetTickCount()));
    GdiplusStartupInput startupInput;
    if (GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr) != Ok) return 1;

    WNDCLASSEXW windowClass = { sizeof(windowClass) };
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"ViraWindow";

    WNDCLASSEXW gameClass = { sizeof(gameClass) };
    gameClass.style = CS_HREDRAW | CS_VREDRAW;
    gameClass.lpfnWndProc = GameWndProc;
    gameClass.hInstance = instance;
    gameClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    gameClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    gameClass.lpszClassName = L"MafanyaTTT";

    WNDCLASSEXW animClass = { sizeof(animClass) };
    animClass.style = CS_HREDRAW | CS_VREDRAW;
    animClass.lpfnWndProc = AnimWndProc;
    animClass.hInstance = instance;
    animClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    animClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    animClass.lpszClassName = L"MafanyaAnim";

    WNDCLASSEXW panelClass = { sizeof(panelClass) };
    panelClass.style = CS_HREDRAW | CS_VREDRAW;
    panelClass.lpfnWndProc = PanelWndProc;
    panelClass.hInstance = instance;
    panelClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    panelClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    panelClass.lpszClassName = L"MafanyaPanel";

    if (!RegisterClassExW(&windowClass) || !RegisterClassExW(&gameClass) ||
        !RegisterClassExW(&animClass) || !RegisterClassExW(&panelClass)) {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                        WS_MINIMIZEBOX | WS_VISIBLE;
    const DWORD exStyle = WS_EX_APPWINDOW;
    RECT rect = { 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    const int windowW = rect.right - rect.left;
    const int windowH = rect.bottom - rect.top;

    HWND hwnd = CreateWindowExW(exStyle, L"ViraWindow", L"Mafanya.exe", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, windowW, windowH,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, nullptr, (screenW - windowW) / 2, (screenH - windowH) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message;
    while (GetMessage(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return static_cast<int>(message.wParam);
}
