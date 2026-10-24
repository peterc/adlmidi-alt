//#ifdef __MINGW32__
//typedef struct vswprintf {} swprintf;
//#endif

#include <vector>
#include <string>
#include <map>
#include <set>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <unistd.h>
#include <stdarg.h>
#include <cstdio>
#include <cstdint>
#include <vector> // vector
#include <deque>  // deque
#include <cmath>  // exp, log, ceil
#include <ctime>
#include <memory>
#include <chrono>
#include <atomic>
#include <array>
#include <filesystem>
#include <cctype>
#include <mutex>
#include <condition_variable>
#include <cerrno>
#include <stdexcept>
#include <limits>

#include <assert.h>

#define SUPPORT_VIDEO_OUTPUT
#define SUPPORT_PUZZLE_GAME

#ifdef __DJGPP__
# include <conio.h>
# include <pc.h>
# include <dpmi.h>
# include <go32.h>
# include <sys/farptr.h>
# include <dos.h>
# define BIOStimer _farpeekl(_dos_ds, 0x46C)
static const unsigned NewTimerFreq = 209;
#elif !defined(__WIN32__) || defined(__CYGWIN__)
# include <termios.h>
# include <fcntl.h>
# include <sys/ioctl.h>
# include <csignal>
#endif

#include <deque>
#include <algorithm>

#include <signal.h>

#include "fraction"

#ifndef __DJGPP__
#include "dbopl.h"

#include "adldata.hh"

static const unsigned long PCM_RATE = 48000;
static const unsigned MaxCards = 100;
static const unsigned MaxSamplesAtTime = 512; // 512=dbopl limitation
#else // DJGPP
static const unsigned MaxCards = 1;
static const unsigned OPLBase = 0x388;
#endif
static unsigned AdlBank    = 1;
static unsigned NumFourOps = 7;
static unsigned NumCards   = std::min(4u, MaxCards);
static bool HighTremoloMode   = false;
static bool HighVibratoMode   = false;
static bool AdlPercussionMode = false;
static bool LogarithmicVolumes = false;
static bool CartoonersVolumes = false;
static volatile sig_atomic_t QuitFlag = false;
static bool FakeDOSshell = false;
static unsigned SkipForward = 0;
static bool DoingInstrumentTesting = false;
static bool QuitWithoutLooping = false;
static bool FolderPlayback = false;
static int TrackChange = 0;
static bool ShowPuzzleGame = false;
static double OutputGainDb = 12.0;
static bool WritePCMfile = false;
static std::string PCMfilepath = "adlmidi.wav";
#ifdef SUPPORT_VIDEO_OUTPUT
static std::string VidFilepath = "adlmidi.mkv";
static bool WriteVideoFile = false;
#endif

static bool ScaleModulators = false;
static unsigned WindowLines = 0;
static unsigned WindowColumns = 80;
static volatile sig_atomic_t WindowResized = 0;
static bool TerminalActive = false;
static bool WritingToTTY;

static unsigned WinHeight()
{
    // MIDI rows and the puzzle layout are independent of the emulated chip count.
    if(TerminalActive)
        return WindowLines ? std::min(54u, std::max(2u, WindowLines)-1) : 36u;
    unsigned result =
        AdlPercussionMode
        ? std::min(2u, NumCards) * 23
        : std::min(3u, NumCards) * 18;

    if(WindowLines) result = std::min(std::max(2u, WindowLines)-1, result);
    return std::max(1u, result);
}

#if (!defined(__WIN32__) || defined(__CYGWIN__)) && defined(TIOCGWINSZ)
extern "C" { static void SigWinchHandler(int); }
static void SigWinchHandler(int)
{
    struct winsize w;
    if(ioctl(2, TIOCGWINSZ, &w) >= 0 || ioctl(1, TIOCGWINSZ, &w) >= 0 || ioctl(0, TIOCGWINSZ, &w) >= 0)
    {
        if(w.ws_row) WindowLines = w.ws_row;
        if(w.ws_col) WindowColumns = w.ws_col;
        WindowResized = 1;
    }
}
#else
static void SigWinchHandler(int) {}
#endif

static void GuessInitialWindowHeight()
{
    auto s = std::getenv("LINES");
    if(s && std::atoi(s)) WindowLines = std::atoi(s);
    SigWinchHandler(0);
}

static const unsigned short Operators[23*2] =
    {// Channels 0-2
     0x000,0x003,0x001,0x004,0x002,0x005, // operators  0, 3,  1, 4,  2, 5
     // Channels 3-5
     0x008,0x00B,0x009,0x00C,0x00A,0x00D, // operators  6, 9,  7,10,  8,11
     // Channels 6-8
     0x010,0x013,0x011,0x014,0x012,0x015, // operators 12,15, 13,16, 14,17
     // Same for second card
     0x100,0x103,0x101,0x104,0x102,0x105, // operators 18,21, 19,22, 20,23
     0x108,0x10B,0x109,0x10C,0x10A,0x10D, // operators 24,27, 25,28, 26,29
     0x110,0x113,0x111,0x114,0x112,0x115, // operators 30,33, 31,34, 32,35
     // Channel 18
     0x010,0x013,   // operators 12,15
     // Channel 19
     0x014,0xFFF,   // operator 16
     // Channel 19
     0x012,0xFFF,   // operator 14
     // Channel 19
     0x015,0xFFF,   // operator 17
     // Channel 19
     0x011,0xFFF }; // operator 13

static const unsigned short Channels[23] =
    {0x000,0x001,0x002, 0x003,0x004,0x005, 0x006,0x007,0x008, // 0..8
     0x100,0x101,0x102, 0x103,0x104,0x105, 0x106,0x107,0x108, // 9..17 (secondary set)
     0x006,0x007,0x008, 0xFFF,0xFFF }; // <- hw percussions, 0xFFF = no support for pitch/pan

/*
    In OPL3 mode:
         0    1    2    6    7    8     9   10   11    16   17   18
       op0  op1  op2 op12 op13 op14  op18 op19 op20  op30 op31 op32
       op3  op4  op5 op15 op16 op17  op21 op22 op23  op33 op34 op35
         3    4    5                   13   14   15
       op6  op7  op8                 op24 op25 op26
       op9 op10 op11                 op27 op28 op29
    Ports:
        +0   +1   +2  +10  +11  +12  +100 +101 +102  +110 +111 +112
        +3   +4   +5  +13  +14  +15  +103 +104 +105  +113 +114 +115
        +8   +9   +A                 +108 +109 +10A
        +B   +C   +D                 +10B +10C +10D

    Percussion:
      bassdrum = op(0): 0xBD bit 0x10, operators 12 (0x10) and 15 (0x13) / channels 6, 6b
      snare    = op(3): 0xBD bit 0x08, operators 16 (0x14)               / channels 7b
      tomtom   = op(4): 0xBD bit 0x04, operators 14 (0x12)               / channels 8
      cym      = op(5): 0xBD bit 0x02, operators 17 (0x17)               / channels 8b
      hihat    = op(2): 0xBD bit 0x01, operators 13 (0x11)               / channels 7


    In OPTi mode ("extended FM" in 82C924, 82C925, 82C931 chips):
         0   1   2    3    4    5    6    7     8    9   10   11   12   13   14   15   16   17
       op0 op4 op6 op10 op12 op16 op18 op22  op24 op28 op30 op34 op36 op38 op40 op42 op44 op46
       op1 op5 op7 op11 op13 op17 op19 op23  op25 op29 op31 op35 op37 op39 op41 op43 op45 op47
       op2     op8      op14      op20       op26      op32
       op3     op9      op15      op21       op27      op33    for a total of 6 quad + 12 dual
    Ports: ???
*/



static std::vector<adlinsdata> dynamic_metainstruments; // Replaces adlins[] when CMF file
static std::vector<adldata>    dynamic_instruments;     // Replaces adl[]    when CMF file
static const unsigned DynamicInstrumentTag = 0x8000u, DynamicMetaInstrumentTag = 0x4000000u;
static const adlinsdata& GetAdlMetaIns(unsigned n)
{
    return (n & DynamicMetaInstrumentTag)
        ? dynamic_metainstruments.at(n & ~DynamicMetaInstrumentTag)
        : adlins[n];
}
static unsigned GetAdlMetaNumber(unsigned midiins)
{
    return (AdlBank == ~0u)
        ? (midiins | DynamicMetaInstrumentTag)
        : banks[AdlBank][midiins];
}
static const adldata& GetAdlIns(unsigned short insno)
{
    return (insno & DynamicInstrumentTag)
        ? dynamic_instruments.at(insno & ~DynamicInstrumentTag)
        : adl[insno];
}


static const char MIDIsymbols[256+1] =
"PPPPPPhcckmvmxbd"  // Ins  0-15
"oooooahoGGGGGGGG"  // Ins 16-31
"BBBBBBBBVVVVVHHM"  // Ins 32-47
"SSSSOOOcTTTTTTTT"  // Ins 48-63
"XXXXTTTFFFFFFFFF"  // Ins 64-79
"LLLLLLLLpppppppp"  // Ins 80-95
"XXXXXXXXGGGGGTSS"  // Ins 96-111
"bbbbMMMcGXXXXXXX"  // Ins 112-127
"????????????????"  // Prc 0-15
"????????????????"  // Prc 16-31
"???DDshMhhhCCCbM"  // Prc 32-47
"CBDMMDDDMMDDDDDD"  // Prc 48-63
"DDDDDDDDDDDDDDDD"  // Prc 64-79
"DD??????????????"  // Prc 80-95
"????????????????"  // Prc 96-111
"????????????????"; // Prc 112-127

static const char* const GMNames[128] =
{
    "Grand Piano", "Bright Piano", "Electric Grand", "Honky-tonk Piano",
    "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
    "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
    "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar",
    "Muted Guitar", "Overdrive Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Fingered Bass", "Picked Bass", "Fretless Bass",
    "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass",
    "Tremolo Strings", "Pizzicato Strings", "Harp", "Timpani",
    "Strings 1", "Strings 2", "Synth Strings 1", "Synth Strings 2",
    "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
    "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
    "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute",
    "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead",
    "Charang Lead", "Voice Lead", "Fifths Lead", "Bass + Lead",
    "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad",
    "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
    "Rain FX", "Soundtrack FX", "Crystal FX", "Atmosphere FX",
    "Brightness FX", "Goblins FX", "Echoes FX", "Sci-fi FX",
    "Sitar", "Banjo", "Shamisen", "Koto",
    "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
    "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
    "Telephone Ring", "Helicopter", "Applause", "Gunshot",
};

static const char PercussionMap[256] =
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"//GM
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" // 3 = bass drum
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" // 4 = snare
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" // 5 = tom
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" // 6 = cymbal
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" // 7 = hihat
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"//GP0
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"//GP16
//2 3 4 5 6 7 8 940 1 2 3 4 5 6 7
"\0\0\0\3\3\0\0\7\0\5\7\5\0\5\7\5"//GP32
//8 950 1 2 3 4 5 6 7 8 960 1 2 3
"\5\6\5\0\6\0\5\6\0\6\0\6\5\5\5\5"//GP48
//4 5 6 7 8 970 1 2 3 4 5 6 7 8 9
"\5\0\0\0\0\0\7\0\0\0\0\0\0\0\0\0"//GP64
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

class Input
{
    enum class EscapeState { None, Pending, Sequence } escape_state = EscapeState::None;
    std::chrono::steady_clock::time_point escape_time;
#ifdef __WIN32__
    void* inhandle;
#endif
#if (!defined(__WIN32__) || defined(__CYGWIN__)) && !defined(__DJGPP__)
    struct termios back{};
    bool terminal_saved = false;
    int original_flags = -1;
#endif
public:
    Input()
    {
#ifdef __WIN32__
        inhandle = GetStdHandle(STD_INPUT_HANDLE);
#endif
#if (!defined(__WIN32__) || defined(__CYGWIN__)) && !defined(__DJGPP__)
        terminal_saved = tcgetattr(0, &back) == 0;
        if(terminal_saved)
        {
            struct termios term = back;
            term.c_lflag &= ~(ICANON|ECHO);
            term.c_cc[VMIN] = 0;
            term.c_cc[VTIME] = 0;
            if(tcsetattr(0, TCSANOW, &term) == 0) return;
        }
        original_flags = fcntl(0, F_GETFL);
        if(original_flags >= 0) fcntl(0, F_SETFL, original_flags | O_NONBLOCK);
#endif
    }
    ~Input()
    {
        Restore();
    }
    void Restore()
    {
#if (!defined(__WIN32__) || defined(__CYGWIN__)) && !defined(__DJGPP__)
        if(terminal_saved) tcsetattr(0, TCSANOW, &back);
        if(original_flags >= 0) fcntl(0, F_SETFL, original_flags);
#endif
    }

    char PeekInput()
    {
#ifdef __DJGPP__
        if(kbhit())
        {
            int c = getch();
            if(c) return c;
            c = getch();
            return c == 77 ? 1 : c == 75 ? 2 : 0;
        }
#endif
#ifdef __WIN32__
        DWORD nread=0;
        INPUT_RECORD inbuf[1];
        while(PeekConsoleInput(inhandle,inbuf,sizeof(inbuf)/sizeof(*inbuf),&nread)&&nread)
        {
            ReadConsoleInput(inhandle,inbuf,sizeof(inbuf)/sizeof(*inbuf),&nread);
            if(inbuf[0].EventType==KEY_EVENT
            && inbuf[0].Event.KeyEvent.bKeyDown)
            {
                char c = inbuf[0].Event.KeyEvent.uChar.AsciiChar;
                unsigned s = inbuf[0].Event.KeyEvent.wVirtualScanCode;
                if(c == 0) c = s == 77 ? 1 : s == 75 ? 2 : 0;
                return c;
        }   }
#endif
#if (!defined(__WIN32__) || defined(__CYGWIN__)) && !defined(__DJGPP__)
        char c = 0;
        if(read(0, &c, 1) == 1) return c;
#endif
        return '\0';
    }
    bool StopRequested()
    {
        while(char c = PeekInput())
        {
            // Arrow/function keys also begin with Escape. Consume their sequence
            // rather than treating its leading byte as a request to quit.
            if(escape_state == EscapeState::Sequence)
            {
                if(c >= 0x40 && c <= 0x7E)
                {
                    if(FolderPlayback && c == 'C') TrackChange = 1;
                    if(FolderPlayback && c == 'D') TrackChange = -1;
                    escape_state = EscapeState::None;
                }
                continue;
            }
            if(escape_state == EscapeState::Pending)
            {
                escape_state = EscapeState::None;
                if(c == '[' || c == 'O')
                {
                    escape_state = EscapeState::Sequence;
                    continue;
                }
            }
#if defined(__WIN32__) || defined(__DJGPP__)
            if(FolderPlayback && c == 1) { TrackChange = 1; continue; }
            if(FolderPlayback && c == 2) { TrackChange = -1; continue; }
#endif
            if(c == 'q' || c == 3) return true;
            if(c == 27)
            {
                escape_state = EscapeState::Pending;
                escape_time = std::chrono::steady_clock::now();
            }
        }
        return escape_state == EscapeState::Pending
            && std::chrono::steady_clock::now() - escape_time >= std::chrono::milliseconds(30);
    }
} Input;

#ifdef SUPPORT_PUZZLE_GAME
#include "puzzlegame.inc"
#endif

#ifdef SUPPORT_VIDEO_OUTPUT
class UIfontBase {};
static unsigned UnicodeToASCIIapproximation(unsigned n) { return n; }
//#include "6x9.inc"
#include "9x15.inc"
#endif

class UserInterface
{
public:
    static constexpr unsigned NColumns = 1216/20; // Video output width
    static constexpr unsigned MaxColumns = 256;
  #ifdef SUPPORT_VIDEO_OUTPUT
    static constexpr unsigned VidWidth  = 1216, VidHeight = 2160;
    static constexpr unsigned FontWidth =   20, FontHeight = 45;
    static constexpr unsigned TxWidth   = (VidWidth/FontWidth), TxHeight = (VidHeight/FontHeight);
    std::vector<unsigned> PixelBuffer; // Allocated by the first VidRender()
    unsigned short CharBuffer[TxWidth*TxHeight] = {0};
    bool           DirtyCells[TxWidth*TxHeight] = {false};
    unsigned       NDirty = 0;
  #endif
  #ifdef __WIN32__
    void* handle;
  #endif
    int x, y, color, txtline, maxy;

    // Text:
    char background[MaxColumns][1 + 23*MaxCards];
    unsigned char backgroundcolor[MaxColumns][1 + 23*MaxCards];
    bool          touched[1 + 23*MaxCards]{false};
    // Notes:
    char slots[MaxColumns][1 + 23*MaxCards];
    unsigned char slotcolors[MaxColumns][1 + 23*MaxCards];

    bool cursor_visible;
    char stderr_buffer[256];
public:
    UserInterface(): x(0), y(0), color(-1), txtline(0),
          maxy(0), cursor_visible(true)
    {
        GuessInitialWindowHeight();
      #ifdef __WIN32__
        handle = GetStdHandle(STD_OUTPUT_HANDLE);
        GotoXY(41,13);
        CONSOLE_SCREEN_BUFFER_INFO tmp;
        GetConsoleScreenBufferInfo(handle,&tmp);
        if(tmp.dwCursorPosition.X != 41)
        {
            // Console is not obeying controls! Probably cygwin xterm.
            handle = 0;
        }
        else
        {
            WindowLines = tmp.dwSize.Y;
            //COORD size = { NColumns, 23*NumCards+5 };
            //SetConsoleScreenBufferSize(handle,size);
        }
      #endif
      #if (!defined(__WIN32__) || defined(__CYGWIN__)) && defined(TIOCGWINSZ)
        std::signal(SIGWINCH, SigWinchHandler);
      #endif
      #ifdef __DJGPP__
        color = 7;
      #endif
        std::memset(slots, '.',      sizeof(slots));
        std::memset(background, '.', sizeof(background));
        std::memset(backgroundcolor, 1, sizeof(backgroundcolor));
        std::setvbuf(stderr, stderr_buffer, _IOFBF, sizeof(stderr_buffer));
        std::memset(slotcolors, 1, sizeof(slotcolors));
    }
    ~UserInterface() { EndScreen(); }
    unsigned Columns() const
    {
        return TerminalActive ? std::min(WindowColumns, MaxColumns) : NColumns;
    }
    bool GameVisible() const
    {
#ifndef SUPPORT_PUZZLE_GAME
        return false;
#else
        return ShowPuzzleGame && (!TerminalActive || (WinHeight() >= 29 && Columns() >= 50));
#endif
    }
    unsigned LogHeight() const
    {
        if(!TerminalActive) return WinHeight();
        return std::min(4u, std::max(3u, WinHeight() - (GameVisible() ? 25u : 1u)));
    }
    unsigned LabelLeft() const { return TerminalActive && GameVisible() ? 32u : 2u; }
    unsigned LabelWidth() const
    {
        return std::min(24u, Columns() > LabelLeft()+8 ? (Columns()-LabelLeft()-8)/2 : 0u);
    }
    unsigned NoteLeft() const
    {
        return TerminalActive ? LabelLeft()+LabelWidth()+2 : 2u;
    }
    struct VisualNote { int ins, pressure; double bend; };
    struct VisualChannel
    {
        unsigned patch = 0;
        std::string name;
        std::map<std::pair<int,int>, VisualNote> notes;
    };
    std::map<unsigned, VisualChannel> visual_channels;
    std::vector<unsigned> visual_order;
    void SetChannel(unsigned channel, unsigned patch, const std::string& name = "")
    {
        if(!visual_channels.count(channel)) visual_order.push_back(channel);
        auto& v = visual_channels[channel];
        v.patch = patch;
        if(!name.empty()) v.name = name;
        if(TerminalActive) DrawChannelLabel(channel);
    }
    int ChannelRow(unsigned channel) const
    {
        auto i = std::find(visual_order.begin(), visual_order.end(), channel);
        return int(LogHeight()) + int(i-visual_order.begin());
    }
    void DrawChannelLabel(unsigned channel)
    {
        int row = ChannelRow(channel);
        if(row >= int(WinHeight()) || !LabelWidth()) return;
        const auto& v = visual_channels.at(channel);
        std::string name = v.name.empty() ? (channel%16 == 9 ? "Drums" : GMNames[v.patch & 127]) : v.name;
        name = std::to_string(channel%16+1) + " " + name;
        for(unsigned n=0; n<LabelWidth(); ++n)
        {
            char c = n < name.size() && (unsigned char)name[n] >= 32 && (unsigned char)name[n] < 127 ? name[n] : ' ';
            Draw(LabelLeft()+n, row, 8, c);
        }
    }
    int NoteColumn(int note) const
    {
        int left = NoteLeft(), span = std::max(1, int(Columns())-left-1);
        return left + ((note+55)%span+span)%span;
    }
    void DrawChannelNotes(unsigned channel)
    {
        int row = ChannelRow(channel);
        if(row >= int(WinHeight())) return;
        DrawChannelLabel(channel);
        for(unsigned col=NoteLeft(); col<Columns(); ++col) Draw(col, row, 1, '.');
        for(const auto& entry: visual_channels.at(channel).notes)
        {
            const auto& n = entry.second;
            char c = n.pressure < 0 ? '%' : n.bend < 0 ? '<' : n.bend > 0 ? '>' : MIDIsymbols[n.ins & 255];
            Draw(NoteColumn(entry.first.second), row, AllocateColor(n.ins & 255), c);
        }
    }
    std::string playing_file;
    void DrawHeader()
    {
        if(!TerminalActive) return;
        std::string lines[2] = {
            "ADLMIDI-ALT | " + playing_file,
            FolderPlayback ? "Left / Right: prev / next | q / Esc / Ctrl-C: quit"
                           : "q / Esc / Ctrl-C: quit"
        };
        for(unsigned row=0; row<2; ++row)
            for(unsigned col=0; col<Columns(); ++col)
            {
                unsigned char c = col < lines[row].size() ? lines[row][col] : ' ';
                Draw(col, row, 15, c >= 32 && c < 127 ? c : '?');
            }
        std::fflush(stderr);
    }
    void SetPlayingFile(const std::string& filename, std::size_t index, std::size_t count)
    {
        playing_file = (FolderPlayback ? "[" + std::to_string(index+1) + "/"
            + std::to_string(count) + "] " : "") + filename;
        DrawHeader();
    }
    unsigned LayoutGeneration = 0;
    void BeginScreen()
    {
#if !defined(__WIN32__) && !defined(__DJGPP__)
        if(!isatty(STDERR_FILENO)) return;
        TerminalActive = true;
        RawPrn("\33[?1049h\33[?7l\33[?25l");
        cursor_visible = false;
        Reflow();
#endif
    }
    void EndScreen()
    {
        if(!TerminalActive) return;
        RawPrn("\33[0m\33[?7h\33[?25h\33[?1049l");
        std::fflush(stderr);
        TerminalActive = false;
    }
    void Reflow()
    {
        if(!TerminalActive) return;
        WindowResized = 0;
        ++LayoutGeneration;
        color = -1;
        Color(1);
        RawPrn("\33[2J\33[H");
        x = y = maxy = 0;
        color = -1;
        txtline = 2;
        std::memset(slots, '.', sizeof(slots));
        std::memset(background, '.', sizeof(background));
        std::memset(backgroundcolor, 1, sizeof(backgroundcolor));
        std::memset(slotcolors, 1, sizeof(slotcolors));
        std::memset(touched, 0, sizeof(touched));
        for(unsigned row=0; row<WinHeight(); ++row)
            for(unsigned col=0; col<Columns(); ++col)
            {
                if(row < LogHeight() || col < NoteLeft())
                    slots[col][row] = background[col][row] = ' ';
                GotoXY(col, row);
                Color(1);
                PutC(background[col][row]);
            }
        DrawHeader();
        for(unsigned channel: visual_order) DrawChannelNotes(channel);
        std::fflush(stderr);
    }
    void HideCursor()
    {
        if(!cursor_visible) return;
        cursor_visible = false;
      #ifdef __WIN32__
        if(handle)
        {
          const CONSOLE_CURSOR_INFO info = {100,false};
          SetConsoleCursorInfo(handle,&info);
          if(!DoingInstrumentTesting)
              CheckTetris();
          return;
        }
      #endif
        if(!DoingInstrumentTesting)
            CheckTetris();
#ifdef __DJGPP__
        { _setcursortype(_NOCURSOR);return;  }
#endif
        RawPrn("\33[?25l"); // hide cursor
    }
    void ShowCursor()
    {
        if(cursor_visible) return;
        cursor_visible = true;
        GotoXY(0,maxy); Color(7);
      #ifdef __WIN32__
        if(handle)
        {
          const CONSOLE_CURSOR_INFO info = {100,true};
          SetConsoleCursorInfo(handle,&info);
          return;
        }
      #endif
#ifdef __DJGPP__
        { _setcursortype(_NORMALCURSOR);return;  }
#endif
        RawPrn("\33[?25h"); // show cursor
        std::fflush(stderr);
    }
    void VidPut(char c)
    {
    #ifndef SUPPORT_VIDEO_OUTPUT
        c = c;
    #else
        unsigned clr = color, tx = x, ty = y;
        unsigned cell_index = ty*TxWidth + tx;
        if(tx < TxWidth && ty < TxHeight)
        {
            unsigned short what = (clr << 8) + (unsigned(c) & 0xFF);
            if(what != CharBuffer[cell_index])
            {
                CharBuffer[cell_index] = what;
                if(!DirtyCells[cell_index])
                {
                    DirtyCells[cell_index] = true;
                    ++NDirty;
                }
            }
        }
    #endif
    }
    #ifdef SUPPORT_VIDEO_OUTPUT
    static unsigned VidTranslateColor(unsigned c)
    {
        static const unsigned colors[16] =
        {
            0x000000,0x00005F,0x00AA00,0x5F5FAF,
            0xAA0000,0xAA00AA,0x87875F,0xAAAAAA,
            0x005F87,0x5555FF,0x55FF55,0x55FFFF,
            0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
        };
        return colors[c % 16];
    }
    void VidRenderCh(unsigned x,unsigned y, unsigned attr, const unsigned char* bitmap)
    {
        unsigned bg = VidTranslateColor(attr >> 4), fg = VidTranslateColor(attr);
        for(unsigned line=0; line<FontHeight; ++line)
        {
            unsigned int* pix = PixelBuffer.data() + (y*FontHeight+line)*VidWidth + x*FontWidth;

            unsigned char bm = bitmap[line*15/FontHeight];
            for(unsigned w=0; w<FontWidth; ++w)
            {
                int shift0 = 7 - w    *9/FontWidth;
                int shift1 = 7 - (w-1)*9/FontWidth;
                int shift2 = 7 - (w+1)*9/FontWidth;
                bool flag = (shift0 >= 0 && (bm & (1u << shift0)));
                if(!flag && (attr & 8))
                {
                    flag = (shift1 >= 0 && (bm & (1u << shift1)))
                        || (shift2 >= 0 && (bm & (1u << shift2)));
                }
                pix[w] = flag ? fg : bg;
            }
        }
    }
    void VidRender()
    {
        static const font9x15 font;
        if(PixelBuffer.empty()) PixelBuffer.resize(VidWidth*VidHeight);
        if(NDirty)
        {
            #pragma omp parallel for schedule(static)
            for(unsigned scan=0; scan<TxWidth*TxHeight; ++scan)
                if(DirtyCells[scan])
                {
                    --NDirty;
                    DirtyCells[scan] = false;
                    VidRenderCh(scan%TxWidth, scan/TxWidth,
                                CharBuffer[scan] >> 8,
                                font.GetBitmap() + 15 * font.GetIndex(CharBuffer[scan] & 0xFF));
                }
        }
    }
    #endif
    void PutC(char c)
    {
      #ifdef __WIN32__
        if(handle) WriteConsole(handle,&c,1, 0,0);
        else
      #endif
        {
      #ifdef __DJGPP__
          putch(c);
      #else
          if(TerminalActive)
          {
              if(x >= 0 && x < int(Columns()) && y >= 0 && y < int(WinHeight()))
                  std::fputc(c == '.' && y >= int(LogHeight()) && x >= int(NoteLeft())
                      && x%4 != 0 ? ' ' : c, stderr);
          }
      #endif
        }
        VidPut(c);
        ++x; // One letter drawn. Update cursor position.
    }
    #ifdef __DJGPP__
    #define RawPrn cprintf
    #else
    static void RawPrn(const char* fmt, ...) __attribute__((format(printf,1,2)))
    {
        // Note: should essentially match PutC, except without updates to x
        va_list ap;
        va_start(ap, fmt);
#if defined(__WIN32__) || defined(__DJGPP__)
        vfprintf(stderr, fmt, ap);
#else
        if(TerminalActive) vfprintf(stderr, fmt, ap);
#endif
        va_end(ap);
    }
    #endif
    int Print(unsigned beginx, unsigned color, bool ln, const char* fmt, va_list ap)
    {
        char Line[1024];
      #ifndef __CYGWIN__
        int nchars = vsnprintf(Line, sizeof(Line), fmt, ap);
      #else
        int nchars = std::vsprintf(Line, fmt, ap); /* SECURITY: POSSIBLE BUFFER OVERFLOW (Cygwin) */
      #endif
        nchars = std::max(0, std::min(nchars, int(sizeof(Line)-1)));
        if(TerminalActive) txtline = 2 + (txtline >= 2 ? txtline-2 : 0) % (LogHeight()-2);

        HideCursor();
        for(unsigned tx = beginx; tx < Columns(); ++tx)
        {
            int n = tx-beginx; // Index into Line[]
            if(n < nchars && Line[n] != '\n')
            {
                GotoXY(tx, txtline);
                Color( backgroundcolor[tx][txtline] = (/*Line[n] == '.' ? 1 :*/ color) );
                slots[tx][txtline] = Line[n];
                slotcolors[tx][txtline] = color;
                PutC( background[tx][txtline] = Line[n] );
            }
            else //if(background[tx][txtline]!='.' && slots[tx][txtline]=='.')
            {
                if(!ln) break;
                GotoXY(tx,txtline);
                Color( backgroundcolor[tx][txtline] = 1);
                char blank = TerminalActive ? ' ' : '.';
                slots[tx][txtline] = blank;
                slotcolors[tx][txtline] = 1;
                PutC( background[tx][txtline] = blank );
            }
        }
        std::fflush(stderr);

        if(ln)
        {
            txtline = TerminalActive
                ? 2 + (txtline-1) % (LogHeight()-2)
                : (txtline + 1) % WinHeight();
        }

        return nchars;
    }
    int Print(unsigned beginx, unsigned color, bool ln, const char* fmt, ...) __attribute__((format(printf,5,6)))
    {
        va_list ap;
        va_start(ap, fmt);
        int r = Print(beginx, color, ln, fmt, ap);
        va_end(ap);
        return r;
    }
    int PrintLn(const char* fmt, ...) __attribute__((format(printf,2,3)))
    {
        va_list ap;
        va_start(ap, fmt);
        int r = Print(2/*column*/, 8/*color*/, true/*line*/, fmt, ap);
        va_end(ap);
        return r;
    }
    void IllustrateNote(int adlchn, int note, int ins, int pressure, double bend, unsigned channel = ~0u)
    {
        if(TerminalActive && channel != ~0u)
        {
            if(!visual_channels.count(channel)) SetChannel(channel, ins & 127);
            auto& notes = visual_channels[channel].notes;
            auto key = std::make_pair(adlchn, note);
            int column = NoteColumn(note), row = ChannelRow(channel);
            if(pressure) notes[key] = {ins, pressure, bend};
            else notes.erase(key);
            DrawChannelLabel(channel);
            Draw(column, row, 1, '.');
            for(const auto& entry: notes)
                if(NoteColumn(entry.first.second) == column)
                {
                    const auto& n = entry.second;
                    char c = n.pressure < 0 ? '%' : n.bend < 0 ? '<' : n.bend > 0 ? '>' : MIDIsymbols[n.ins & 255];
                    Draw(column, row, AllocateColor(n.ins & 255), c);
                }
            std::fflush(stderr);
            return;
        }
        HideCursor();
    #if 1
        const int left = NoteLeft();
        const int span = std::max(1, int(Columns())-left-1);
        int notex = left + ((note+55)%span+span)%span;
        int limit = WinHeight(), minline = TerminalActive ? LogHeight() : 3;
        if(minline >= limit || notex >= int(Columns())) return;

        int notey = minline + adlchn % std::max(1, limit-minline);

        char illustrate_char = background[notex][notey];
        if(pressure > 0)
        {
            illustrate_char = MIDIsymbols[ins];
            if(bend < 0) illustrate_char = '<';
            if(bend > 0) illustrate_char = '>';
        }
        else if(pressure < 0)
        {
            illustrate_char = '%';
        }
        // Special exceptions for '.' (background slot)
        //                        '&' (tetris edges)
        Draw(notex,notey,
            pressure!=0
                ? AllocateColor(ins)        /* use note's color if active */
                : illustrate_char=='.' ? backgroundcolor[notex][notey]
                : illustrate_char=='&' ? 1
                : 8,
            illustrate_char);
    #endif
        std::fflush(stderr);
    }

    void Draw(int notex,int notey, int color, char ch)
    {
        if(notex < 0 || notex >= int(Columns()) || notey < 0 || notey >= int(WinHeight())) return;
        if(slots[notex][notey] != ch
        || slotcolors[notex][notey] != color)
        {
            slots[notex][notey] = ch;
            slotcolors[notex][notey] = color;
            GotoXY(notex, notey);
            Color(color);
            PutC(ch);

            if(!touched[notey])
            {
                touched[notey] = true;

                GotoXY(0, notey);
                for(int tx=0; tx<int(Columns()); ++tx)
                {
                    if(slots[tx][notey] != '.')
                    {
                        Color(slotcolors[tx][notey]);
                        PutC(slots[tx][notey]);
                    }
                    else
                    {
                        Color(backgroundcolor[tx][notey]);
                        PutC(background[tx][notey]);
                    }
                }
            }
        }
    }

    void IllustrateVolumes(double left, double right)
    {
        const unsigned maxy = WinHeight();
        const unsigned white_threshold  = maxy/23;
        const unsigned red_threshold    = maxy*4/23;
        const unsigned yellow_threshold = maxy*8/23;

        double amp[2] = {left*maxy, right*maxy};
        for(unsigned y=TerminalActive ? LogHeight()-1 : 2; y+1<maxy; ++y)
            for(unsigned w=0; w<2; ++w)
            {
                char c = amp[w] > (maxy-1)-y ? '|' : background[w][y+1];
                Draw(w,y+1,
                     c=='|' ? y<white_threshold ? 15
                            : y<red_threshold ? 12
                            : y<yellow_threshold ? 14
                            : 10 : (c=='.' ? 1 : 8),
                     c);
            }
        std::fflush(stderr);
    }

    // Move tty cursor to the indicated position.
    // Movements will be done in relative terms
    // to the current cursor position only.
    void GotoXY(int newx, int newy)
    {
        // Record the maximum line count seen
        if(newy > maxy) maxy = newy;
        if(TerminalActive)
        {
            if(newx != x || newy != y) RawPrn("\33[%d;%dH", newy+1, newx+1);
            x = newx;
            y = newy;
            return;
        }
#if !defined(__WIN32__) && !defined(__DJGPP__)
        x = newx;
        y = newy;
        return;
#endif
        // Go down with '\n' (resets cursor at beginning of line)
        while(newy > y)
        {
            std::fputc('\n', stderr); y+=1; x=0;
        }
      #ifdef __WIN32__
        if(handle)
        {
          CONSOLE_SCREEN_BUFFER_INFO tmp;
          GetConsoleScreenBufferInfo(handle, &tmp);
          COORD tmp2 = { x = newx, tmp.dwCursorPosition.Y } ;
          if(newy < y) { tmp2.Y -= (y-newy); y = newy; }
          SetConsoleCursorPosition(handle, tmp2);
        }
      #endif
#ifdef __DJGPP__
        { gotoxy(x=newx, wherey()-(y-newy)); y=newy; return; }
#endif
        // Go up with \33[A
        if(newy < y) { RawPrn("\33[%dA", y-newy); y = newy; }
        // Adjust X coordinate
        if(newx != x)
        {
            // Use '\r' to go to column 0
            if(newx == 0) // || (newx<10 && std::abs(newx-x)>=10))
                { std::fputc('\r', stderr); x = 0; }
            // Go left  with \33[D
            if(newx < x) RawPrn("\33[%dD", x-newx);
            // Go right with \33[C
            if(newx > x) RawPrn("\33[%dC", newx-x);
            x = newx;
        }
    }
    // Set color (4-bit). Bits: 1=blue, 2=green, 4=red, 8=+intensity
    void Color(int newcolor)
    {
        if(color != newcolor)
        {
          #ifdef __WIN32__
            if(handle)
              SetConsoleTextAttribute(handle, newcolor);
            else
          #endif
#ifdef __DJGPP__
            textattr(newcolor);
            if(0)
#endif
            {
              if(TerminalActive)
              {
                  static const unsigned palette[16] = {
                      0x000000,0x18183F,0x00AA00,0x5F5FAF,
                      0xAA0000,0xAA00AA,0x87875F,0xAAAAAA,
                      0x337799,0x5555FF,0x55FF55,0x55FFFF,
                      0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF
                  };
                  unsigned rgb = palette[newcolor & 15];
                  RawPrn("\33[0;38;2;%u;%u;%u;48;2;0;0;0m",
                         rgb >> 16, (rgb >> 8) & 255, rgb & 255);
                  color = newcolor;
                  return;
              }
              static const char map[8+1] = "04261537";
              RawPrn("\33[0;%s40;3%c", (newcolor&8) ? "1;" : "", map[newcolor&7]);
              // If xterm-256color is used, try using improved colors:
              //        Translate 8 (dark gray) into #003366 (bluish dark cyan)
              //        Translate 1 (dark blue) into #000033 (darker blue)
              if(newcolor==8) RawPrn(";38;5;24;25");
              if(newcolor==6) RawPrn(";38;5;101;25");
              if(newcolor==3) RawPrn(";38;5;61;25");
              if(newcolor==1) RawPrn(";38;5;17;25");
              RawPrn("m");
            }
            color = newcolor;
        }
    }
    // Choose a permanent color for given instrument
    int AllocateColor(int ins)
    {
        static char ins_colors[256] = { 0 }, ins_color_counter = 0;
        if(ins_colors[ins])
            return ins_colors[ins];
        if(ins & 0x80)
        {
            static const char shuffle[] = {2,3,4,5,6,7};
            return ins_colors[ins] = shuffle[ins_color_counter++ % 6];
        }
        else
        {
            static const char shuffle[] = {10,11,12,13,14,15};
            return ins_colors[ins] = shuffle[ins_color_counter++ % 6];
        }
    }

    bool DoCheckTetris()
    {
    #ifdef SUPPORT_PUZZLE_GAME
        if(!GameVisible()) return true;
        static std::unique_ptr<ADLMIDI_PuzzleGame::TetrisAI> player;
        static unsigned generation = ~0u;
        if(!player || generation != LayoutGeneration)
        {
            player.reset(new ADLMIDI_PuzzleGame::TetrisAI(2));
            generation = LayoutGeneration;
        }
        player->GameLoop();
        return player->DelayOpinion() >= 0;
    #else
        return true;
    #endif
    }

    bool TetrisLaunched = false;
    bool CheckTetris()
    {
        if(!DoingInstrumentTesting && Input.StopRequested())
        {
            QuitFlag = true;
            return true;
        }
        return StepDisplay();
    }
    // The part of CheckTetris() that does not read the keyboard.
    bool StepDisplay()
    {
        if(TerminalActive && WindowResized) Reflow();
        if(TetrisLaunched && ShowPuzzleGame) return DoCheckTetris();
        return true;
    }
} UI;

#ifdef SUPPORT_PUZZLE_GAME
namespace ADLMIDI_PuzzleGame
{
    static void PutCell(int x, int y, unsigned cell)
    {
        static const unsigned char valid_attrs[] = {8,6,5,3};
        unsigned char ch = cell, attr = cell >> 8;
        if(!UI.GameVisible()) return;
        int height = WinHeight();
        y += TerminalActive ? UI.LogHeight() : std::max(0, std::min(height, 40) - 25);
        if(x < 0 || x >= int(UI.Columns()) || y < 0 || y >= height) return;
        if(ch == 0xDB) ch = '#';
        if(ch == 0xB0) ch = '*';
        if(attr != 1) attr = valid_attrs[attr % sizeof(valid_attrs)];

        //bool diff = UI.background[x][y] != UI.slots[x][y];
        UI.backgroundcolor[x][y] = attr;
        UI.background[x][y]      = ch;
        UI.Draw(x,y, attr, ch);
        //UI.Draw(x,y, attr, ch);
    }
}
#endif

struct OPL3
{
    unsigned NumChannels;

#ifndef __DJGPP__
    std::vector<DBOPL::Handler> cards;
#endif
private:
    std::vector<unsigned short> ins; // index to adl[], cached, needed by Touch()
    std::vector<unsigned char> pit;  // value poked to B0, cached, needed by NoteOff)(
    std::vector<unsigned char> regBD;
public:
    std::vector<char> four_op_category; // 1 = quad-master, 2 = quad-slave, 0 = regular
                                        // 3 = percussion BassDrum
                                        // 4 = percussion Snare
                                        // 5 = percussion Tom
                                        // 6 = percussion Crash cymbal
                                        // 7 = percussion Hihat
                                        // 8 = percussion slave

    void Poke(unsigned card, unsigned index, unsigned value)
    {
#ifdef __DJGPP__
        unsigned o = index >> 8;
        unsigned port = OPLBase + o * 2;
        outportb(port, index);
        for(unsigned c=0; c<6; ++c) inportb(port);
        outportb(port+1, value);
        for(unsigned c=0; c<35; ++c) inportb(port);
#else
        cards[card].WriteReg(index, value);
#endif
    }
    void NoteOff(unsigned c)
    {
        unsigned card = c/23, cc = c%23;
        if(cc >= 18)
        {
            regBD[card] &= ~(0x10 >> (cc-18));
            Poke(card, 0xBD, regBD[card]);
            return;
        }
        Poke(card, 0xB0 + Channels[cc], pit[c] & 0xDF);
    }
    void NoteOn(unsigned c, double hertz) // Hertz range: 0..131071
    {
        unsigned card = c/23, cc = c%23;
        unsigned x = 0x2000;
        if(hertz < 0 || hertz > 131071) // Avoid infinite loop
            return;
        while(hertz >= 1023.5) { hertz /= 2.0; x += 0x400; } // Calculate octave

        x += (int)(hertz + 0.5);
        unsigned chn = Channels[cc];
        if(cc >= 18)
        {
            regBD[card] |= (0x10 >> (cc-18));
            Poke(card, 0x0BD, regBD[card]);
            x &= ~0x2000;
            //x |= 0x800; // for test
        }
        if(chn != 0xFFF)
        {
            Poke(card, 0xA0 + chn, x & 0xFF);
            Poke(card, 0xB0 + chn, pit[c] = x >> 8);
        }
    }
    void Touch_Real(unsigned c, unsigned volume)
    {
        if(volume > 63) volume = 63;
        unsigned card = c/23, cc = c%23;
        unsigned i = ins[c];
        unsigned o1 = Operators[cc*2+0];
        unsigned o2 = Operators[cc*2+1];

        const adldata& adli = GetAdlIns(i);
        unsigned x = adli.modulator_40, y = adli.carrier_40;

        unsigned mode = 1; // 2-op AM
        if(four_op_category[c] == 0 || four_op_category[c] == 3)
        {
            mode = adli.feedconn & 1; // 2-op FM or 2-op AM
        }
        else if(four_op_category[c] == 1 || four_op_category[c] == 2)
        {
            unsigned i0, i1;
            if ( four_op_category[c] == 1 )
            {
                i0 = i;
                i1 = ins[c + 3];
                mode = 2; // 4-op xx-xx ops 1&2
            }
            else
            {
                i0 = ins[c - 3];
                i1 = i;
                mode = 6; // 4-op xx-xx ops 3&4
            }
            mode += (GetAdlIns(i0).feedconn & 1) + (GetAdlIns(i1).feedconn & 1) * 2;
        }
        static const bool do_ops[10][2] =
          { { false, true },  /* 2 op FM */
            { true,  true },  /* 2 op AM */
            { false, false }, /* 4 op FM-FM ops 1&2 */
            { true,  false }, /* 4 op AM-FM ops 1&2 */
            { false, true  }, /* 4 op FM-AM ops 1&2 */
            { true,  false }, /* 4 op AM-AM ops 1&2 */
            { false, true  }, /* 4 op FM-FM ops 3&4 */
            { false, true  }, /* 4 op AM-FM ops 3&4 */
            { false, true  }, /* 4 op FM-AM ops 3&4 */
            { true,  true  }  /* 4 op AM-AM ops 3&4 */
          };

        if(CartoonersVolumes)
        {
            Poke(card, 0x40+o1, x);
            if(o2 != 0xFFF)
            Poke(card, 0x40+o2, y - volume/2);
        }
        else
        {
            bool do_modulator = do_ops[ mode ][ 0 ] || ScaleModulators;
            bool do_carrier   = do_ops[ mode ][ 1 ] || ScaleModulators;

            Poke(card, 0x40+o1, do_modulator ? (x|63) - volume + volume*(x&63)/63 : x);
            if(o2 != 0xFFF)
            Poke(card, 0x40+o2, do_carrier   ? (y|63) - volume + volume*(y&63)/63 : y);
            //Poke(card, 0x40+o1, do_modulator ? (x|63) - (63-(x&63))*volume/63 : x);
            //if(o2 != 0xFFF)
            //Poke(card, 0x40+o2, do_carrier   ? (y|63) - (63-(y&63))*volume/63 : y);
        }

        // Correct formula (ST3, AdPlug):
        //   63 - ((63-instrvol)/63.0)*chanvol
        // Reduces to (tested identical):
        //   63 - chanvol + chanvol*instrvol/63
        // Also (slower, floats):
        //   63 + chanvol * (instrvol / 63.0 - 1)
    }
    void Touch(unsigned c, unsigned volume) // Volume maxes at 127*127*127
    {
        // Produce a value in 0-63 range
        if(LogarithmicVolumes)
        {
            Touch_Real(c, volume*63/(127*127*127));
        }
        else
        {
            // The formula below: SOLVE(V=127^3 * 2^( (A-63.49999) / 8), A)
            Touch_Real(c, volume>8725  ? std::log(volume)*11.541561 + (0.5 - 104.22845) : 0);

            // The incorrect formula below: SOLVE(V=127^3 * (2^(A/63)-1), A)
            //Touch_Real(c, volume>11210 ? 91.61112 * std::log(4.8819E-7*volume + 1.0)+0.5 : 0);
        }
    }
    void Patch(unsigned c, unsigned i)
    {
        unsigned card = c/23, cc = c%23;
        static const unsigned char data[4] = {0x20,0x60,0x80,0xE0};
        ins[c] = i;
        unsigned o1 = Operators[cc*2+0];
        unsigned o2 = Operators[cc*2+1];

        const adldata& adli = GetAdlIns(i);
        unsigned x = adli.modulator_E862, y = adli.carrier_E862;
        for(unsigned a=0; a<4; ++a, x>>=8, y>>=8)
        {
            Poke(card, data[a]+o1, x&0xFF);
            if(o2 != 0xFFF)
            Poke(card, data[a]+o2, y&0xFF);
        }
    }
    void Pan(unsigned c, unsigned value)
    {
        unsigned card = c/23, cc = c%23;
        if(Channels[cc] != 0xFFF)
            Poke(card, 0xC0 + Channels[cc], GetAdlIns(ins[c]).feedconn | value);
    }
    void Silence() // Silence all OPL channels.
    {
        for(unsigned c=0; c<NumChannels; ++c) { NoteOff(c); Touch_Real(c,0); }
    }
    void Reset()
    {
#ifndef __DJGPP__
        cards.resize(NumCards);
#endif
        NumChannels = NumCards * 23;
        ins.resize(NumChannels,     189);
        pit.resize(NumChannels,       0);
        regBD.resize(NumCards);
        four_op_category.resize(NumChannels);
        for(unsigned p=0, a=0; a<NumCards; ++a)
        {
            for(unsigned b=0; b<18; ++b) four_op_category[p++] = 0;
            for(unsigned b=0; b< 5; ++b) four_op_category[p++] = 8;
        }

        static const short data[] =
        { 0x004,96, 0x004,128,        // Pulse timer
          0x105, 0, 0x105,1, 0x105,0, // Pulse OPL3 enable
          0x001,32, 0x105,1           // Enable wave, OPL3 extensions
        };
        unsigned fours = NumFourOps;
        for(unsigned card=0; card<NumCards; ++card)
        {
#ifndef __DJGPP__
            cards[card].Init(PCM_RATE);
#endif
            for(unsigned a=0; a< 18; ++a) Poke(card, 0xB0+Channels[a], 0x00);
            for(unsigned a=0; a< sizeof(data)/sizeof(*data); a+=2)
                Poke(card, data[a], data[a+1]);
            Poke(card, 0x0BD, regBD[card] = (HighTremoloMode*0x80
                                           + HighVibratoMode*0x40
                                           + AdlPercussionMode*0x20) );
            unsigned fours_this_card = std::min(fours, 6u);
            Poke(card, 0x104, (1 << fours_this_card) - 1);
            //fprintf(stderr, "Card %u: %u four-ops.\n", card, fours_this_card);
            fours -= fours_this_card;
        }

        // Mark all channels that are reserved for four-operator function
        if(AdlPercussionMode)
            for(unsigned a=0; a<NumCards; ++a)
            {
                for(unsigned b=0; b<5; ++b) four_op_category[a*23 + 18 + b] = b+3;
                for(unsigned b=0; b<3; ++b) four_op_category[a*23 + 6  + b] = 8;
            }

        unsigned nextfour = 0;
        for(unsigned a=0; a<NumFourOps; ++a)
        {
            four_op_category[nextfour  ] = 1;
            four_op_category[nextfour+3] = 2;
            switch(a % 6)
            {
                case 0: case 1: nextfour += 1; break;
                case 2:         nextfour += 9-2; break;
                case 3: case 4: nextfour += 1; break;
                case 5:         nextfour += 23-9-2; break;
            }
        }

        /**/
        if (WritingToTTY)
        {
            UI.PrintLn("Channels used as:");
            std::string s;
            for(size_t a=0; a<four_op_category.size(); ++a)
            {
                s += ' ';
                s += std::to_string(four_op_category[a]);
                if(a%23 == 22) { UI.PrintLn("%s", s.c_str()); s.clear(); }
            }
            if(!s.empty()) { UI.PrintLn("%s", s.c_str()); }
        }
        /**/
        /*
        In two-op mode, channels 0..8 go as follows:
                      Op1[port]  Op2[port]
          Channel 0:  00  00     03  03
          Channel 1:  01  01     04  04
          Channel 2:  02  02     05  05
          Channel 3:  06  08     09  0B
          Channel 4:  07  09     10  0C
          Channel 5:  08  0A     11  0D
          Channel 6:  12  10     15  13
          Channel 7:  13  11     16  14
          Channel 8:  14  12     17  15
        In four-op mode, channels 0..8 go as follows:
                      Op1[port]  Op2[port]  Op3[port]  Op4[port]
          Channel 0:  00  00     03  03     06  08     09  0B
          Channel 1:  01  01     04  04     07  09     10  0C
          Channel 2:  02  02     05  05     08  0A     11  0D
          Channel 3:  CHANNEL 0 SLAVE
          Channel 4:  CHANNEL 1 SLAVE
          Channel 5:  CHANNEL 2 SLAVE
          Channel 6:  12  10     15  13
          Channel 7:  13  11     16  14
          Channel 8:  14  12     17  15
         Same goes principally for channels 9-17 respectively.
        */

        Silence();
    }
};

// All file offsets and MIDI event lengths pass through these bounded readers.
// Keeping the file in memory also prevents corrupt track lengths from causing
// allocations larger than the input itself.
class MIDIFileReader
{
    std::vector<unsigned char> bytes;
    std::size_t pos = 0;
public:
    explicit MIDIFileReader(const std::string& filename)
    {
        std::unique_ptr<std::FILE, decltype(&std::fclose)> fp(
            std::fopen(filename.c_str(), "rb"), std::fclose);
        if(!fp) throw std::runtime_error(std::strerror(errno));
        if(std::fseek(fp.get(), 0, SEEK_END) != 0)
            throw std::runtime_error("Cannot seek input file");
        long length = std::ftell(fp.get());
        if(length < 0 || length > 64*1024*1024)
            throw std::runtime_error("Input exceeds the 64 MiB file limit");
        std::rewind(fp.get());
        bytes.resize(length);
        if(std::fread(bytes.data(), 1, bytes.size(), fp.get()) != bytes.size())
            throw std::runtime_error("Cannot read complete input file");
    }
    std::size_t Tell() const { return pos; }
    std::size_t Remaining() const { return bytes.size() - pos; }
    void Limit(std::size_t end)
    {
        if(end < pos || end > bytes.size()) throw std::runtime_error("Invalid file boundary");
        bytes.resize(end);
    }
    void Seek(long offset, int origin)
    {
        long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? pos : bytes.size();
        if(offset < -base || offset > long(bytes.size()) - base)
            throw std::runtime_error("File offset outside input");
        pos = base + offset;
    }
    void Read(void* target, std::size_t length)
    {
        if(length > Remaining()) throw std::runtime_error("Truncated input file");
        if(length) std::memcpy(target, bytes.data() + pos, length);
        pos += length;
    }
    unsigned char Byte()
    {
        unsigned char result;
        Read(&result, 1);
        return result;
    }
};

class MIDITrackReader
{
    const std::vector<unsigned char>& bytes;
    std::size_t& pos;
public:
    MIDITrackReader(const std::vector<unsigned char>& data, std::size_t& position)
        : bytes(data), pos(position) {}
    std::string_view Take(std::size_t length)
    {
        if(pos > bytes.size() || length > bytes.size() - pos)
            throw std::runtime_error("Truncated MIDI event");
        if(!length) return {};
        const char* start = reinterpret_cast<const char*>(bytes.data() + pos);
        pos += length;
        return {start, length};
    }
    unsigned char Byte() { return static_cast<unsigned char>(Take(1)[0]); }
    unsigned long VarLen()
    {
        unsigned long value = 0;
        for(unsigned n=0; n<4; ++n)
        {
            unsigned char byte = Byte();
            value = (value << 7) | (byte & 0x7F);
            if(!(byte & 0x80)) return value;
        }
        throw std::runtime_error("MIDI variable-length value exceeds four bytes");
    }
};

class MIDIplay
{
    // Information about each track
    struct Position
    {
        bool began;
        double wait;
        struct TrackInfo
        {
            size_t ptr;
            long   delay;
            int    status;

            TrackInfo(): ptr(0), delay(0), status(0) { }
        };
        std::vector<TrackInfo> track;

        Position(): began(false), wait(0.0), track() { }
    } CurrentPosition, LoopBeginPosition;

    std::map<std::string, unsigned> devices;
    std::map<unsigned/*track*/, unsigned/*channel begin index*/> current_device;
    std::map<unsigned, std::string> track_names;

    // Persistent settings for each MIDI channel
    struct MIDIchannelSettings
    {
        unsigned short portamento = 0;
        unsigned char bank_lsb = 0, bank_msb = 0;
        unsigned char patch = 0;
        unsigned char volume = 100, expression = 100;
        unsigned char panning = 0x30, vibrato = 0, sustain = 0;
        double bend = 0.0, bendsense = 2 / 8192.0;
        double vibpos = 0, vibspeed = 2*3.141592653*5.0, vibdepth = 0.5/127;
        long   vibdelay = 0;
        unsigned char lastlrpn = 0, lastmrpn = 0; bool nrpn = false;
    };
    // The settings, plus the notes the channel is currently playing
    struct MIDIchannel: MIDIchannelSettings
    {
        struct NoteInfo
        {
            // Current pressure
            unsigned char  vol;
            // Tone selected on noteon:
            short tone;
            // Patch selected on noteon; index to banks[AdlBank][]
            unsigned char midiins;
            // Index to physical adlib data structure, adlins[]
            unsigned insmeta;
            // List of adlib channels it is currently occupying.
            std::map<unsigned short/*adlchn*/,
                     unsigned short/*ins, inde to adl[]*/
                    > phys;
        };
        typedef std::map<unsigned char,NoteInfo> activenotemap_t;
        typedef activenotemap_t::iterator activenoteiterator;
        activenotemap_t activenotes;
    };
    std::vector<MIDIchannel> Ch;
    std::vector<MIDIchannelSettings> LoopBeginSettings; // Goes with LoopBeginPosition
    bool cmf_percussion_mode = false;

    // Additional information about AdLib channels
    struct AdlChannel
    {
        // For collisions
        struct Location
        {
            unsigned short MidCh;
            unsigned char  note;
            bool operator==(const Location&b) const
                { return MidCh==b.MidCh && note==b.note; }
            bool operator< (const Location&b) const
                { return MidCh<b.MidCh || (MidCh==b.MidCh&& note<b.note); }
        };
        struct LocationData
        {
            bool sustained;
            unsigned short ins;  // a copy of that in phys[]
            long kon_time_until_neglible;
            long vibdelay;
        };
        typedef std::map<Location, LocationData> users_t;
        users_t users;

        // If the channel is keyoff'd
        long koff_time_until_neglible;
        // For channel allocation:
        AdlChannel(): users(), koff_time_until_neglible(0) { }

        void AddAge(long ms)
        {
            if(users.empty())
                koff_time_until_neglible =
                    std::max(koff_time_until_neglible-ms, -0x1FFFFFFFl);
            else
            {
                koff_time_until_neglible = 0;
                for(users_t::iterator i = users.begin(); i != users.end(); ++i)
                {
                    i->second.kon_time_until_neglible =
                    std::max(i->second.kon_time_until_neglible-ms, -0x1FFFFFFFl);
                    i->second.vibdelay += ms;
                }
            }
        }
    };
    std::vector<AdlChannel> ch;

    std::vector< std::vector<unsigned char> > TrackData;
    bool imf_events = false;
    bool ending = false;
    bool has_sound = false;
    double tail_remaining = 0;
    double loop_elapsed = 0;
    fraction<long> LoopBeginTempo;
    std::string source_name;
public:
    fraction<long> InvDeltaTicks, Tempo;
    bool loopStart, loopEnd;
    bool finished = false;
    bool failed = false;
    OPL3 opl;
public:
    static unsigned long ReadBEint(const void* buffer, unsigned nbytes)
    {
        unsigned long result=0;
        const unsigned char* data = (const unsigned char*) buffer;
        for(unsigned n=0; n<nbytes; ++n)
            result = (result << 8) + data[n];
        return result;
    }
    static unsigned long ReadLEint(const void* buffer, unsigned nbytes)
    {
        unsigned long result=0;
        const unsigned char* data = (const unsigned char*) buffer;
        for(unsigned n=0; n<nbytes; ++n)
            result = result + (static_cast<unsigned long>(data[n]) << (n*8));
        return result;
    }
    unsigned long ReadVarLen(unsigned tk)
    {
        return MIDITrackReader(TrackData[tk], CurrentPosition.track[tk].ptr).VarLen();
    }

    struct Event
    {
        unsigned char status, meta = 0;
        std::string_view data;
    };
    Event ReadEvent(std::size_t tk, std::size_t& ptr, int& running_status) const
    {
        MIDITrackReader reader(TrackData[tk], ptr);
        unsigned char status = reader.Byte();
        if(status < 0x80)
        {
            if(running_status < 0x80 || running_status >= 0xF0)
                throw std::runtime_error("MIDI data without a running status");
            status = running_status;
            --ptr;
        }
        Event event{status, 0, {}};
        if(status == 0xFF)
        {
            event.meta = reader.Byte();
            event.data = reader.Take(reader.VarLen());
            if(event.meta == 0x2F && !event.data.empty())
                throw std::runtime_error("Invalid end-of-track event");
            if(event.meta == 0x51)
            {
                if(event.data.size() != (imf_events ? 4u : 3u)
                    || !ReadBEint(event.data.data(), event.data.size()))
                    throw std::runtime_error("Invalid MIDI tempo");
            }
            if(event.meta == 0xE3 && (!imf_events || event.data.size() != 2))
                throw std::runtime_error("Invalid AdLib register event");
        }
        else if(status == 0xF0 || status == 0xF7)
        {
            event.data = reader.Take(reader.VarLen());
            running_status = 0;
        }
        else
        {
            unsigned length;
            if(status < 0xF0)
            {
                running_status = status;
                length = ((status >> 4) == 0xC || (status >> 4) == 0xD) ? 1 : 2;
            }
            else if(status == 0xF2 || status == 0xF3)
            {
                length = status == 0xF2 ? 2 : 1;
                running_status = 0;
            }
            else throw std::runtime_error("Unsupported MIDI status byte");
            event.data = reader.Take(length);
            for(unsigned char byte: event.data)
                if(byte >= 0x80) throw std::runtime_error("Invalid MIDI data byte");
        }
        return event;
    }
    void ValidateTrack(std::size_t tk, bool first_delta) const
    {
        std::size_t ptr = 0;
        int status = 0;
        if(TrackData[tk].empty()) throw std::runtime_error("Empty MIDI track");
        while(ptr < TrackData[tk].size())
        {
            if(first_delta) MIDITrackReader(TrackData[tk], ptr).VarLen();
            first_delta = true;
            Event event = ReadEvent(tk, ptr, status);
            if(event.status == 0xFF && event.meta == 0x2F) return;
        }
    }

    bool LoadMIDI(const std::string& filename) try
    {
        source_name = filename;
        MIDIFileReader file(filename);
        const unsigned HeaderSize = 4+4+2+2+2; // 14
        char HeaderBuf[HeaderSize]="";
        file.Read(HeaderBuf, HeaderSize);
        if(std::memcmp(HeaderBuf, "RIFF", 4) == 0)
        {
            if(std::memcmp(HeaderBuf+8, "RMID", 4) != 0)
                throw std::runtime_error("RIFF input is not an RMID file");
            const unsigned long riff_size = ReadLEint(HeaderBuf+4, 4);
            if(riff_size < 4 || riff_size > file.Remaining()+HeaderSize-8)
                throw std::runtime_error("Truncated RIFF container");
            file.Seek(12, SEEK_SET);
            const std::size_t riff_end = riff_size + 8;
            bool found = false;
            while(file.Tell()+8 <= riff_end)
            {
                char chunk[8];
                file.Read(chunk, sizeof(chunk));
                std::size_t length = ReadLEint(chunk+4, 4);
                if(length > riff_end-file.Tell())
                    throw std::runtime_error("Truncated RIFF chunk");
                if(std::memcmp(chunk, "data", 4) == 0)
                {
                    // The MIDI parser must not consume another RIFF chunk.
                    file.Limit(file.Tell()+length);
                    file.Read(HeaderBuf, HeaderSize);
                    found = true;
                    break;
                }
                file.Seek(length + (length & 1), SEEK_CUR);
            }
            if(!found) throw std::runtime_error("RMID container has no data chunk");
        }
        size_t DeltaTicks=192, TrackCount=1;

        bool is_GMF = false; // GMD/MUS files (ScummVM)
        bool is_MUS = false; // MUS/DMX files (Doom)
        bool is_IMF = false; // IMF
        bool is_CMF = false; // Creative Music format (CMF/CTMF)
        bool is_RSXX = false; // RSXX, such as Cartooners
        std::vector<unsigned char> MUS_instrumentList;

        if(std::memcmp(HeaderBuf, "GMF\1", 4) == 0)
        {
            // GMD/MUS files (ScummVM)
            file.Seek(7, SEEK_SET);
            is_GMF = true;
        }
        else if(std::memcmp(HeaderBuf, "MUS\1x1A", 4) == 0)
        {
            // MUS/DMX files (Doom)
            unsigned start = ReadLEint(HeaderBuf+8, 2);
            is_MUS = true;
            file.Seek(int(start-HeaderSize), SEEK_CUR);
        }
        else if(std::memcmp(HeaderBuf, "CTMF", 4) == 0)
        {
            // Creative Music Format (CMF).
            // When playing CTMF files, use the following commandline:
            // adlmidi song8.ctmf -p -v 1 1 0
            // i.e. enable percussion mode, deeper vibrato, and use only 1 card.

            is_CMF = true;
            //unsigned version   = ReadLEint(HeaderBuf+4, 2);
            unsigned ins_start = ReadLEint(HeaderBuf+6, 2);
            unsigned mus_start = ReadLEint(HeaderBuf+8, 2);
            //unsigned deltas    = ReadLEint(HeaderBuf+10, 2);
            unsigned ticks     = ReadLEint(HeaderBuf+12, 2);
            // Read title, author, remarks start offsets in file
            file.Read(HeaderBuf, 6);
            //unsigned long notes_starts[3] = {ReadLEint(HeaderBuf+0,2),ReadLEint(HeaderBuf+0,4),ReadLEint(HeaderBuf+0,6)};
            file.Seek(16, SEEK_CUR); // Skip the channels-in-use table
            file.Read(HeaderBuf, 4);
            unsigned ins_count =  ReadLEint(HeaderBuf+0, 2);//, basictempo = ReadLEint(HeaderBuf+2, 2);
            if(ins_count == 0 || ins_count > 256)
                throw std::runtime_error("Invalid CMF instrument count");
            dynamic_instruments.clear();
            dynamic_metainstruments.clear();
            file.Seek(ins_start, SEEK_SET);
            //std::printf("%u instruments\n", ins_count);
            for(unsigned i=0; i<ins_count; ++i)
            {
                unsigned char InsData[16];
                file.Read(InsData, 16);
                /*std::printf("Ins %3u: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X\n",
                    i, InsData[0],InsData[1],InsData[2],InsData[3], InsData[4],InsData[5],InsData[6],InsData[7],
                       InsData[8],InsData[9],InsData[10],InsData[11], InsData[12],InsData[13],InsData[14],InsData[15]);*/
                struct adldata    adl;
                struct adlinsdata adlins;
                adl.modulator_E862 =
                    (InsData[8] << 24)
                  + (InsData[6] << 16)
                  + (InsData[4] << 8)
                  + (InsData[0] << 0);
                adl.carrier_E862 =
                    (InsData[9] << 24)
                  + (InsData[7] << 16)
                  + (InsData[5] << 8)
                  + (InsData[1] << 0);
                adl.modulator_40 = InsData[2];
                adl.carrier_40   = InsData[3];
                adl.feedconn     = InsData[10];
                adl.finetune = 0;

                adlins.adlno1 = dynamic_instruments.size() | DynamicInstrumentTag;
                adlins.adlno2 = adlins.adlno1;
                adlins.ms_sound_kon  = 1000;
                adlins.ms_sound_koff = 500;
                adlins.tone  = 0;
                adlins.flags = 0;
                dynamic_metainstruments.push_back(adlins);
                dynamic_instruments.push_back(adl);
            }
            file.Seek(mus_start, SEEK_SET);
            TrackCount = 1;
            DeltaTicks = ticks;
            AdlBank    = ~0u; // Ignore AdlBank number, use dynamic banks instead
            //std::printf("CMF deltas %u ticks %u, basictempo = %u\n", deltas, ticks, basictempo);
            LogarithmicVolumes = true;
        }
        else if(std::memcmp(HeaderBuf, "MThd", 4) == 0)
        {
            if(ReadBEint(HeaderBuf+4, 4) != 6 || ReadBEint(HeaderBuf+8, 2) > 1)
                throw std::runtime_error("Unsupported MIDI header or format");
            TrackCount = ReadBEint(HeaderBuf+10, 2);
            DeltaTicks = ReadBEint(HeaderBuf+12, 2);
            if(!TrackCount || (!ReadBEint(HeaderBuf+8, 2) && TrackCount != 1))
                throw std::runtime_error("Invalid MIDI track count");
            if(DeltaTicks & 0x8000)
                throw std::runtime_error("SMPTE MIDI timing is not supported");
        }
        else
        {
            // Try parsing as EA RSXX file
            char rsxxbuf[8]{};
            {long p = file.Tell();
            long offset = static_cast<unsigned char>(HeaderBuf[0])-0x10;
            if(offset >= 0 && std::size_t(offset)+6 <= file.Tell()+file.Remaining())
            {
                file.Seek(offset, SEEK_SET);
                file.Read(rsxxbuf, 6);
            }
            file.Seek(p, SEEK_SET);}
            if(std::memcmp(rsxxbuf, "rsxx}u", 6) == 0)
            {
                is_RSXX = true;
                std::fprintf(stderr, "Detected RSXX format\n");
                file.Seek(static_cast<unsigned char>(HeaderBuf[0]), SEEK_SET);
                TrackCount = 1;
                DeltaTicks = 60;
                LogarithmicVolumes = true;
                CartoonersVolumes = true;
            }
            else
            {
                // Try parsing as an IMF file
                if(1)
                {
                    unsigned end = (unsigned char)HeaderBuf[0] + 256*(unsigned char)HeaderBuf[1];
                    if(!end || (end & 3) || end+2 > file.Tell()+file.Remaining())
                        throw std::runtime_error("Invalid format or truncated IMF data");

                    long backup_pos = file.Tell();
                    unsigned sum1 = 0, sum2 = 0;
                    file.Seek(2, SEEK_SET);
                    for(unsigned n=0; n<std::min(42u, end/4); ++n)
                    {
                        unsigned value1 = file.Byte(); value1 += file.Byte() << 8; sum1 += value1;
                        unsigned value2 = file.Byte(); value2 += file.Byte() << 8; sum2 += value2;
                    }
                    file.Seek(backup_pos, SEEK_SET);
                    if(sum1 > sum2)
                    {
                        is_IMF = true;
                        DeltaTicks = 1;
                    }
                }

                if(!is_IMF)
                    throw std::runtime_error("Invalid format");
            }
        }
        if(!DeltaTicks) throw std::runtime_error("MIDI time division must be nonzero");
        imf_events = is_IMF;
        TrackData.resize(TrackCount);
        CurrentPosition.track.resize(TrackCount);
        InvDeltaTicks = fraction<long>(1, 1000000l * DeltaTicks);
        // SMF defaults to 500,000 microseconds per quarter note. Legacy formats
        // use their own tick rates instead.
        Tempo = (is_GMF || is_MUS || is_IMF || is_CMF || is_RSXX)
              ? fraction<long>(1, DeltaTicks) : fraction<long>(1, 2*DeltaTicks);

        static const unsigned char EndTag[4] = {0xFF,0x2F,0x00,0x00};

        for(size_t tk = 0; tk < TrackCount; ++tk)
        {
            // Read track header
            size_t TrackLength;
            if(is_IMF)
            {
                //std::fprintf(stderr, "Reading IMF file...\n");
                std::size_t end = 2 + (unsigned char)HeaderBuf[0] + 256*(unsigned char)HeaderBuf[1];

                unsigned IMF_tempo = 1428;
                static const unsigned char imf_tempo[] = {0xFF,0x51,0x4,
                    (unsigned char)(IMF_tempo>>24),
                    (unsigned char)(IMF_tempo>>16),
                    (unsigned char)(IMF_tempo>>8),
                    (unsigned char)(IMF_tempo)};
                TrackData[tk].insert(TrackData[tk].end(), imf_tempo, imf_tempo + sizeof(imf_tempo));
                TrackData[tk].push_back(0x00);

                file.Seek(2, SEEK_SET);
                while(file.Tell() < end)
                {
                    unsigned char special_event_buf[5];
                    special_event_buf[0] = 0xFF;
                    special_event_buf[1] = 0xE3;
                    special_event_buf[2] = 0x02;
                    special_event_buf[3] = file.Byte(); // port index
                    special_event_buf[4] = file.Byte(); // port value
                    unsigned delay = file.Byte(); delay += 256 * file.Byte();

                    //if(special_event_buf[3] <= 8) continue;

                    //fprintf(stderr, "Put %02X <- %02X, plus %04X delay\n", special_event_buf[3],special_event_buf[4], delay);

                    TrackData[tk].insert(TrackData[tk].end(), special_event_buf, special_event_buf+5);
                    //if(delay>>21) TrackData[tk].push_back( 0x80 | ((delay>>21) & 0x7F ) );
                    if(delay>>14) TrackData[tk].push_back( 0x80 | ((delay>>14) & 0x7F ) );
                    if(delay>> 7) TrackData[tk].push_back( 0x80 | ((delay>> 7) & 0x7F ) );
                    TrackData[tk].push_back( ((delay>>0) & 0x7F ) );
                }
                TrackData[tk].insert(TrackData[tk].end(), EndTag+0, EndTag+4);
                CurrentPosition.track[tk].delay = 0;
                CurrentPosition.began = true;
                //std::fprintf(stderr, "Done reading IMF file\n");
            }
            else
            {
                if(is_GMF || is_CMF || is_RSXX) // Take the rest of the file
                {
                    long pos = file.Tell();
                    file.Seek(0, SEEK_END);
                    TrackLength = file.Tell() - pos;
                    file.Seek(pos, SEEK_SET);
                }
                else if(is_MUS) // Read TrackLength from file position 4
                {
                    long pos = file.Tell();
                    file.Seek(4, SEEK_SET);
                    TrackLength = file.Byte(); TrackLength += (file.Byte() << 8);
                    file.Seek(pos, SEEK_SET);
                }
                else // Read MTrk header
                {
                    file.Read(HeaderBuf, 8);
                    if(std::memcmp(HeaderBuf, "MTrk", 4) != 0)
                        throw std::runtime_error("Missing MIDI track header");
                    TrackLength = ReadBEint(HeaderBuf+4, 4);
                }
                // Read track data
                if(!TrackLength || TrackLength > file.Remaining())
                    throw std::runtime_error("Invalid MIDI track length");
                TrackData[tk].resize(TrackLength);
                file.Read(TrackData[tk].data(), TrackLength);
                if(is_GMF || is_MUS) // Note: CMF does include the track end tag.
                {
                    TrackData[tk].insert(TrackData[tk].end(), EndTag+0, EndTag+4);
                }
                ValidateTrack(tk, !is_RSXX);
                // Read next event time
                if(!is_RSXX)
                    CurrentPosition.track[tk].delay = ReadVarLen(tk);
            }
            if(is_IMF) ValidateTrack(tk, false);
        }
        loopStart = true;
        CurrentPosition.began = true;

        opl.Reset(); // Reset AdLib
        //opl.Reset(); // ...twice (just in case someone misprogrammed OPL3 previously)
        ch.clear();
        ch.resize(opl.NumChannels);
        return true;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", filename.c_str(), error.what());
        return false;
    }

    /* Periodic tick handler.
     *   Input: s           = seconds since last call
     *   Input: granularity = don't expect intervals smaller than this, in seconds
     *   Output: desired number of seconds until next call
     */
    double Tick(double s, double granularity)
    {
        if(ending)
        {
            tail_remaining = std::max(0.0, tail_remaining-s);
            if(tail_remaining < granularity * 0.5) finished = true;
            return finished ? 0 : tail_remaining;
        }
        if(CurrentPosition.began) CurrentPosition.wait -= s;
        try
        {
            unsigned batches = 0;
            while(!finished && !ending && !QuitFlag && !TrackChange
                && CurrentPosition.wait <= granularity * 0.5)
            {
                // Bound each visit so even very dense or looping input returns
                // to the main loop, where keys and signals are handled.
                if(++batches > 4096) return granularity;
                ProcessEvents();
            }
        }
        catch(const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", source_name.c_str(), error.what());
            failed = true;
            BeginEnding();
        }

        if(ending) return tail_remaining;
        for(unsigned c = 0; c < opl.NumChannels; ++c)
            ch[c].AddAge(s * 1000);

        UpdateVibrato(s);
        UpdateArpeggio(s);
        return std::max(0.0, CurrentPosition.wait);
    }

private:
    enum { Upd_Patch  = 0x1,
           Upd_Pan    = 0x2,
           Upd_Volume = 0x4,
           Upd_Pitch  = 0x8,
           Upd_All    = Upd_Pan + Upd_Volume + Upd_Pitch,
           Upd_Off    = 0x20 };

    void ReleaseNotes()
    {
        for(unsigned MidCh=0; MidCh<Ch.size(); ++MidCh)
        {
            Ch[MidCh].sustain = 0;
            NoteUpdate_All(MidCh, Upd_Off);
        }
        KillSustainingNotes();
        // IMF writes registers directly rather than maintaining MIDI notes.
        for(unsigned c=0; c<opl.NumChannels; ++c) opl.NoteOff(c);
    }
    void BeginEnding()
    {
        ReleaseNotes();
        ending = true;
        tail_remaining = has_sound ? 2.0 : 0.0;
        finished = tail_remaining == 0;
    }

    void NoteUpdate
        (unsigned MidCh,
         MIDIchannel::activenoteiterator i,
         unsigned props_mask,
         int select_adlchn = -1)
    {
        MIDIchannel::NoteInfo& info = i->second;
        const int tone    = info.tone;
        const int vol     = info.vol;
        const int midiins = info.midiins;
        const int insmeta = info.insmeta;
        const adlinsdata& ains = GetAdlMetaIns(insmeta);

        AdlChannel::Location my_loc;
        my_loc.MidCh = MidCh;
        my_loc.note  = i->first;

        for(auto jnext = info.phys.begin(); jnext != info.phys.end(); )
        {
            auto j = jnext++;
            int c   = j->first;
            int ins = j->second;
            if(select_adlchn >= 0 && c != select_adlchn) continue;

            if(props_mask & Upd_Patch)
            {
                opl.Patch(c, ins);
                AdlChannel::LocationData& d = ch[c].users[my_loc];
                d.sustained = false; // inserts if necessary
                d.vibdelay  = 0;
                d.kon_time_until_neglible = ains.ms_sound_kon;
                d.ins       = ins;
            }
        }
        for(auto jnext = info.phys.begin(); jnext != info.phys.end(); )
        {
            auto j = jnext++;
            int c   = j->first;
            int ins = j->second;
            if(select_adlchn >= 0 && c != select_adlchn) continue;

            if(props_mask & Upd_Off) // note off
            {
                if(Ch[MidCh].sustain == 0)
                {
                    AdlChannel::users_t::iterator k = ch[c].users.find(my_loc);
                    if(k != ch[c].users.end())
                        ch[c].users.erase(k);
                    UI.IllustrateNote(c, tone, midiins, 0, 0.0, MidCh);

                    if(ch[c].users.empty())
                    {
                        opl.NoteOff(c);
                        ch[c].koff_time_until_neglible =
                            ains.ms_sound_koff;
                    }
                }
                else
                {
                    // Sustain: Forget about the note, but don't key it off.
                    //          Also will avoid overwriting it very soon.
                    AdlChannel::LocationData& d = ch[c].users[my_loc];
                    d.sustained = true; // note: not erased!
                    UI.IllustrateNote(c, tone, midiins, -1, 0.0, MidCh);
                }
                info.phys.erase(j);
                continue;
            }
            if(props_mask & Upd_Pan)
            {
                opl.Pan(c, Ch[MidCh].panning);
            }
            if(props_mask & Upd_Volume)
            {
                int volume = vol * Ch[MidCh].volume * Ch[MidCh].expression;
                /* If the channel has arpeggio, the effective volume of
                 * *this* instrument is actually lower due to timesharing.
                 * To compensate, add extra volume that corresponds to the
                 * time this note is *not* heard.
                 * Empirical tests however show that a full equal-proportion
                 * increment sounds wrong. Therefore, using the square root.
                 */
                //volume = (int)(volume * std::sqrt( (double) ch[c].users.size() ));
                opl.Touch(c, volume);
            }
            if(props_mask & Upd_Pitch)
            {
                AdlChannel::LocationData& d = ch[c].users[my_loc];
                // Don't bend a sustained note
                if(!d.sustained)
                {
                    double bend = Ch[MidCh].bend + GetAdlIns(ins).finetune;
                    double phase = 0.0;

                    if((ains.flags & adlinsdata::Flag_Pseudo4op) && ins == ains.adlno2)
                    {
                        phase = 0.125; // Detune the note slightly (this is what Doom does)
                    }

                    //phase -= 12; // hack

                    if(Ch[MidCh].vibrato && d.vibdelay >= Ch[MidCh].vibdelay)
                        bend += Ch[MidCh].vibrato * Ch[MidCh].vibdepth * std::sin(Ch[MidCh].vibpos);
                    opl.NoteOn(c, 172.00093 * std::exp(0.057762265 * (tone + bend + phase)));
                    UI.IllustrateNote(c, tone, midiins, vol, Ch[MidCh].bend, MidCh);
                }
            }
        }
        if(info.phys.empty())
            Ch[MidCh].activenotes.erase(i);
    }

    void ProcessEvents()
    {
        loopEnd = false;
        const size_t TrackCount = TrackData.size();
        const Position RowBeginPosition ( CurrentPosition );
        for(size_t tk = 0; tk < TrackCount; ++tk)
        {
            if(CurrentPosition.track[tk].status >= 0
            && CurrentPosition.track[tk].delay <= 0)
            {
                // Handle event
                HandleEvent(tk);
                // Read next event time (unless the track just ended)
                if(CurrentPosition.track[tk].ptr >= TrackData[tk].size())
                    CurrentPosition.track[tk].status = -1;
                if(CurrentPosition.track[tk].status >= 0)
                    CurrentPosition.track[tk].delay += ReadVarLen(tk);
            }
        }
        // Find shortest delay from all track
        long shortest = -1;
        for(size_t tk=0; tk<TrackCount; ++tk)
            if(CurrentPosition.track[tk].status >= 0
            && (shortest == -1
               || CurrentPosition.track[tk].delay < shortest))
            {
                shortest = CurrentPosition.track[tk].delay;
            }
        //if(shortest > 0) UI.PrintLn("shortest: %ld", shortest);

        if(loopStart)
        {
            LoopBeginPosition = RowBeginPosition;
            LoopBeginSettings.assign(Ch.begin(), Ch.end());
            LoopBeginTempo = Tempo;
            loop_elapsed = 0;
            loopStart = false;
        }
        if(shortest < 0 || (loopEnd && !FolderPlayback))
        {
            loopEnd = false;
            if(FolderPlayback || QuitWithoutLooping || loop_elapsed <= 0)
            {
                // A zero-duration loop cannot advance playback. Finish it
                // instead of repeatedly replaying events at the same instant.
                BeginEnding();
                return;
            }
            ReleaseNotes();
            CurrentPosition = LoopBeginPosition;
            Tempo = LoopBeginTempo;
            loop_elapsed = 0;
            for(std::size_t MidCh=0; MidCh<Ch.size(); ++MidCh)
                static_cast<MIDIchannelSettings&>(Ch[MidCh]) =
                    MidCh < LoopBeginSettings.size() ? LoopBeginSettings[MidCh]
                                                     : MIDIchannelSettings();
            return;
        }

        for(size_t tk=0; tk<TrackCount; ++tk)
            if(CurrentPosition.track[tk].status >= 0)
                CurrentPosition.track[tk].delay -= shortest;
        const double seconds = (shortest * Tempo).valuel();
        CurrentPosition.wait += seconds;
        loop_elapsed += seconds;
    }
    void HandleEvent(size_t tk)
    {
        Event event = ReadEvent(tk, CurrentPosition.track[tk].ptr, CurrentPosition.track[tk].status);
        unsigned char byte = event.status;
        if(byte == 0xF7 || byte == 0xF0) // Ignore SysEx
        {
            UI.PrintLn("SysEx %02X: %zu bytes", byte, event.data.size());
            return;
        }
        if(byte == 0xFF)
        {
            // Special event FF
            unsigned char evtype = event.meta;
            std::string data(event.data);
            if(evtype == 0x2F) { CurrentPosition.track[tk].status = -1; return; }
            if(evtype == 0x51) { Tempo = InvDeltaTicks * fraction<long>( (long) ReadBEint(data.data(), data.size())); return; }
            if(evtype == 6 && data == "loopStart") loopStart = true;
            if(evtype == 6 && data == "loopEnd"  ) loopEnd   = true;
            if(evtype == 9) current_device[tk] = ChooseDevice(data);
            if(evtype == 4 || (evtype == 3 && TrackData.size() > 1)) track_names[tk] = data;
            if(evtype >= 1 && evtype <= 6 && evtype != 4 && (evtype != 3 || TrackData.size() == 1))
                UI.PrintLn("Meta %d: %s", evtype, data.c_str());

            if(evtype == 0xE3) // Special non-spec ADLMIDI special for IMF playback: Direct poke to AdLib
            {
                unsigned char i = data[0], v = data[1];
                if( (i&0xF0) == 0xC0 ) v |= 0x30;
                //fprintf(stderr, "OPL poke %02X, %02X\n", i,v);
                opl.Poke(0, i,v);
                has_sound = true;
            }
            return;
        }
        // Any normal event (80..EF)
        if(byte == 0xF3 || byte == 0xF2) return;
        const auto* data = reinterpret_cast<const unsigned char*>(event.data.data());
        unsigned MidCh = byte & 0x0F, EvType = byte >> 4;
        MidCh += current_device[tk];

        CurrentPosition.track[tk].status = byte;
        if(EvType == 0x8 || EvType == 0x9 || EvType == 0xA || EvType == 0xC)
            UI.SetChannel(MidCh, Ch[MidCh].patch, track_names[tk]);
        switch(EvType)
        {
            case 0x8: // Note off
            case 0x9: // Note on
            {
                int note = *data++;
                int  vol = *data++;
                //if(MidCh != 9) note -= 12; // HACK for OpenGL video for changing octaves
                if(CartoonersVolumes && vol != 0)
                {
                    // Check if this is just a note after-touch
                    auto i = Ch[MidCh].activenotes.find(note);
                    if(i != Ch[MidCh].activenotes.end())
                    {
                        i->second.vol = vol;
                        NoteUpdate(MidCh, i, Upd_Volume);
                        break;
                    }
                }
                NoteOff(MidCh, note);
                // On Note on, Keyoff the note first, just in case keyoff
                // was omitted; this fixes Dance of sugar-plum fairy
                // by Microsoft. Now that we've done a Keyoff,
                // check if we still need to do a Keyon.
                // vol=0 and event 8x are both Keyoff-only.
                if(vol == 0 || EvType == 0x8) break;

                unsigned midiins = Ch[MidCh].patch;
                if(MidCh%16 == 9) midiins = 128 + note; // Percussion instrument

                /*
                if(MidCh%16 == 9 || (midiins != 32 && midiins != 46 && midiins != 48 && midiins != 50))
                    break; // HACK
                if(midiins == 46) vol = (vol*7)/10;          // HACK
                if(midiins == 48 || midiins == 50) vol /= 4; // HACK
                */
                //if(midiins == 56) vol = vol*6/10; // HACK

                static std::set<unsigned> bank_warnings;
                if(Ch[MidCh].bank_msb)
                {
                    unsigned bankid = midiins + 256*Ch[MidCh].bank_msb;
                    std::set<unsigned>::iterator
                        i = bank_warnings.lower_bound(bankid);
                    if(i == bank_warnings.end() || *i != bankid)
                    {
                        UI.PrintLn("[%u]Bank %u undefined, patch=%c%u",
                            MidCh,
                            Ch[MidCh].bank_msb,
                            (midiins&128)?'P':'M', midiins&127);
                        bank_warnings.insert(i, bankid);
                    }
                }
                if(Ch[MidCh].bank_lsb)
                {
                    unsigned bankid = Ch[MidCh].bank_lsb*65536;
                    std::set<unsigned>::iterator
                        i = bank_warnings.lower_bound(bankid);
                    if(i == bank_warnings.end() || *i != bankid)
                    {
                        UI.PrintLn("[%u]Bank lsb %u undefined",
                            MidCh,
                            Ch[MidCh].bank_lsb);
                        bank_warnings.insert(i, bankid);
                    }
                }

                const unsigned meta    = GetAdlMetaNumber(midiins);
                const adlinsdata& ains = GetAdlMetaIns(meta);

                int tone = note;
                if(ains.tone)
                {
                    // 0..19:    add x
                    // 20..127:  set x
                    // 128..255: subtract x-128
                    if(ains.tone < 20)
                        tone += ains.tone;
                    else if(ains.tone < 128)
                        tone = ains.tone;
                    else
                        tone -= ains.tone-128;
                }
                int i[2] = { ains.adlno1, ains.adlno2 };
                bool pseudo_4op = ains.flags & adlinsdata::Flag_Pseudo4op;

                if(AdlPercussionMode && PercussionMap[midiins & 0xFF]) i[1] = i[0];

                static std::set<unsigned char> missing_warnings;
                if(!missing_warnings.count(midiins) && (ains.flags & adlinsdata::Flag_NoSound))
                {
                    UI.PrintLn("[%i]Playing missing instrument %i", MidCh, midiins);
                    missing_warnings.insert(midiins);
                }

                // Allocate AdLib channel (the physical sound channel for the note)
                int adlchannel[2] = { -1, -1 };
                for(unsigned ccount = 0; ccount < 2; ++ccount)
                {
                    if(ccount == 1)
                    {
                        if(i[0] == i[1]) break; // No secondary channel
                        if(adlchannel[0] == -1) break; // No secondary if primary failed
                    }

                    int c = -1;
                    long bs = -0x7FFFFFFFl;
                    for(int a = 0; a < (int)opl.NumChannels; ++a)
                    {
                        if(ccount == 1 && a == adlchannel[0]) continue;
                        // ^ Don't use the same channel for primary&secondary

                        if(i[0] == i[1] || pseudo_4op)
                        {
                            // Only use regular channels
                            int expected_mode = 0;
                            if(AdlPercussionMode)
                            {
                                if(cmf_percussion_mode)
                                    expected_mode = MidCh < 11 ? 0 : (3+MidCh-11); // CMF
                                else
                                    expected_mode = PercussionMap[midiins & 0xFF];
                            }
                            if(opl.four_op_category[a] != expected_mode)
                                continue;
                        }
                        else
                        {
                            if(ccount == 0)
                            {
                                // Only use four-op master channels
                                if(opl.four_op_category[a] != 1)
                                    continue;
                            }
                            else
                            {
                                // The secondary must be played on a specific channel.
                                if(a != adlchannel[0] + 3)
                                    continue;
                            }
                        }

                        long s = CalculateAdlChannelGoodness(a, i[ccount], MidCh);
                        if(s > bs) { bs=s; c = a; } // Best candidate wins
                    }

                    if(c < 0)
                    {
                        //UI.PrintLn("ignored unplaceable note");
                        continue; // Could not play this note. Ignore it.
                    }
                    PrepareAdlChannelForNewNote(c, i[ccount]);
                    adlchannel[ccount] = c;
                }
                if(adlchannel[0] < 0 && adlchannel[1] < 0)
                {
                    // The note could not be played, at all.
                    break;
                }
                //UI.PrintLn("i1=%d:%d, i2=%d:%d", i[0],adlchannel[0], i[1],adlchannel[1]);

                // Allocate active note for MIDI channel
                auto ir = Ch[MidCh].activenotes.insert(std::make_pair(note, MIDIchannel::NoteInfo()));
                ir.first->second.vol     = vol;
                ir.first->second.tone    = tone;
                ir.first->second.midiins = midiins;
                ir.first->second.insmeta = meta;
                for(unsigned ccount=0; ccount<2; ++ccount)
                {
                    int c = adlchannel[ccount];
                    if(c < 0) continue;
                    ir.first->second.phys[ adlchannel[ccount] ] = i[ccount];
                }
                CurrentPosition.began  = true;
                has_sound = true;
                NoteUpdate(MidCh, ir.first, Upd_All | Upd_Patch);
                break;
            }
            case 0xA: // Note touch
            {
                int note = *data++;
                int  vol = *data++;
                auto i = Ch[MidCh].activenotes.find(note);
                if(i == Ch[MidCh].activenotes.end())
                {
                    // Ignore touch if note is not active
                    break;
                }
                i->second.vol = vol;
                NoteUpdate(MidCh, i, Upd_Volume);
                break;
            }
            case 0xB: // Controller change
            {
                int ctrlno = *data++;
                int  value = *data++;
                switch(ctrlno)
                {
                    case 1: // Adjust vibrato
                        //UI.PrintLn("%u:vibrato %d", MidCh,value);
                        Ch[MidCh].vibrato = value; break;
                    case 0: // Set bank msb (GM bank)
                        Ch[MidCh].bank_msb = value;
                        break;
                    case 32: // Set bank lsb (XG bank)
                        Ch[MidCh].bank_lsb = value;
                        break;
                    case 5: // Set portamento msb
                        Ch[MidCh].portamento = (Ch[MidCh].portamento & 0x7F) | (value<<7);
                        UpdatePortamento(MidCh);
                        break;
                    case 37: // Set portamento lsb
                        Ch[MidCh].portamento = (Ch[MidCh].portamento & 0x3F80) | (value);
                        UpdatePortamento(MidCh);
                        break;
                    case 65: // Enable/disable portamento
                        // value >= 64 ? enabled : disabled
                        //UpdatePortamento(MidCh);
                        break;
                    case 7: // Change volume
                        Ch[MidCh].volume = value;
                        NoteUpdate_All(MidCh, Upd_Volume);
                        break;
                    case 64: // Enable/disable sustain
                        Ch[MidCh].sustain = value;
                        if(!value) KillSustainingNotes( MidCh );
                        break;
                    case 11: // Change expression (another volume factor)
                        Ch[MidCh].expression = value;
                        NoteUpdate_All(MidCh, Upd_Volume);
                        break;
                    case 10: // Change panning
                        Ch[MidCh].panning = 0x00;
                        if(value  < 64+32) Ch[MidCh].panning |= 0x10;
                        if(value >= 64-32) Ch[MidCh].panning |= 0x20;
                        NoteUpdate_All(MidCh, Upd_Pan);
                        break;
                    case 121: // Reset all controllers
                        Ch[MidCh].bend       = 0;
                        Ch[MidCh].volume     = CartoonersVolumes ? 127 : 100;
                        Ch[MidCh].expression = CartoonersVolumes ? 127 : 100;
                        Ch[MidCh].sustain    = 0;
                        Ch[MidCh].vibrato    = 0;
                        Ch[MidCh].vibspeed   = 2*3.141592653*5.0;
                        Ch[MidCh].vibdepth   = 0.5/127;
                        Ch[MidCh].vibdelay   = 0;
                        Ch[MidCh].panning    = 0x30;
                        Ch[MidCh].portamento = 0;
                        UpdatePortamento(MidCh);
                        NoteUpdate_All(MidCh, Upd_Pan+Upd_Volume+Upd_Pitch);
                        // Kill all sustained notes
                        KillSustainingNotes(MidCh);
                        break;
                    case 123: // All notes off
                        NoteUpdate_All(MidCh, Upd_Off);
                        break;
                    case 91: break; // Reverb effect depth. We don't do per-channel reverb.
                    case 92: break; // Tremolo effect depth. We don't do...
                    case 93: break; // Chorus effect depth. We don't do.
                    case 94: break; // Celeste effect depth. We don't do.
                    case 95: break; // Phaser effect depth. We don't do.
                    case 98: Ch[MidCh].lastlrpn=value; Ch[MidCh].nrpn=true; break;
                    case 99: Ch[MidCh].lastmrpn=value; Ch[MidCh].nrpn=true; break;
                    case 100:Ch[MidCh].lastlrpn=value; Ch[MidCh].nrpn=false; break;
                    case 101:Ch[MidCh].lastmrpn=value; Ch[MidCh].nrpn=false; break;
                    case 113: break; // Related to pitch-bender, used by missimp.mid in Duke3D
                    case  6: SetRPN(MidCh, value, true); break;
                    case 38: SetRPN(MidCh, value, false); break;

                    case 103: cmf_percussion_mode = value; break; // CMF (ctrl 0x67) rhythm mode
                    default:
                        UI.PrintLn("Ctrl %d <- %d (ch %u)", ctrlno, value, MidCh);
                }
                break;
            }
            case 0xC: // Patch change
                Ch[MidCh].patch = *data++;
                UI.SetChannel(MidCh, Ch[MidCh].patch, track_names[tk]);
                break;
            case 0xD: // Channel after-touch
            {
                // TODO: Verify, is this correct action?
                int  vol = *data++;
                // Set this pressure to all active notes on the channel
                for(auto& ch: Ch[MidCh].activenotes) { ch.second.vol = vol; }
                NoteUpdate_All(MidCh, Upd_Volume);
                break;
            }
            case 0xE: // Wheel/pitch bend
            {
                int a = *data++;
                int b = *data++;
                Ch[MidCh].bend = (a + b*128 - 8192) * Ch[MidCh].bendsense;
                NoteUpdate_All(MidCh, Upd_Pitch);
                break;
            }
        }
    }

    // Determine how good a candidate this adlchannel
    // would be for playing a note from this instrument.
    long CalculateAdlChannelGoodness
        (unsigned c, unsigned ins, unsigned /*MidCh*/) const
    {
        long s = -ch[c].koff_time_until_neglible;

        // Same midi-instrument = some stability
        //if(c == MidCh) s += 4;
        for(AdlChannel::users_t::const_iterator
            j = ch[c].users.begin();
            j != ch[c].users.end();
            ++j)
        {
            s -= 4000;
            if(!j->second.sustained)
                s -= j->second.kon_time_until_neglible;
            else
                s -= j->second.kon_time_until_neglible / 2;

            MIDIchannel::activenotemap_t::const_iterator
                k = Ch[j->first.MidCh].activenotes.find(j->first.note);
            if(k != Ch[j->first.MidCh].activenotes.end())
            {
                // Same instrument = good
                if(j->second.ins == ins)
                {
                    s += 300;
                    // Arpeggio candidate = even better
                    if(j->second.vibdelay < 70
                    || j->second.kon_time_until_neglible > 20000)
                        s += 0;
                }
                // Percussion is inferior to melody
                s += 50 * (k->second.midiins / 128);

                /*
                if(k->second.midiins >= 25
                && k->second.midiins < 40
                && j->second.ins != ins)
                {
                    s -= 14000; // HACK: Don't clobber the bass or the guitar
                }
                */
            }

            // If there is another channel to which this note
            // can be evacuated to in the case of congestion,
            // increase the score slightly.
            unsigned n_evacuation_stations = 0;
            for(unsigned c2 = 0; c2 < opl.NumChannels; ++c2)
            {
                if(c2 == c) continue;
                if(opl.four_op_category[c2]
                != opl.four_op_category[c]) continue;
                for(AdlChannel::users_t::const_iterator
                    m = ch[c2].users.begin();
                    m != ch[c2].users.end();
                    ++m)
                {
                    if(m->second.sustained)       continue;
                    if(m->second.vibdelay >= 200) continue;
                    if(m->second.ins != j->second.ins) continue;
                    n_evacuation_stations += 1;
                }
            }
            s += n_evacuation_stations * 4;
        }
        return s;
    }

    // A new note will be played on this channel using this instrument.
    // Kill existing notes on this channel (or don't, if we do arpeggio)
    void PrepareAdlChannelForNewNote(int c, int ins)
    {
        if(ch[c].users.empty()) return; // Nothing to do
        //bool doing_arpeggio = false;
        for(AdlChannel::users_t::iterator
            jnext = ch[c].users.begin();
            jnext != ch[c].users.end();
            )
        {
            AdlChannel::users_t::iterator j(jnext++);
            if(!j->second.sustained)
            {
                // Collision: Kill old note,
                // UNLESS we're going to do arpeggio

                MIDIchannel::activenoteiterator i
                ( Ch[j->first.MidCh].activenotes.find( j->first.note ) );

                // Check if we can do arpeggio.
                if((j->second.vibdelay < 70
                 || j->second.kon_time_until_neglible > 20000)
                && j->second.ins == ins)
                {
                    // Do arpeggio together with this note.
                    //doing_arpeggio = true;
                    continue;
                }

                KillOrEvacuate(c,j,i);
                // ^ will also erase j from ch[c].users.
            }
        }

        // Kill all sustained notes on this channel
        // Don't keep them for arpeggio, because arpeggio requires
        // an intact "activenotes" record. This is a design flaw.
        KillSustainingNotes(-1, c);

        // Keyoff the channel so that it can be retriggered,
        // unless the new note will be introduced as just an arpeggio.
        if(ch[c].users.empty())
            opl.NoteOff(c);
    }

    void KillOrEvacuate(
        unsigned from_channel,
        AdlChannel::users_t::iterator j,
        MIDIchannel::activenoteiterator i)
    {
        // Before killing the note, check if it can be
        // evacuated to another channel as an arpeggio
        // instrument. This helps if e.g. all channels
        // are full of strings and we want to do percussion.
        // FIXME: This does not care about four-op entanglements.
        for(unsigned c = 0; c < opl.NumChannels; ++c)
        {
            if(c == from_channel) continue;
            if(opl.four_op_category[c]
            != opl.four_op_category[from_channel]
              ) continue;
            for(AdlChannel::users_t::iterator
                m = ch[c].users.begin();
                m != ch[c].users.end();
                ++m)
            {
                if(m->second.vibdelay >= 200
                && m->second.kon_time_until_neglible < 10000) continue;
                if(m->second.ins != j->second.ins) continue;

                // the note can be moved here!
                UI.IllustrateNote(
                    from_channel,
                    i->second.tone,
                    i->second.midiins, 0, 0.0, j->first.MidCh);
                UI.IllustrateNote(
                    c,
                    i->second.tone,
                    i->second.midiins,
                    i->second.vol,
                    0.0, j->first.MidCh);

                i->second.phys.erase(from_channel);
                i->second.phys[c] = j->second.ins;
                ch[c].users.insert( *j );
                ch[from_channel].users.erase( j );
                return;
            }
        }

        /*UI.PrintLn(
            "collision @%u: [%ld] <- ins[%3u]",
            c,
            //ch[c].midiins<128?'M':'P', ch[c].midiins&127,
            ch[c].age, //adlins[ch[c].insmeta].ms_sound_kon,
            ins
            );*/

        // Kill it
        NoteUpdate(j->first.MidCh,
                   i,
                   Upd_Off,
                   from_channel);
    }

    void KillSustainingNotes(int MidCh = -1, int this_adlchn = -1)
    {
        unsigned first=0, last=opl.NumChannels;
        if(this_adlchn >= 0) { first=this_adlchn; last=first+1; }
        for(unsigned c = first; c < last; ++c)
        {
            if(ch[c].users.empty()) continue; // Nothing to do
            for(AdlChannel::users_t::iterator
                jnext = ch[c].users.begin();
                jnext != ch[c].users.end();
                )
            {
                AdlChannel::users_t::iterator j(jnext++);
                if((MidCh < 0 || j->first.MidCh == MidCh)
                && j->second.sustained)
                {
                    int midiins = '?';
                    UI.IllustrateNote(c, j->first.note, midiins, 0, 0.0, j->first.MidCh);
                    ch[c].users.erase(j);
                }
            }
            // Keyoff the channel, if there are no users left.
            if(ch[c].users.empty())
                opl.NoteOff(c);
        }
    }

    void SetRPN(unsigned MidCh, unsigned value, bool MSB)
    {
        bool nrpn = Ch[MidCh].nrpn;
        unsigned addr = Ch[MidCh].lastmrpn*0x100 + Ch[MidCh].lastlrpn;
        switch(addr + nrpn*0x10000 + MSB*0x20000)
        {
            case 0x0000 + 0*0x10000 + 1*0x20000: // Pitch-bender sensitivity
                Ch[MidCh].bendsense = value/8192.0;
                break;
            case 0x0108 + 1*0x10000 + 1*0x20000: // Vibrato speed
                if(value == 64)
                    Ch[MidCh].vibspeed = 1.0;
                else if(value < 100)
                    Ch[MidCh].vibspeed = 1.0/(1.6e-2*(value?value:1));
                else
                    Ch[MidCh].vibspeed = 1.0/(0.051153846*value-3.4965385);
                Ch[MidCh].vibspeed *= 2*3.141592653*5.0;
                break;
            case 0x0109 + 1*0x10000 + 1*0x20000: // Vibrato depth
                Ch[MidCh].vibdepth = ((value-64)*0.15)*0.01;
                break;
            case 0x010A + 1*0x10000 + 1*0x20000: // Vibrato delay in millisecons
                Ch[MidCh].vibdelay =
                    value ? long(0.2092 * std::exp(0.0795 * value)) : 0.0;
                break;

            default: UI.PrintLn("%s %04X <- %d (%cSB) (ch %u)",
                "NRPN"+!nrpn, addr, value, "LM"[MSB], MidCh);
        }
    }

    void UpdatePortamento(unsigned MidCh)
    {
        // mt = 2^(portamento/2048) * (1.0 / 5000.0)
        /*
        double mt = std::exp(0.00033845077 * Ch[MidCh].portamento);
        NoteUpdate_All(MidCh, Upd_Pitch);
        */
        UI.PrintLn("Portamento %u: %u (unimplemented)", MidCh, Ch[MidCh].portamento);
    }

    void NoteUpdate_All(unsigned MidCh, unsigned props_mask)
    {
        for(MIDIchannel::activenoteiterator
            i = Ch[MidCh].activenotes.begin();
            i != Ch[MidCh].activenotes.end();
            )
        {
            MIDIchannel::activenoteiterator j(i++);
            NoteUpdate(MidCh, j, props_mask);
        }
    }

    void NoteOff(unsigned MidCh, int note)
    {
        MIDIchannel::activenoteiterator
            i = Ch[MidCh].activenotes.find(note);
        if(i != Ch[MidCh].activenotes.end())
        {
            NoteUpdate(MidCh, i, Upd_Off);
        }
    }

    void UpdateVibrato(double amount)
    {
        for(unsigned a=0, b=Ch.size(); a<b; ++a)
            if(Ch[a].vibrato && !Ch[a].activenotes.empty())
            {
                NoteUpdate_All(a, Upd_Pitch);
                Ch[a].vibpos += amount * Ch[a].vibspeed;
            }
            else
                Ch[a].vibpos = 0.0;
    }

    void UpdateArpeggio(double /*amount*/) // amount = amount of time passed
    {
        // If there is an adlib channel that has multiple notes
        // simulated on the same channel, arpeggio them.
    #if 0
        const unsigned desired_arpeggio_rate = 40; // Hz (upper limit)
       #if 1
        static unsigned cache=0;
        amount=amount; // Ignore amount. Assume we get a constant rate.
        cache += MaxSamplesAtTime * desired_arpeggio_rate;
        if(cache < PCM_RATE) return;
        cache %= PCM_RATE;
      #else
        static double arpeggio_cache = 0;
        arpeggio_cache += amount * desired_arpeggio_rate;
        if(arpeggio_cache < 1.0) return;
        arpeggio_cache = 0.0;
      #endif
    #endif
        static unsigned arpeggio_counter = 0;
        ++arpeggio_counter;

        for(unsigned c = 0; c < opl.NumChannels; ++c)
        {
        retry_arpeggio:;
            size_t n_users = ch[c].users.size();
            /*if(true)
            {
                UI.GotoXY(64,c+1); UI.Color(2);
                std::fprintf(stderr, "%7ld/%7ld,%3u\r",
                    ch[c].keyoff,
                    (unsigned) n_users);
                UI.x = 0;
            }*/
            if(n_users > 1)
            {
                AdlChannel::users_t::const_iterator i = ch[c].users.begin();
                size_t rate_reduction = 3;
                if(n_users >= 3) rate_reduction = 2;
                if(n_users >= 4) rate_reduction = 1;
                std::advance(i, (arpeggio_counter / rate_reduction) % n_users);
                if(i->second.sustained == false)
                {
                    if(i->second.kon_time_until_neglible <= 0l)
                    {
                        NoteUpdate(
                            i->first.MidCh,
                            Ch[ i->first.MidCh ].activenotes.find( i->first.note ),
                            Upd_Off,
                            c);
                        goto retry_arpeggio;
                    }
                    NoteUpdate(
                        i->first.MidCh,
                        Ch[ i->first.MidCh ].activenotes.find( i->first.note ),
                        Upd_Pitch | Upd_Volume | Upd_Pan,
                        c);
                }
            }
        }
    }

public:
    unsigned ChooseDevice(const std::string& name)
    {
        std::map<std::string, unsigned>::iterator i = devices.find(name);
        if(i != devices.end()) return i->second;
        size_t n = devices.size() * 16;
        if(n > std::numeric_limits<unsigned short>::max()-15u)
            throw std::runtime_error("Too many MIDI devices");
        devices.insert( std::make_pair(name, n) );
        Ch.resize(n+16);
        return n;
    }
};

#ifndef __DJGPP__
struct Reverb /* This reverb implementation is based on Freeverb impl. in Sox */
{
    float feedback, hf_damping, gain;
    struct FilterArray
    {
        struct Filter
        {
            std::vector<float> Ptr;  size_t pos;  float Store;
            void Create(size_t size) { Ptr.resize(size); pos = 0; Store = 0.f; }
            float Update(float a, float b)
            {
                Ptr[pos] = a;
                if(!pos) pos = Ptr.size()-1; else --pos;
                return b;
            }
            float ProcessComb(float input, const float feedback, const float hf_damping)
            {
                Store = Ptr[pos] + (Store - Ptr[pos]) * hf_damping;
                return Update(input + feedback * Store, Ptr[pos]);
            }
            float ProcessAllPass(float input)
            {
                return Update(input + Ptr[pos] * .5f, Ptr[pos]-input);
            }
        } comb[8], allpass[4];
        void Create(double rate, double scale, double offset)
        {
            /* Filter delay lengths in samples (44100Hz sample-rate) */
            static const int comb_lengths[8] = {1116,1188,1277,1356,1422,1491,1557,1617};
            static const int allpass_lengths[4] = {225,341,441,556};
            double r = rate * (1 / 44100.0); // Compensate for actual sample-rate
            const int stereo_adjust = 12;
            for(size_t i=0; i<8; ++i, offset=-offset)
                comb[i].Create( scale * r * (comb_lengths[i] + stereo_adjust * offset) + .5 );
            for(size_t i=0; i<4; ++i, offset=-offset)
                allpass[i].Create( r * (allpass_lengths[i] + stereo_adjust * offset) + .5 );
        }
        void Process(size_t length,
            const std::deque<float>& input, std::vector<float>& output,
            const float feedback, const float hf_damping, const float gain)
        {
            for(size_t a=0; a<length; ++a)
            {
                float out = 0, in = input[a];
                for(size_t i=8; i-- > 0; ) out += comb[i].ProcessComb(in, feedback, hf_damping);
                for(size_t i=4; i-- > 0; ) out += allpass[i].ProcessAllPass(out);
                output[a] = out * gain;
            }
        }
    } chan[2];
    std::vector<float> out[2];
    std::deque<float> input_fifo;

    void Create(double sample_rate_Hz,
        float wet_gain_dB,
        float room_scale, float reverberance, float fhf_damping, /* 0..1 */
        float pre_delay_s, float stereo_depth,
        size_t buffer_size)
    {
        size_t delay = pre_delay_s  * sample_rate_Hz + .5;
        double scale = room_scale * .9 + .1;
        double depth = stereo_depth;
        double a =  -1 /  std::log(1 - /**/.3 /**/);          // Set minimum feedback
        double b = 100 / (std::log(1 - /**/.98/**/) * a + 1); // Set maximum feedback
        feedback = 1 - std::exp((reverberance*100.0 - b) / (a * b));
        hf_damping = fhf_damping * .3 + .2;
        gain = std::exp(wet_gain_dB * (std::log(10.0) * 0.05)) * .015;
        input_fifo.insert(input_fifo.end(), delay, 0.f);
        for(size_t i = 0; i < 2; ++i)
        {
            chan[i].Create(sample_rate_Hz, scale, i * depth);
            out[i].resize(buffer_size);
        }
    }
    void Process(size_t length)
    {
        for(size_t i=0; i<2; ++i)
            if(!out[i].empty())
                chan[i].Process(length,
                    input_fifo,
                    out[i], feedback, hf_damping, gain);
        input_fifo.erase(input_fifo.begin(), input_fifo.begin() + length);
    }
};
static struct ReverbSettings
{
    float do_reverb = 1.f;
    float wet_gain_db = 24.f;
    float room_scale = .7f;
    float reverberance = .6f;
    float hf_damping = .8f;
    float pre_delay_s = 0.f;
    float stereo_depth = 1.f;
} ReverbSpecs;
static struct MyReverbData
{
    bool enabled;
    Reverb chan[2];

    MyReverbData()
    {
        ReInit();
    }
    void ReInit()
    {
        enabled = ReverbSpecs.do_reverb > 0;
        for(std::size_t i=0; i<2; ++i)
        {
            chan[i] = Reverb();
            chan[i].Create(PCM_RATE,
                ReverbSpecs.wet_gain_db,
                ReverbSpecs.room_scale,
                ReverbSpecs.reverberance,
                ReverbSpecs.hf_damping,
                ReverbSpecs.pre_delay_s,
                ReverbSpecs.stereo_depth,
                MaxSamplesAtTime);
        }
    }
} reverb_data;

static bool ParseNumber(const char* text, double minimum, double maximum, double& value)
{
    const char* first = (*text == '-' || *text == '+') ? text+1 : text;
    if((*first < '0' || *first > '9') && *first != '.') return false;
    char* end = nullptr;
    errno = 0;
    value = std::strtod(text, &end);
    return end != text && !*end && errno != ERANGE
        && value >= minimum && value <= maximum;
}

static bool ParseReverb(std::string_view specs)
{
    // Parse into a copy so a bad term never leaves partially applied settings.
    auto next = ReverbSpecs;
    if(specs.find_first_not_of(':') == specs.npos)
    {
        std::fprintf(stderr, "-reverb requires settings or 'none'.\n");
        return false;
    }
    while(!specs.empty())
    {
        const auto colon = specs.find(':');
        const auto term = specs.substr(0, colon);
        if(colon == specs.npos) specs = {}; else specs.remove_prefix(colon+1);
        if(term.empty()) continue;
        const auto eq = term.find('=');
        const auto key = term.substr(0, eq);
        if(key == "none" || key == "off" || key == "false" || key == "no" || key == "n")
        {
            if(eq != term.npos)
            {
                std::fprintf(stderr, "Reverb '%.*s' takes no value.\n", int(key.size()), key.data());
                return false;
            }
            next.do_reverb = 0;
            continue;
        }
        struct Setting { std::string_view names; float ReverbSettings::* field; double low, high; };
        static const Setting settings[] = {
            {"|gain|wet_gain|wetgain|wet|wet-gain|g|", &ReverbSettings::wet_gain_db, -60, 60},
            {"|room|room_scale|scale|room-scale|roomscale|r|", &ReverbSettings::room_scale, 0, 1},
            {"|factor|reverberance|reverb|amount|f|", &ReverbSettings::reverberance, 0, 1},
            {"|hf|hf_damping|hf-damping|damping|dampening|damp|d|", &ReverbSettings::hf_damping, 0, 1},
            {"|delay|pre_delay|pre-delay|predelay|wait|seconds|w|", &ReverbSettings::pre_delay_s, 0, .5},
            {"|stereo|depth|stereo_depth|stereo-depth|stereodepth|width|wide|s|", &ReverbSettings::stereo_depth, 0, 1}
        };
        const Setting* setting = nullptr;
        const std::string name = "|" + std::string(key) + "|";
        if(key.find('|') == key.npos)
            for(const auto& candidate: settings)
                if(candidate.names.find(name) != candidate.names.npos) setting = &candidate;
        if(!setting)
        {
            std::fprintf(stderr, "Unknown reverb setting: %.*s\n", int(key.size()), key.data());
            return false;
        }
        double value;
        if(eq == term.npos || !ParseNumber(std::string(term.substr(eq+1)).c_str(),
                                           setting->low, setting->high, value))
        {
            std::fprintf(stderr, "Reverb %.*s requires a number between %g and %g.\n",
                int(key.size()), key.data(), setting->low, setting->high);
            return false;
        }
        next.*(setting->field) = value;
    }
    ReverbSpecs = next;
    std::fprintf(stderr, "Reverb settings: Enabled=%g gain=%g room=%g reverb=%g damping=%g delay=%g stereo=%g\n",
        next.do_reverb, next.wet_gain_db, next.room_scale, next.reverberance,
        next.hf_damping, next.pre_delay_s, next.stereo_depth);
    return true;
}

#ifdef __WIN32__
namespace WindowsAudio
{
  static const unsigned BUFFER_COUNT = 16;
  static const unsigned BUFFER_SIZE  = 8192;
  static HWAVEOUT hWaveOut;
  static WAVEHDR headers[BUFFER_COUNT];
  static volatile unsigned buf_read=0, buf_write=0;

  static void CALLBACK Callback(HWAVEOUT,UINT msg,DWORD,DWORD,DWORD)
  {
      if(msg == WOM_DONE)
      {
          buf_read = (buf_read+1) % BUFFER_COUNT;
      }
  }
  static void Open(const int rate, const int channels, const int bits)
  {
      WAVEFORMATEX wformat;
      MMRESULT result;

      //fill waveformatex
      memset(&wformat, 0, sizeof(wformat));
      wformat.nChannels       = channels;
      wformat.nSamplesPerSec  = rate;
      wformat.wFormatTag      = WAVE_FORMAT_PCM;
      wformat.wBitsPerSample  = bits;
      wformat.nBlockAlign     = wformat.nChannels * (wformat.wBitsPerSample >> 3);
      wformat.nAvgBytesPerSec = wformat.nSamplesPerSec * wformat.nBlockAlign;

      //open sound device
      //WAVE_MAPPER always points to the default wave device on the system
      result = waveOutOpen
      (
        &hWaveOut,WAVE_MAPPER,&wformat,
        (DWORD_PTR)Callback,0,CALLBACK_FUNCTION
      );
      if(result == WAVERR_BADFORMAT)
      {
          fprintf(stderr, "ao_win32: format not supported\n");
          return;
      }
      if(result != MMSYSERR_NOERROR)
      {
          fprintf(stderr, "ao_win32: unable to open wave mapper device\n");
          return;
      }
      char* buffer = new char[BUFFER_COUNT*BUFFER_SIZE];
      std::memset(headers,0,sizeof(headers));
      std::memset(buffer, 0,BUFFER_COUNT*BUFFER_SIZE);
      for(unsigned a=0; a<BUFFER_COUNT; ++a)
          headers[a].lpData = buffer + a*BUFFER_SIZE;
  }
  static void Close()
  {
      waveOutReset(hWaveOut);
      waveOutClose(hWaveOut);
  }
  static void Write(const unsigned char* Buf, unsigned len)
  {
      static std::vector<unsigned char> cache;
      size_t cache_reduction = 0;
      if(0&&len < BUFFER_SIZE&&cache.size()+len<=BUFFER_SIZE)
      {
          cache.insert(cache.end(), Buf, Buf+len);
          Buf = &cache[0];
          len = cache.size();
          if(len < BUFFER_SIZE/2)
              return;
          cache_reduction = cache.size();
      }

      while(len > 0)
      {
          unsigned buf_next = (buf_write+1) % BUFFER_COUNT;
          WAVEHDR* Work = &headers[buf_write];
          while(buf_next == buf_read)
          {
              /*UI.Color(4);
              UI.GotoXY(60,-5+5); fprintf(stderr, "waits\r"); UI.x=0; std::fflush(stderr);
              UI.Color(4);
              UI.GotoXY(60,-4+5); fprintf(stderr, "r%u w%u n%u\r",buf_read,buf_write,buf_next); UI.x=0; std::fflush(stderr);
              */
              /* Wait until at least one of the buffers is free */
              Sleep(0);
              /*UI.Color(2);
              UI.GotoXY(60,-3+5); fprintf(stderr, "wait completed\r"); UI.x=0; std::fflush(stderr);*/
          }

          unsigned npending = (buf_write + BUFFER_COUNT - buf_read) % BUFFER_COUNT;
          static unsigned counter=0, lo=0;
          if(!counter-- || npending < lo) { lo = npending; counter=100; }

          if(!DoingInstrumentTesting)
          {
              if(UI.maxy >= 5) {
                  UI.Color(9);
                  UI.GotoXY(70,-5+6); fprintf(stderr, "%3u bufs\r", (unsigned)npending); UI.x=0; std::fflush(stderr);
                  UI.GotoXY(71,-4+6); fprintf(stderr, "lo:%3u\r", lo); UI.x=0; }
          }

          //unprepare the header if it is prepared
          if(Work->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(hWaveOut, Work, sizeof(WAVEHDR));
          unsigned x = BUFFER_SIZE; if(x > len) x = len;
          std::memcpy(Work->lpData, Buf, x);
          Buf += x; len -= x;
          //prepare the header and write to device
          Work->dwBufferLength = x;
          {int err=waveOutPrepareHeader(hWaveOut, Work, sizeof(WAVEHDR));
           if(err != MMSYSERR_NOERROR) fprintf(stderr, "waveOutPrepareHeader: %d\n", err);}
          {int err=waveOutWrite(hWaveOut, Work, sizeof(WAVEHDR));
           if(err != MMSYSERR_NOERROR) fprintf(stderr, "waveOutWrite: %d\n", err);}
          buf_write = buf_next;
          //if(npending>=BUFFER_COUNT-2)
          //    buf_read=(buf_read+1)%BUFFER_COUNT; // Simulate a read
      }
      if(cache_reduction)
          cache.erase(cache.begin(), cache.begin()+cache_reduction);
  }
}
#else
static std::deque<short> AudioBuffer;
static std::mutex AudioBuffer_lock;
static std::condition_variable AudioBuffer_drained; // Signalled by the callback
static constexpr unsigned StopFadeFrames = PCM_RATE / 50; // 20 ms
static std::atomic<bool> AudioStopping{false};
static std::atomic<unsigned> StopFadeRemaining{0};
static void AdlAudioCallback(void*, Uint8* stream, int len)
{
    // SDL does not require the supplied buffer to contain silence already.
    // Always initialize the tail if the producer has not queued enough audio.
    std::memset(stream, 0, len);
    short* target = (short*) stream;
    {
        std::lock_guard lock(AudioBuffer_lock);
        /*if(len != AudioBuffer.size())
            fprintf(stderr, "len=%d stereo samples, AudioBuffer has %u stereo samples",
                len/4, (unsigned) AudioBuffer.size()/2);*/
        unsigned ate = len/2; // number of shorts
        if(ate > AudioBuffer.size()) ate = AudioBuffer.size();
        for(unsigned a=0; a<ate; ++a)
            target[a] = AudioBuffer[a];
        AudioBuffer.erase(AudioBuffer.begin(), AudioBuffer.begin() + ate);
        //fprintf(stderr, " - remain %u\n", (unsigned) AudioBuffer.size()/2);
    }
    // Wake the main loop, which sleeps while the queue is full.
    AudioBuffer_drained.notify_one();
    if(AudioStopping.load())
    {
        unsigned remaining = StopFadeRemaining.load();
        for(unsigned frame=0; frame<unsigned(len)/4; ++frame)
        {
            float gain = remaining / float(StopFadeFrames);
            target[frame*2]   *= gain;
            target[frame*2+1] *= gain;
            if(remaining) --remaining;
        }
        StopFadeRemaining.store(remaining);
    }
}
#endif // WIN32

// Streams 16-bit stereo PCM into a WAV file. The length fields of the header
// are filled in by Close(), so a block of audio costs a single fwrite.
class WavWriter
{
    static constexpr long HeaderSize = 44;
    std::FILE* fp = nullptr;

    void Put(std::string_view tag) { std::fwrite(tag.data(), 1, tag.size(), fp); }
    void Put(std::uint32_t value, unsigned nbytes) // Little-endian
    {
        for(unsigned n=0; n<nbytes; ++n) std::fputc((value >> (n*8)) & 0xFF, fp);
    }
    void PutHeader(std::uint32_t datasize)
    {
        constexpr unsigned channels = 2, bits = 16, blockalign = channels * bits / 8;
        Put("RIFF"); Put(HeaderSize - 8 + datasize, 4);
        Put("WAVE");
        Put("fmt "); Put(16, 4);        // fmt subchunk, which is 16 bytes:
        Put(1, 2);                      //   PCM
        Put(channels, 2);
        Put(PCM_RATE, 4);               //   sampling rate
        Put(PCM_RATE * blockalign, 4);  //   byte rate
        Put(blockalign, 2);
        Put(bits, 2);
        Put("data"); Put(datasize, 4);
    }
public:
    WavWriter() = default;
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    ~WavWriter() { Close(); }

    bool Open(const std::string& path) // "-" = stdout
    {
        fp = path == "-" ? stdout : std::fopen(path.c_str(), "wb");
        if(fp) PutHeader(0);
        return fp != nullptr;
    }
    void Write(const short* samples, std::size_t n)
    {
        if(fp) std::fwrite(samples, sizeof(*samples), n, fp);
    }
    void Close()
    {
        if(!fp) return;
        // A pipe cannot be rewound; the lengths stay zero there.
        const long end = std::ftell(fp);
        if(end >= HeaderSize && std::fseek(fp, 0, SEEK_SET) == 0)
        {
            PutHeader(end - HeaderSize);
            std::fseek(fp, end, SEEK_SET);
        }
        if(fp == stdout) std::fflush(fp); else std::fclose(fp);
        fp = nullptr;
    }
};
static WavWriter WavOutput;

static float PreviousAudioInput[2] = {0,0}, PreviousAudioOutput[2] = {0,0};

static void SendStereoAudio(unsigned long count, int* samples)
{
    if(count > MaxSamplesAtTime)
    {
        SendStereoAudio(MaxSamplesAtTime, samples);
        SendStereoAudio(count-MaxSamplesAtTime, samples+MaxSamplesAtTime*2);
        return;
    }
#if 0
    if(count % 2 == 1)
    {
        // An uneven number of samples? To avoid complicating matters,
        // just ignore the odd sample.
        count   -= 1;
        samples += 1;
    }
#endif
    if(!count) return;

    // Average is used only for the level meters. DC is removed per sample below.
    double average[2]={0,0};
    for(unsigned w=0; w<2; ++w)
        for(unsigned long p = 0; p < count; ++p)
            average[w] += samples[p*2+w];
    // Figure out the amplitude of both channels
    if(!DoingInstrumentTesting)
    {
        static unsigned amplitude_display_counter = 0;
        if(!amplitude_display_counter--)
        {
            amplitude_display_counter = (PCM_RATE / count) / 24;
            double amp[2]={0,0};
            for(unsigned w=0; w<2; ++w)
            {
                average[w] /= double(count);
                for(unsigned long p = 0; p < count; ++p)
                    amp[w] += std::fabs(samples[p*2+w] - average[w]);
                amp[w] /= double(count);
                // Turn into logarithmic scale
                const double dB = std::log(amp[w]<1 ? 1 : amp[w]) * 4.328085123;
                const double maxdB = 3*16; // = 3 * log2(65536)
                amp[w] = dB/maxdB;
            }
            UI.IllustrateVolumes(amp[0], amp[1]);
        }
    }

    //static unsigned counter = 0; if(++counter < 8000)  return;

#if defined(__WIN32__) && 0
    // Cheat on dosbox recording: easier on the cpu load.
   {count*=2;
    std::vector<short> AudioBuffer(count);
    for(unsigned long p = 0; p < count; ++p)
        AudioBuffer[p] = samples[p];
    WindowsAudio::Write( (const unsigned char*) &AudioBuffer[0], count*2);
    return;}
#endif

    // Remove DC before reverb so offsets do not consume its headroom.
    static const float dc_pole = std::exp(-2.0 * 3.141592653589793 * 5.0 / PCM_RATE);
    std::vector<float> dry[2];
    for(unsigned w=0; w<2; ++w)
    {
        dry[w].resize(count);
        for(unsigned long p = 0; p < count; ++p)
        {
            float input = samples[p*2+w];
            float filtered = input - PreviousAudioInput[w] + dc_pole * PreviousAudioOutput[w];
            PreviousAudioInput[w] = input;
            PreviousAudioOutput[w] = filtered;
            dry[w][p] = filtered * double(0.3/32768.0);
        }
        // ^  Note: ftree-vectorize causes an error in this loop on g++-4.4.5
        if(reverb_data.enabled)
            reverb_data.chan[w].input_fifo.insert(
                reverb_data.chan[w].input_fifo.end(), dry[w].begin(), dry[w].end());
    }
    // Reverbify it
    if(reverb_data.enabled)
        for(unsigned w=0; w<2; ++w)
            reverb_data.chan[w].Process(count);

    // Convert to signed 16-bit int format
    std::array<short, MaxSamplesAtTime*2> block; // count <= MaxSamplesAtTime here
    static const float output_gain = std::pow(10.0, OutputGainDb / 20.0);
    for(unsigned long p = 0; p < count; ++p)
    {
        float out[2];
        for(unsigned w=0; w<2; ++w)
        {
            // Preserve the direct signal; reverb adds ambience rather than
            // replacing the instrument with an effect-only signal.
            float wet = reverb_data.enabled
                ? .5f * (reverb_data.chan[0].out[w][p] + reverb_data.chan[1].out[w][p])
                : 0;
            out[w] = (dry[w][p] + .25f * wet) * output_gain;
        }
        // Gently limit loud peaks, with the same gain in both channels to
        // preserve stereo balance. Normal levels remain linear.
        float peak = std::max(std::fabs(out[0]), std::fabs(out[1]));
        const float knee = .9f, ceiling = 32766.f / 32768.f;
        if(peak > knee)
        {
            float limited = knee + (ceiling-knee) * std::tanh((peak-knee)/(ceiling-knee));
            float scale = limited / peak;
            out[0] *= scale;
            out[1] *= scale;
        }
        for(unsigned w=0; w<2; ++w)
        {
            float pcm = out[w] * 32768.f;
            block[p*2+w] =
                pcm<-32768.f ? -32768 :
                pcm>32767.f ?  32767 : pcm;
        }
    }
    if(WritePCMfile)
        WavOutput.Write(block.data(), count*2);
    else
    {
        // Put to playback queue
#ifdef __WIN32__
        WindowsAudio::Write( (const unsigned char*) block.data(), count*2*sizeof(short));
#else
        std::lock_guard lock(AudioBuffer_lock);
        AudioBuffer.insert(AudioBuffer.end(), block.begin(), block.begin() + count*2);
#endif
    }
#ifdef SUPPORT_VIDEO_OUTPUT
    if(WriteVideoFile)
    {
        static constexpr unsigned framerate = 15;
        static FILE* fp = nullptr;
        static unsigned long samples_carry = 0;

        if(!fp)
        {
            std::string cmdline =
                "ffmpeg -f rawvideo"
                " -pixel_format bgra "
                " -video_size " + std::to_string(UI.VidWidth) + "x" + std::to_string(UI.VidHeight) +
                " -framerate " + std::to_string(framerate) +
                " -i -"
                " -c:v h264"
                " -aspect " + std::to_string(UI.VidWidth) + "/" + std::to_string(UI.VidHeight) +
                " -pix_fmt yuv420p"
                " -preset superfast -partitions all -refs 2 -tune animation -y '" + VidFilepath + "'"; // FIXME: escape filename
            cmdline += " >/dev/null 2>/dev/null";
            fp = popen(cmdline.c_str(), "w");
        }
        if(fp)
        {
            samples_carry += count;
            while(samples_carry >= PCM_RATE / framerate)
            {
                UI.VidRender();

                const unsigned char* source = (const unsigned char*)UI.PixelBuffer.data();
                std::size_t bytes_remain    = UI.PixelBuffer.size() * sizeof(UI.PixelBuffer[0]);
                while(bytes_remain)
                {
                    int r = std::fwrite(source, 1, bytes_remain, fp);
                    if(r == 0) break;
                    bytes_remain -= r;
                    source       += r;
                }
                samples_carry -= PCM_RATE / framerate;
            }
        }
    }
#endif
}
#endif /* not DJGPP */

class Tester
{
    unsigned cur_gm;
    unsigned ins_idx;
    std::vector<unsigned> adl_ins_list;
    OPL3& opl;
public:
    Tester(OPL3& o) : opl(o)
    {
        cur_gm   = 0;
        ins_idx  = 0;
    }
    ~Tester()
    {
    }

    // Find list of adlib instruments that supposedly implement this GM
    void FindAdlList()
    {
        const unsigned NumBanks = sizeof(banknames)/sizeof(*banknames);

        std::set<unsigned> adl_ins_set;
        for(unsigned bankno=0; bankno<NumBanks; ++bankno)
            adl_ins_set.insert(banks[bankno][cur_gm]);
        adl_ins_list.assign( adl_ins_set.begin(), adl_ins_set.end() );
        ins_idx = 0;
        NextAdl(0);
        opl.Silence();
    }

    void DoNote(int note)
    {
        if(adl_ins_list.empty()) FindAdlList();
        const unsigned meta = adl_ins_list[ins_idx];
        const adlinsdata& ains = GetAdlMetaIns(meta);

        int tone = (cur_gm & 128) ? (cur_gm & 127) : (note+50);
        if(ains.tone)
        {
            if(ains.tone < 20)
                tone += ains.tone;
            else if(ains.tone < 128)
                tone = ains.tone;
            else
                tone -= ains.tone-128;
        }
        double hertz = 172.00093 * std::exp(0.057762265 * (tone + 0.0));
        int i[2] = { ains.adlno1, ains.adlno2 };
        int adlchannel[2] = { 0, 3 };
        if(i[0] == i[1])
        {
            adlchannel[1] = -1;
            adlchannel[0] = 6; // single-op
            std::printf("noteon at %d(%d) for %g Hz\n",
                adlchannel[0], i[0], hertz);
        }
        else
        {
            std::printf("noteon at %d(%d) and %d(%d) for %g Hz\n",
                adlchannel[0], i[0], adlchannel[1], i[1], hertz);
        }

        opl.NoteOff(0); opl.NoteOff(3); opl.NoteOff(6);
        for(unsigned c=0; c<2; ++c)
        {
            if(adlchannel[c] < 0) continue;
            opl.Patch(adlchannel[c], i[c]);
            opl.Touch(adlchannel[c], 127*127*100);
            opl.Pan(adlchannel[c], 0x30);
            opl.NoteOn(adlchannel[c], hertz);
        }
    }

    void NextGM(int offset)
    {
        cur_gm = (cur_gm + 256 + offset) & 0xFF;
        FindAdlList();
    }

    void NextAdl(int offset)
    {
        if(adl_ins_list.empty()) FindAdlList();
        const unsigned NumBanks = sizeof(banknames)/sizeof(*banknames);
        ins_idx = (ins_idx + adl_ins_list.size() + offset) % adl_ins_list.size();

        UI.Color(15); std::fflush(stderr);
        std::printf("SELECTED G%c%d\t%s\n",
            cur_gm<128?'M':'P', cur_gm<128?cur_gm+1:cur_gm-128,
            "<-> select GM, ^v select ins, qwe play note");
        std::fflush(stdout);
        UI.Color(7); std::fflush(stderr);
        for(unsigned a=0; a<adl_ins_list.size(); ++a)
        {
            const unsigned i = adl_ins_list[a];
            const adlinsdata& ains = GetAdlMetaIns(i);

            char ToneIndication[8] = "   ";
            if(ains.tone)
            {
                if(ains.tone < 20)
                    sprintf(ToneIndication, "+%-2d", ains.tone);
                else if(ains.tone < 128)
                    sprintf(ToneIndication, "=%-2d", ains.tone);
                else
                    sprintf(ToneIndication, "-%-2d", ains.tone-128);
            }
            std::printf("%s%s%s%u\t",
                ToneIndication,
                ains.adlno1 != ains.adlno2 ? "[2]" : "   ",
                (ins_idx == a) ? "->" : "\t",
                i
            );

            for(unsigned bankno=0; bankno<NumBanks; ++bankno)
                if(banks[bankno][cur_gm] == i)
                    std::printf(" %u", bankno);

            std::printf("\n");
        }
    }

    void HandleInputChar(char ch)
    {
        static const char notes[] = "zsxdcvgbhnjmq2w3er5t6y7ui9o0p";
        //                           c'd'ef'g'a'bC'D'EF'G'A'Bc'd'e
        switch(ch)
        {
            case '/': case 'H': case 'A': NextAdl(-1); break;
            case '*': case 'P': case 'B': NextAdl(+1); break;
            case '-': case 'K': case 'D': NextGM(-1); break;
            case '+': case 'M': case 'C': NextGM(+1); break;
            case 3:
        #if !((!defined(__WIN32__) || defined(__CYGWIN__)) && !defined(__DJGPP__))
            case 27:
        #endif
                QuitFlag=true; break;
            default:
                const char* p = strchr(notes, ch);
                if(p && *p) DoNote( (p - notes) - 12 );
    }   }

    double Tick(double /*eat_delay*/, double /*mindelay*/)
    {
        HandleInputChar( Input.PeekInput() );
        //return eat_delay;
        return 0.1;
    }
};

static void TidyupAndExit(int)
{
    // Let the playback loop fade and close audio, then restore the terminal.
    QuitFlag = true;
}

#ifdef __WIN32__
/* Parse a command line buffer into arguments */
static void UnEscapeQuotes( char *arg )
{
    for(char *last=0; *arg != '\0'; last=arg++)
        if( *arg == '"' && *last == '\\' ) {
            char *c_last = last;
            for(char*c_curr=arg; *c_curr; ++c_curr) {
                *c_last = *c_curr;
                c_last = c_curr;
            }
            *c_last = '\0';
        }
}
static int ParseCommandLine(char *cmdline, char **argv)
{
    char *bufp, *lastp=NULL;
    int argc=0, last_argc=0;
    for (bufp = cmdline; *bufp; ) {
        /* Skip leading whitespace */
        while ( std::isspace(*bufp) ) ++bufp;
        /* Skip over argument */
        if ( *bufp == '"' ) {
            ++bufp;
            if ( *bufp ) { if (argv) argv[argc]=bufp; ++argc; }
            /* Skip over word */
            while ( *bufp && ( *bufp != '"' || *lastp == '\\' ) ) {
                lastp = bufp;
                ++bufp;
            }
        } else {
            if ( *bufp ) { if (argv) argv[argc] = bufp; ++argc; }
            /* Skip over word */
            while ( *bufp && ! std::isspace(*bufp) ) ++bufp;
        }
        if(*bufp) { if(argv) *bufp = '\0'; ++bufp; }
        /* Strip out \ from \" sequences */
        if( argv && last_argc != argc ) UnEscapeQuotes( argv[last_argc]);
        last_argc = argc;
    }
    if(argv) argv[argc] = 0;
    return(argc);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw)
{
    extern int main(int,char**);
    char* cmdline = GetCommandLineA();
    int argc = ParseCommandLine(cmdline, NULL);
    char**argv = new char* [argc+1];
    ParseCommandLine(cmdline, argv);
#else
#undef main

int main(int argc, char** argv)
{
#endif
    // How long is SDL buffer, in seconds?
    // The smaller the value, the more often AdlAudioCallBack()
    // is called.
    const double AudioBufferLength = 0.045;
    // How much do WE buffer, in seconds? The smaller the value,
    // the more prone to sound chopping we are.
    const double OurHeadRoomLength = 0.1;
    // The lag between visual content and audio content equals
    // the sum of these two buffers.

    WritingToTTY = isatty(STDOUT_FILENO);
    if (WritingToTTY)
    {
        UI.Print(0, 15,true,
#ifdef __DJGPP__
            "ADLMIDI_A: MIDI player for OPL3 hardware"
#else
            "ADLMIDI: MIDI (etc.) player with OPL3 emulation"
#endif
        );
    }
    if (WritingToTTY)
    {
        UI.Print(0,3, true, "(C) -- http://iki.fi/bisqwit/source/adlmidi.html");
    }
    UI.Color(7); std::fflush(stderr);

    signal(SIGINT, TidyupAndExit);

    if(argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")
    {
        UI.Color(7);  std::fflush(stderr);
        std::printf(
            "Usage: adlmidi <midifilename-or-folder> [ <options> ] [ <banknumber> [ <numcards> [ <numfourops>] ] ]\n"
            "       adlmidi <midifilename> -1   To enter instrument tester\n"
            " -p              Enables adlib percussion instrument mode (use with CMF files)\n"
            " -t              Enables tremolo amplification mode\n"
            " -v              Enables vibrato amplification mode\n"
            " -s              Enables scaling of modulator volumes\n"
            " Folders: play .mid/.midi/.rmi files alphabetically; Left/Right selects prev/next.\n"
            " -nl             Quit after one song, or one pass through a folder\n"
            " -game           Show the puzzle game (disabled by default)\n"
            " -no-game        Hide the puzzle game\n"
#ifndef __DJGPP__
            " -reverb <specs> Controls reverb (default: gain=24:room=.7:factor=.6:damping=.8:predelay=0:stereo=1)\n"
            " -reverb none    Disables reverb (also -nr)\n"
            " -gain <dB>      Output gain in dB (-60..30, default: 12)\n"
#endif
            " -w [<filename>] Write WAV file rather than playing\n"
#ifdef SUPPORT_VIDEO_OUTPUT
            " -d [<filename>] Write video file using ffmpeg\n"
#endif
        );
        for(unsigned a=0; a<sizeof(banknames)/sizeof(*banknames); ++a)
            std::printf("%10s%2u = %s\n",
                a?"":"Banks:",
                a,
                banknames[a]);
        std::printf(
            "     Use banks 2-5 to play Descent \"q\" soundtracks.\n"
            "     Look up the relevant bank number from descent.sng.\n"
            "\n"
            "     Default: bank %u (Bisqwit), %u OPL3 cards; automatic four-op allocation.\n"
            "\n"
            "     <numfourops> can be used to specify the number\n"
            "     of four-op channels to use. Each four-op channel eats\n"
            "     the room of two regular channels. Use as many as required.\n"
            "     The Doom & Hexen sets require one or two, while\n"
            "     Miles four-op set requires the maximum of numcards*6.\n"
            "\n"
            "     When playing Creative Music Files (CMF), try the\n"
            "     -p and -v options if it sounds wrong otherwise.\n"
            "\n", AdlBank, NumCards
            );
        UI.ShowCursor();
        return 0;
    }

    std::srand(std::time(0));

    std::vector<const char*> positional;
    for(int arg=2; arg<argc; ++arg)
    {
        const std::string_view option = argv[arg];
        if(option == "-p") AdlPercussionMode = true;
        else if(option == "-v") HighVibratoMode = true;
        else if(option == "-t") HighTremoloMode = true;
        else if(option == "-nl") QuitWithoutLooping = true;
        else if(option == "-no-game") ShowPuzzleGame = false;
        else if(option == "-game") ShowPuzzleGame = true;
        else if(option == "-s") ScaleModulators = true;
#ifndef __DJGPP__
        else if(option == "-nr") ParseReverb("none");
        else if(option == "-gain")
        {
            if(arg+1 >= argc || !ParseNumber(argv[arg+1], -60, 30, OutputGainDb))
            {
                std::fprintf(stderr, "-gain requires a number in dB between -60 and 30.\n");
                return 1;
            }
            ++arg;
        }
        else if(option == "-reverb")
        {
            if(arg+1 >= argc)
            {
                std::fprintf(stderr, "-reverb requires settings or 'none'.\n");
                return 1;
            }
            if(!ParseReverb(argv[++arg])) return 1;
        }
#endif
        else if(option == "-w" || option == "-d")
        {
            std::string* output = &PCMfilepath;
            if(option == "-w") WritePCMfile = true;
            else
            {
#ifdef SUPPORT_VIDEO_OUTPUT
                WriteVideoFile = true;
                output = &VidFilepath;
#else
                std::fprintf(stderr, "Video output not enabled at compilation time.\n");
                return 1;
#endif
            }
            if(arg+1 < argc)
            {
                const std::string_view next = argv[arg+1];
                // Preserve the optional output filename and positional bank syntax.
                if(!next.empty() && (next.front() != '-' || next == "-")
                    && next.find_first_not_of("0123456789") != next.npos)
                    *output = argv[++arg];
            }
        }
        else if(option.size() > 1 && option.front() == '-' && option != "-1")
        {
            std::fprintf(stderr, "Unknown option: %s\n", argv[arg]);
            return 1;
        }
        else positional.push_back(argv[arg]);
    }
    if(positional.size() > 3)
    {
        std::fprintf(stderr, "Expected at most bank, card count, and four-op count.\n");
        return 1;
    }
    auto integer = [](const char* text, long low, long high, unsigned& result)
    {
        const char* first = (*text == '-' || *text == '+') ? text+1 : text;
        if(*first < '0' || *first > '9') return false;
        char* end = nullptr;
        errno = 0;
        long value = std::strtol(text, &end, 10);
        if(errno == ERANGE || *end || value < low || value > high) return false;
        result = value;
        return true;
    };
    if(!positional.empty())
    {
        const unsigned NumBanks = sizeof(banknames)/sizeof(*banknames);
        if(std::string_view(positional[0]) == "-1")
        {
            AdlBank = 0;
            DoingInstrumentTesting = true;
        }
        else if(!integer(positional[0], 0, NumBanks-1, AdlBank))
        {
            std::fprintf(stderr, "bank number must be an integer in 0..%u (or -1 for the tester).\n", NumBanks-1);
            return 1;
        }
        if(WritingToTTY) UI.PrintLn("FM instrument bank %u selected.", AdlBank);
    }

    unsigned n_fourop[2] = {0,0}, n_total[2] = {0,0};
    for(unsigned a=0; a<256; ++a)
    {
        unsigned insno = banks[AdlBank][a];
        if(insno == 198) continue;
        ++n_total[a/128];
        if(adlins[insno].adlno1 != adlins[insno].adlno2)
            ++n_fourop[a/128];
    }
    if (WritingToTTY)
    {
        UI.PrintLn("This bank has %u/%u four-op melodic instruments", n_fourop[0], n_total[0]);
        UI.PrintLn("          and %u/%u percussive ones.", n_fourop[1], n_total[1]);
    }

    if(positional.size() >= 2 && !integer(positional[1], 1, MaxCards, NumCards))
    {
        std::fprintf(stderr, "number of cards must be an integer in 1..%u.\n", MaxCards);
        return 1;
    }
    if(positional.size() >= 3)
    {
        if(!integer(positional[2], 0, 6*NumCards, NumFourOps))
        {
            std::fprintf(stderr, "number of four-op channels must be an integer in 0..%u.\n", 6*NumCards);
            return 1;
        }
    }
    else
        NumFourOps =
            DoingInstrumentTesting ? 2
          : (n_fourop[0] >= n_total[0]*7/8) ? NumCards * 6
          : (n_fourop[0] < n_total[0]*1/8) ? 0
          : (NumCards==1 ? 1 : NumCards*4);
    if (WritingToTTY)
    {
        UI.PrintLn("Simulating %u OPL3 cards for a total of %u operators.", NumCards, NumCards*36);
        std::string s = "Operator set-up: "
                       + std::to_string(NumFourOps)
                       + " 4op, "
                       + std::to_string((AdlPercussionMode ? 15 : 18) * NumCards - NumFourOps*2)
                       + " 2op";
        if(AdlPercussionMode)
            s += ", " + std::to_string(NumCards * 5) + " percussion";
        s += " channels";
        UI.PrintLn("%s", s.c_str());
    }

    std::vector<std::filesystem::path> playlist;
    std::error_code folder_error;
    FolderPlayback = std::filesystem::is_directory(argv[1], folder_error);
    if(FolderPlayback)
    {
        if(WritePCMfile || DoingInstrumentTesting
#ifdef SUPPORT_VIDEO_OUTPUT
            || WriteVideoFile
#endif
        )
        {
            std::fprintf(stderr, "Folder playback is available for live playback only.\n");
            return 2;
        }
        std::filesystem::directory_iterator entries(argv[1], folder_error), end;
        for(; !folder_error && entries != end; entries.increment(folder_error))
        {
            std::error_code file_error;
            if(!entries->is_regular_file(file_error)) continue;
            std::string extension = entries->path().extension().string();
            for(char& c: extension) c = std::tolower(static_cast<unsigned char>(c));
            if(extension == ".mid" || extension == ".midi" || extension == ".rmi")
                playlist.push_back(entries->path());
        }
        if(folder_error)
        {
            std::fprintf(stderr, "%s: %s\n", argv[1], folder_error.message().c_str());
            return 2;
        }
        std::sort(playlist.begin(), playlist.end());
        if(playlist.empty())
        {
            std::fprintf(stderr, "%s: No MIDI files found (.mid, .midi, .rmi).\n", argv[1]);
            return 2;
        }
    }
    else playlist.emplace_back(argv[1]);

    MIDIplay player;
    std::size_t current_track = 0;
    const unsigned selected_bank = AdlBank;
    const bool selected_logarithmic_volumes = LogarithmicVolumes;
    // Construct a fresh player for each file, including failed load attempts.
    auto load_track = [&](int direction)
    {
        for(std::size_t attempts=0; attempts<playlist.size(); ++attempts)
        {
            AdlBank = selected_bank;
            LogarithmicVolumes = selected_logarithmic_volumes;
            CartoonersVolumes = false;
            MIDIplay next;
            next.ChooseDevice("");
            if(next.LoadMIDI(playlist[current_track].string()))
            {
                player = std::move(next);
                UI.visual_channels.clear();
                UI.visual_order.clear();
                UI.SetPlayingFile(playlist[current_track].filename().string(), current_track, playlist.size());
                UI.Reflow();
                return true;
            }
            UI.PrintLn("Skipping unreadable file: %s", playlist[current_track].filename().string().c_str());
            if(!FolderPlayback) break;
            if(direction > 0 && QuitWithoutLooping && current_track+1 == playlist.size()) break;
            current_track = direction < 0 ? (current_track+playlist.size()-1)%playlist.size()
                                          : (current_track+1)%playlist.size();
        }
        return false;
    };
    UI.Color(7);
    if(!load_track(1))
    {
        UI.ShowCursor();
        return 2;
    }

    if(n_fourop[0] >= n_total[0]*15/16 && NumFourOps == 0)
    {
        std::fprintf(stderr,
            "ERROR: You have selected a bank that consists almost exclusively of four-op patches.\n"
            "       The results (silence + much cpu load) would be probably\n"
            "       not what you want, therefore ignoring the request.\n");
        return 1;
    }

#ifndef __DJGPP__
    if(WritePCMfile && !WavOutput.Open(PCMfilepath))
    {
        std::perror(PCMfilepath.c_str());
        UI.ShowCursor();
        return 2;
    }
#endif

#if !defined(__DJGPP__) && !defined(__WIN32__)
    SDL_AudioSpec spec{}, obtained{};
    spec.freq = PCM_RATE;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = spec.freq * AudioBufferLength;
    spec.callback = AdlAudioCallback;
    if(!WritePCMfile)
    {
        if(SDL_Init(SDL_INIT_AUDIO) < 0)
        {
            std::fprintf(stderr, "Couldn't initialize audio: %s\n", SDL_GetError());
            return 1;
        }
        std::atexit(SDL_Quit);
        if(SDL_OpenAudio(&spec, &obtained) < 0)
        {
            std::fprintf(stderr, "Couldn't open audio: %s\n", SDL_GetError());
            return 1;
        }
        if(obtained.freq != spec.freq || obtained.format != spec.format || obtained.channels != spec.channels)
        {
            std::fprintf(stderr, "Audio device does not support 48 kHz, 16-bit stereo playback.\n");
            SDL_CloseAudio();
            return 1;
        }
    }
#endif

    UI.BeginScreen();
    if(AdlBank < sizeof(banknames)/sizeof(*banknames))
        UI.PrintLn("Bank %u: %s", AdlBank, banknames[AdlBank]);
    UI.PrintLn("%u OPL3 cards | %u four-op channels", NumCards, NumFourOps);

#ifdef __DJGPP__

    unsigned TimerPeriod = 0x1234DDul / NewTimerFreq;
    //disable();
    outportb(0x43, 0x34);
    outportb(0x40, TimerPeriod & 0xFF);
    outportb(0x40, TimerPeriod >>   8);
    //enable();
    unsigned long BIOStimer_begin = BIOStimer;

#else

    const double mindelay = 1 / (double)PCM_RATE;
    const double maxdelay = MaxSamplesAtTime / (double)PCM_RATE;
    reverb_data.ReInit();

#ifdef __WIN32
    WindowsAudio::Open(PCM_RATE, 2, 16);
#else
    SDL_PauseAudio(0);
#endif

#endif /* djgpp */

    Tester InstrumentTester(player.opl);

    UI.TetrisLaunched = true;
    double carry = 0.0;
    bool natural_completion = false;
    int exit_status = 0;
    for(double delay=0; !QuitFlag; )
    {
        if(!DoingInstrumentTesting && Input.StopRequested()) break;
        if(!DoingInstrumentTesting && !FolderPlayback && player.finished)
        {
            natural_completion = true;
            exit_status = player.failed ? 2 : 0;
            break;
        }
        if(FolderPlayback && (TrackChange || player.finished))
        {
            bool automatic = !TrackChange;
            int direction = TrackChange ? TrackChange : 1;
            TrackChange = 0;
            if(automatic && QuitWithoutLooping && current_track+1 == playlist.size())
            {
                natural_completion = true;
                break;
            }
            current_track = direction < 0 ? (current_track+playlist.size()-1)%playlist.size()
                                          : (current_track+1)%playlist.size();
#ifndef __DJGPP__
#ifndef __WIN32__
            if(!automatic)
            {
                std::lock_guard lock(AudioBuffer_lock);
                AudioBuffer.clear();
            }
#endif
            reverb_data.ReInit();
            std::fill_n(PreviousAudioInput, 2, 0.f);
            std::fill_n(PreviousAudioOutput, 2, 0.f);
#endif
            if(!load_track(direction))
            {
                natural_completion = automatic;
                break;
            }
            delay = 0;
            carry = 0;
            SkipForward = 0;
        }
    #ifndef __DJGPP__
        const double eat_delay = delay < maxdelay ? delay : maxdelay;
        delay -= eat_delay;

        carry += PCM_RATE * eat_delay;
        const unsigned long n_samples = (unsigned) carry;
        carry -= n_samples;

        if(SkipForward > 0)
            SkipForward -= 1;
        else
        {
            if(NumCards == 1)
            {
                player.opl.cards[0].Generate(0, SendStereoAudio, n_samples);
            }
            else if(n_samples > 0)
            {
                /* Mix together the audio from different cards */
                static std::vector<int> sample_buf;
                sample_buf.clear();
                sample_buf.resize(n_samples*2);
                struct Mix
                {
                    static void AddStereoAudio(unsigned long count, int* samples)
                    {
                        for(unsigned long a=0; a<count*2; ++a)
                            sample_buf[a] += samples[a];
                    }
                };
                for(unsigned card = 0; card < NumCards; ++card)
                {
                    player.opl.cards[card].Generate(
                        0,
                        Mix::AddStereoAudio,
                        n_samples);
                }
                /* Process it */
                SendStereoAudio(n_samples, &sample_buf[0]);
            }

            //fprintf(stderr, "Enter: %u (%.2f ms)\n", (unsigned)AudioBuffer.size(),
            //    AudioBuffer.size() * .5e3 / obtained.freq);
        #ifndef __WIN32__
            if(WritePCMfile)
            {
                // Nothing paces a file render, so step the display once per
                // millisecond of audio, the cadence it gets in live playback.
                static unsigned long unstepped = 0;
                for(unstepped += n_samples; unstepped >= PCM_RATE/1000; unstepped -= PCM_RATE/1000)
                    UI.StepDisplay();
            }
            else
            {
                // Sleep until the audio callback has made room in the queue.
                // The puzzle game is stepped from here and wants a ~1 ms
                // cadence; otherwise wake just often enough to notice keys
                // and terminal resizes.
                const std::size_t high_water = obtained.samples + (obtained.freq*2) * OurHeadRoomLength;
                const std::chrono::milliseconds nap(UI.GameVisible() ? 1 : 20);
                bool settle = true;
                std::unique_lock lock(AudioBuffer_lock);
                for(unsigned grant=0; !QuitFlag && !TrackChange && AudioBuffer.size() > high_water; ++grant)
                {
                    if(settle || grant%4==0)
                        AudioBuffer_drained.wait_for(lock, nap);
                    lock.unlock();
                    settle = UI.CheckTetris();
                    lock.lock();
                }
            }
        #else
            //Sleep(1e3 * eat_delay);
        #endif
            //fprintf(stderr, "Exit: %u\n", (unsigned)AudioBuffer.size());
        }
    #else /* DJGPP */
        UI.IllustrateVolumes(0,0);
        const double mindelay = 1.0 / NewTimerFreq;

        //__asm__ volatile("sti\nhlt");
        //usleep(10000);
        __dpmi_yield();

        static unsigned long PrevTimer = BIOStimer;
        const unsigned long CurTimer = BIOStimer;
        const double eat_delay = (CurTimer - PrevTimer) / (double)NewTimerFreq;
        PrevTimer = CurTimer;
    #endif

        if(TrackChange) continue;
        double nextdelay =
            DoingInstrumentTesting
            ? InstrumentTester.Tick(eat_delay, mindelay)
            : player.Tick(eat_delay, mindelay);

        UI.GotoXY(0,0);
        if(!TerminalActive) UI.ShowCursor();

        delay = nextdelay;
    }

#ifdef __DJGPP__

    // Fix the skewed clock and reset BIOS tick rate
    _farpokel(_dos_ds, 0x46C, BIOStimer_begin +
        (BIOStimer - BIOStimer_begin)
        * (0x1234DD/65536.0) / NewTimerFreq );
    //disable();
    outportb(0x43, 0x34);
    outportb(0x40, 0);
    outportb(0x40, 0);
    //enable();

#else

#ifdef __WIN32__
    WindowsAudio::Close();
#else
    if(!WritePCMfile)
    {
        if(natural_completion)
        {
            // Finish queued audio on natural completion. Keep cancellation
            // responsive and bound the wait if a device stops calling back.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            std::unique_lock lock(AudioBuffer_lock);
            while(!AudioBuffer.empty() && !QuitFlag && std::chrono::steady_clock::now() < deadline)
            {
                AudioBuffer_drained.wait_for(lock, std::chrono::milliseconds(20));
                lock.unlock();
                if(Input.StopRequested()) QuitFlag = true;
                lock.lock();
            }
            natural_completion = AudioBuffer.empty() && !QuitFlag;
        }
        if(!natural_completion)
        {
            StopFadeRemaining.store(StopFadeFrames);
            AudioStopping.store(true);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while(StopFadeRemaining.load() && std::chrono::steady_clock::now() < deadline)
            SDL_Delay(1);
        // The callback fills a buffer ahead of the speakers. Allow the faded
        // buffer to play before closing the device and discarding queued audio.
        if(!StopFadeRemaining.load() && obtained.freq > 0)
            SDL_Delay(std::min(100u, 1000u * obtained.samples / obtained.freq + 10u));
    }
    SDL_CloseAudio();
#endif

#endif /* djgpp */
    if(FakeDOSshell)
    {
        fprintf(stderr,
            "Going TSR. Type 'EXIT' to return to ADLMIDI.\n"
            "\n"
          /*"Megasoft(R) Orifices 98\n"
            "    (C)Copyright Megasoft Corp 1981 - 1999.\n"*/
            ""
        );
    }
    return exit_status;
}
