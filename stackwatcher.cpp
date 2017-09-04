/*
    g++ stackwatcher.cpp -O2 -Wall -use-dynld
*/

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/dos.h>

#include <cstdio>
#include <cstring>

#include <map>
#include <string>
#include <sstream>

static const int samplesPerSecond = 50;

static const float warningThreshold = 50.0f;
static const float dangerThreshold = 90.0f;

struct TimerIFace *ITimer = NULL;

struct StackInfo {
    StackInfo(unsigned upper, unsigned lower, unsigned current) :
        upper(upper),
        lower(lower),
        current(current)
    {
        total = upper - lower;
        used = upper - current;
    }

    unsigned upper;
    unsigned lower;
    unsigned current;
    unsigned used;
    unsigned total;
};

struct TaskData {
    TaskData(const STRPTR name, unsigned used, unsigned total) :
        name(name),
        maxUsage(used),
        total(total),
        warningAt(0.0f),
        dangerAt(0.0f)
    {
    }

    TaskData() :
        maxUsage(0),
        total(0),
        warningAt(0.0f),
        dangerAt(0.0f)
    {
    }

    std::string name;
    unsigned maxUsage;
    unsigned total;
    float warningAt;
    float dangerAt;
};

struct Context {
    Context() :
        timerDevice(-1),
        timerPort(NULL),
        timerReq(NULL),
        ownTask(NULL),
        oldName(NULL),
        verbose(true),
        serial(false)
    {
    }

    BYTE timerDevice;
    struct MsgPort *timerPort;
    struct TimeRequest *timerReq;
    struct Task *ownTask;
    STRPTR oldName;

    bool verbose;
    bool serial;

    std::map<struct Task *, TaskData> tasks;
    std::stringstream stream;
};

static float percentage(unsigned used, unsigned total)
{
    if (total == 0) {
        return 0.0f;
    }

    return 100.0f * used / total;
}

static void addTask(Context *ctx, struct Task *t, const STRPTR name, const StackInfo *si)
{
    if (ctx->tasks.find(t) == ctx->tasks.end()) {
        ctx->tasks[t] = TaskData(name, si->used, si->total);

        if (ctx->verbose) {
            ctx->stream << "Added task '" << name << "' (" << t << ")" << std::endl;
        }
    }
}

static void checkLimits(Context *ctx, struct Task *t, const STRPTR name, const StackInfo *si)
{
    const float p = percentage(si->used, si->total);

    if (p >= dangerThreshold) {
        if (p > ctx->tasks[t].dangerAt) {
            ctx->stream << "DANGER: '" << name << "' uses " << p << "% of its stack space" << std::endl;
            ctx->tasks[t].dangerAt = p;
        }
    } else if (p >= warningThreshold) {
        if (p > ctx->tasks[t].warningAt) {
            ctx->stream << "Warning: '" << name << "' uses " << p << "% of its stack space" << std::endl;
            ctx->tasks[t].warningAt = p;
        }
    }

    if (si->current < si->lower || si->current > si->upper) {
        ctx->stream << "ERROR: '" << name << "' stack pointer " << si->current
                    << " is outside bounds [" << si->lower << ", " << si->upper << "]" << std::endl;
    }
}

static void updateUsage(Context *ctx, struct Task *t, const STRPTR name, const StackInfo *si)
{
    if (si->used > ctx->tasks[t].maxUsage) {
        ctx->tasks[t].maxUsage = si->used;

        if (ctx->verbose) {
            ctx->stream << "'" << name << "' uses now " << si->used << " bytes of stack" << std::endl;
        }
    }
}

static void sampleStackUsage(Context *ctx, struct Task *t)
{
    const STRPTR name = ((struct Node *)t)->ln_Name;

    StackInfo si((unsigned)t->tc_SPUpper, (unsigned)t->tc_SPLower, (unsigned)t->tc_SPReg);

    addTask(ctx, t, name, &si);
    checkLimits(ctx, t, name, &si);
    updateUsage(ctx, t, name, &si);
}

static void printStatistics(Context *ctx)
{
    std::map<struct Task *, TaskData>::iterator i;

    for (i = ctx->tasks.begin(); i != ctx->tasks.end(); ++i) {
        TaskData& t = i->second;

        const float p = percentage(t.maxUsage, t.total);

        printf("%32s: %3.2f%% (%u/%u)\n", t.name.c_str(), p, t.maxUsage, t.total);
    }
}

static void iterateTaskList(Context *ctx, struct List *list)
{
    for (struct Node *exec_node = IExec->GetHead(list);
         exec_node;
         exec_node = IExec->GetSucc(exec_node)) {

         sampleStackUsage(ctx, (struct Task *)exec_node);
    }
}

static void resetTextBuffer(Context *ctx)
{
    ctx->stream.str(std::string());
    ctx->stream.clear();
}

static void printTextBuffer(Context *ctx)
{
    const std::string& s = ctx->stream.str();

    if (!s.empty()) {
        if (ctx->serial) {
            IExec->DebugPrintF("%s", s.c_str());
        }

        printf("%s", s.c_str());
        fflush(stdout);
    }
}

static void iterateTasks(Context *ctx)
{
    struct ExecBase *eb = (struct ExecBase *)SysBase;

    resetTextBuffer(ctx);

    IExec->Disable();

    iterateTaskList(ctx, &eb->TaskWait);
    iterateTaskList(ctx, &eb->TaskReady);

    IExec->Enable();

    sampleStackUsage(ctx, ctx->ownTask);

    printTextBuffer(ctx);
}

static void startTimer(Context *ctx)
{
    struct TimeVal dest, source;

    const unsigned micros = 1000000 / samplesPerSecond;

    ITimer->GetSysTime(&dest);

    source.Seconds = 0;
    source.Microseconds = micros;

    ITimer->AddTime(&dest, &source);

    ctx->timerReq->Request.io_Command = TR_ADDREQUEST;
    ctx->timerReq->Time.Seconds = dest.Seconds;
    ctx->timerReq->Time.Microseconds = dest.Microseconds;

    IExec->SendIO((struct IORequest *) ctx->timerReq);
}

static void stopTimer(Context *ctx)
{
    if (!IExec->CheckIO((struct IORequest *) ctx->timerReq)) {
        IExec->AbortIO((struct IORequest *) ctx->timerReq);
        IExec->WaitIO((struct IORequest *) ctx->timerReq);
    }
}

static bool setup(Context *ctx)
{
    bool result = false;

    ctx->ownTask = IExec->FindTask(NULL);

    ctx->oldName = ((struct Node *)ctx->ownTask)->ln_Name;
    ((struct Node *)ctx->ownTask)->ln_Name = (STRPTR)"Stackwatcher";


    ctx->timerPort = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT,
        ASOPORT_Name, "timer_port",
        TAG_DONE);

    if (!ctx->timerPort) {
        puts("Couldn't create timer port");
        goto out;
    }

    ctx->timerReq = (struct TimeRequest *)IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_Size, sizeof(struct TimeRequest),
        ASOIOR_ReplyPort, ctx->timerPort,
        TAG_DONE);

    if (!ctx->timerReq) {
        puts("Couldn't create IO request");
        goto out;
    }

    ctx->timerDevice = IExec->OpenDevice(TIMERNAME, UNIT_WAITUNTIL,
        (struct IORequest *)ctx->timerReq, 0);

    if (ctx->timerDevice) {
        printf("Couldn't open %s\n", TIMERNAME);
        goto out;
    }

    ITimer = (struct TimerIFace *) IExec->GetInterface(
        (struct Library *) ctx->timerReq->Request.io_Device, "main", 1, NULL);

    if (!ITimer) {
        puts("Failed to get ITimer interface");
        goto out;
    }

    result = true;
out:

    return result;
}

static void cleanup(Context *ctx)
{
    if (ITimer) {
        IExec->DropInterface((struct Interface *)ITimer);
    }

    if (ctx->timerReq) {
        if (ctx->timerDevice == 0) {
            IExec->CloseDevice((struct IORequest *) ctx->timerReq);
        }

        IExec->FreeSysObject(ASOT_IOREQUEST, ctx->timerReq);
    }

    if (ctx->timerPort) {
        IExec->FreeSysObject(ASOT_PORT, ctx->timerPort);
    }

    if (ctx->oldName) {
        ((struct Node *)ctx->ownTask)->ln_Name = ctx->oldName;
    }
}

static void printHelp(Context *ctx)
{
    puts("Stackwatcher started...");

    printf("\tSamples per second: %d\n", samplesPerSecond);
    printf("\tWarning threshold: %3.2f%%\n", warningThreshold);
    printf("\tDanger threshold: %3.2f%%\n", dangerThreshold);
    printf("\tQuiet mode: %s\n", ctx->verbose ? "off" : "on");
    printf("\tSerial output: %s\n", ctx->serial ? "on" : "off");

    puts("\t...press Control-C to quit");
}

static void run(Context *ctx)
{
    do {
        startTimer(ctx);

        const ULONG timerSig = 1L << ctx->timerPort->mp_SigBit;
        const ULONG sigs = IExec->Wait(SIGBREAKF_CTRL_C | timerSig);

        if (sigs & timerSig) {
            iterateTasks(ctx);
        }

        if (sigs & SIGBREAKF_CTRL_C) {
            puts("Control-C pressed - printing all statistics:\n"
                 "============================================");

            printStatistics(ctx);
            break;
        }
    } while (true);

    stopTimer(ctx);
}

static void checkArgs(Context *ctx, int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "quiet") == 0) {
            ctx->verbose = false;
        } else  if (strcmp(argv[i], "serial") == 0) {
            ctx->serial = true;
        } else {
            printf("\tUnknown parameter '%s'\n", argv[i]);
        }
    }
}

int main(int argc, char** argv)
{
    Context ctx;

    checkArgs(&ctx, argc, argv);

    printHelp(&ctx);

    if (setup(&ctx)) {
        run(&ctx);
    }

    cleanup(&ctx);

    return 0;
}

