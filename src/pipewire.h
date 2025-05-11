
#include <atomic>


void PipeWireInit(int argc, char *argv[]);


void PipeWireHalt();


struct ThreadShared
{
    std::atomic_size_t InReady = 0;
    std::atomic_size_t InProcessed = 0;

    std::atomic_size_t OutReady = 0;
    std::atomic_size_t OutWritten = 0;

    float* InSamples = nullptr;
    float* OutSamples = nullptr;

    size_t InSampleCount = 0;
    size_t OutSampleCount = 0;

    ThreadShared(float* BufferA, size_t SizeA, float* BufferC, size_t SizeC);
};


struct FilterRealTimeThread
{
    void SetupPorts(ThreadShared* InBufferState, struct pw_filter* Filter);

    static void OnProcess(void *UserData, struct spa_io_position* Position);

private:
    void* InPort = nullptr;
    void* OutPort = nullptr;
    ThreadShared* BufferState;

    void OnProcessInner(const spa_io_position& Position);
};


struct PipeWireFilter
{
    struct pw_thread_loop* Loop = nullptr;
    struct pw_filter* Filter = nullptr;
    FilterRealTimeThread RealTimeThread;
    std::atomic_bool Live = false;

    static void OnQuit(void *UserData, int Signal);

    PipeWireFilter(ThreadShared* BufferState, int SampleRate);

    void Run();

    void Reset();

    ~PipeWireFilter();
};
