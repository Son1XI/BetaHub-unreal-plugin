#include "BH_Runnable.h"
#include "BH_Log.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFilemanager.h"

FString FBH_Runnable::RunCommand(const FString& Command, const FString& Params, const FString& WorkingDirectory)
{
    int32 ExitCode;
    return RunCommand(Command, Params, WorkingDirectory, ExitCode);
}

FString FBH_Runnable::RunCommand(const FString& Command, const FString& Params, const FString& WorkingDirectory, int32& ExitCode)
{
    FBH_Runnable Runnable(Command, Params, WorkingDirectory);
    Runnable.WaitForExit();

    Runnable.IsProcessRunning(&ExitCode);

    return Runnable.GetBufferedOutput();
}

FBH_Runnable::FBH_Runnable(const FString& Command, const FString& Params, const FString& WorkingDirectory)
    : Command(Command), Params(Params), WorkingDirectory(WorkingDirectory), ProcessHandle(nullptr),
      StdInReadPipe(nullptr), StdInWritePipe(nullptr), StdOutReadPipe(nullptr), StdOutWritePipe(nullptr), StopTaskCounter(0),
      bTerminateByStdinFlag(false)
{
    FPlatformProcess::CreatePipe(StdOutReadPipe, StdOutWritePipe);
    FPlatformProcess::CreatePipe(StdInReadPipe, StdInWritePipe, true);
    Thread = FRunnableThread::Create(this, TEXT("FBH_RunnableThread"), 0, TPri_Normal);
    if (!Thread)
    {
        UE_LOG(LogBetaHub, Error, TEXT("Failed to create runnable thread."));
    }
}

FBH_Runnable::~FBH_Runnable()
{
    Terminate();
    if (Thread)
    {
        Thread->WaitForCompletion();
        delete Thread;
        Thread = nullptr;
    }

    FPlatformProcess::ClosePipe(StdInReadPipe, StdInWritePipe);
    FPlatformProcess::ClosePipe(StdOutReadPipe, StdOutWritePipe);
}

uint32 FBH_Runnable::Run()
{
    UE_LOG(LogBetaHub, Log, TEXT("Starting process %s %s."), *Command, *Params);
    
    ProcessHandle = FPlatformProcess::CreateProc(*Command, *Params, true, false, false, nullptr, 0, *WorkingDirectory, StdOutWritePipe, StdInReadPipe);
    if (!ProcessHandle.IsValid())
    {
        UE_LOG(LogBetaHub, Error, TEXT("Failed to start process."));
        return 1;
    }

    bool bExitedGracefully = false;

    while (StopTaskCounter.GetValue() == 0)
    {
        FString Output = FPlatformProcess::ReadPipe(StdOutReadPipe);
        if (!Output.IsEmpty())
        {
            std::lock_guard<std::mutex> lock(BufferMutex);
            OutputBuffer += Output;
        }

        int exitCode;
        if (IsProcessRunning(&exitCode))
        {
            FPlatformProcess::Sleep(0.1f);
        }
        else
        {
            UE_LOG(LogBetaHub, Log, TEXT("Process exited with code %d."), exitCode);
            bExitedGracefully = true;
            StopTaskCounter.Increment();
        }
    }

    if (!bExitedGracefully)
    {
        if (bTerminateByStdinFlag)
        {
            // close stdin pipe to terminate the process
            FPlatformProcess::ClosePipe(StdInReadPipe, StdInWritePipe);
            StdInReadPipe = nullptr;
            StdInWritePipe = nullptr;
        }
        else
        {
            TerminateProcess();
        }

        UE_LOG(LogBetaHub, Log, TEXT("Process stopped."));
    }

    return 0;
}

void FBH_Runnable::TerminateProcess()
{
    if (ProcessHandle.IsValid())
    {
        FPlatformProcess::TerminateProc(ProcessHandle);
        FPlatformProcess::WaitForProc(ProcessHandle);
        FPlatformProcess::CloseProc(ProcessHandle);
    }
}

void FBH_Runnable::WriteToPipe(const TArray<uint8>& Data)
{
    if (IsProcessRunning())
    {
        int32 BytesWritten;
        FPlatformProcess::WritePipe(StdInWritePipe, Data.GetData(), Data.Num(), &BytesWritten);
        // UE_LOG(LogBetaHub, Log, TEXT("Written %d bytes to pipe."), BytesWritten); // Added log for debug purposes
    }
    else
    {
        UE_LOG(LogBetaHub, Warning, TEXT("Process is not running, skipping write."));
    }
}

FString FBH_Runnable::GetBufferedOutput()
{
    std::lock_guard<std::mutex> lock(BufferMutex);
    FString BufferedOutput = OutputBuffer;
    OutputBuffer.Empty();
    return BufferedOutput;
}

void FBH_Runnable::Terminate(bool bCloseStdin)
{
    if (Thread)
    {
        UE_LOG(LogBetaHub, Log, TEXT("Stopping process %s %s."), *Command, *Params);

        if (bCloseStdin)
        {
            bTerminateByStdinFlag = true;
        }

        StopTaskCounter.Increment();

        Thread->WaitForCompletion();
        delete Thread;
        Thread = nullptr;
    }
}

bool FBH_Runnable::IsProcessRunning(int32* ExitCode)
{
    int32 exitCode;
    if (FPlatformProcess::GetProcReturnCode(ProcessHandle, &exitCode))
    {
        if (ExitCode)
        {
            *ExitCode = exitCode;
        }
        return false;
    }
    else
    {
        return true;
    }
}

void FBH_Runnable::WaitForExit()
{
    if (Thread)
    {
        Thread->WaitForCompletion();
    }
}