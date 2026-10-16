// ChordbloomClapNoteTest: a tiny headless CLAP host that checks Chordbloom.clap really emits notes.
//
//   scripts\build.bat build-clap ChordbloomClapNoteTest
//   build-clap\ChordbloomClapNoteTest.exe build-clap\Chordbloom_artefacts\Release\CLAP\Chordbloom.clap
//
// Checks (exit code 0 = all passed):
//   1. descriptor + one MIDI-dialect note output port (and a note input port)
//   2. live input pass-through: MIDI note-on/off in -> CLAP_EVENT_MIDI note-on/off out
//   3. clap.state load of a two-chord progression asks the host to rescan parameter values
//   4. playback: 16 beats of a playing 120 BPM transport -> chord notes (ch 1) and bass notes (ch 2) out,
//      and after the transport stops every note-on has had its note-off (no hanging notes)
//   5. audio output is finite throughout
// No editor is created and no window is shown.

#include <clap/clap.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
//==============================================================================
// Host side
DWORD gMainThreadId = 0;
thread_local bool tlIsAudioThread = false;
bool gCallbackRequested = false;
uint32_t gRescanFlags = 0;
int gFlushRequests = 0;

bool hostIsMainThread (const clap_host_t*) { return GetCurrentThreadId() == gMainThreadId; }
bool hostIsAudioThread (const clap_host_t*) { return tlIsAudioThread; }
const clap_host_thread_check_t gThreadCheck { hostIsMainThread, hostIsAudioThread };

void hostParamsRescan (const clap_host_t*, clap_param_rescan_flags flags) { gRescanFlags |= flags; }
void hostParamsClear (const clap_host_t*, clap_id, clap_param_clear_flags) {}
void hostParamsRequestFlush (const clap_host_t*) { ++gFlushRequests; }
const clap_host_params_t gHostParams { hostParamsRescan, hostParamsClear, hostParamsRequestFlush };

const void* hostGetExtension (const clap_host_t*, const char* id)
{
    if (std::strcmp (id, CLAP_EXT_THREAD_CHECK) == 0)
        return &gThreadCheck;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0)
        return &gHostParams;
    return nullptr;
}
void hostRequestRestart (const clap_host_t*) {}
void hostRequestProcess (const clap_host_t*) {}
void hostRequestCallback (const clap_host_t*) { gCallbackRequested = true; }

const clap_host_t gHost { CLAP_VERSION_INIT, nullptr, "ChordbloomClapNoteTest", "Kujar3", "", "1.0.0",
                          hostGetExtension, hostRequestRestart, hostRequestProcess, hostRequestCallback };

//==============================================================================
// Event lists
struct InList
{
    std::vector<clap_event_midi_t> events;
    clap_input_events_t api {};

    InList()
    {
        api.ctx = this;
        api.size = [] (const clap_input_events_t* l) { return (uint32_t) static_cast<const InList*> (l->ctx)->events.size(); };
        api.get = [] (const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t*
        {
            auto* self = static_cast<const InList*> (l->ctx);
            return i < self->events.size() ? &self->events[i].header : nullptr;
        };
    }

    void addMidi (uint32_t time, uint8_t s, uint8_t d1, uint8_t d2)
    {
        clap_event_midi_t e {};
        e.header.size = sizeof (e);
        e.header.time = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_MIDI;
        e.port_index = 0;
        e.data[0] = s;
        e.data[1] = d1;
        e.data[2] = d2;
        events.push_back (e);
    }
};

struct OutMidi
{
    int64_t time;
    uint8_t status, d1, d2;
    bool isNoteOn() const { return (status & 0xf0) == 0x90 && d2 > 0; }
    bool isNoteOff() const { return (status & 0xf0) == 0x80 || ((status & 0xf0) == 0x90 && d2 == 0); }
    int channel() const { return (status & 0x0f) + 1; }
};

struct OutList
{
    std::vector<OutMidi> midi;
    int clapNoteEvents = 0, paramEvents = 0, otherEvents = 0;
    int64_t blockStart = 0;
    uint32_t blockFrames = 0;
    bool badTime = false;
    clap_output_events_t api {};

    OutList()
    {
        api.ctx = this;
        api.try_push = [] (const clap_output_events_t* l, const clap_event_header_t* e)
        {
            auto* self = static_cast<OutList*> (l->ctx);
            if (e->time >= self->blockFrames)
                self->badTime = true;
            if (e->space_id != CLAP_CORE_EVENT_SPACE_ID)
                ++self->otherEvents;
            else if (e->type == CLAP_EVENT_MIDI)
            {
                auto* m = reinterpret_cast<const clap_event_midi_t*> (e);
                self->midi.push_back ({ self->blockStart + e->time, m->data[0], m->data[1], m->data[2] });
            }
            else if (e->type == CLAP_EVENT_NOTE_ON || e->type == CLAP_EVENT_NOTE_OFF)
                ++self->clapNoteEvents;
            else if (e->type == CLAP_EVENT_PARAM_VALUE || e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN
                     || e->type == CLAP_EVENT_PARAM_GESTURE_END)
                ++self->paramEvents;
            else
                ++self->otherEvents;
            return true;
        };
    }
};

//==============================================================================
struct ReadStream
{
    const std::vector<uint8_t>& data;
    size_t pos = 0;
    clap_istream_t api {};

    explicit ReadStream (const std::vector<uint8_t>& d) : data (d)
    {
        api.ctx = this;
        api.read = [] (const clap_istream_t* s, void* buffer, uint64_t size) -> int64_t
        {
            auto* self = static_cast<ReadStream*> (s->ctx);
            const auto n = (size_t) std::min<uint64_t> (size, self->data.size() - self->pos);
            std::memcpy (buffer, self->data.data() + self->pos, n);
            self->pos += n;
            return (int64_t) n;
        };
    }
};

// Same layout as juce::AudioProcessor::copyXmlToBinary: magic, string length, UTF-8 XML, terminating 0.
std::vector<uint8_t> makeJuceXmlBlob (const std::string& xml)
{
    std::vector<uint8_t> out (8);
    const uint32_t magic = 0x21324356, len = (uint32_t) xml.size();
    std::memcpy (out.data(), &magic, 4);
    std::memcpy (out.data() + 4, &len, 4);
    out.insert (out.end(), xml.begin(), xml.end());
    out.push_back (0);
    return out;
}

//==============================================================================
int gFailures = 0;
void check (bool ok, const std::string& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++gFailures;
}

void pumpMainThread (const clap_plugin_t* plugin)
{
    MSG msg;
    while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage (&msg);
        DispatchMessageW (&msg);
    }
    if (gCallbackRequested)
    {
        gCallbackRequested = false;
        plugin->on_main_thread (plugin);
    }
}

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlock = 512;

struct Engine
{
    const clap_plugin_t* plugin = nullptr;
    std::vector<std::vector<std::vector<float>>> outData;   // [port][channel][frame]
    std::vector<std::vector<float*>> outPtrs;
    std::vector<clap_audio_buffer_t> outBuffers;
    int64_t steadyTime = 0;
    float peak = 0.0f;
    bool nonFinite = false;

    void setupAudio (const clap_plugin_audio_ports_t* ports)
    {
        const uint32_t n = ports != nullptr ? ports->count (plugin, false) : 0;
        outData.resize (n);
        outPtrs.resize (n);
        outBuffers.resize (n);
        for (uint32_t p = 0; p < n; ++p)
        {
            clap_audio_port_info_t info {};
            ports->get (plugin, p, false, &info);
            outData[p].assign (info.channel_count, std::vector<float> (kBlock, 0.0f));
            outPtrs[p].clear();
            for (auto& ch : outData[p])
                outPtrs[p].push_back (ch.data());
            outBuffers[p] = {};
            outBuffers[p].data32 = outPtrs[p].data();
            outBuffers[p].channel_count = info.channel_count;
        }
    }

    // Must be called on the audio thread.
    void processBlock (InList& in, OutList& out, const clap_event_transport_t* transport)
    {
        out.blockFrames = kBlock;
        clap_process_t proc {};
        proc.steady_time = steadyTime;
        proc.frames_count = kBlock;
        proc.transport = transport;
        proc.audio_inputs = nullptr;
        proc.audio_inputs_count = 0;
        proc.audio_outputs = outBuffers.empty() ? nullptr : outBuffers.data();
        proc.audio_outputs_count = (uint32_t) outBuffers.size();
        proc.in_events = &in.api;
        proc.out_events = &out.api;
        plugin->process (plugin, &proc);

        for (auto& port : outData)
            for (auto& ch : port)
                for (float s : ch)
                {
                    if (! std::isfinite (s))
                        nonFinite = true;
                    else
                        peak = std::max (peak, std::abs (s));
                }
        steadyTime += kBlock;
        out.blockStart += kBlock;
    }
};

clap_event_transport_t makeTransport (double beats, bool playing)
{
    clap_event_transport_t t {};
    t.header.size = sizeof (t);
    t.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    t.header.type = CLAP_EVENT_TRANSPORT;
    t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE
              | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0u);
    t.tempo = 120.0;
    t.song_pos_beats = (clap_beattime) std::llround (beats * (double) CLAP_BEATTIME_FACTOR);
    t.song_pos_seconds = (clap_sectime) std::llround (beats * 0.5 * (double) CLAP_SECTIME_FACTOR);
    t.bar_start = (clap_beattime) std::llround (std::floor (beats / 4.0) * 4.0 * (double) CLAP_BEATTIME_FACTOR);
    t.bar_number = (int32_t) std::floor (beats / 4.0);
    t.tsig_num = 4;
    t.tsig_denom = 4;
    return t;
}

// Runs fn on a separate "audio" thread (as a real host does) and waits for it.
template <typename Fn>
void onAudioThread (Fn&& fn)
{
    std::thread t ([&] {
        tlIsAudioThread = true;
        fn();
        tlIsAudioThread = false;
    });
    t.join();
}

int run (const std::wstring& path, const std::string& pathUtf8)
{
    gMainThreadId = GetCurrentThreadId();

    HMODULE lib = LoadLibraryW (path.c_str());
    if (lib == nullptr)
    {
        std::printf ("could not load %s (error %lu)\n", pathUtf8.c_str(), GetLastError());
        return 2;
    }
    auto* entry = reinterpret_cast<const clap_plugin_entry_t*> (GetProcAddress (lib, "clap_entry"));
    if (entry == nullptr || ! entry->init (pathUtf8.c_str()))
    {
        std::printf ("no clap_entry / init failed\n");
        return 2;
    }

    auto* factory = static_cast<const clap_plugin_factory_t*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    std::printf ("1. descriptor and ports\n");
    check (factory != nullptr && factory->get_plugin_count (factory) == 1, "factory with exactly one plugin");
    if (factory == nullptr || factory->get_plugin_count (factory) < 1)
        return 1;

    const auto* desc = factory->get_plugin_descriptor (factory, 0);
    std::printf ("  id=%s name=%s vendor=%s version=%s\n", desc->id, desc->name, desc->vendor, desc->version);
    check (std::strcmp (desc->id, "com.kujar3.chordbloom") == 0, "CLAP id com.kujar3.chordbloom");

    const clap_plugin_t* plugin = factory->create_plugin (factory, &gHost, desc->id);
    if (plugin == nullptr || ! plugin->init (plugin))
    {
        std::printf ("create/init failed\n");
        return 1;
    }

    auto* notePorts = static_cast<const clap_plugin_note_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_NOTE_PORTS));
    auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS));
    auto* state = static_cast<const clap_plugin_state_t*> (plugin->get_extension (plugin, CLAP_EXT_STATE));

    check (notePorts != nullptr, "note-ports extension");
    if (notePorts != nullptr)
    {
        check (notePorts->count (plugin, true) == 1, "one note input port");
        const auto outs = notePorts->count (plugin, false);
        check (outs == 1, "one note output port");
        if (outs >= 1)
        {
            clap_note_port_info_t info {};
            notePorts->get (plugin, 0, false, &info);
            std::printf ("  output port '%s' supported_dialects=0x%x preferred=0x%x\n", info.name,
                         info.supported_dialects, info.preferred_dialect);
            check ((info.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0, "output port speaks the MIDI dialect");
        }
    }

    Engine engine;
    engine.plugin = plugin;
    engine.setupAudio (audioPorts);
    std::printf ("  audio outputs: %zu port(s)%s\n", engine.outBuffers.size(),
                 engine.outBuffers.empty() ? "" : (", " + std::to_string (engine.outBuffers[0].channel_count) + " ch").c_str());

    check (plugin->activate (plugin, kSampleRate, 1, kBlock), "activate(48 kHz, 512)");
    pumpMainThread (plugin);

    //==========================================================================
    std::printf ("2. live input pass-through (transport stopped)\n");
    {
        OutList out;
        const auto stopped = makeTransport (0.0, false);
        onAudioThread ([&] {
            plugin->start_processing (plugin);
            for (int b = 0; b < 40; ++b)
            {
                InList in;
                if (b == 2)
                    in.addMidi (100, 0x90, 64, 100);   // E4 on, ch 1
                if (b == 20)
                    in.addMidi (7, 0x80, 64, 0);       // E4 off
                engine.processBlock (in, out, &stopped);
            }
        });
        pumpMainThread (plugin);

        int ons = 0, offs = 0;
        for (auto& m : out.midi)
        {
            if (m.d1 == 64 && m.isNoteOn()) ++ons;
            if (m.d1 == 64 && m.isNoteOff()) ++offs;
        }
        std::printf ("  out: %zu MIDI events (E4 on=%d off=%d), %d CLAP note events, %d param events\n",
                     out.midi.size(), ons, offs, out.clapNoteEvents, out.paramEvents);
        check (ons == 1, "input note-on comes out as CLAP_EVENT_MIDI note-on");
        check (offs == 1, "input note-off comes out as CLAP_EVENT_MIDI note-off");
        check (! out.badTime, "all output event times inside their block");
        std::printf ("  preview synth peak while held: %.4f\n", engine.peak);
    }

    //==========================================================================
    std::printf ("3. state load (progression C - G, 4 beats each)\n");
    {
        check (state != nullptr, "state extension");
        const auto blob = makeJuceXmlBlob (
            "<ChordbloomState version=\"1\"><Progression>"
            "<Slot root=\"0\" quality=\"0\" length=\"4\" inversion=\"-1\" octave=\"0\"/>"
            "<Slot root=\"7\" quality=\"0\" length=\"4\" inversion=\"-1\" octave=\"0\"/>"
            "</Progression></ChordbloomState>");
        gRescanFlags = 0;
        ReadStream rs (blob);
        const bool loaded = state != nullptr && state->load (plugin, &rs.api);
        pumpMainThread (plugin);
        check (loaded, "state.load() returns true");
        check ((gRescanFlags & CLAP_PARAM_RESCAN_VALUES) != 0, "host asked to rescan parameter values after load");
    }

    //==========================================================================
    std::printf ("4. playback: 16 beats at 120 BPM, then stop\n");
    {
        OutList out;
        const double beatsPerBlock = (double) kBlock / kSampleRate * 2.0;   // 120 BPM = 2 beats/s
        const int playBlocks = (int) std::ceil (16.0 / beatsPerBlock);
        onAudioThread ([&] {
            double beat = 0.0;
            for (int b = 0; b < playBlocks; ++b, beat += beatsPerBlock)
            {
                InList in;
                const auto t = makeTransport (beat, true);
                engine.processBlock (in, out, &t);
            }
            for (int b = 0; b < 20; ++b)
            {
                InList in;
                const auto t = makeTransport (beat, false);
                engine.processBlock (in, out, &t);
            }
        });
        pumpMainThread (plugin);

        std::map<int, int> onsPerChannel;
        std::map<std::pair<int, int>, int> held;
        int maxHeld = 0, unmatchedOffs = 0, current = 0;
        for (auto& m : out.midi)
        {
            if (m.isNoteOn())
            {
                ++onsPerChannel[m.channel()];
                ++held[{ m.channel(), m.d1 }];
                maxHeld = std::max (maxHeld, ++current);
            }
            else if (m.isNoteOff())
            {
                auto& h = held[{ m.channel(), m.d1 }];
                if (h > 0) { --h; --current; }
                else ++unmatchedOffs;
            }
        }
        int hanging = 0;
        for (auto& [key, count] : held)
            hanging += count;

        std::printf ("  out: %zu MIDI events; note-ons per channel:", out.midi.size());
        for (auto& [ch, n] : onsPerChannel)
            std::printf (" ch%d=%d", ch, n);
        std::printf ("; max simultaneous=%d; unmatched note-offs=%d\n", maxHeld, unmatchedOffs);
        check (onsPerChannel[1] > 0, "chord notes on MIDI channel 1");
        check (onsPerChannel[2] > 0, "bass notes on MIDI channel 2");
        check (hanging == 0, "no hanging notes after the transport stops (" + std::to_string (hanging) + " still on)");
        check (! out.badTime, "all output event times inside their block");
    }

    check (! engine.nonFinite, "audio output finite");
    std::printf ("  overall audio peak: %.4f\n", engine.peak);

    //==========================================================================
    // Informational (does not affect the exit code): value -> text -> value -> text must be stable,
    // which clap-validator's param-conversions test requires and hosts rely on for typed-in values.
    std::printf ("5. parameter text round trip (informational)\n");
    if (auto* params = static_cast<const clap_plugin_params_t*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS)))
    {
        int unstable = 0;
        const uint32_t n = params->count (plugin);
        for (uint32_t i = 0; i < n; ++i)
        {
            clap_param_info_t info {};
            if (! params->get_info (plugin, i, &info))
                continue;
            for (int k = 0; k <= 99; ++k)
            {
                const double v = info.min_value + (info.max_value - info.min_value) * k / 99.0;
                char t1[256] = {}, t2[256] = {};
                double v1 = 0.0;
                if (! params->value_to_text (plugin, info.id, v, t1, sizeof (t1))
                    || ! params->text_to_value (plugin, info.id, t1, &v1)
                    || ! params->value_to_text (plugin, info.id, v1, t2, sizeof (t2)))
                    continue;
                if (std::strcmp (t1, t2) != 0)
                {
                    std::printf ("  [WARN] '%s': %.6f -> '%s' -> %.9f -> '%s'\n", info.name, v, t1, v1, t2);
                    ++unstable;
                    break;
                }
            }
        }
        std::printf ("  %u parameters, %d with unstable text round trip\n", n, unstable);
    }

    onAudioThread ([&] { plugin->stop_processing (plugin); });
    plugin->deactivate (plugin);
    pumpMainThread (plugin);
    plugin->destroy (plugin);
    entry->deinit();
    FreeLibrary (lib);

    std::printf ("%s (%d failure%s)\n", gFailures == 0 ? "ALL PASSED" : "FAILED", gFailures, gFailures == 1 ? "" : "s");
    return gFailures == 0 ? 0 : 1;
}
} // namespace

int wmain (int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: ChordbloomClapNoteTest <path to Chordbloom.clap>\n");
        return 2;
    }
    const std::wstring path = argv[1];
    const int n = WideCharToMultiByte (CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8 ((size_t) std::max (n - 1, 0), '\0');
    WideCharToMultiByte (CP_UTF8, 0, path.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    return run (path, utf8);
}
