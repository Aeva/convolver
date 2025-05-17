
#include <spa/pod/builder.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/format-utils.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include <vector>
#include <print>

#include "pipewire.h"


void PipeWireInit(int argc, char *argv[])
{
    pw_init(&argc, &argv);
}


void PipeWireHalt()
{
    pw_deinit();
}


ThreadShared::ThreadShared(float* BufferA, size_t SizeA, float* BufferC, size_t SizeC)
{
    InSamples = BufferA;
    InSampleCount = SizeA;
    OutSamples = BufferC;
    OutSampleCount = SizeC;
}


void FilterRealTimeThread::SetupPorts(ThreadShared* InBufferState, pw_filter* Filter)
{
    BufferState = InBufferState;

    InPort = pw_filter_add_port(
        Filter,
        PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(FilterRealTimeThread),
        pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "input",
            nullptr),
        nullptr, 0);

    OutPort = pw_filter_add_port(
        Filter,
        PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(FilterRealTimeThread),
        pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "output",
            nullptr),
        nullptr, 0);
}


void FilterRealTimeThread::OnProcess(void *UserData, spa_io_position* Position)
{
    FilterRealTimeThread* Data = (FilterRealTimeThread*)UserData;
    Data->OnProcessInner(*Position);
}


void FilterRealTimeThread::OnProcessInner(const spa_io_position& Position)
{
    const size_t Count = Position.clock.duration;

    float* In = (float*)pw_filter_get_dsp_buffer(InPort, Count);
    float* Out = (float*)pw_filter_get_dsp_buffer(OutPort, Count);

    const size_t PrecedingInReady = BufferState->InReady.load();
    const size_t PrecedingInProcessed = BufferState->InProcessed.load();
    const size_t PrecedingOutReady = BufferState->OutReady.load();
    const size_t PrecedingOutWritten = BufferState->OutWritten.load();

    if (In && Out)
    {
        {
            const size_t InSampleCount = BufferState->InSampleCount;
            size_t ReadStart = 0;
            size_t WriteStart = PrecedingInReady;

            while (ReadStart < Count)
            {
                size_t MaxWrite = InSampleCount - (WriteStart % InSampleCount);
                size_t WriteCount = std::min(Count, MaxWrite);

                float* ReadHead = In + ReadStart;
                float* WriteHead = BufferState->InSamples + (WriteStart % InSampleCount);
                memcpy(WriteHead, ReadHead, WriteCount * sizeof(float));
                ReadStart += WriteCount;
                WriteStart += WriteCount;

                if (WriteCount <= 0)
                {
                    std::print("in inf loop!!\n");
                    break;
                }
            }

            BufferState->InReady += Count;
        }

        {
            const size_t OutSampleCount = BufferState->OutSampleCount;
            const size_t Pending = std::min(PrecedingOutReady - PrecedingOutWritten, Count);
            const size_t MuteStart = Pending;
            const size_t MuteCount = Count - Pending;
            size_t ReadStart = PrecedingOutWritten;
            size_t WriteStart = 0;

            while (WriteStart < Pending)
            {
                size_t MaxRead = OutSampleCount - (ReadStart % OutSampleCount);
                size_t ReadCount = std::min(Pending, MaxRead);

                float* WriteHead = Out + WriteStart;
                float* ReadHead = BufferState->OutSamples + (ReadStart % OutSampleCount);
                memcpy(WriteHead, ReadHead, ReadCount * sizeof(float));
                WriteStart += ReadCount;
                ReadStart += ReadCount;

                if (ReadCount <= 0)
                {
                    std::print("out inf loop!!\n");
                    break;
                }
            }

            BufferState->OutWritten += Pending;

            for (int i = WriteStart; i < Count; ++i)
            {
                Out[i] = 0.0f;
            }

            if (Pending < Count)
            {
                std::print("Not enough output samples ready, padding with zeros!\n");
            }
        }
    }
    else if (Out)
    {
        for (int i = 0; i < Count; ++i)
        {
            Out[i] = 0.0f;
        }
    }
}


void PipeWireFilter::OnQuit(void *UserData, int Signal)
{
    PipeWireFilter* Data = (PipeWireFilter*)UserData;
    Data->Live.store(false);
}


PipeWireFilter::PipeWireFilter(ThreadShared* BufferState, int SampleRate)
{
    std::vector<const spa_pod*> Params;

    uint8_t BuilderBuffer[1024];
    spa_pod_builder PodBuilder = SPA_POD_BUILDER_INIT(BuilderBuffer, sizeof(BuilderBuffer));

    Loop = pw_thread_loop_new("convolver", nullptr);
    pw_thread_loop_lock(Loop);

    pw_loop_add_signal(pw_thread_loop_get_loop(Loop), SIGINT, PipeWireFilter::OnQuit, this);
    pw_loop_add_signal(pw_thread_loop_get_loop(Loop), SIGTERM, PipeWireFilter::OnQuit, this);

    static const pw_filter_events FilterEvents =
    {
        .version = PW_VERSION_FILTER_EVENTS,
        .process = FilterRealTimeThread::OnProcess,
    };

    Filter = pw_filter_new_simple(
        pw_thread_loop_get_loop(Loop),
        "convolver",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            nullptr),
        &FilterEvents,
        &RealTimeThread);

    RealTimeThread.SetupPorts(BufferState, Filter);

    {
        spa_process_latency_info ProcessLatencyInfo =
        {
            .ns = 10 * SPA_NSEC_PER_MSEC
        };
        Params.push_back(spa_process_latency_build(&PodBuilder, SPA_PARAM_ProcessLatency, &ProcessLatencyInfo));
    }

    {
        spa_audio_info_raw StreamFormat =
        {
            .format = SPA_AUDIO_FORMAT_DSP_F32,
            .rate = (uint32_t)SampleRate,
            .channels = 1
        };
        Params.push_back(spa_format_audio_raw_build(&PodBuilder, SPA_PARAM_EnumFormat, &StreamFormat));
    }

    pw_thread_loop_unlock(Loop);

    if (pw_filter_connect(Filter, PW_FILTER_FLAG_RT_PROCESS, Params.data(), Params.size()) < 0)
    {
        std::print("Can't connect?\n");
        Reset();
    }
}


void PipeWireFilter::Run()
{
    if (Loop && Filter)
    {
        pw_thread_loop_lock(Loop);
        Live.store(true);
        pw_thread_loop_start(Loop);
        pw_thread_loop_unlock(Loop);
    }
}


void PipeWireFilter::Reset()
{
    Live.store(false);
    if (Loop)
    {
        pw_thread_loop_lock(Loop);
    }
    if (Filter)
    {
        pw_filter_destroy(Filter);
        Filter = nullptr;
    }
    if (Loop)
    {
        pw_thread_loop_unlock(Loop);
        pw_thread_loop_stop(Loop);
        pw_thread_loop_destroy(Loop);
        Loop = nullptr;
    }
}


PipeWireFilter::~PipeWireFilter()
{
    Reset();
}
