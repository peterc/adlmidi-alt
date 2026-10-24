// Browser adapter for ADLMIDI. Distributed under GPL-3.0.
#include "core.inc"
#include <emscripten/emscripten.h>

static std::unique_ptr<MIDIplay> player;
static std::array<int, 1024> mixed;
static std::array<float, 1024> output;
static float previousInput[2]={}, previousOutput[2]={};
static float gain=3.981071706f, dcPole=0;
static double delay=0, carry=0, elapsed=0, pendingStep=0;
static bool ended=false;
static unsigned framesPending=0;
static std::array<float, 16> channelPeaks={}, levelSnapshot={};
static void mixStereo(unsigned long count, int* data);
static void mixMono(unsigned long count, int* data);

static void generateAudio(unsigned frames) {
    std::array<float, 16> levels={};
    for(unsigned card=0; card<player->opl.cards.size(); ++card) {
        auto& handler=player->opl.cards[card];
        for(auto& channel:handler.chip.chan) channel.meterPeak=0;
        handler.Generate(mixMono,mixStereo,frames);
        for(unsigned voice=0; voice<18; ++voice) {
            // DBOPL stores four-op partners next to one another internally.
            const unsigned local=voice%9;
            const unsigned index=(voice/9)*9+(local<6 ? (local%3)*2+local/3 : local);
            const unsigned midi=unsigned(VisualNotes[(card*23+voice)*4])%16;
            levels[midi]+=handler.chip.chan[index].meterPeak*(.3f/32768.f)*gain;
        }
    }
    // Sum voice peaks per MIDI channel; this is a pre-reverb level estimate,
    // not a separate post-effect audio bus or a note-velocity display.
    for(unsigned channel=0; channel<16; ++channel)
        channelPeaks[channel]=std::max(channelPeaks[channel],levels[channel]);
}

static void mixStereo(unsigned long count, int* data) {
    for(unsigned i=0; i<count*2; ++i) mixed[i]+=data[i];
}
static void mixMono(unsigned long count, int* data) {
    for(unsigned i=0; i<count; ++i) mixed[i*2]+=data[i], mixed[i*2+1]+=data[i];
}
static void processAudio(unsigned count, float* target) {
    float dry[2][512];
    for(unsigned w=0; w<2; ++w) {
        for(unsigned p=0; p<count; ++p) {
            float input=mixed[p*2+w];
            float filtered=input-previousInput[w]+dcPole*previousOutput[w];
            previousInput[w]=input; previousOutput[w]=filtered;
            dry[w][p]=filtered*(0.3f/32768.f);
        }
        if(reverb_data.enabled) {
            auto& r=reverb_data.chan[w];
            r.input_fifo.insert(r.input_fifo.end(),dry[w],dry[w]+count);
            r.Process(count);
        }
    }
    for(unsigned p=0;p<count;++p) {
        float out[2];
        for(unsigned w=0;w<2;++w) {
            float wet=reverb_data.enabled ? .5f*(reverb_data.chan[0].out[w][p]+reverb_data.chan[1].out[w][p]) : 0;
            out[w]=(dry[w][p]+.25f*wet)*gain;
        }
        float peak=std::max(std::fabs(out[0]),std::fabs(out[1]));
        constexpr float knee=.9f, ceiling=32766.f/32768.f;
        if(peak>knee) {
            float limited=knee+(ceiling-knee)*std::tanh((peak-knee)/(ceiling-knee));
            out[0]*=limited/peak; out[1]*=limited/peak;
        }
        // Retain the desktop player's 16-bit quantization before Web Audio output.
        for(unsigned w=0;w<2;++w) target[p*2+w]=int16_t(out[w]*32768.f)/32768.f;
    }
}
extern "C" {
// Applied before loading a song, like bank/card changes, to reset all voices.
EMSCRIPTEN_KEEPALIVE void engine_synth_options(int tremolo,int vibrato,int modulators) {
    HighTremoloMode=tremolo!=0;
    HighVibratoMode=vibrato!=0;
    ScaleModulators=modulators!=0;
}
EMSCRIPTEN_KEEPALIVE int engine_load(int rate,int bank,int cards) {
    if(rate<8000 || rate>192000 || bank<0 || bank>=67 || cards<1 || cards>16) return 0;
    player.reset(); PCM_RATE=rate; AdlBank=bank; NumCards=cards;
    unsigned four=0,total=0;
    for(unsigned a=0;a<128;++a) {
        unsigned i=banks[AdlBank][a]; if(i==198) continue;
        ++total; if(adlins[i].adlno1!=adlins[i].adlno2) ++four;
    }
    NumFourOps=four>=total*7/8 ? NumCards*6 : four<total/8 ? 0 : NumCards==1 ? 1 : NumCards*4;
    QuitFlag=false; FolderPlayback=true;
    std::fill(std::begin(VisualNotes),std::end(VisualNotes),0);
    std::fill(std::begin(ChannelPrograms),std::end(ChannelPrograms),0);
    channelPeaks.fill(0); levelSnapshot.fill(0);
    player=std::make_unique<MIDIplay>();
    player->ChooseDevice("");
    if(!player->LoadMIDI("/song.mid")) { player.reset(); return 0; }
    reverb_data.ReInit();
    previousInput[0]=previousInput[1]=previousOutput[0]=previousOutput[1]=0;
    dcPole=std::exp(-2.0*3.141592653589793*5.0/PCM_RATE);
    delay=carry=elapsed=pendingStep=0; framesPending=0; ended=false;
    return 1;
}
EMSCRIPTEN_KEEPALIVE void engine_settings(float gainDb,int reverb,float wetDb,int loop) {
    BrowserLoop=loop!=0;
    gain=std::pow(10.f,std::clamp(gainDb,-60.f,30.f)/20.f);
    reverb_data.enabled=reverb!=0;
    ReverbSpecs.do_reverb=reverb!=0;
    ReverbSpecs.wet_gain_db=std::clamp(wetDb,0.f,36.f);
    for(auto& r:reverb_data.chan) r.gain=std::exp(ReverbSpecs.wet_gain_db*float(std::log(10.0)*.05))*.015f;
}
EMSCRIPTEN_KEEPALIVE void engine_reverb(float room,float decay,float damping,float predelay,float stereo) {
    if(!std::isfinite(room) || !std::isfinite(decay) || !std::isfinite(damping)
        || !std::isfinite(predelay) || !std::isfinite(stereo)) return;
    room=std::clamp(room,0.f,1.f); decay=std::clamp(decay,0.f,1.f);
    damping=std::clamp(damping,0.f,1.f); predelay=std::clamp(predelay,0.f,.5f);
    stereo=std::clamp(stereo,0.f,1.f);
    if(room==ReverbSpecs.room_scale && decay==ReverbSpecs.reverberance
        && damping==ReverbSpecs.hf_damping && predelay==ReverbSpecs.pre_delay_s
        && stereo==ReverbSpecs.stereo_depth) return;
    ReverbSpecs.room_scale=room; ReverbSpecs.reverberance=decay;
    ReverbSpecs.hf_damping=damping; ReverbSpecs.pre_delay_s=predelay;
    ReverbSpecs.stereo_depth=stereo;
    // Rebuild effect buffers only on a parameter change, preserving song time.
    reverb_data.ReInit();
}
EMSCRIPTEN_KEEPALIVE float* engine_render(unsigned frames) {
    frames=std::min(frames,512u); std::fill(output.begin(),output.end(),0);
    if(!player || ended) return output.data();
    unsigned done=0, guard=0;
    while(done<frames) {
        if(++guard>20000) { ended=true; break; }
        if(!framesPending) {
            if(player->finished) {
                ended=true; break;
            } else {
                if(delay<=0) delay=player->Tick(0,1.0/PCM_RATE);
                if(player->finished) continue;
                double step=std::min(delay,512.0/PCM_RATE);
                delay-=step; carry+=step*PCM_RATE;
                framesPending=unsigned(carry); carry-=framesPending;
                // Events advance after the scheduled audio has been generated.
                if(!framesPending) delay=player->Tick(step,1.0/PCM_RATE);
                else pendingStep=step;
                if(!framesPending) continue;
            }
        }
        unsigned n=std::min(frames-done,framesPending);
        std::fill(mixed.begin(),mixed.end(),0);
        generateAudio(n);
        processAudio(n,output.data()+done*2);
        done+=n; framesPending-=n; elapsed+=double(n)/PCM_RATE;
        if(!framesPending) {
            delay=player->Tick(pendingStep,1.0/PCM_RATE);
            if(player->finished) ended=true;
        }
    }
    return output.data();
}
EMSCRIPTEN_KEEPALIVE int engine_ended() { return ended; }
EMSCRIPTEN_KEEPALIVE double engine_time() { return elapsed; }
EMSCRIPTEN_KEEPALIVE double engine_bpm() {
    return player ? 60000000.0 * player->InvDeltaTicks.valuel() / player->Tempo.valuel() : 0;
}
EMSCRIPTEN_KEEPALIVE unsigned engine_signature() { return player ? player->BrowserSignature : 0; }
EMSCRIPTEN_KEEPALIVE int engine_fourops() { return NumFourOps; }
EMSCRIPTEN_KEEPALIVE int32_t* engine_notes() { return VisualNotes; }
EMSCRIPTEN_KEEPALIVE int32_t* engine_programs() { return ChannelPrograms; }
EMSCRIPTEN_KEEPALIVE float* engine_levels() {
    levelSnapshot=channelPeaks;
    channelPeaks.fill(0);
    return levelSnapshot.data();
}
EMSCRIPTEN_KEEPALIVE const char* engine_bank(int index) { return index>=0 && index<67 ? banknames[index] : ""; }
}
