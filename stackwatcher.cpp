/*
    Stackwatcher - a shell tool to watch stack usage on AmigaOS 4.

    Written by Juha Niemimaki.

    PUBLIC DOMAIN.
*/

#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/dos.h>

#include <cstdio>
#include <cstring>

#include <map>
#include <string>
#include <sstream>

static __attribute__((used)) const char *versionString = "$VER: Stackwatcher 0.1 (9.9.2017)";

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

struct StackWatcher {
    StackWatcher(int argc, char **argv) :
        timerDevice(-1),
        timerPort(NULL),
        timerReq(NULL),
        ownTask(NULL),
        oldName(NULL),
        verbose(true),
        serial(false)
    {
        checkArgs(argc, argv);
        printHelp();
    }

    ~StackWatcher()
    {
        cleanup();
    }

    bool taskPtrFound(struct Task *t);
    bool sameName(struct Task *t, const STRPTR name);
    void addTask(struct Task *t, const STRPTR name, const StackInfo *si);
    void checkLimits(struct Task *t, const STRPTR name, const StackInfo *si);
    void updateUsage(struct Task *t, const STRPTR name, const StackInfo *si);
    void sampleStackUsage(struct Task *t);
    void printStatistics();
    void iterateTaskList(struct List *list);
    void resetTextBuffer();
    void printTextBuffer();
    void iterateTasks();
    void startTimer();
    void stopTimer();
    bool setup();
    void printHelp() const;
    void cleanup();
    void run();
    void checkArgs(int argc, char** argv);

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

bool StackWatcher::taskPtrFound(struct Task *t)
{
    return tasks.find(t) != tasks.end();
}

bool StackWatcher::sameName(struct Task *t, const STRPTR name)
{
    return !strcmp(name, tasks[t].name.c_str());
}

void StackWatcher::addTask(struct Task *t, const STRPTR name, const StackInfo *si)
{
    // It's possible that task y replaces task x in memory. Should we keep book on all tasks
    // somehow, alive or dead?
    if (taskPtrFound(t) && sameName(t, name)) {
        return;
    }

    tasks[t] = TaskData(name, si->used, si->total);

    if (verbose) {
        stream << "Added task '" << name << "' (@" << t <<
                  ") stack: " << si->used << "/" << si->total << std::endl;
    }
}

void StackWatcher::checkLimits(struct Task *t, const STRPTR name, const StackInfo *si)
{
    const float p = percentage(si->used, si->total);

    if (p >= dangerThreshold) {
        if (p > tasks[t].dangerAt) {
            stream << "DANGER: '" << name << "' uses " << p << "% of its stack space" << std::endl;
            tasks[t].dangerAt = p;
        }
    } else if (p >= warningThreshold) {
        if (p > tasks[t].warningAt) {
            stream << "Warning: '" << name << "' uses " << p << "% of its stack space" << std::endl;
            tasks[t].warningAt = p;
        }
    }

    if (si->current < si->lower || si->current > si->upper) {
        stream << "ERROR: '" << name << "' stack pointer " << si->current <<
                  " is outside bounds [" << si->lower << ", " << si->upper << "]" << std::endl;
    }
}

void StackWatcher::updateUsage(struct Task *t, const STRPTR name, const StackInfo *si)
{
    if (si->used > tasks[t].maxUsage) {
        tasks[t].maxUsage = si->used;

        if (verbose) {
            stream << "'" << name << "' uses now " << si->used << " bytes of stack" << std::endl;
        }
    }

    if (si->total != tasks[t].total) {
        if (verbose) {
            stream << "'" << name << "': stack size changed from " << tasks[t].total <<
                      " to " << si->total << std::endl;
        }

        tasks[t].total = si->total;
    }
}

void StackWatcher::sampleStackUsage(struct Task *t)
{
    const STRPTR name = ((struct Node *)t)->ln_Name;

    StackInfo si((unsigned)t->tc_SPUpper, (unsigned)t->tc_SPLower, (unsigned)t->tc_SPReg);

    addTask(t, name, &si);
    checkLimits(t, name, &si);
    updateUsage(t, name, &si);
}

void StackWatcher::printStatistics()
{
    std::map<struct Task *, TaskData>::iterator i;

    for (i = tasks.begin(); i != tasks.end(); ++i) {
        TaskData& t = i->second;

        const float p = percentage(t.maxUsage, t.total);

        std::streamsize oldWidth = stream.width();

        stream.width(40);

        stream << t.name;

        stream.width(oldWidth);

        stream << ": " << p << "% " << "(" << t.maxUsage << "/" << t.total << ")" << std::endl;
    }

    const std::string& s = stream.str();
    if (serial) {
        IExec->DebugPrintF("%s", s.c_str());
    }

    printf("%s", s.c_str());

    resetTextBuffer();
}

void StackWatcher::iterateTaskList(struct List *list)
{
    for (struct Node *exec_node = IExec->GetHead(list);
         exec_node;
         exec_node = IExec->GetSucc(exec_node)) {

         sampleStackUsage((struct Task *)exec_node);
    }
}

void StackWatcher::resetTextBuffer()
{
    stream.str(std::string());
    stream.clear();
}

void StackWatcher::printTextBuffer()
{
    const std::string& s = stream.str();

    if (!s.empty()) {
        if (serial) {
            IExec->DebugPrintF("%s", s.c_str());
        }

        printf("%s", s.c_str());
        fflush(stdout);
    }

    resetTextBuffer();
}

void StackWatcher::iterateTasks()
{
    struct ExecBase *eb = (struct ExecBase *)SysBase;

    IExec->Disable();

    iterateTaskList(&eb->TaskWait);
    iterateTaskList(&eb->TaskReady);

    IExec->Enable();

    sampleStackUsage(ownTask);

    printTextBuffer();
}

void StackWatcher::startTimer()
{
    struct TimeVal dest, source;

    const unsigned micros = 1000000 / samplesPerSecond;

    ITimer->GetSysTime(&dest);

    source.Seconds = 0;
    source.Microseconds = micros;

    ITimer->AddTime(&dest, &source);

    timerReq->Request.io_Command = TR_ADDREQUEST;
    timerReq->Time.Seconds = dest.Seconds;
    timerReq->Time.Microseconds = dest.Microseconds;

    IExec->SendIO((struct IORequest *) timerReq);
}

void StackWatcher::stopTimer()
{
    struct IORequest *req = (struct IORequest *)timerReq;

    if (!IExec->CheckIO(req)) {
        IExec->AbortIO(req);
        IExec->WaitIO(req);
    }
}

bool StackWatcher::setup()
{
    bool result = false;

    ownTask = IExec->FindTask(NULL);

    oldName = ((struct Node *)ownTask)->ln_Name;
    ((struct Node *)ownTask)->ln_Name = (STRPTR)"Stackwatcher";


    timerPort = (struct MsgPort *)IExec->AllocSysObjectTags(ASOT_PORT,
        ASOPORT_Name, "timer_port",
        TAG_DONE);

    if (!timerPort) {
        puts("Couldn't create timer port");
        goto out;
    }

    timerReq = (struct TimeRequest *)IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_Size, sizeof(struct TimeRequest),
        ASOIOR_ReplyPort, timerPort,
        TAG_DONE);

    if (!timerReq) {
        puts("Couldn't create IO request");
        goto out;
    }

    timerDevice = IExec->OpenDevice(TIMERNAME, UNIT_WAITUNTIL,
        (struct IORequest *)timerReq, 0);

    if (timerDevice) {
        printf("Couldn't open %s\n", TIMERNAME);
        goto out;
    }

    ITimer = (struct TimerIFace *) IExec->GetInterface(
        (struct Library *) timerReq->Request.io_Device, "main", 1, NULL);

    if (!ITimer) {
        puts("Failed to get ITimer interface");
        goto out;
    }

    result = true;
out:

    return result;
}

void StackWatcher::cleanup()
{
    if (ITimer) {
        IExec->DropInterface((struct Interface *)ITimer);
    }

    if (timerReq) {
        if (timerDevice == 0) {
            IExec->CloseDevice((struct IORequest *) timerReq);
        }

        IExec->FreeSysObject(ASOT_IOREQUEST, timerReq);
    }

    if (timerPort) {
        IExec->FreeSysObject(ASOT_PORT, timerPort);
    }

    if (oldName) {
        ((struct Node *)ownTask)->ln_Name = oldName;
    }
}

void StackWatcher::printHelp() const
{
    puts("Stackwatcher started...");

    printf("\tSamples per second: %d\n", samplesPerSecond);
    printf("\tWarning threshold: %3.2f%%\n", warningThreshold);
    printf("\tDanger threshold: %3.2f%%\n", dangerThreshold);
    printf("\tQuiet mode: %s\n", verbose ? "off" : "on");
    printf("\tSerial output: %s\n", serial ? "on" : "off");

    puts("\t...press Control-C to quit");
}

void StackWatcher::run()
{
    do {
        startTimer();

        const ULONG timerSig = 1L << timerPort->mp_SigBit;
        const ULONG sigs = IExec->Wait(SIGBREAKF_CTRL_C | timerSig);

        if (sigs & timerSig) {
            iterateTasks();
        }

        if (sigs & SIGBREAKF_CTRL_C) {
            puts("Control-C pressed - printing all statistics:\n"
                 "============================================");

            printStatistics();
            break;
        }
    } while (true);

    stopTimer();
}

void StackWatcher::checkArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "quiet") == 0) {
            verbose = false;
        } else  if (strcmp(argv[i], "serial") == 0) {
            serial = true;
        } else {
            printf("\tUnknown parameter '%s'\n", argv[i]);
        }
    }
}

int main(int argc, char** argv)
{
    StackWatcher sw(argc, argv);

    if (sw.setup()) {
        sw.run();
    }

    return 0;
}

