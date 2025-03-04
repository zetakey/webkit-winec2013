/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2004, 2005, 2006, 2007, 2008, 2012 Apple Inc. All rights reserved.
 *  Copyright (C) 2006 Bjoern Graf (bjoern.graf@gmail.com)
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"

#include "ButterflyInlines.h"
#include "BytecodeGenerator.h"
#include "Completion.h"
#include "CopiedSpaceInlines.h"
#include "ExceptionHelpers.h"
#include "HeapStatistics.h"
#include "InitializeThreading.h"
#include "Interpreter.h"
#include "JSArray.h"
#include "JSCTypedArrayStubs.h"
#include "JSFunction.h"
#include "JSLock.h"
#include "JSProxy.h"
#include "JSString.h"
#include "Operations.h"
#include "SamplingTool.h"
#include "StructureRareDataInlines.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wtf/CurrentTime.h>
#include <wtf/MainThread.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringBuilder.h>

#if !OS(WINDOWS)
#include <unistd.h>
#endif

#if HAVE(READLINE)
// readline/history.h has a Function typedef which conflicts with the WTF::Function template from WTF/Forward.h
// We #define it to something else to avoid this conflict.
#define Function ReadlineFunction
#include <readline/history.h>
#include <readline/readline.h>
#undef Function
#endif

#if HAVE(SYS_TIME_H)
#include <sys/time.h>
#endif

#if HAVE(SIGNAL_H)
#include <signal.h>
#endif

#if COMPILER(MSVC) && !OS(WINCE)
#include <crtdbg.h>
#include <mmsystem.h>
#include <windows.h>
#endif

#if PLATFORM(QT)
#include <QCoreApplication>
#include <QDateTime>
#endif

#if PLATFORM(IOS)
#include <fenv.h>
#include <arm/arch.h>
#endif

using namespace JSC;
using namespace WTF;

static bool fillBufferWithContentsOfFile(const String& fileName, Vector<char>& buffer);

static EncodedJSValue JSC_HOST_CALL functionPrint(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionDebug(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionDescribe(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionJSCStack(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionGC(ExecState*);
#ifndef NDEBUG
static EncodedJSValue JSC_HOST_CALL functionReleaseExecutableMemory(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionDumpCallFrame(ExecState*);
#endif
static EncodedJSValue JSC_HOST_CALL functionVersion(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionRun(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionLoad(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionCheckSyntax(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionReadline(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionPreciseTime(ExecState*);
static NO_RETURN_WITH_VALUE EncodedJSValue JSC_HOST_CALL functionQuit(ExecState*);

#if ENABLE(SAMPLING_FLAGS)
static EncodedJSValue JSC_HOST_CALL functionSetSamplingFlags(ExecState*);
static EncodedJSValue JSC_HOST_CALL functionClearSamplingFlags(ExecState*);
#endif

struct Script {
    bool isFile;
    char* argument;

    Script(bool isFile, char *argument)
        : isFile(isFile)
        , argument(argument)
    {
    }
};

class CommandLine {
public:
    CommandLine(int argc, char** argv)
        : m_interactive(false)
        , m_dump(false)
        , m_exitCode(false)
        , m_profile(false)
    {
        parseArguments(argc, argv);
    }

    bool m_interactive;
    bool m_dump;
    bool m_exitCode;
    Vector<Script> m_scripts;
    Vector<String> m_arguments;
    bool m_profile;
    String m_profilerOutput;

    void parseArguments(int, char**);
};

static const char interactivePrompt[] = "> ";

class StopWatch {
public:
    void start();
    void stop();
    long getElapsedMS(); // call stop() first

private:
    double m_startTime;
    double m_stopTime;
};

void StopWatch::start()
{
    m_startTime = currentTime();
}

void StopWatch::stop()
{
    m_stopTime = currentTime();
}

long StopWatch::getElapsedMS()
{
    return static_cast<long>((m_stopTime - m_startTime) * 1000);
}

class GlobalObject : public JSGlobalObject {
private:
    GlobalObject(JSGlobalData&, Structure*);

public:
    typedef JSGlobalObject Base;

    static GlobalObject* create(JSGlobalData& globalData, Structure* structure, const Vector<String>& arguments)
    {
        GlobalObject* object = new (NotNull, allocateCell<GlobalObject>(globalData.heap)) GlobalObject(globalData, structure);
        object->finishCreation(globalData, arguments);
        globalData.heap.addFinalizer(object, destroy);
        object->setGlobalThis(globalData, JSProxy::create(globalData, JSProxy::createStructure(globalData, object, object->prototype()), object));
        return object;
    }

    static const bool needsDestruction = false;

    static const ClassInfo s_info;
    static const GlobalObjectMethodTable s_globalObjectMethodTable;

    static Structure* createStructure(JSGlobalData& globalData, JSValue prototype)
    {
        return Structure::create(globalData, 0, prototype, TypeInfo(GlobalObjectType, StructureFlags), &s_info);
    }

    static bool javaScriptExperimentsEnabled(const JSGlobalObject*) { return true; }

protected:
    void finishCreation(JSGlobalData& globalData, const Vector<String>& arguments)
    {
        Base::finishCreation(globalData);
        
        addFunction(globalData, "debug", functionDebug, 1);
        addFunction(globalData, "describe", functionDescribe, 1);
        addFunction(globalData, "print", functionPrint, 1);
        addFunction(globalData, "quit", functionQuit, 0);
        addFunction(globalData, "gc", functionGC, 0);
#ifndef NDEBUG
        addFunction(globalData, "dumpCallFrame", functionDumpCallFrame, 0);
        addFunction(globalData, "releaseExecutableMemory", functionReleaseExecutableMemory, 0);
#endif
        addFunction(globalData, "version", functionVersion, 1);
        addFunction(globalData, "run", functionRun, 1);
        addFunction(globalData, "load", functionLoad, 1);
        addFunction(globalData, "checkSyntax", functionCheckSyntax, 1);
        addFunction(globalData, "jscStack", functionJSCStack, 1);
        addFunction(globalData, "readline", functionReadline, 0);
        addFunction(globalData, "preciseTime", functionPreciseTime, 0);
#if ENABLE(SAMPLING_FLAGS)
        addFunction(globalData, "setSamplingFlags", functionSetSamplingFlags, 1);
        addFunction(globalData, "clearSamplingFlags", functionClearSamplingFlags, 1);
#endif
        
        addConstructableFunction(globalData, "Uint8Array", constructJSUint8Array, 1);
        addConstructableFunction(globalData, "Uint8ClampedArray", constructJSUint8ClampedArray, 1);
        addConstructableFunction(globalData, "Uint16Array", constructJSUint16Array, 1);
        addConstructableFunction(globalData, "Uint32Array", constructJSUint32Array, 1);
        addConstructableFunction(globalData, "Int8Array", constructJSInt8Array, 1);
        addConstructableFunction(globalData, "Int16Array", constructJSInt16Array, 1);
        addConstructableFunction(globalData, "Int32Array", constructJSInt32Array, 1);
        addConstructableFunction(globalData, "Float32Array", constructJSFloat32Array, 1);
        addConstructableFunction(globalData, "Float64Array", constructJSFloat64Array, 1);

        JSArray* array = constructEmptyArray(globalExec(), 0);
        for (size_t i = 0; i < arguments.size(); ++i)
            array->putDirectIndex(globalExec(), i, jsString(globalExec(), arguments[i]));
        putDirect(globalData, Identifier(globalExec(), "arguments"), array);
    }

    void addFunction(JSGlobalData& globalData, const char* name, NativeFunction function, unsigned arguments)
    {
        Identifier identifier(globalExec(), name);
        putDirect(globalData, identifier, JSFunction::create(globalExec(), this, arguments, identifier.string(), function));
    }
    
    void addConstructableFunction(JSGlobalData& globalData, const char* name, NativeFunction function, unsigned arguments)
    {
        Identifier identifier(globalExec(), name);
        putDirect(globalData, identifier, JSFunction::create(globalExec(), this, arguments, identifier.string(), function, NoIntrinsic, function));
    }
};

COMPILE_ASSERT(!IsInteger<GlobalObject>::value, WTF_IsInteger_GlobalObject_false);

const ClassInfo GlobalObject::s_info = { "global", &JSGlobalObject::s_info, 0, ExecState::globalObjectTable, CREATE_METHOD_TABLE(GlobalObject) };
const GlobalObjectMethodTable GlobalObject::s_globalObjectMethodTable = { &allowsAccessFrom, &supportsProfiling, &supportsRichSourceInfo, &shouldInterruptScript, &javaScriptExperimentsEnabled };


GlobalObject::GlobalObject(JSGlobalData& globalData, Structure* structure)
    : JSGlobalObject(globalData, structure, &s_globalObjectMethodTable)
{
}

static inline SourceCode jscSource(const char* utf8, const String& filename)
{
    // Find the the first non-ascii character, or nul.
    const char* pos = utf8;
    while (*pos > 0)
        pos++;
    size_t asciiLength = pos - utf8;

    // Fast case - string is all ascii.
    if (!*pos)
        return makeSource(String(utf8, asciiLength), filename);

    // Slow case - contains non-ascii characters, use fromUTF8WithLatin1Fallback.
    ASSERT(*pos < 0);
    ASSERT(strlen(utf8) == asciiLength + strlen(pos));
    String source = String::fromUTF8WithLatin1Fallback(utf8, asciiLength + strlen(pos));
    return makeSource(source.impl(), filename);
}

EncodedJSValue JSC_HOST_CALL functionPrint(ExecState* exec)
{
    for (unsigned i = 0; i < exec->argumentCount(); ++i) {
        if (i)
            putchar(' ');

        printf("%s", exec->argument(i).toString(exec)->value(exec).utf8().data());
    }

    putchar('\n');
    fflush(stdout);
    return JSValue::encode(jsUndefined());
}

#ifndef NDEBUG
EncodedJSValue JSC_HOST_CALL functionDumpCallFrame(ExecState* exec)
{
    if (!exec->callerFrame()->hasHostCallFrameFlag())
        exec->globalData().interpreter->dumpCallFrame(exec->callerFrame());
    return JSValue::encode(jsUndefined());
}
#endif

EncodedJSValue JSC_HOST_CALL functionDebug(ExecState* exec)
{
    fprintf(stderr, "--> %s\n", exec->argument(0).toString(exec)->value(exec).utf8().data());
    return JSValue::encode(jsUndefined());
}

EncodedJSValue JSC_HOST_CALL functionDescribe(ExecState* exec)
{
    fprintf(stderr, "--> %s\n", toCString(exec->argument(0)).data());
    return JSValue::encode(jsUndefined());
}

EncodedJSValue JSC_HOST_CALL functionJSCStack(ExecState* exec)
{
    StringBuilder trace;
    trace.appendLiteral("--> Stack trace:\n");

    Vector<StackFrame> stackTrace;
    Interpreter::getStackTrace(&exec->globalData(), stackTrace);
    int i = 0;

    for (Vector<StackFrame>::iterator iter = stackTrace.begin(); iter < stackTrace.end(); iter++) {
        StackFrame level = *iter;
        trace.append(String::format("    %i   %s\n", i, level.toString(exec).utf8().data()));
        i++;
    }
    fprintf(stderr, "%s", trace.toString().utf8().data());
    return JSValue::encode(jsUndefined());
}

EncodedJSValue JSC_HOST_CALL functionGC(ExecState* exec)
{
    JSLockHolder lock(exec);
    exec->heap()->collectAllGarbage();
    return JSValue::encode(jsUndefined());
}

#ifndef NDEBUG
EncodedJSValue JSC_HOST_CALL functionReleaseExecutableMemory(ExecState* exec)
{
    JSLockHolder lock(exec);
    exec->globalData().releaseExecutableMemory();
    return JSValue::encode(jsUndefined());
}
#endif

EncodedJSValue JSC_HOST_CALL functionVersion(ExecState*)
{
    // We need this function for compatibility with the Mozilla JS tests but for now
    // we don't actually do any version-specific handling
    return JSValue::encode(jsUndefined());
}

EncodedJSValue JSC_HOST_CALL functionRun(ExecState* exec)
{
    String fileName = exec->argument(0).toString(exec)->value(exec);
    Vector<char> script;
    if (!fillBufferWithContentsOfFile(fileName, script))
        return JSValue::encode(throwError(exec, createError(exec, "Could not open file.")));

    GlobalObject* globalObject = GlobalObject::create(exec->globalData(), GlobalObject::createStructure(exec->globalData(), jsNull()), Vector<String>());

    JSValue exception;
    StopWatch stopWatch;
    stopWatch.start();
    evaluate(globalObject->globalExec(), jscSource(script.data(), fileName), JSValue(), &exception);
    stopWatch.stop();

    if (!!exception) {
        throwError(globalObject->globalExec(), exception);
        return JSValue::encode(jsUndefined());
    }
    
    return JSValue::encode(jsNumber(stopWatch.getElapsedMS()));
}

EncodedJSValue JSC_HOST_CALL functionLoad(ExecState* exec)
{
    String fileName = exec->argument(0).toString(exec)->value(exec);
    Vector<char> script;
    if (!fillBufferWithContentsOfFile(fileName, script))
        return JSValue::encode(throwError(exec, createError(exec, "Could not open file.")));

    JSGlobalObject* globalObject = exec->lexicalGlobalObject();
    
    JSValue evaluationException;
    JSValue result = evaluate(globalObject->globalExec(), jscSource(script.data(), fileName), JSValue(), &evaluationException);
    if (evaluationException)
        throwError(exec, evaluationException);
    return JSValue::encode(result);
}

EncodedJSValue JSC_HOST_CALL functionCheckSyntax(ExecState* exec)
{
    String fileName = exec->argument(0).toString(exec)->value(exec);
    Vector<char> script;
    if (!fillBufferWithContentsOfFile(fileName, script))
        return JSValue::encode(throwError(exec, createError(exec, "Could not open file.")));

    JSGlobalObject* globalObject = exec->lexicalGlobalObject();

    StopWatch stopWatch;
    stopWatch.start();

    JSValue syntaxException;
    bool validSyntax = checkSyntax(globalObject->globalExec(), jscSource(script.data(), fileName), &syntaxException);
    stopWatch.stop();

    if (!validSyntax)
        throwError(exec, syntaxException);
    return JSValue::encode(jsNumber(stopWatch.getElapsedMS()));
}

#if ENABLE(SAMPLING_FLAGS)
EncodedJSValue JSC_HOST_CALL functionSetSamplingFlags(ExecState* exec)
{
    for (unsigned i = 0; i < exec->argumentCount(); ++i) {
        unsigned flag = static_cast<unsigned>(exec->argument(i).toNumber(exec));
        if ((flag >= 1) && (flag <= 32))
            SamplingFlags::setFlag(flag);
    }
    return JSValue::encode(jsNull());
}

EncodedJSValue JSC_HOST_CALL functionClearSamplingFlags(ExecState* exec)
{
    for (unsigned i = 0; i < exec->argumentCount(); ++i) {
        unsigned flag = static_cast<unsigned>(exec->argument(i).toNumber(exec));
        if ((flag >= 1) && (flag <= 32))
            SamplingFlags::clearFlag(flag);
    }
    return JSValue::encode(jsNull());
}
#endif

EncodedJSValue JSC_HOST_CALL functionReadline(ExecState* exec)
{
    Vector<char, 256> line;
    int c;
    while ((c = getchar()) != EOF) {
        // FIXME: Should we also break on \r? 
        if (c == '\n')
            break;
        line.append(c);
    }
    line.append('\0');
    return JSValue::encode(jsString(exec, line.data()));
}

EncodedJSValue JSC_HOST_CALL functionPreciseTime(ExecState*)
{
    return JSValue::encode(jsNumber(currentTime()));
}

EncodedJSValue JSC_HOST_CALL functionQuit(ExecState*)
{
    exit(EXIT_SUCCESS);

#if COMPILER(MSVC) && OS(WINCE)
    // Without this, Visual Studio will complain that this method does not return a value.
    return JSValue::encode(jsUndefined());
#endif
}

// Use SEH for Release builds only to get rid of the crash report dialog
// (luckily the same tests fail in Release and Debug builds so far). Need to
// be in a separate main function because the jscmain function requires object
// unwinding.

#if COMPILER(MSVC) && !COMPILER(INTEL) && !defined(_DEBUG) && !OS(WINCE)
#define TRY       __try {
#define EXCEPT(x) } __except (EXCEPTION_EXECUTE_HANDLER) { x; }
#else
#define TRY
#define EXCEPT(x)
#endif

int jscmain(int argc, char** argv);

int main(int argc, char** argv)
{
#if PLATFORM(IOS)
    // Enabled IEEE754 denormal support.
    fenv_t env;
    fegetenv( &env );
    env.__fpscr &= ~0x01000000u;
    fesetenv( &env );
#endif

#if OS(WINDOWS)
#if !OS(WINCE)
    // Cygwin calls ::SetErrorMode(SEM_FAILCRITICALERRORS), which we will inherit. This is bad for
    // testing/debugging, as it causes the post-mortem debugger not to be invoked. We reset the
    // error mode here to work around Cygwin's behavior. See <http://webkit.org/b/55222>.
    ::SetErrorMode(0);
#endif

#if defined(_DEBUG)
# if _WIN32_WCE < 0x800
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
#endif
#endif

    timeBeginPeriod(1);
#endif

#if PLATFORM(QT)
    QCoreApplication app(argc, argv);
#endif

    // Initialize JSC before getting JSGlobalData.
#if ENABLE(SAMPLING_REGIONS)
    WTF::initializeMainThread();
#endif
    JSC::initializeThreading();

    // We can't use destructors in the following code because it uses Windows
    // Structured Exception Handling
    int res = 0;
    TRY
        res = jscmain(argc, argv);
    EXCEPT(res = 3)
    if (Options::logHeapStatisticsAtExit())
        HeapStatistics::reportSuccess();
    return res;
}

static bool runWithScripts(GlobalObject* globalObject, const Vector<Script>& scripts, bool dump)
{
    const char* script;
    String fileName;
    Vector<char> scriptBuffer;

    if (dump)
        JSC::Options::dumpGeneratedBytecodes() = true;

    JSGlobalData& globalData = globalObject->globalData();

#if ENABLE(SAMPLING_FLAGS)
    SamplingFlags::start();
#endif

    bool success = true;
    for (size_t i = 0; i < scripts.size(); i++) {
        if (scripts[i].isFile) {
            fileName = scripts[i].argument;
            if (!fillBufferWithContentsOfFile(fileName, scriptBuffer))
                return false; // fail early so we can catch missing files
            script = scriptBuffer.data();
        } else {
            script = scripts[i].argument;
            fileName = "[Command Line]";
        }

        globalData.startSampling();

        JSValue evaluationException;
        JSValue returnValue = evaluate(globalObject->globalExec(), jscSource(script, fileName), JSValue(), &evaluationException);
        success = success && !evaluationException;
        if (dump && !evaluationException)
            printf("End: %s\n", returnValue.toString(globalObject->globalExec())->value(globalObject->globalExec()).utf8().data());
        if (evaluationException) {
            printf("Exception: %s\n", evaluationException.toString(globalObject->globalExec())->value(globalObject->globalExec()).utf8().data());
            Identifier stackID(globalObject->globalExec(), "stack");
            JSValue stackValue = evaluationException.get(globalObject->globalExec(), stackID);
            if (!stackValue.isUndefinedOrNull())
                printf("%s\n", stackValue.toString(globalObject->globalExec())->value(globalObject->globalExec()).utf8().data());
        }

        globalData.stopSampling();
        globalObject->globalExec()->clearException();
    }

#if ENABLE(SAMPLING_FLAGS)
    SamplingFlags::stop();
#endif
#if ENABLE(SAMPLING_REGIONS)
    SamplingRegion::dump();
#endif
    globalData.dumpSampleData(globalObject->globalExec());
#if ENABLE(SAMPLING_COUNTERS)
    AbstractSamplingCounter::dump();
#endif
#if ENABLE(REGEXP_TRACING)
    globalData.dumpRegExpTrace();
#endif
    return success;
}

#define RUNNING_FROM_XCODE 0

static void runInteractive(GlobalObject* globalObject)
{
    String interpreterName("Interpreter");

    while (true) {
#if HAVE(READLINE) && !RUNNING_FROM_XCODE
        char* line = readline(interactivePrompt);
        if (!line)
            break;
        if (line[0])
            add_history(line);
        JSValue evaluationException;
        JSValue returnValue = evaluate(globalObject->globalExec(), jscSource(line, interpreterName), JSValue(), &evaluationException);
        free(line);
#else
        printf("%s", interactivePrompt);
        Vector<char, 256> line;
        int c;
        while ((c = getchar()) != EOF) {
            // FIXME: Should we also break on \r? 
            if (c == '\n')
                break;
            line.append(c);
        }
        if (line.isEmpty())
            break;
        line.append('\0');

        JSValue evaluationException;
        JSValue returnValue = evaluate(globalObject->globalExec(), jscSource(line.data(), interpreterName), JSValue(), &evaluationException);
#endif
        if (evaluationException)
            printf("Exception: %s\n", evaluationException.toString(globalObject->globalExec())->value(globalObject->globalExec()).utf8().data());
        else
            printf("%s\n", returnValue.toString(globalObject->globalExec())->value(globalObject->globalExec()).utf8().data());

        globalObject->globalExec()->clearException();
    }
    printf("\n");
}

static NO_RETURN void printUsageStatement(bool help = false)
{
    fprintf(stderr, "Usage: jsc [options] [files] [-- arguments]\n");
    fprintf(stderr, "  -d         Dumps bytecode (debug builds only)\n");
    fprintf(stderr, "  -e         Evaluate argument as script code\n");
    fprintf(stderr, "  -f         Specifies a source file (deprecated)\n");
    fprintf(stderr, "  -h|--help  Prints this help message\n");
    fprintf(stderr, "  -i         Enables interactive mode (default if no files are specified)\n");
#if HAVE(SIGNAL_H)
    fprintf(stderr, "  -s         Installs signal handlers that exit on a crash (Unix platforms only)\n");
#endif
    fprintf(stderr, "  -p <file>  Outputs profiling data to a file\n");
    fprintf(stderr, "  -x         Output exit code before terminating\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  --options                  Dumps all JSC VM options and exits\n");
    fprintf(stderr, "  --dumpOptions              Dumps all JSC VM options before continuing\n");
    fprintf(stderr, "  --<jsc VM option>=<value>  Sets the specified JSC VM option\n");
    fprintf(stderr, "\n");

    exit(help ? EXIT_SUCCESS : EXIT_FAILURE);
}

void CommandLine::parseArguments(int argc, char** argv)
{
    int i = 1;
    bool needToDumpOptions = false;
    bool needToExit = false;

    for (; i < argc; ++i) {
        const char* arg = argv[i];
        if (!strcmp(arg, "-f")) {
            if (++i == argc)
                printUsageStatement();
            m_scripts.append(Script(true, argv[i]));
            continue;
        }
        if (!strcmp(arg, "-e")) {
            if (++i == argc)
                printUsageStatement();
            m_scripts.append(Script(false, argv[i]));
            continue;
        }
        if (!strcmp(arg, "-i")) {
            m_interactive = true;
            continue;
        }
        if (!strcmp(arg, "-d")) {
            m_dump = true;
            continue;
        }
        if (!strcmp(arg, "-p")) {
            if (++i == argc)
                printUsageStatement();
            m_profile = true;
            m_profilerOutput = argv[i];
            continue;
        }
        if (!strcmp(arg, "-s")) {
#if HAVE(SIGNAL_H)
            signal(SIGILL, _exit);
            signal(SIGFPE, _exit);
            signal(SIGBUS, _exit);
            signal(SIGSEGV, _exit);
#endif
            continue;
        }
        if (!strcmp(arg, "-x")) {
            m_exitCode = true;
            continue;
        }
        if (!strcmp(arg, "--")) {
            ++i;
            break;
        }
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help"))
            printUsageStatement(true);

        if (!strcmp(arg, "--options")) {
            needToDumpOptions = true;
            needToExit = true;
            continue;
        }
        if (!strcmp(arg, "--dumpOptions")) {
            needToDumpOptions = true;
            continue;
        }

        // See if the -- option is a JSC VM option.
        // NOTE: At this point, we know that the arg starts with "--". Skip it.
        if (JSC::Options::setOption(&arg[2])) {
            // The arg was recognized as a VM option and has been parsed.
            continue; // Just continue with the next arg. 
        }

        // This arg is not recognized by the VM nor by jsc. Pass it on to the
        // script.
        m_scripts.append(Script(true, argv[i]));
    }

    if (m_scripts.isEmpty())
        m_interactive = true;

    for (; i < argc; ++i)
        m_arguments.append(argv[i]);

    if (needToDumpOptions)
        JSC::Options::dumpAllOptions(stderr);
    if (needToExit)
        exit(EXIT_SUCCESS);
}

int jscmain(int argc, char** argv)
{
    // Note that the options parsing can affect JSGlobalData creation, and thus
    // comes first.
    CommandLine options(argc, argv);
    RefPtr<JSGlobalData> globalData = JSGlobalData::create(LargeHeap);
    JSLockHolder lock(globalData.get());
    int result;

    if (options.m_profile)
        globalData->m_perBytecodeProfiler = adoptPtr(new Profiler::Database(*globalData));
    
    GlobalObject* globalObject = GlobalObject::create(*globalData, GlobalObject::createStructure(*globalData, jsNull()), options.m_arguments);
    bool success = runWithScripts(globalObject, options.m_scripts, options.m_dump);
    if (options.m_interactive && success)
        runInteractive(globalObject);

    result = success ? 0 : 3;

    if (options.m_exitCode)
        printf("jsc exiting %d\n", result);
    
    if (options.m_profile) {
        if (!globalData->m_perBytecodeProfiler->save(options.m_profilerOutput.utf8().data()))
            fprintf(stderr, "could not save profiler output.\n");
    }

    return result;
}

static bool fillBufferWithContentsOfFile(const String& fileName, Vector<char>& buffer)
{
    FILE* f = fopen(fileName.utf8().data(), "r");
    if (!f) {
        fprintf(stderr, "Could not open file: %s\n", fileName.utf8().data());
        return false;
    }

    size_t bufferSize = 0;
    size_t bufferCapacity = 1024;

    buffer.resize(bufferCapacity);

    while (!feof(f) && !ferror(f)) {
        bufferSize += fread(buffer.data() + bufferSize, 1, bufferCapacity - bufferSize, f);
        if (bufferSize == bufferCapacity) { // guarantees space for trailing '\0'
            bufferCapacity *= 2;
            buffer.resize(bufferCapacity);
        }
    }
    fclose(f);
    buffer[bufferSize] = '\0';

    if (buffer[0] == '#' && buffer[1] == '!')
        buffer[0] = buffer[1] = '/';

    return true;
}
