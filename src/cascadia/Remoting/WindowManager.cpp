// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WindowManager.h"
#include "MonarchFactory.h"
#include "CommandlineArgs.h"

#include "WindowManager.g.cpp"
#include "../../types/inc/utils.hpp"

using namespace winrt;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::Foundation;
using namespace ::Microsoft::Console;

namespace winrt::Microsoft::Terminal::Remoting::implementation
{
    WindowManager::WindowManager()
    {
        _monarchWaitInterrupt.create();

        // Register with COM as a server for the Monarch class
        _registerAsMonarch();
        // Instantiate an instance of the Monarch. This may or may not be in-proc!
        _createMonarchAndCallbacks();
    }

    WindowManager::~WindowManager()
    {
        // IMPORTANT! Tear down the registration as soon as we exit. If we're not a
        // real peasant window (the monarch passed our commandline to someone else),
        // then the monarch dies, we don't want our registration becoming the active
        // monarch!
        CoRevokeClassObject(_registrationHostClass);
        _registrationHostClass = 0;
        _monarchWaitInterrupt.SetEvent();

        if (_electionThread.joinable())
        {
            _electionThread.join();
        }
    }

    void WindowManager::ProposeCommandline(const Remoting::CommandlineArgs& args)
    {
        const bool isKing = _areWeTheKing();
        // If we're the king, we _definitely_ want to process the arguments, we were
        // launched with them!
        //
        // Otherwise, the King will tell us if we should make a new window
        _shouldCreateWindow = isKing ||
                              _monarch.ProposeCommandline(args);

        // TODO:projects/5 The monarch may respond back "you should be a new
        // window, with ID,name of (id, name)". Really the responses are:
        // * You should not create a new window
        // * Create a new window (but without a given ID or name). The Monarch
        //   will assign your ID/name later
        // * Create a new window, and you'll have this ID or name
        //   - This is the case where the user provides `wt -w 1`, and there's
        //     no existing window 1

        if (_shouldCreateWindow)
        {
            // If we should create a new window, then instantiate our Peasant
            // instance, and tell that peasant to handle that commandline.
            _createOurPeasant();

            // Spawn a thread to wait on the monarch, and handle the election
            if (!isKing)
            {
                _createPeasantThread();
            }

            _peasant.ExecuteCommandline(args);
        }
        // Otherwise, we'll do _nothing_.
    }

    bool WindowManager::ShouldCreateWindow()
    {
        return _shouldCreateWindow;
    }

    void WindowManager::_registerAsMonarch()
    {
        winrt::check_hresult(CoRegisterClassObject(Monarch_clsid,
                                                   winrt::make<::MonarchFactory>().get(),
                                                   CLSCTX_LOCAL_SERVER,
                                                   REGCLS_MULTIPLEUSE,
                                                   &_registrationHostClass));
    }

    void WindowManager::_createMonarch()
    {
        // Heads up! This only works because we're using
        // "metadata-based-marshalling" for our WinRT types. That means the OS is
        // using the .winmd file we generate to figure out the proxy/stub
        // definitions for our types automatically. This only works in the following
        // cases:
        //
        // * If we're running unpackaged: the .winmd must be a sibling of the .exe
        // * If we're running packaged: the .winmd must be in the package root
        _monarch = create_instance<Remoting::Monarch>(Monarch_clsid,
                                                      CLSCTX_LOCAL_SERVER);
    }

    void WindowManager::_createMonarchAndCallbacks()
    {
        _createMonarch();
        const auto isKing = _areWeTheKing();

        TraceLoggingWrite(g_hRemotingProvider,
                          "WindowManager_ConnectedToMonarch",
                          TraceLoggingUInt64(_monarch.GetPID(), "monarchPID", "The PID of the new Monarch"),
                          TraceLoggingBoolean(isKing, "isKing", "true if we are the new monarch"),
                          TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

        if (_peasant)
        {
            // Inform the monarch of the time we were last activated
            _monarch.HandleActivatePeasant(_peasant.GetLastActivatedArgs());
        }

        if (!isKing)
        {
            return;
        }
        // Here, we're the king!
        //
        // This is where you should do any aditional setup that might need to be
        // done when we become the king. THis will be called both for the first
        // window, and when the current monarch dies.

        _monarch.FindTargetWindowRequested({ this, &WindowManager::_raiseFindTargetWindowRequested });
    }

    bool WindowManager::_areWeTheKing()
    {
        const auto kingPID{ _monarch.GetPID() };
        const auto ourPID{ GetCurrentProcessId() };
        return (ourPID == kingPID);
    }

    Remoting::IPeasant WindowManager::_createOurPeasant()
    {
        auto p = winrt::make_self<Remoting::implementation::Peasant>();
        _peasant = *p;
        _monarch.AddPeasant(_peasant);

        TraceLoggingWrite(g_hRemotingProvider,
                          "WindowManager_CreateOurPeasant",
                          TraceLoggingUInt64(_peasant.GetID(), "peasantID", "The ID of our new peasant"),
                          TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

        return _peasant;
    }

    bool WindowManager::_electionNight2020()
    {
        _createMonarchAndCallbacks();

        // Tell the new monarch who we are. We might be that monarch!
        _monarch.AddPeasant(_peasant);

        if (_areWeTheKing())
        {
            // This is only called when a _new_ monarch is elected. So don't do
            // anything here that needs to be done for all monarch windows. This
            // should only be for work that's done when a window _becomes_ a
            // monarch, after the death of the previous monarch.
            return true;
        }
        return false;
    }

    void WindowManager::_createPeasantThread()
    {
        // If we catch an exception trying to get at the monarch ever, we can
        // set the _monarchWaitInterrupt, and use that to trigger a new
        // election. Though, we wouldn't be able to retry the function that
        // caused the exception in the first place...

        _electionThread = std::thread([this] {
            _waitOnMonarchThread();
        });
    }

    void WindowManager::_waitOnMonarchThread()
    {
        HANDLE waits[2];
        waits[1] = _monarchWaitInterrupt.get();

        bool exitRequested = false;
        while (!exitRequested)
        {
            wil::unique_handle hMonarch{ OpenProcess(PROCESS_ALL_ACCESS,
                                                     FALSE,
                                                     static_cast<DWORD>(_monarch.GetPID())) };
            // TODO:MG If we fail to open the monarch, then they don't exist
            //  anymore! Go straight to an election.
            //
            // TODO:MG At any point in all this, the current monarch might die.
            // We go straight to a new election, right? Worst case, eventually,
            // we'll become the new monarch.
            //
            // if (hMonarch.get() == nullptr)
            // {
            //     const auto gle = GetLastError();
            //     return false;
            // }
            waits[0] = hMonarch.get();
            auto waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

            switch (waitResult)
            {
            case WAIT_OBJECT_0 + 0: // waits[0] was signaled

                TraceLoggingWrite(g_hRemotingProvider,
                                  "WindowManager_MonarchDied",
                                  TraceLoggingUInt64(_peasant.GetID(), "peasantID", "Our peasant ID"),
                                  TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));
                // Connect to the new monarch, which might be us!
                // If we become the monarch, then we'll return true and exit this thread.
                exitRequested = _electionNight2020();
                break;
            case WAIT_OBJECT_0 + 1: // waits[1] was signaled

                TraceLoggingWrite(g_hRemotingProvider,
                                  "WindowManager_MonarchWaitInterrupted",
                                  TraceLoggingUInt64(_peasant.GetID(), "peasantID", "Our peasant ID"),
                                  TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

                exitRequested = true;
                break;

            case WAIT_TIMEOUT:
                printf("Wait timed out. This should be impossible.\n");
                exitRequested = true;
                break;

            // Return value is invalid.
            default:
            {
                auto gle = GetLastError();
                printf("WaitForMultipleObjects returned: %d\n", waitResult);
                printf("Wait error: %d\n", gle);
                ExitProcess(0);
            }
            }
        }
    }

    Remoting::Peasant WindowManager::CurrentWindow()
    {
        return _peasant;
    }

    void WindowManager::_raiseFindTargetWindowRequested(const winrt::Windows::Foundation::IInspectable& sender,
                                                        const winrt::Microsoft::Terminal::Remoting::FindTargetWindowArgs& args)
    {
        _FindTargetWindowRequestedHandlers(sender, args);
    }
}
