#include "../../src/Video_Plugin_API.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
namespace {
using Clock=std::chrono::steady_clock;
std::string Root="Resources",Selected,Error;
bool Logging=false;
struct Picture { std::vector<uint8_t> Pixels;int W=0,H=0;double Time=0; };
struct Sound { std::vector<int16_t> Samples;double Time=0; };
struct Clip {
    AVFormatContext *Format=nullptr;AVCodecContext *Video=nullptr,*Audio=nullptr;
    SwsContext *Scale=nullptr;SwrContext *Resample=nullptr;
    AVFrame *Frame=av_frame_alloc();AVPacket *Packet=av_packet_alloc();
    int VI=-1,AI=-1;double Base=0,Duration=0,Start=0,Audio_Through=0,End=0,Seek_Target=0;
    bool Playing=false,Loop=false,Eof=false,Ended=false;
    Clock::time_point Epoch=Clock::now();std::deque<Picture> Pictures;std::deque<Sound> Sounds;
    Picture Current;Sound Delivered;uint64_t Serial=0;
    ~Clip(){av_packet_free(&Packet);av_frame_free(&Frame);sws_freeContext(Scale);swr_free(&Resample);avcodec_free_context(&Video);avcodec_free_context(&Audio);avformat_close_input(&Format);}
    double Position() const {return Playing ? Base+std::chrono::duration<double>(Clock::now()-Epoch).count() : Base;}
};
std::map<std::string,std::unique_ptr<Clip>> Clips;
Clip &Current(){auto i=Clips.find(Selected);if(i==Clips.end()) throw std::runtime_error("No video selected");return *i->second;}
std::string Failure(int code){char text[AV_ERROR_MAX_STRING_SIZE];av_strerror(code,text,sizeof(text));return text;}
void Check(int code,const char *what){if(code<0) throw std::runtime_error(std::string(what)+": "+Failure(code));}
AVCodecContext *Decoder(AVFormatContext *f,int stream) {
    const auto *codec=avcodec_find_decoder(f->streams[stream]->codecpar->codec_id);
    if(!codec) throw std::runtime_error("No decoder for this stream");
    AVCodecContext *ctx=avcodec_alloc_context3(codec);
    if(!ctx) throw std::bad_alloc();
    try {Check(avcodec_parameters_to_context(ctx,f->streams[stream]->codecpar),"Stream parameters");Check(avcodec_open2(ctx,codec,nullptr),"Open decoder");}
    catch(...){avcodec_free_context(&ctx);throw;}
    return ctx;
}
bool Rewind(Clip &c,double seconds) {
    if(!std::isfinite(seconds) || seconds<0 || (c.Duration>0 && seconds>c.Duration)) {Error="Seek position out of range";return false;}
    const int result=av_seek_frame(c.Format,-1,int64_t((seconds+c.Start)*AV_TIME_BASE),AVSEEK_FLAG_BACKWARD);
    if(result<0){Error=Failure(result);return false;}
    avcodec_flush_buffers(c.Video);if(c.Audio) avcodec_flush_buffers(c.Audio);
    if(c.Resample){swr_close(c.Resample);if(swr_init(c.Resample)<0){Error="Could not reset audio converter";return false;}}
    c.Pictures.clear();c.Sounds.clear();c.Current={};c.Delivered={};c.Base=seconds;c.Seek_Target=seconds;c.Epoch=Clock::now();c.Eof=false;c.Ended=false;c.Audio_Through=seconds;c.End=seconds;
    return true;
}
void Receive(Clip &c,AVCodecContext *codec,bool video) {
    for(;;) {
        const int result=avcodec_receive_frame(codec,c.Frame);
        if(result==AVERROR(EAGAIN) || result==AVERROR_EOF) return;
        Check(result,"Decode frame");
        const int stream=video?c.VI:c.AI;
        const double time=c.Frame->best_effort_timestamp==AV_NOPTS_VALUE ? c.End : c.Frame->best_effort_timestamp*av_q2d(c.Format->streams[stream]->time_base)-c.Start;
        if(video) {
            const int w=c.Frame->width,h=c.Frame->height;
            if(w<=0 || h<=0 || w>8192 || h>8192 || int64_t(w)*h>33554432) throw std::runtime_error("Video frame exceeds supported dimensions");
            Picture picture;picture.W=w;picture.H=h;picture.Time=std::max(0.0,time);picture.Pixels.resize(size_t(w)*h*4);
            c.Scale=sws_getCachedContext(c.Scale,w,h,AVPixelFormat(c.Frame->format),w,h,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
            if(!c.Scale) throw std::runtime_error("Could not create video converter");
            uint8_t *dest[]={picture.Pixels.data(),nullptr,nullptr,nullptr};int strides[]={w*4,0,0,0};
            sws_scale(c.Scale,c.Frame->data,c.Frame->linesize,0,h,dest,strides);
            const auto rate=av_guess_frame_rate(c.Format,c.Format->streams[c.VI],nullptr);
            c.End=std::max(c.End,picture.Time+(rate.num>0 ? av_q2d(av_inv_q(rate)) : 0.04));
            c.Pictures.push_back(std::move(picture));
        } else {
            if(!c.Resample) {
                AVChannelLayout layout=AV_CHANNEL_LAYOUT_STEREO;
                Check(swr_alloc_set_opts2(&c.Resample,&layout,AV_SAMPLE_FMT_S16,48000,&c.Frame->ch_layout,AVSampleFormat(c.Frame->format),c.Frame->sample_rate,0,nullptr),"Create audio converter");
                Check(swr_init(c.Resample),"Initialize audio converter");
            }
            const int capacity=swr_get_out_samples(c.Resample,c.Frame->nb_samples);
            if(capacity<0 || capacity>480000) throw std::runtime_error("Invalid audio frame size");
            Sound sound;sound.Time=std::max(0.0,time);sound.Samples.resize(size_t(capacity)*2);
            uint8_t *dest[]={reinterpret_cast<uint8_t*>(sound.Samples.data())};
            const int samples=swr_convert(c.Resample,dest,capacity,const_cast<const uint8_t**>(c.Frame->extended_data),c.Frame->nb_samples);
            Check(samples,"Convert audio");sound.Samples.resize(size_t(samples)*2);
            const int skip=std::clamp(int(std::max(0.0,c.Seek_Target-time)*48000),0,samples);
            if(skip>0){sound.Samples.erase(sound.Samples.begin(),sound.Samples.begin()+size_t(skip)*2);sound.Time+=double(skip)/48000;}
            c.Audio_Through=std::max(c.Audio_Through,sound.Time+double(sound.Samples.size()/2)/48000);
            if(!sound.Samples.empty()) c.Sounds.push_back(std::move(sound));
        }
        av_frame_unref(c.Frame);
    }
}
void Submit(Clip &c,AVCodecContext *codec,const AVPacket *packet,bool video) {
    int result=avcodec_send_packet(codec,packet);
    if(result==AVERROR(EAGAIN)){Receive(c,codec,video);result=avcodec_send_packet(codec,packet);}
    if(result!=AVERROR_EOF) Check(result,"Submit packet");
    Receive(c,codec,video);
}
void Pump(Clip &c) {
    const double now=c.Position();
    for(int n=0;n<256 && !c.Eof;++n) {
        if(!c.Pictures.empty() && c.Pictures.back().Time>=now+0.12 && (!c.Audio || c.Audio_Through>=now+0.15)) break;
        if(c.Pictures.size()>=16 || c.Sounds.size()>=64 || (!c.Pictures.empty() && c.Pictures.size()*c.Pictures.back().Pixels.size()>=64*1024*1024)) break;
        const int result=av_read_frame(c.Format,c.Packet);
        if(result==AVERROR_EOF){c.Eof=true;Submit(c,c.Video,nullptr,true);if(c.Audio)Submit(c,c.Audio,nullptr,false);break;}
        Check(result,"Read video packet");
        try {if(c.Packet->stream_index==c.VI) Submit(c,c.Video,c.Packet,true);else if(c.Packet->stream_index==c.AI) Submit(c,c.Audio,c.Packet,false);}
        catch(...){av_packet_unref(c.Packet);throw;}
        av_packet_unref(c.Packet);
    }
    while(!c.Pictures.empty() && (c.Pictures.front().Time<=now+0.005 || c.Current.Pixels.empty())) {
        c.Current=std::move(c.Pictures.front());c.Pictures.pop_front();++c.Serial;
        if(c.Current.Time>now) break;
    }
    const double end=c.Duration>0?c.Duration:std::max(c.End,c.Audio_Through);
    if(c.Playing && c.Eof && c.Pictures.empty() && now>=end) {
        if(c.Loop){if(!Rewind(c,0)) c.Playing=false;}
        else {c.Base=end;c.Playing=false;c.Ended=true;}
    }
}
}
extern "C" void Constructor(){avformat_network_init();av_log_set_level(AV_LOG_ERROR);}
extern "C" void Destructor(){Clips.clear();Selected.clear();avformat_network_deinit();}
extern "C" void Debug(bool value){Logging=value;av_log_set_level(value?AV_LOG_INFO:AV_LOG_ERROR);}
extern "C" void Set_Paths(std::string root){Root=std::move(root);}
extern "C" bool Load(std::string name,std::string file) {
    Error.clear();
    try {
        if(name.empty() || file.empty()) throw std::runtime_error("Video name and source are required");
        auto c=std::make_unique<Clip>();if(!c->Frame || !c->Packet) throw std::bad_alloc();
        const std::string source=file.find("://")!=std::string::npos || std::filesystem::u8path(file).is_absolute() ? file : (std::filesystem::u8path(Root)/"Video/Movies"/std::filesystem::u8path(file)).u8string();
        AVDictionary *options=nullptr;av_dict_set(&options,"rw_timeout","5000000",0);
        const int opened=avformat_open_input(&c->Format,source.c_str(),nullptr,&options);av_dict_free(&options);Check(opened,"Open video");
        Check(avformat_find_stream_info(c->Format,nullptr),"Read video streams");
        c->VI=av_find_best_stream(c->Format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);Check(c->VI,"Find video stream");
        c->Video=Decoder(c->Format,c->VI);c->AI=av_find_best_stream(c->Format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
        if(c->AI>=0)c->Audio=Decoder(c->Format,c->AI);
        c->Start=c->Format->start_time==AV_NOPTS_VALUE?0:double(c->Format->start_time)/AV_TIME_BASE;
        c->Duration=c->Format->duration==AV_NOPTS_VALUE?0:double(c->Format->duration)/AV_TIME_BASE;
        Pump(*c);Clips[name]=std::move(c);Selected=name;return true;
    }catch(const std::exception &e){Error=e.what();return false;}
}
extern "C" bool Use(std::string name){if(!Clips.count(name)){Error="Unknown video: "+name;return false;}Selected=name;return true;}
extern "C" void Close(std::string name){Clips.erase(name);if(Selected==name)Selected=Clips.empty()?"":Clips.begin()->first;}
extern "C" void Close_All(){Clips.clear();Selected.clear();}
extern "C" void Play(){auto &c=Current();if(c.Ended)Rewind(c,0);if(!c.Playing){c.Epoch=Clock::now();c.Playing=true;}}
extern "C" void Pause(){auto &c=Current();c.Base=c.Position();c.Playing=false;}
extern "C" void Stop(){auto &c=Current();c.Playing=false;Rewind(c,0);}
extern "C" bool Seek(double seconds){return Rewind(Current(),seconds);}
extern "C" void Set_Current_Loop(bool loop){Current().Loop=loop;}
extern "C" void Update(){Error.clear();for(auto &entry:Clips)try{if(entry.second->Playing || entry.second->Current.Pixels.empty())Pump(*entry.second);}catch(const std::exception &e){Error=e.what();entry.second->Playing=false;}}
extern "C" std::string Info(std::string name,std::string field){auto it=Clips.find(name);if(it==Clips.end())return "";auto &c=*it->second;
    if(field=="Width")return std::to_string(c.Video->width);
    if(field=="Height")return std::to_string(c.Video->height);
    if(field=="Duration")return std::to_string(c.Duration);
    if(field=="Position")return std::to_string(c.Position());
    if(field=="Is_Playing")return c.Playing?"1":"0";
    if(field=="Ended")return c.Ended?"1":"0";
    if(field=="Has_Audio")return c.Audio?"1":"0";
    if(field=="Looping")return c.Loop?"1":"0";
    return "";}
extern "C" std::string Last_Error(){return Error;}
extern "C" bool Get_Frame(std::string name,Video_Frame *out){auto it=Clips.find(name);if(!out || out->Version!=1 || it==Clips.end() || it->second->Current.Pixels.empty())return false;
    auto &c=*it->second;*out={1,c.Current.W,c.Current.H,c.Current.W*4,c.Current.Pixels.data(),c.Serial,c.Current.Time};return true;}
extern "C" bool Get_Audio(std::string name,Video_PCM *out){auto it=Clips.find(name);if(!out || out->Version!=1 || it==Clips.end() || it->second->Sounds.empty())return false;
    auto &c=*it->second;c.Delivered=std::move(c.Sounds.front());c.Sounds.pop_front();*out={1,48000,2,int(c.Delivered.Samples.size()/2),c.Delivered.Samples.data(),c.Delivered.Time};return true;}
